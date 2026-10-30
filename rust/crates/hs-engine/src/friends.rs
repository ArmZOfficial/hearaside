//! The Hub's friends room, audio side (port of `libs/ssengine/src/hub_friends.cpp`): every friend's
//! microphone comes in through shared memory (`friendAudio[i]`, written by the Hub's share server), is
//! played with a small jitter buffer and drift control, and is mixed into
//!   * the headphones (you hear x level, not panned, not delayed), and
//!   * the Stream Mix (viewers hear x level x pan x solo), AFTER the Line-up delay and BEFORE the
//!     limiter, so the limiter sees the final sum.
//!
//! Line up: the Stream Mix as the DAW makes it waits D frames (the slowest friend), and each friend is
//! delayed D - (how late that friend is), so every voice lands on the beat for the viewers. The
//! headphones and the copy of the mix that friends hear (`friendMixAudio`) are never delayed.
// index loops over several parallel per-friend / per-output arrays, kept as in the C++ to compare
#![allow(clippy::needless_range_loop)]

use super::{HubEngine, HubParams, Stereo};
use crate::friend_reader::FriendReader;
use crate::measure::{FRIEND_MON, FRIEND_SOLO, FRIEND_STR};
use hs_bus::bus::{bits_float, float_bits};
use hs_bus::protocol::*;
use hs_bus::ring::ring_write;
use hs_bus::shm::now_ns;
use hs_dsp::dsp::{Smoother, db_to_gain, pan_gains, vector_add};
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering::{Acquire, Relaxed};

/// no packet for this long: the friend is gone for now
const FRIEND_STALE_NS: u64 = 800_000_000;
const MAX_FRIEND_MS: f32 = 1000.0;

/// Audio tap for recording / metering friends: called on the audio thread for every friend and block,
/// with that friend's played audio. Must not allocate or lock.
pub type FriendTap = Box<dyn FnMut(usize, [&[f32]; 2], i64, bool) + Send>;

/// A ring of the newest input of one signal (stereo).
#[derive(Default)]
pub(crate) struct DelayLine {
    pub buf: Stereo,
    pub pos: u32,
}

impl DelayLine {
    pub fn assign(&mut self, cap: u32) {
        for b in &mut self.buf {
            *b = vec![0.0; cap as usize];
        }
        self.pos = 0;
    }

    pub fn clear(&mut self) {
        for b in &mut self.buf {
            b.fill(0.0);
        }
        self.pos = 0;
    }
}

/// `out[i] = in[i - d]` over a ring that always holds the newest input; `d` is cut to `cap - n` (the
/// C++ relied on callers keeping it there and wrapped unsigned otherwise).
fn read_tap(ring: &[f32], end_pos: u32, n: usize, d: u32, out: &mut [f32]) {
    let cap = ring.len();
    if cap == 0 {
        out[..n].fill(0.0);
        return;
    }
    let d = (d as usize).min(cap - n);
    let at = (end_pos as usize + cap - n - d) % cap;
    let first = n.min(cap - at); // at most two contiguous pieces
    out[..first].copy_from_slice(&ring[at..at + first]);
    if first < n {
        out[first..n].copy_from_slice(&ring[..n - first]);
    }
}

/// `ring[pos ..) = src`, wrapping once.
fn write_tap(ring: &mut [f32], pos: u32, src: &[f32]) {
    let n = src.len();
    let pos = pos as usize;
    let first = n.min(ring.len() - pos);
    ring[pos..pos + first].copy_from_slice(&src[..first]);
    if first < n {
        ring[..n - first].copy_from_slice(&src[first..]);
    }
}

pub(crate) struct FriendPlayer {
    pub reader: FriendReader,
    pub mon: Smoother,
    pub str_: Smoother,
    pub pan_l: Smoother,
    pub pan_r: Smoother,
    pub direct: Smoother,
    pub out: Stereo,
    /// the viewers' side, delayed by D - own delay
    pub dl: DelayLine,
    pub id: u32,
    pub live: bool,
    pub peak: f32,
}

fn put(ring: &[AtomicU32], cap_len: u32, at: u64, v: f32) {
    ring[(at % u64::from(cap_len)) as usize].store(v.to_bits(), Relaxed);
}

impl HubEngine {
    pub(super) fn friends_prepare(&mut self) {
        let sr = self.sample_rate;
        let mb = self.max_block;
        self.delay_cap = (1.5 * sr) as u32 + 4 * mb as u32 + 64;
        self.delay_scratch = [vec![0.0; mb], vec![0.0; mb]];
        for d in &mut self.out_delay {
            d.assign(self.delay_cap);
        }
        self.friends = (0..kMaxFriends)
            .map(|_| {
                let mut p = FriendPlayer {
                    reader: FriendReader::new(),
                    mon: Smoother::default(),
                    str_: Smoother::default(),
                    pan_l: Smoother::default(),
                    pan_r: Smoother::default(),
                    direct: Smoother::default(),
                    out: [vec![0.0; mb], vec![0.0; mb]],
                    dl: DelayLine::default(),
                    id: 0,
                    live: false,
                    peak: 0.0,
                };
                p.reader.prepare(sr, mb);
                p.dl.assign(self.delay_cap);
                p.mon.prepare(sr, 10.0);
                p.str_.prepare(sr, 20.0);
                p.pan_l.prepare(sr, 20.0);
                p.pan_l.snap(1.0);
                p.pan_r.prepare(sr, 20.0);
                p.pan_r.snap(1.0);
                p.direct.prepare(sr, 20.0);
                p.direct.snap(1.0);
                p
            })
            .collect();
        for info in &self.shared.friend_info {
            info.playing.store(0, Relaxed);
            info.leak_detected.store(0, Relaxed);
            info.echo_in_master.store(0, Relaxed);
        }
        self.friend_quiet = [self.hist.cap_len; kMaxFriends];
        self.dec_friend_mix = 0.0;
        self.dec_friends = [0.0; kMaxFriends];
        self.line_d = 0;
        self.line_old_d = 0;
        self.line_xf_remain = 0;
        self.line_hold = 0;
        self.used_tot = [0; kMaxFriends];
        self.old_tot = [0; kMaxFriends];
        self.shared.line_up_ms_bits.store(0, Relaxed);
        self.shared.line_up_slowest.store(-1, Relaxed);
    }

    pub(super) fn friends_reset(&mut self) {
        for p in &mut self.friends {
            p.reader.reset();
            p.live = false;
            p.mon.snap(0.0);
            p.str_.snap(0.0);
            p.peak = 0.0;
        }
        self.friend_quiet = [self.hist.cap_len; kMaxFriends];
        self.dec_friend_mix = 0.0;
        self.dec_friends = [0.0; kMaxFriends];
        for info in &self.shared.friend_info {
            info.leak_detected.store(0, Relaxed);
            info.echo_in_master.store(0, Relaxed);
        }
        self.line_d = 0;
        self.line_old_d = 0;
        self.line_xf_remain = 0;
        self.line_hold = 0;
        self.shared.line_up_ms_bits.store(0, Relaxed);
    }

    pub(super) fn friends_solo_active(&self, b: &BusLayout) -> bool {
        (0..kMaxFriends).any(|j| {
            let f = self.shared.friend_ctl[j].flags.load(Relaxed);
            b.friends.0[j].state.load(Relaxed) == FriendState::kFriendLive
                && f & FRIEND_SOLO != 0
                && f & FRIEND_STR != 0
        })
    }

    /// Called with the Stream Mix as the DAW made it in `outs` (all used outputs) and the headphone mix
    /// in `phones`; `w0..w` is this chunk's span of the latency history (decimation `phase`).
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(super) fn friends_mix(
        &mut self,
        b: &BusLayout,
        n: usize,
        p: &HubParams,
        used: &[bool; kNumStreamOuts],
        solo_active: bool,
        time_samples: i64,
        playing: bool,
        bufs: &mut super::ChunkBuffers,
        w0: u64,
        w: u64,
        phase: i32,
    ) {
        if self.friends.is_empty() {
            return;
        }
        let sr = (self.sample_rate + 0.5) as u32;
        let hist = std::sync::Arc::clone(&self.hist);
        let cap_len = hist.cap_len;
        let probe_on = self.shared.probe_feeder.load(Relaxed) >= 0;
        let [scratch0, scratch1] = &mut self.delay_scratch;

        // ---- what friends hear: the Stream Mix as the DAW made it, never delayed -----------------------
        // Friends never hear friends: the direct ones are mixed below and the DAW-carried ones wait in
        // `late`, so nobody gets their own voice back a round trip later (mix-minus for every friend).
        {
            self.friend_mix_gain
                .set_target(if probe_on { 0.0 } else { 1.0 }); // silent while the probe sweep passes
            let scaled =
                self.friend_mix_gain.is_smoothing() || self.friend_mix_gain.current() != 1.0;
            if scaled {
                for i in 0..n {
                    let g = self.friend_mix_gain.next_value();
                    scratch0[i] = bufs.outs[0][0][i] * g;
                    scratch1[i] = bufs.outs[0][1][i] * g;
                }
            }
            let (s0, s1): (&[f32], &[f32]) = if scaled {
                (&scratch0[..n], &scratch1[..n])
            } else {
                (&bufs.outs[0][0][..n], &bufs.outs[0][1][..n])
            };
            ring_write(
                &b.friendMixAudio.0,
                &b.friendMixWrite.0,
                &[s0, s1],
                n as u32,
            );
            if cap_len > 0 {
                let mut acc = self.dec_friend_mix;
                let mut at = w0;
                let mut k = phase;
                for i in 0..n {
                    acc += s0[i] + s1[i];
                    k += 1;
                    if k == 4 {
                        put(&hist.friend_mix, cap_len, at, acc);
                        at += 1;
                        acc = 0.0;
                        k = 0;
                    }
                }
                self.dec_friend_mix = acc;
            }
        }

        // ---- play every friend --------------------------------------------------------------------------
        let mut have = [false; kMaxFriends];
        let now = now_ns();
        for j in 0..kMaxFriends {
            let f = &mut self.friends[j];
            let h = &b.friends.0[j];
            let info = &self.shared.friend_info[j];
            let id = h.id.load(Acquire);
            let live_state = h.state.load(Acquire) == FriendState::kFriendLive
                && id != 0
                && now.saturating_sub(h.heartbeatNs.load(Relaxed)) < FRIEND_STALE_NS;
            if id != f.id {
                f.id = id;
                f.reader.reset();
                f.mon.snap(0.0);
                f.str_.snap(0.0);
                f.dl.clear(); // (the C++ reallocated it here, on the audio thread)
            }
            let rate = h.sampleRate.load(Relaxed);
            f.live = false;
            if !live_state {
                f.reader.reset();
                info.playing.store(0, Relaxed);
                info.underruns.store(0, Relaxed);
                info.concealed_ms.store(0, Relaxed);
                info.jitter_p95_ms_bits.store(0, Relaxed);
                for c in &mut f.out {
                    c[..n].fill(0.0);
                }
                f.peak *= (-(n as f32) / (0.3 * sr as f32)).exp();
                if let Some(tap) = self.friend_tap.as_mut() {
                    tap(j, [&f.out[0][..n], &f.out[1][..n]], time_samples, playing);
                }
                continue;
            }
            let was_primed = f.reader.primed();
            let ok = {
                let [o0, o1] = &mut f.out;
                f.reader.pull(
                    &b.friendAudio.0[j],
                    &h.writePos.0,
                    rate,
                    [&mut o0[..n], &mut o1[..n]],
                    n,
                )
            };
            if let Some(tap) = self.friend_tap.as_mut() {
                tap(j, [&f.out[0][..n], &f.out[1][..n]], time_samples, playing);
            }
            if !was_primed && f.reader.primed() {
                f.mon.snap(0.0);
                f.str_.snap(0.0); // no click when the voice (re)starts
            }
            info.underruns.store(f.reader.underruns(), Relaxed);
            info.concealed_ms.store(
                f.reader.concealed_ms().min(f64::from(u32::MAX)) as u32,
                Relaxed,
            );
            info.jitter_p95_ms_bits.store(
                float_bits(f.reader.jitter_p95_ms().max(0.0) as f32),
                Relaxed,
            );
            if !ok {
                info.playing.store(0, Relaxed);
                continue;
            }
            f.live = true;
            have[j] = true;
            info.buffer_ms_bits
                .store(float_bits(f.reader.buffer_ms() as f32), Relaxed);
            info.playing.store(1, Relaxed);
        }

        if cap_len > 0 {
            for j in 0..kMaxFriends {
                let f = &self.friends[j];
                let ring = hist.friend_ring(j);
                if f.live {
                    let mut acc = self.dec_friends[j];
                    let mut at = w0;
                    let mut k = phase;
                    for i in 0..n {
                        acc += f.out[0][i] + f.out[1][i];
                        k += 1;
                        if k == 4 {
                            put(ring, cap_len, at, acc);
                            at += 1;
                            acc = 0.0;
                            k = 0;
                        }
                    }
                    self.dec_friends[j] = acc;
                    self.friend_quiet[j] = 0;
                } else {
                    if self.friend_quiet[j] < cap_len {
                        for at in w0..w {
                            put(ring, cap_len, at, 0.0);
                        }
                        self.friend_quiet[j] += (w - w0) as u32;
                    }
                    self.dec_friends[j] = 0.0;
                }
            }
        }

        // ---- Line up: D = the slowest friend, a crossfade whenever it really changes ---------------------
        let line_on = self.shared.line_up_on.load(Acquire);
        let limit_ms = MAX_FRIEND_MS.min(bits_float(self.shared.line_up_limit_bits.load(Relaxed)));
        let mut tot = [0u32; kMaxFriends]; // frames: how late each friend is against the music
        let mut max_tot = 0u32;
        let mut slowest = -1i32;
        for j in 0..kMaxFriends {
            let info = &self.shared.friend_info[j];
            if !have[j] {
                info.over_limit.store(0, Relaxed);
                continue;
            }
            let d = bits_float(b.friends.0[j].delayBits.load(Relaxed));
            let tms = d.max(0.0) + bits_float(info.buffer_ms_bits.load(Relaxed));
            info.total_ms_bits.store(float_bits(tms), Relaxed);
            // (a float from shared memory: `as` saturates and maps NaN to 0, the C++ cast was UB)
            tot[j] = (tms * self.sample_rate as f32 * 0.001 + 0.5) as u32;
            let measured = d >= 0.0;
            let over = tms > limit_ms;
            info.over_limit.store(u32::from(over), Relaxed);
            if line_on && measured && !over && tot[j] >= max_tot {
                max_tot = tot[j];
                slowest = j as i32;
            }
        }
        let cap_d = self.delay_cap - 4 * self.max_block as u32 - 32;
        let target = if line_on { max_tot.min(cap_d) } else { 0 };
        self.shared
            .line_up_slowest
            .store(if line_on { slowest } else { -1 }, Relaxed);
        {
            let tol = ((0.008 * self.sample_rate) as u32).max(target / 20); // 8 ms or 5 %
            let mut changed = target.abs_diff(self.line_d) > tol;
            if !line_on && self.line_d != 0 {
                changed = true; // switched off: back to live
            }
            for j in 0..kMaxFriends {
                if changed {
                    break;
                }
                if have[j] && tot[j].abs_diff(self.used_tot[j]) > (0.008 * self.sample_rate) as u32
                {
                    changed = true;
                }
            }
            if probe_on && line_on {
                changed = false; // the probe holds the mute for the D it started with
            }
            let urgent = !line_on;
            if changed && self.line_xf_remain == 0 {
                self.line_hold += n as u32;
                if urgent || self.line_hold >= (0.5 * self.sample_rate) as u32 {
                    // stable for half a second
                    self.line_hold = 0;
                    self.line_old_d = self.line_d;
                    self.old_tot = self.used_tot;
                    self.line_d = target;
                    self.used_tot = tot;
                    self.line_xf_remain = ((0.020 * self.sample_rate) as u32).max(16);
                    self.shared.line_up_ms_bits.store(
                        float_bits((f64::from(self.line_d) * 1000.0 / self.sample_rate) as f32),
                        Relaxed,
                    );
                    self.shared.line_up_changes.fetch_add(1, Relaxed);
                }
            } else if !changed {
                self.line_hold = 0;
            }
        }
        let xf_total = ((0.020 * self.sample_rate) as u32).max(16);
        let xf_start = self.line_xf_remain; // crossfade progress at the start of this block
        let xf_gain = |i: usize| {
            let done = xf_total - xf_total.min(xf_start) + i as u32;
            (done as f32 / xf_total as f32).min(1.0)
        };

        // ---- the Stream Mix and the stems wait D (history is always written, so D can start any time) ---
        for o in 0..kNumStreamOuts {
            if !used[o] {
                continue;
            }
            let d = &mut self.out_delay[o];
            let cap = d.buf[0].len() as u32;
            for c in 0..2 {
                write_tap(&mut d.buf[c], d.pos, &bufs.outs[o][c][..n]);
            }
            let end_pos = (d.pos + n as u32) % cap;
            for c in 0..2 {
                if self.line_d == 0 && xf_start == 0 {
                    continue; // live: the block is already right
                }
                let x = &mut bufs.outs[o][c];
                read_tap(&d.buf[c], end_pos, n, self.line_d, scratch0);
                if xf_start > 0 {
                    read_tap(&d.buf[c], end_pos, n, self.line_old_d, scratch1);
                    for i in 0..n {
                        x[i] = scratch1[i] + (scratch0[i] - scratch1[i]) * xf_gain(i);
                    }
                } else {
                    x[..n].copy_from_slice(&scratch0[..n]);
                }
            }
            d.pos = end_pos;
        }
        self.line_xf_remain = xf_start.saturating_sub(n as u32);

        // ---- friends that a DAW track carries: already delayed by D - own delay, so after the delay -----
        if self.late_any {
            for o in 0..kNumStreamOuts {
                if used[o] {
                    for c in 0..2 {
                        vector_add(&mut bufs.outs[o][c][..n], &bufs.late[o][c][..n]);
                    }
                }
            }
        }

        // ---- mix every friend -------------------------------------------------------------------------
        let pk_decay = (-(n as f32) / (0.3 * sr as f32)).exp();
        for j in 0..kMaxFriends {
            let f = &mut self.friends[j];
            let h = &b.friends.0[j];
            let ctl = &self.shared.friend_ctl[j];
            let live = f.live;
            let flags = ctl.flags.load(Relaxed);
            let g = db_to_gain(bits_float(ctl.gain_bits.load(Relaxed)));
            // direct path only while the Hub plays this friend (not while a DAW track carries them)
            let direct = h.route.load(Relaxed) == FriendRoute::kRouteDirect;
            f.direct.set_target(if direct { 1.0 } else { 0.0 });
            f.mon.set_target(if live && flags & FRIEND_MON != 0 {
                g
            } else {
                0.0
            });
            let mut gv = if live && flags & FRIEND_STR != 0 {
                g
            } else {
                0.0
            };
            if solo_active && flags & FRIEND_SOLO == 0 {
                gv = 0.0;
            }
            if p.panic {
                gv = 0.0;
            }
            f.str_.set_target(gv);
            let (gl, gr) = pan_gains(bits_float(ctl.pan_bits.load(Relaxed)));
            f.pan_l.set_target(gl);
            f.pan_r.set_target(gr);

            let silent_now = !live
                && !f.mon.is_smoothing()
                && !f.str_.is_smoothing()
                && f.mon.current() == 0.0
                && f.str_.current() == 0.0;
            // the friend's own history goes into its delay line even while the viewers don't hear it
            let mut pk = 0.0f32;
            let vo = &mut bufs.voice[j];
            let gf = if live { g * 0.5 } else { 0.0 }; // mono: (l + r) / 2
            if silent_now {
                scratch0[..n].fill(0.0);
                scratch1[..n].fill(0.0);
                for _ in 0..n {
                    f.direct.next_value();
                    f.pan_l.next_value();
                    f.pan_r.next_value();
                }
            } else {
                for i in 0..n {
                    let dg = f.direct.next_value();
                    let gm = f.mon.next_value() * dg;
                    let gs = f.str_.next_value() * dg;
                    let (l, r) = (f.out[0][i], f.out[1][i]);
                    vo[i] += (l + r) * gf * dg;
                    bufs.phones[0][i] += l * gm;
                    bufs.phones[1][i] += r * gm;
                    let vl = l * gs * f.pan_l.next_value();
                    let vr = r * gs * f.pan_r.next_value();
                    scratch0[i] = vl;
                    scratch1[i] = vr;
                    pk = pk.max(vl.abs().max(vr.abs()));
                }
            }
            let d = &mut f.dl;
            let cap = d.buf[0].len() as u32;
            write_tap(&mut d.buf[0], d.pos, &scratch0[..n]);
            write_tap(&mut d.buf[1], d.pos, &scratch1[..n]);
            let end_pos = (d.pos + n as u32) % cap;
            let d_new = self.line_d.saturating_sub(self.used_tot[j]);
            let d_old = self.line_old_d.saturating_sub(self.old_tot[j]);
            self.shared.friend_info[j].line_ms_bits.store(
                float_bits((f64::from(d_new) * 1000.0 / self.sample_rate) as f32),
                Relaxed,
            );
            for c in 0..2 {
                read_tap(&d.buf[c], end_pos, n, d_new, scratch0);
                let dst = &mut bufs.outs[0][c];
                if xf_start > 0 {
                    read_tap(&d.buf[c], end_pos, n, d_old, scratch1);
                    for i in 0..n {
                        dst[i] += scratch1[i] + (scratch0[i] - scratch1[i]) * xf_gain(i);
                    }
                } else {
                    vector_add(&mut dst[..n], &scratch0[..n]);
                }
            }
            d.pos = end_pos;
            f.peak = pk.max(f.peak * pk_decay);
        }

        // ---- what friends hear of each other: written with friendMixAudio, so the positions match -------
        for k in 0..kMaxFriends / 2 {
            ring_write(
                &b.friendVoiceAudio.0[k],
                &b.friendVoiceWrite.0[k],
                &[&bufs.voice[2 * k][..n], &bufs.voice[2 * k + 1][..n]],
                n as u32,
            );
        }
    }
}
