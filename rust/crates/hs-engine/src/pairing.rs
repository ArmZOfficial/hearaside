//! Pairing a friend input (a HEARASIDE Track in "friend input" mode) with the Track at the end of its DAW
//! channel (S7), pairs chosen by hand and remembered with the project, and the probe sweep that finds the
//! Track by sound. Port of the matching parts of `HubEngine::measureLatencies`, `pairWith`, `remember`,
//! `applyMemory`, `startProbe`, `endProbe` and `probeStep` (`libs/ssengine/src/hub_engine.cpp`).
use crate::hub_engine::{HUB_STALE_NS, Role, SLOT_STALE_NS};
use crate::measure::{HubMeasurer, PairMemory, best_lag};
use hs_bus::bus::{
    beacon_publish, bits_float, float_bits, post_reply, read_feeder_identity, read_slot_identity,
    reclaim_dead_slots, try_claim_hub,
};
use hs_bus::protocol::*;
use hs_bus::shm::now_ns;
use std::sync::atomic::Ordering::{AcqRel, Acquire, Relaxed, Release};

const FEEDER_STALE_NS: u64 = 800_000_000;

/// A manual pair in one word: slot + 1 (8 bits), the slot's claim (32), the friend id (24). Valid while
/// the slot still has the same claim, so a Track that took over the index is not mistaken for the one
/// that was chosen.
pub(crate) fn pair_word(b: &BusLayout, slot: usize, fid: u32) -> u64 {
    let claim = b.slots[slot].claimNs.load(Relaxed);
    (slot as u64 + 1)
        | u64::from((claim ^ (claim >> 32)) as u32) << 8
        | u64::from(fid & 0xFF_FFFF) << 40
}

pub(crate) fn pair_slot(word: u64) -> i32 {
    (word & 0xFF) as i32 - 1
}

/// The Track at the end of the channel carries this friend: let go of it when the pairing ends.
fn unpair_slot(b: &BusLayout, slot: i32, fid: u32) {
    let Some(s) = usize::try_from(slot).ok().filter(|&s| s < kMaxSlots) else {
        return;
    };
    let sh = &b.slots[s];
    if sh.fedBy.load(Relaxed) != fid {
        return;
    }
    // ViewersViaHub first (the Hub also treats fedBy 0 as nobody, which covers the moment in between)
    sh.flags.fetch_and(!SlotFlags::kFlagViewersViaHub, Release);
    sh.fedBy.store(0, Release);
    sh.fxLatencyBits.store(0, Relaxed);
}

fn publish_pair(b: &BusLayout, fh: &FriendHeader, fid: u32, slot: usize, li: f64) {
    let old = fh.outSlot.load(Relaxed);
    if old != slot as i32 {
        unpair_slot(b, old, fid);
    }
    fh.outSlot.store(slot as i32, Release);
    let sh = &b.slots[slot];
    sh.fedBy.store(fid, Release);
    sh.fxLatencyBits.store(float_bits(li as f32), Relaxed);
    // always through the Hub (Line up or not): only the Hub can keep the friend out of the mix that
    // friends hear. Line up off = D is 0, so the friend is simply not delayed.
    sh.flags.fetch_or(SlotFlags::kFlagViewersViaHub, Release);
}

/// Probe timing, in history frames (rate = sample rate / 4): by time, so it holds at 44.1 / 96 kHz too.
/// Silence before the sweep starts (the mute needs ~20 ms), and after it has left.
fn probe_lead_in(rate: f64) -> u64 {
    (0.04 * rate) as u64
}

/// The 300 ms sweep + the longest chain latency we look for + the Hub's read.
fn probe_listen(rate: f64) -> u64 {
    (0.475 * rate) as u64
}

fn friend_of(b: &BusLayout, fid: u32) -> Option<usize> {
    if fid == 0 {
        return None;
    }
    (0..kMaxFriends).find(|&j| b.friends.0[j].id.load(Acquire) == fid)
}

fn feeder_live(f: &FeederRecord, now: u64) -> bool {
    // (saturating: a heartbeat written after `now` was read made the C++ see a 584-year-old feeder)
    f.state.load(Acquire) == SlotState::kSlotActive
        && now.saturating_sub(f.heartbeatNs.load(Relaxed)) <= FEEDER_STALE_NS
}

fn read_tap(b: &BusLayout, feeder: usize, tap_w: u64, m: usize) -> Vec<f32> {
    // SAFETY: the tap is plain floats in shared memory written by the feeder; a torn read is only noise
    // in a correlation.
    let tap = unsafe { &*b.feederTap.0[feeder].mono.get() };
    (0..m)
        .map(|k| tap[((tap_w - m as u64 + k as u64) % kTapFrames as u64) as usize])
        .collect()
}

fn prefix_energy(v: &[f32]) -> Vec<f64> {
    let mut cum = Vec::with_capacity(v.len() + 1);
    cum.push(0.0);
    let mut acc = 0.0f64;
    for &s in v {
        acc += f64::from(s) * f64::from(s);
        cum.push(acc);
    }
    cum
}

impl HubMeasurer {
    /// Call periodically (e.g. 2 Hz) from any non-audio thread: claims ownership if free / stale, reclaims dead
    /// slots, applies remembered pairs, refreshes the beacon. Needs no access to the audio side (the C++
    /// version, and a Rust one on `&mut self`, had to stop the audio thread for it).
    pub fn maintain(&self) {
        let Some(shm) = self.snapshot_bus() else {
            self.shared.role.store(0, Release);
            return;
        };
        let b = shm.layout();
        let owner = try_claim_hub(b, self.shared.token, HUB_STALE_NS);
        self.shared.role.store(if owner { 1 } else { 2 }, Release);
        if owner {
            reclaim_dead_slots(b, SLOT_STALE_NS);
            self.apply_memory(b);
            if let Some(bc) = crate::measure::lock(&self.shared.beacon).as_ref() {
                beacon_publish(bc.header());
            }
        }
        self.shared.rescan.store(true, Release);
    }

    pub fn role(&self) -> Role {
        match self.shared.role.load(Acquire) {
            1 => Role::Owner,
            2 => Role::Secondary,
            _ => Role::Disconnected,
        }
    }

    fn reserved(&self, j: usize, s: usize) -> bool {
        // a Track another friend input chose by hand is theirs: matching it by name or sound would flip
        // fedBy between two friends
        (0..kMaxFriends)
            .any(|k| k != j && pair_slot(self.shared.manual_pair[k].load(Acquire)) == s as i32)
    }

    fn release_friend_output(&self, b: &BusLayout, j: usize, fid: u32) {
        let fh = &b.friends.0[j];
        let out_s = fh.outSlot.load(Relaxed);
        if (0..kMaxSlots as i32).contains(&out_s) {
            unpair_slot(b, out_s, fid);
            fh.outSlot.store(-1, Release);
        }
    }

    /// S7: match active feeders with the Track at the end of their channel (by hand, by name, by sound)
    /// and measure Li, the latency of the plug-ins between them.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(crate) fn pair_friend_inputs(
        &self,
        b: &BusLayout,
        hist: &crate::measure::History,
        w: u64,
        m: usize,
        n: usize,
        k: usize,
        rate: f64,
        ms_per_lag: f64,
        have_hist: bool,
    ) {
        let now = now_ns();
        let mut x = Vec::new();
        for j in 0..kMaxFriends {
            let fh = &b.friends.0[j];
            let fid = fh.id.load(Acquire);
            if fid == 0 {
                continue;
            }
            let fidx = fh.feeder.load(Acquire);
            let feeder_ok = usize::try_from(fidx)
                .ok()
                .filter(|&f| f < kMaxFeeders)
                .filter(|&f| {
                    let fr = &b.feeders[f];
                    feeder_live(fr, now) && fr.friendId.load(Relaxed) == fid
                });
            let Some(fidx) = feeder_ok else {
                // the friend input is gone (or not this friend's any more): so is its choice
                self.release_friend_output(b, j, fid);
                self.shared.manual_pair[j].store(0, Release);
                continue;
            };
            let feeder = &b.feeders[fidx];
            let feeder_name = read_feeder_identity(feeder)
                .map(|(nm, _)| nm)
                .unwrap_or_default();
            let mut matched: Option<usize> = None;

            // 0. A pair chosen by hand wins, whatever the names say; it ends with its Track
            let word = self.shared.manual_pair[j].load(Acquire);
            if word != 0 {
                let s = pair_slot(word);
                let ok = usize::try_from(s).ok().filter(|&s| {
                    s < kMaxSlots
                        && b.slots[s].state.load(Acquire) == SlotState::kSlotActive
                        && b.slots[s].flags.load(Relaxed) & SlotFlags::kFlagApp == 0
                        && pair_word(b, s, fid) == word
                });
                if ok.is_some() {
                    matched = ok;
                } else if self.shared.manual_pair[j]
                    .compare_exchange(word, 0, AcqRel, Relaxed)
                    .is_ok()
                {
                    self.shared.manual_li[j].store(0, Relaxed);
                }
            }

            // 1. Fast match by track name
            if matched.is_none() && !feeder_name.is_empty() {
                matched = (0..kMaxSlots).find(|&s| {
                    b.slots[s].state.load(Acquire) == SlotState::kSlotActive
                        && !self.reserved(j, s)
                        && b.slots[s].flags.load(Relaxed) & SlotFlags::kFlagApp == 0
                        && read_slot_identity(&b.slots[s])
                            .is_some_and(|(nm, _, _)| nm == feeder_name)
                });
            }

            // 2. Correlation match if not matched by name (needs history and a tap long enough for the window)
            let tap_w = feeder.tapWrite.0.load(Acquire);
            let tap_n = n;
            let ktap = k.min((rate * 0.2) as usize);
            let f_start = m - ktap - tap_n;
            // (the C++ read a window longer than the 8192-frame tap at 96 kHz and correlated stale samples)
            let tap_fits = m - (f_start - ktap) <= kTapFrames;
            let mut tap: Option<(Vec<f32>, Vec<f64>)> = None;
            if have_hist && tap_fits && tap_w >= m as u64 {
                let f_tap = read_tap(b, fidx, tap_w, m);
                let cum_tap = prefix_energy(&f_tap);
                let e_tap = cum_tap[f_start + tap_n] - cum_tap[f_start];
                if e_tap > tap_n as f64 * 1e-5 && f_start > 0 {
                    if matched.is_none() {
                        let mut tried = 0;
                        for s in 0..kMaxSlots {
                            if tried >= 8 {
                                break;
                            }
                            if b.slots[s].state.load(Acquire) != SlotState::kSlotActive
                                || self.reserved(j, s)
                                || b.slots[s].flags.load(Relaxed) & SlotFlags::kFlagApp != 0
                            {
                                continue;
                            }
                            tried += 1;
                            hist.copy_last(hist.slot_ring(s), w, m, &mut x);
                            let ey: f64 = x[f_start..f_start + tap_n]
                                .iter()
                                .map(|&v| f64::from(v) * f64::from(v))
                                .sum();
                            if ey < tap_n as f64 * 1e-6 {
                                continue;
                            }
                            let (lag, _) = best_lag(
                                &x,
                                &f_tap,
                                tap_n,
                                f_start,
                                0,
                                ktap as i32,
                                &cum_tap,
                                ey,
                                0.6,
                                false,
                                4,
                            );
                            if lag.is_some() {
                                matched = Some(s);
                                break;
                            }
                        }
                    }
                    tap = Some((f_tap, cum_tap));
                }
            }

            // 3. Measure Li and publish the pairing
            let Some(s) = matched else {
                self.release_friend_output(b, j, fid);
                continue;
            };
            let probed = self.shared.manual_li[j].load(Relaxed);
            let fallback_li = if probed != 0 {
                f64::from(probed - 1) * 0.01
            } else {
                f64::from(bits_float(b.slots[s].chainLatencyBits.load(Relaxed)))
            };
            let li = match &tap {
                Some((f_tap, cum_tap)) => {
                    hist.copy_last(hist.slot_ring(s), w, m, &mut x);
                    let ey: f64 = x[f_start..f_start + tap_n]
                        .iter()
                        .map(|&v| f64::from(v) * f64::from(v))
                        .sum();
                    let (lag, _) = best_lag(
                        &x,
                        f_tap,
                        tap_n,
                        f_start,
                        0,
                        ktap as i32,
                        cum_tap,
                        ey,
                        0.5,
                        false,
                        4,
                    );
                    lag.map_or(fallback_li, |l| (l * ms_per_lag).max(0.0))
                }
                None => fallback_li,
            };
            publish_pair(b, fh, fid, s, li);
        }
    }

    /// Message thread. By hand: `arg` = slot of the Track at the end of that channel, or `kPairAutomatic`
    /// to let the Hub find it by name / sound again. `false` = refused (no such friend input, the slot is
    /// not an active Track, an App Audio, or it already carries another friend). A manual pair is kept
    /// until its Track or the friend input goes away.
    pub fn pair_with(&self, feeder: usize, arg: u32) -> bool {
        let Some(shm) = self.snapshot_bus() else {
            return false;
        };
        let b = shm.layout();
        if feeder >= kMaxFeeders {
            return false;
        }
        let fid = b.feeders[feeder].friendId.load(Relaxed);
        let Some(j) = friend_of(b, fid) else {
            return false;
        };
        if b.friends.0[j].feeder.load(Acquire) != feeder as i32 {
            return false; // not linked to the friend yet
        }
        if arg == kPairAutomatic {
            self.shared.manual_pair[j].store(0, Release);
            self.shared.manual_li[j].store(0, Relaxed);
            let name = read_feeder_identity(&b.feeders[feeder])
                .map(|(nm, _)| nm)
                .unwrap_or_default();
            crate::measure::lock(&self.shared.memory).retain(|m| m.feeder != name);
            return true;
        }
        let s = arg as usize;
        if s >= kMaxSlots {
            return false;
        }
        let sh = &b.slots[s];
        if sh.state.load(Acquire) != SlotState::kSlotActive
            || sh.flags.load(Relaxed) & SlotFlags::kFlagApp != 0
        {
            return false;
        }
        let by = sh.fedBy.load(Relaxed);
        if by != 0 && by != fid {
            return false;
        }
        if self.reserved(j, s) {
            return false; // another friend input already chose it
        }
        self.shared.manual_li[j].store(0, Relaxed);
        self.shared.manual_pair[j].store(pair_word(b, s, fid), Release);
        self.remember(b, feeder, s);
        true
    }

    fn remember(&self, b: &BusLayout, feeder: usize, slot: usize) {
        let Some((name, _)) = read_feeder_identity(&b.feeders[feeder]) else {
            return;
        };
        let Some((_, uuid, _)) = read_slot_identity(&b.slots[slot]) else {
            return;
        };
        if uuid.is_empty() {
            return;
        }
        let mut mem = crate::measure::lock(&self.shared.memory);
        mem.retain(|o| o.feeder != name && o.slot_uuid != uuid);
        mem.push(PairMemory {
            feeder: name,
            slot_uuid: uuid,
        });
    }

    /// What to save with the project.
    pub fn pair_memory(&self) -> Vec<PairMemory> {
        crate::measure::lock(&self.shared.memory).clone()
    }

    /// Given back on load.
    pub fn set_pair_memory(&self, memory: Vec<PairMemory>) {
        *crate::measure::lock(&self.shared.memory) = memory;
    }

    /// Pairs again whenever both the friend input and its remembered Track are there (the DAW loads its
    /// plug-ins one by one, or wakes a resting track). Called by `HubEngine::maintain`.
    pub(crate) fn apply_memory(&self, b: &BusLayout) {
        let memory = self.pair_memory();
        let now = now_ns();
        for m in &memory {
            for k in 0..kMaxFeeders {
                let f = &b.feeders[k];
                if !feeder_live(f, now)
                    || read_feeder_identity(f).is_none_or(|(nm, _)| nm != m.feeder)
                {
                    continue;
                }
                let fid = f.friendId.load(Relaxed);
                let j = (0..kMaxFriends).rev().find(|&q| {
                    fid != 0
                        && b.friends.0[q].id.load(Acquire) == fid
                        && b.friends.0[q].feeder.load(Acquire) == k as i32
                });
                let Some(j) = j else { continue };
                if self.shared.manual_pair[j].load(Acquire) != 0 {
                    continue;
                }
                for s in 0..kMaxSlots {
                    if b.slots[s].state.load(Acquire) != SlotState::kSlotActive
                        || read_slot_identity(&b.slots[s]).is_none_or(|(_, u, _)| u != m.slot_uuid)
                    {
                        continue;
                    }
                    if self.pair_with(k, s as u32) {
                        break; // the same checks as by hand
                    }
                }
            }
        }
    }

    /// Message thread: starts the probe for `feeder` (see `probe_step`). `false` = refused (not a linked,
    /// live friend input, a probe is already running, or above 48 kHz where the analysis windows are too
    /// short for the sweep and the answer would be a false "not found").
    pub fn start_probe(&self, feeder: usize) -> bool {
        let Some(shm) = self.snapshot_bus() else {
            return false;
        };
        let b = shm.layout();
        if feeder >= kMaxFeeders || self.snapshot().0.sample_rate > 48000.5 {
            return false;
        }
        let f = &b.feeders[feeder];
        let fid = f.friendId.load(Relaxed);
        let linked = fid != 0
            && (0..kMaxFriends).any(|j| {
                b.friends.0[j].id.load(Acquire) == fid
                    && b.friends.0[j].feeder.load(Acquire) == feeder as i32
            });
        if !linked || !feeder_live(f, now_ns()) {
            return false;
        }
        self.shared
            .probe_feeder
            .compare_exchange(-1, feeder as i32, AcqRel, Relaxed)
            .is_ok()
    }

    /// Ends a probe in progress with `kReplyFailed` (the Hub was re-prepared or moved to another bus).
    pub(crate) fn abort_probe(&self) {
        let _st = crate::measure::lock(&self.shared.probe);
        let Ok(f) = usize::try_from(self.shared.probe_feeder.load(Acquire)) else {
            return;
        };
        match self.snapshot_bus() {
            Some(shm) if f < kMaxFeeders => self.end_probe(shm.layout(), f, kReplyFailed),
            _ => {
                self.shared.probe_stage.store(0, Relaxed);
                self.shared.probe_feeder.store(-1, Release);
            }
        }
    }

    fn end_probe(&self, b: &BusLayout, feeder: usize, reply: u32) {
        // a Track that wakes up later must not play the sweep (1 = "bring back" stays)
        let _ = b.feeders[feeder]
            .hubCommand
            .compare_exchange(2, 0, Relaxed, Relaxed);
        post_reply(&b.feeders[feeder], reply);
        self.shared.probe_stage.store(0, Relaxed);
        self.shared.probe_feeder.store(-1, Release);
    }

    /// The latency thread, every ~20 ms while `probing()`: makes the feeder play the sweep, looks for it in
    /// the Tracks that carry no friend, stores the best as a manual pair and answers the feeder (reply 1 =
    /// paired, `kReplyNoMatch` = not found). Over after about half a second.
    #[allow(clippy::too_many_lines)]
    pub fn probe_step(&self) {
        let mut st = crate::measure::lock(&self.shared.probe);
        let fidx = self.shared.probe_feeder.load(Acquire);
        let (hist, shm) = self.snapshot();
        let Ok(fidx) = usize::try_from(fidx) else {
            return;
        };
        let Some(shm) = shm else {
            self.shared.probe_feeder.store(-1, Release);
            return;
        };
        let b = shm.layout();
        if hist.cap_len == 0 {
            self.end_probe(b, fidx, kReplyFailed); // the Hub is not ready: say so, it is not an old Hub
            return;
        }
        let feeder = &b.feeders[fidx];
        let fid = feeder.friendId.load(Relaxed);
        let j = friend_of(b, fid);
        let now = now_ns();
        let w = hist.write.load(Acquire);
        let rate = hist.sample_rate / 4.0;
        let stage = self.shared.probe_stage.load(Relaxed);
        if stage == 0 {
            st.wall_ns = now;
            st.from = w;
        }
        let line_up_ms = f64::from(bits_float(self.shared.line_up_ms_bits.load(Relaxed)));
        // Stay silent until the sweep has left the Stream Mix: its way is (what is still to come of the
        // sweep) + the Track's way to the master bus + the Line-up delay (frozen during a probe) + a margin.
        let hold = |st: &mut crate::measure::ProbeState, slot: i32, li: f64, pending: u64| {
            st.best = slot;
            st.li = li;
            st.word = if slot >= 0 {
                pair_word(b, slot as usize, fid)
            } else {
                0
            };
            self.shared.probe_stage.store(3, Relaxed);
            // no Track may start or go on with the sweep from now on
            let _ = feeder.hubCommand.compare_exchange(2, 0, Relaxed, Relaxed);
            // from the clock after the correlation and the call-off, not from `w` read before them
            let to_master = f64::from(bits_float(self.shared.slot_to_master_ms.load(Relaxed)));
            st.hold_to = hist.write.load(Acquire)
                + pending
                + ((to_master + line_up_ms) * rate / 1000.0) as u64
                + probe_lead_in(rate);
        };
        let lost = j.is_none() || !feeder_live(feeder, now); // the feeder left, or the DAW stopped processing audio
        let elapsed = now.saturating_sub(st.wall_ns);
        if stage < 3 && (lost || elapsed > 3_000_000_000 + (line_up_ms.max(0.0) * 1e6) as u64) {
            if stage < 2 {
                self.end_probe(b, fidx, kReplyNoMatch); // no sweep was asked for yet
            } else {
                hold(&mut st, -1, 0.0, probe_listen(rate)); // it may be on its way: wait for it, then say no
            }
            return;
        }
        if stage == 3 && elapsed > 10_000_000_000 {
            self.end_probe(b, fidx, kReplyNoMatch); // audio stuck: never stay silent for good
            return;
        }
        let jj = j.unwrap_or(0);
        let fh = &b.friends.0[jj];
        if stage == 0 {
            // a Track that carries this friend now would play the sweep to the other friends and the viewers
            unpair_slot(b, fh.outSlot.load(Relaxed), fid);
            fh.outSlot.store(-1, Release);
            self.shared.probe_stage.store(1, Relaxed);
            return;
        }
        if stage == 1 {
            if w.saturating_sub(st.from) < probe_lead_in(rate) {
                return;
            }
            feeder.hubCommand.store(2, Relaxed);
            feeder.hubCommandSeq.fetch_add(1, Release);
            st.from = w;
            self.shared.probe_stage.store(2, Relaxed);
            return;
        }
        if stage == 3 {
            if w < st.hold_to {
                return;
            }
            let s = st.best;
            // the answer only counts while the same Track holds the slot and the same feeder is still there
            let ok = !lost
                && usize::try_from(s).is_ok_and(|s| {
                    b.slots[s].state.load(Acquire) == SlotState::kSlotActive
                        && pair_word(b, s, fid) == st.word
                });
            if !ok {
                self.end_probe(b, fidx, kReplyNoMatch);
                return;
            }
            let s = s as usize;
            self.shared.manual_li[jj].store((st.li * 100.0).round() as u32 + 1, Relaxed);
            self.shared.manual_pair[jj].store(st.word, Release);
            publish_pair(b, fh, fid, s, st.li);
            self.remember(b, fidx, s);
            self.end_probe(b, fidx, 1);
            return;
        }
        if w.saturating_sub(st.from) < probe_listen(rate) {
            return;
        }

        // The sweep is in the feeder's tap and, if a chain leads to a Track, in that Track's history Li later.
        let ms_per_lag = 4000.0 / hist.sample_rate;
        let lead = (f64::from(hist.sync_safety.load(Relaxed))
            * f64::from(hist.last_block.load(Relaxed))
            / 4.0)
            .round()
            .max(0.0) as usize;
        let n = 4200usize;
        let k = 1700usize.min((rate * 0.2) as usize);
        let kneg = lead + 8;
        let m = n + k + kneg;
        let tap_w = feeder.tapWrite.0.load(Acquire);
        if m > kTapFrames || tap_w < m as u64 || w < m as u64 || m + 4096 > hist.cap_len as usize {
            hold(&mut st, -1, 0.0, 0);
            return;
        }
        let tap = read_tap(b, fidx, tap_w, m);
        let cum_tap = prefix_energy(&tap);
        let y_start = m - kneg - n;
        let (mut best, mut best_score, mut best_lag_frames) = (-1i32, 0.5f64, 0.0f64); // the threshold the plan asks for
        let mut x = Vec::new();
        for s in 0..kMaxSlots {
            let sh = &b.slots[s];
            if sh.state.load(Acquire) != SlotState::kSlotActive
                || sh.flags.load(Relaxed) & SlotFlags::kFlagApp != 0
            {
                continue;
            }
            let by = sh.fedBy.load(Relaxed);
            if by != 0 && by != fid {
                continue;
            }
            if self.reserved(jj, s) {
                continue;
            }
            hist.copy_last(hist.slot_ring(s), w, m, &mut x);
            let ey: f64 = x[y_start..y_start + n]
                .iter()
                .map(|&v| f64::from(v) * f64::from(v))
                .sum();
            if ey < n as f64 * 1e-6 {
                continue; // this Track is silent for the viewers right now
            }
            let (lag, score) = best_lag(
                &x, &tap, n, y_start, 0, k as i32, &cum_tap, ey, best_score, false, 1,
            );
            if let Some(lag) = lag {
                best = s as i32;
                best_score = score;
                best_lag_frames = lag;
            }
        }
        let li = if best < 0 {
            0.0
        } else {
            (best_lag_frames * ms_per_lag).max(0.0)
        };
        hold(&mut st, best, li, 0);
    }
}
