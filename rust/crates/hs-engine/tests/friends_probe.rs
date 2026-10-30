//! The rest of `tests/unit/test_friends.cpp`: the friend input's jitter-buffer start, the probe sweep
//! (finds the Track at the end of a friend input's channel and nobody hears it), speaker leak and echo
//! detection, and the direct-monitor command. The room harness here adds hooks the probe needs (a DAW
//! stand-in that runs before the Hub every cycle, custom friend audio, the master-bus capture).
#![allow(clippy::needless_range_loop)]

use hs_bus::bus::{
    bits_float, claim_feeder, claim_slot, float_bits, make_uuid, poll_commands, post_command,
    set_feeder_identity, set_slot_identity,
};
use hs_bus::protocol::*;
use hs_bus::ring::{ring_read_at, ring_write};
use hs_bus::shm::{current_pid, now_ns};
use hs_engine::friend_reader::FriendReader;
use hs_engine::hub_engine::{HubEngine, HubParams, probe_sweep};
use hs_engine::track_publisher::TrackPublisher;
use std::sync::atomic::Ordering::SeqCst;

const MUSIC: u32 = 11;
const VOICE: u32 = 29;
const RATE: i64 = 48000;

fn unique_bus(tag: &str) -> String {
    // a counter, not the clock: two tests starting in the same tick shared a bus (the second Hub was not the owner)
    static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    format!("frp_{tag}_{}_{n}", current_pid())
}

fn noise_at(t: i64, seed: u32) -> f32 {
    let mut h = (t as u32).wrapping_mul(2_654_435_761) ^ seed.wrapping_mul(40503);
    h ^= h >> 15;
    h = h.wrapping_mul(2_246_822_519);
    h ^= h >> 13;
    h = h.wrapping_mul(3_266_489_917);
    h ^= h >> 16;
    ((h & 0xffff) as f32 / 65535.0 - 0.5) * 0.5
}

#[derive(Default, Clone)]
struct Friend {
    seed: u32,
    net_frames: i64,
    sent: i64,
    open: bool,
    /// what the friend sends instead of noise(seed)
    custom: Option<fn(i64) -> f32>,
}

type Hook<'a> = &'a mut dyn FnMut(&BusLayout, &HubEngine, i64, &mut Vec<f32>);

struct Room {
    bus: String,
    sr: i64,
    hub: HubEngine,
    params: HubParams,
    t: i64,
    block: usize,
    music: bool,
    master_in: Vec<f32>,
    stream: Vec<f32>,
    friend_mix: Vec<f32>,
    stream_read: u64,
    mix_read: u64,
    read_init: bool,
    friends: [Friend; kMaxFriends],
}

impl Room {
    fn new(tag: &str, sr: i64) -> Self {
        let bus = unique_bus(tag);
        let mut hub = HubEngine::new();
        hub.connect(&bus);
        hub.prepare(sr as f64, 1024);
        hub.maintain();
        hub.bus()
            .unwrap()
            .streamHeader
            .sampleRate
            .store(sr as u32, SeqCst);
        Room {
            bus,
            sr,
            hub,
            params: HubParams::default(),
            t: 0,
            block: 256,
            music: true,
            master_in: vec![],
            stream: vec![],
            friend_mix: vec![],
            stream_read: 0,
            mix_read: 0,
            read_init: false,
            friends: std::array::from_fn(|_| Friend::default()),
        }
    }

    fn b(&self) -> &BusLayout {
        self.hub.bus().unwrap()
    }

    fn open_friend(&mut self, j: usize, seed: u32, net_ms: f64) {
        let f = &mut self.friends[j];
        f.seed = seed;
        f.net_frames = (net_ms * self.sr as f64 / 1000.0 + 0.5) as i64;
        f.open = true;
        f.sent = (self.t / 960) * 960;
        let h = &self.hub.bus().unwrap().friends.0[j];
        h.id.store(100 + j as u32, SeqCst);
        h.sampleRate.store(self.sr as u32, SeqCst);
        h.delayBits
            .store(float_bits((net_ms + 10.0) as f32), SeqCst);
        h.route.store(FriendRoute::kRouteDirect, SeqCst);
        h.state.store(FriendState::kFriendLive, SeqCst);
    }

    fn cycle(&mut self, hook: Hook<'_>) {
        let block = self.block;
        {
            let b = self.hub.bus().unwrap();
            for j in 0..kMaxFriends {
                let f = &mut self.friends[j];
                if !f.open {
                    continue;
                }
                let h = &b.friends.0[j];
                h.heartbeatNs.store(now_ns(), SeqCst);
                while f.sent + 960 + f.net_frames <= self.t {
                    let pk: Vec<f32> = (0..960)
                        .map(|i| {
                            f.custom
                                .map_or_else(|| noise_at(f.sent + i, f.seed), |c| c(f.sent + i))
                        })
                        .collect();
                    ring_write(&b.friendAudio.0[j], &h.writePos.0, &[&pk], 960);
                    f.sent += 960;
                }
            }
        }
        let mut l: Vec<f32> = (0..block)
            .map(|i| {
                if self.music {
                    noise_at(self.t + i as i64, MUSIC)
                } else {
                    0.0
                }
            })
            .collect();
        hook(self.hub.bus().unwrap(), &self.hub, self.t, &mut l);
        let mut r = l.clone();
        self.master_in.extend_from_slice(&l);
        self.hub
            .process(&mut [&mut l, &mut r], &self.params, self.t, true, false);
        let b = self.hub.bus().unwrap();
        let w = b.streamHeader.writePos.0[0].load(SeqCst);
        if !self.read_init {
            self.stream_read = w - block as u64;
            self.mix_read = b.friendMixWrite.0.load(SeqCst) - block as u64;
            self.read_init = true;
        }
        let len = (w - self.stream_read) as usize;
        let (mut a, mut bb) = (vec![0.0f32; len], vec![0.0f32; len]);
        ring_read_at(
            &b.streamAudio.0[0],
            &b.streamHeader.writePos.0[0],
            self.stream_read,
            &mut [&mut a, &mut bb],
            len as u32,
            None,
        );
        self.stream_read = w;
        self.stream.extend_from_slice(&a);
        let mw = b.friendMixWrite.0.load(SeqCst);
        let len = (mw - self.mix_read) as usize;
        let (mut m, mut mb) = (vec![0.0f32; len], vec![0.0f32; len]);
        ring_read_at(
            &b.friendMixAudio.0,
            &b.friendMixWrite.0,
            self.mix_read,
            &mut [&mut m, &mut mb],
            len as u32,
            None,
        );
        self.mix_read = mw;
        self.friend_mix.extend_from_slice(&m);
        self.t += block as i64;
    }

    fn run(&mut self, seconds: f64, hook: Hook<'_>) {
        for _ in 0..(seconds * self.sr as f64 / self.block as f64) as usize {
            self.cycle(hook);
        }
    }
}

fn none(_: &BusLayout, _: &HubEngine, _: i64, _: &mut Vec<f32>) {}

#[test]
fn a_track_prepared_for_its_real_block_starts_the_jitter_buffer_about_30ms_lower_than_for_2048() {
    let start_buffer_ms = |block: usize| {
        // SAFETY: floats; all-zero is valid.
        let ring: Box<ChannelRing> = unsafe {
            Box::from_raw(std::alloc::alloc_zeroed(std::alloc::Layout::new::<ChannelRing>()).cast())
        };
        let wp = std::sync::atomic::AtomicU64::new(0);
        let mut rd = FriendReader::new();
        rd.prepare(RATE as f64, block);
        let (mut o0, mut o1) = (vec![0.0f32; block], vec![0.0f32; block]);
        let mut sent = 0i64;
        for cycle in 0..(2 * RATE as usize / block) {
            // 2 s: the reader primes, then plays
            while sent + 960 <= (cycle * block) as i64 {
                let pk: Vec<f32> = (0..960).map(|i| noise_at(sent + i, VOICE)).collect();
                ring_write(&ring, &wp, &[&pk], 960);
                sent += 960;
            }
            rd.pull(&ring, &wp, RATE as u32, [&mut o0, &mut o1], block);
        }
        assert!(rd.primed());
        rd.buffer_ms()
    };
    let (small, big) = (start_buffer_ms(256), start_buffer_ms(2048));
    assert!(big - small > 30.0, "{small} {big}"); // at least the ~37 ms between the two blocks
}

/// The feeder and three DAW Tracks: the sweep the feeder plays goes through a small EQ, a gain and a 7 ms
/// delay into the "end" Track (Track 2); the other two carry unrelated audio.
struct Daw {
    fidx: usize,
    tracks: Vec<TrackPublisher>,
    sweep: Vec<f32>,
    chain: Vec<f32>,
    cmd_seen: u32,
    /// DAW frame the feeder began the sweep at
    sweep_from: i64,
    tw: u64,
    tap_acc: f32,
    end_hears_sweep: bool,
    lag_frames: usize,
    lag_pos: usize,
    lag_ring: [Vec<f32>; 3],
    block: usize,
}

impl Daw {
    fn feeder_and_tracks(&mut self, b: &BusLayout, t: i64, master: &mut [f32]) {
        let f = &b.feeders[self.fidx];
        f.heartbeatNs.store(now_ns(), SeqCst);
        let cs = f.hubCommandSeq.load(SeqCst);
        if cs != self.cmd_seen {
            // the Hub's command: start the sweep
            self.cmd_seen = cs;
            if f.hubCommand.load(SeqCst) == 2 {
                self.sweep_from = t;
            }
        }
        // SAFETY: the test plays the feeder, the only writer of its tap.
        let tap = unsafe { &mut *b.feederTap.0[self.fidx].mono.get() };
        for i in 0..self.block {
            let at = if self.sweep_from >= 0 {
                t + i as i64 - self.sweep_from
            } else {
                -1
            };
            self.tap_acc += if at >= 0 && (at as usize) < self.sweep.len() {
                self.sweep[at as usize]
            } else {
                0.0
            };
            if i & 3 == 3 {
                tap[(self.tw % kTapFrames as u64) as usize] = self.tap_acc * 0.25;
                self.tw += 1;
                self.tap_acc = 0.0;
            }
        }
        f.tapWrite.0.store(self.tw, SeqCst);
        let mut c = vec![0.0f32; self.block];
        for k in 0..3 {
            for (i, x) in c.iter_mut().enumerate() {
                let at = if self.sweep_from >= 0 {
                    t + i as i64 - self.sweep_from
                } else {
                    -1
                };
                *x = if k == 1 {
                    if self.end_hears_sweep && at >= 0 && (at as usize) < self.chain.len() {
                        self.chain[at as usize]
                    } else {
                        0.0
                    }
                } else {
                    0.1 * noise_at(t + i as i64, 500 + k as u32)
                };
            }
            self.tracks[k].process(&[&c], self.block, t, true, false, false);
            // the DAW plays each Track into the master bus, unless that Track sends the viewers' side to the Hub
            if self.tracks[k].slot().unwrap().flags.load(SeqCst) & SlotFlags::kFlagViewersViaHub
                == 0
            {
                if self.lag_ring[k].len() != self.lag_frames {
                    self.lag_ring[k] = vec![0.0; self.lag_frames];
                }
                for i in 0..self.block {
                    if self.lag_frames == 0 {
                        master[i] += c[i];
                        continue;
                    }
                    let slotted = &mut self.lag_ring[k][(self.lag_pos + i) % self.lag_frames];
                    master[i] += *slotted;
                    *slotted = c[i];
                }
            }
        }
        self.lag_pos = if self.lag_frames == 0 {
            0
        } else {
            (self.lag_pos + self.block) % self.lag_frames
        };
    }
}

struct ProbeRig {
    r: Room,
    fidx: usize,
    end_slot: usize,
    daw: Daw,
    start_out: usize,
    end_out: usize,
    replied: bool,
}

impl ProbeRig {
    /// `master_lag_ms`: how late the DAW brings every Track's sound to the master bus (delay compensation)
    fn new(
        end_hears: bool,
        friend_ms: f64,
        line_up_limit: f32,
        master_lag_ms: f64,
        rate: i64,
    ) -> Self {
        let mut r = Room::new("probe", rate);
        let sweep = probe_sweep(rate as f64);
        r.params.sync_safety = 0;
        r.open_friend(0, VOICE, friend_ms);
        if line_up_limit > 0.0 {
            r.hub.set_line_up(true, line_up_limit);
        }
        let b = r.hub.bus().unwrap();
        let fidx = claim_feeder(b).unwrap();
        set_feeder_identity(&b.feeders[fidx], "Lead Vocal", 0);
        b.feeders[fidx].friendId.store(100, SeqCst);
        b.feeders[fidx]
            .status
            .store(FeederStatus::kFeederFlowing, SeqCst);
        b.friends.0[0].feeder.store(fidx as i32, SeqCst);
        let cmd_seen = b.feeders[fidx].hubCommandSeq.load(SeqCst);
        // 0.8 EQ-ish: y = 0.8 x[n] + 0.2 x[n-2], gain 0.5, then 7 ms
        let d = (0.007 * rate as f64 + 0.5) as usize;
        let mut chain = vec![0.0f32; sweep.len() + (0.007 * rate as f64) as usize + 4];
        for i in 0..sweep.len() {
            chain[i + d] = 0.5 * (0.8 * sweep[i] + 0.2 * if i >= 2 { sweep[i - 2] } else { 0.0 });
        }
        let mut tracks = Vec::new();
        for name in ["Track 1", "Track 2", "Track 3"] {
            let mut t = TrackPublisher::new();
            t.connect(&r.bus, "", rate as u32, 1);
            t.prepare(rate as u32, 1);
            t.set_identity(name, 0);
            tracks.push(t);
        }
        let end_slot = tracks[1].slot_index().unwrap();
        let block = r.block;
        ProbeRig {
            r,
            fidx,
            end_slot,
            daw: Daw {
                fidx,
                tracks,
                sweep,
                chain,
                cmd_seen,
                sweep_from: -1,
                tw: 0,
                tap_acc: 0.0,
                end_hears_sweep: end_hears,
                lag_frames: (master_lag_ms * rate as f64 / 1000.0 + 0.5) as usize,
                lag_pos: 0,
                lag_ring: [vec![], vec![], vec![]],
                block,
            },
            start_out: 0,
            end_out: 0,
            replied: false,
        }
    }

    fn b(&self) -> &BusLayout {
        self.r.b()
    }

    fn sweep(&self) -> &[f32] {
        &self.daw.sweep
    }

    fn run(&mut self, seconds: f64) {
        let daw = &mut self.daw;
        self.r
            .run(seconds, &mut |b, _, t, m| daw.feeder_and_tracks(b, t, m));
    }

    /// Runs the probe like the Hub's latency thread would; returns the DAW frames it took. `during` runs every
    /// cycle with the DAW frames since the sweep began (-1 before).
    fn probe(&mut self, mut during: impl FnMut(&BusLayout, &HubEngine, i64)) -> usize {
        let m = self.r.hub.measurer();
        let seq = self.b().feeders[self.fidx].replySeq.load(SeqCst);
        assert!(m.start_probe(self.fidx), "probe refused");
        let from = self.r.stream.len();
        self.start_out = from;
        let mut c = 0;
        while c < 1000 && m.probing() {
            let daw = &mut self.daw;
            self.r
                .cycle(&mut |b, _, t, mm| daw.feeder_and_tracks(b, t, mm));
            m.probe_step();
            let since = if self.daw.sweep_from >= 0 {
                self.r.t - self.daw.sweep_from
            } else {
                -1
            };
            during(self.r.b(), &self.r.hub, since);
            c += 1;
        }
        self.end_out = self.r.stream.len();
        self.replied = self.b().feeders[self.fidx].replySeq.load(SeqCst) != seq;
        self.end_out - from
    }
}

fn peak_of(v: &[f32], from: usize, to: usize) -> f32 {
    v[from.min(v.len())..to.min(v.len())]
        .iter()
        .fold(0.0, |p, x| p.max(x.abs()))
}

fn tail_peak(v: &[f32]) -> f32 {
    peak_of(v, v.len().saturating_sub(4800), v.len())
}

/// How loud, as a fraction of the sweep itself, the sweep is anywhere in x[from, ..): a matched filter at a
/// quarter of the rate.
fn sweep_in(x: &[f32], from: usize, sweep: &[f32]) -> f64 {
    let to = x.len();
    let (s_len, d_len) = (sweep.len() / 4, (to - from) / 4);
    if d_len <= s_len {
        return 0.0;
    }
    let s: Vec<f32> = (0..s_len)
        .map(|k| 0.25 * (sweep[4 * k] + sweep[4 * k + 1] + sweep[4 * k + 2] + sweep[4 * k + 3]))
        .collect();
    let d: Vec<f32> = (0..d_len)
        .map(|k| {
            0.25 * (x[from + 4 * k]
                + x[from + 4 * k + 1]
                + x[from + 4 * k + 2]
                + x[from + 4 * k + 3])
        })
        .collect();
    let es: f64 = s.iter().map(|&v| f64::from(v) * f64::from(v)).sum();
    let mut best = 0.0f64;
    for lag in 0..=d_len - s_len {
        let dot: f64 = (0..s_len)
            .map(|k| f64::from(s[k]) * f64::from(d[lag + k]))
            .sum();
        best = best.max(dot.abs() / es);
    }
    best
}

#[test]
fn the_probe_sweep_finds_the_track_at_the_end_of_the_channel_and_nobody_hears_it() {
    // Line up on and ~300 ms late: the sweep that goes out through the end Track's own DAW channel reaches the
    // Stream Mix D later, and the mute has to cover that too
    let mut rig = ProbeRig::new(true, 290.0, 600.0, 0.0, RATE);
    rig.run(1.5); // the tap fills; music plays
    let d = f64::from(rig.r.hub.measurer().line_up_ms());
    assert!(d > 250.0);
    assert!(tail_peak(&rig.r.stream) > 0.05);
    assert!(tail_peak(&rig.r.friend_mix) > 0.05);
    rig.r.music = false; // so the only thing that could be heard is the sweep
    rig.run(0.5);

    let took = rig.probe(|_, _, _| {});
    let b = rig.b();
    assert!(rig.replied);
    assert_eq!(b.feeders[rig.fidx].reply.load(SeqCst), 1);
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), rig.end_slot as i32);
    assert_eq!(b.slots[rig.end_slot].fedBy.load(SeqCst), 100);
    assert!((bits_float(b.slots[rig.end_slot].fxLatencyBits.load(SeqCst)) - 7.0).abs() < 1.0);
    assert!(
        (took as f64 / RATE as f64) < 0.9 + d / 1000.0,
        "took {took}"
    );
    // the Stream Mix is silent after the 20 ms ramp, and so is what friends hear
    let quiet_from = rig.start_out + (0.05 * RATE as f64) as usize;
    assert!(peak_of(&rig.r.stream, quiet_from, rig.end_out) < 1e-5);
    assert!(peak_of(&rig.r.friend_mix, quiet_from, rig.end_out) < 1e-6);

    b.friends.0[0].route.store(FriendRoute::kRouteDaw, SeqCst); // the Hub does this once paired
    rig.run(1.0); // and the sweep is not late either: it left the Stream Mix while the mute still held
    assert!(sweep_in(&rig.r.master_in, rig.start_out, rig.sweep()) > 0.3); // it was there to be heard
    assert!(sweep_in(&rig.r.stream, rig.start_out, rig.sweep()) < 0.05);
    assert!(sweep_in(&rig.r.friend_mix, rig.start_out, rig.sweep()) < 0.05);
    rig.r.music = true; // they come back
    rig.run(0.5);
    assert!(tail_peak(&rig.r.stream) > 0.05);
    assert!(tail_peak(&rig.r.friend_mix) > 0.05);

    // the pair is kept like a manual one: the names differ and it stays
    rig.r.hub.measurer().measure_latencies();
    assert_eq!(
        rig.b().friends.0[0].outSlot.load(SeqCst),
        rig.end_slot as i32
    );
}

#[test]
fn the_probe_says_so_when_no_track_plays_the_sweep_and_leaves_nothing_paired() {
    let mut rig = ProbeRig::new(false, 40.0, 0.0, 0.0, RATE);
    rig.run(1.0);
    rig.probe(|_, _, _| {});
    assert!(rig.replied);
    assert!(!rig.r.hub.measurer().probing());
    assert_eq!(rig.b().feeders[rig.fidx].reply.load(SeqCst), kReplyNoMatch);
    assert_eq!(rig.b().friends.0[0].outSlot.load(SeqCst), -1);
    rig.run(0.5);
    assert!(tail_peak(&rig.r.stream) > 0.05);
}

/// The abnormal cases: the sweep is still in the DAW and in the Line-up delay after the Hub stopped listening.
fn check_sweep_never_heard(rig: &mut ProbeRig) {
    rig.b().friends.0[0]
        .route
        .store(FriendRoute::kRouteDaw, SeqCst);
    rig.run(1.5);
    assert!(sweep_in(&rig.r.master_in, rig.start_out, rig.sweep()) > 0.3);
    assert!(sweep_in(&rig.r.stream, rig.start_out, rig.sweep()) < 0.05);
    assert!(sweep_in(&rig.r.friend_mix, rig.start_out, rig.sweep()) < 0.05);
}

#[test]
fn the_probe_sweep_is_held_back_for_a_daw_that_brings_the_tracks_to_the_master_450ms_late() {
    let mut rig = ProbeRig::new(true, 290.0, 600.0, 450.0, RATE);
    rig.run(1.5);
    rig.r.hub.measurer().measure_latencies(); // learns how late the Tracks reach the master
    rig.r.music = false;
    rig.run(0.5);
    rig.probe(|_, _, _| {});
    assert_eq!(rig.b().feeders[rig.fidx].reply.load(SeqCst), 1);
    assert_eq!(
        rig.b().friends.0[0].outSlot.load(SeqCst),
        rig.end_slot as i32
    );
    check_sweep_never_heard(&mut rig);
}

#[test]
fn the_probe_stays_silent_for_the_sweep_that_is_already_out_when_the_feeder_disappears() {
    let mut rig = ProbeRig::new(true, 290.0, 600.0, 0.0, RATE);
    rig.run(1.5);
    rig.r.music = false;
    rig.run(0.5);
    let fidx = rig.fidx;
    let mut gone = false;
    rig.probe(|b, _, since| {
        if since >= (0.1 * RATE as f64) as i64 && !gone {
            gone = true;
            b.feeders[fidx].state.store(SlotState::kSlotFree, SeqCst);
        }
    });
    assert!(gone);
    assert_eq!(rig.b().feeders[rig.fidx].reply.load(SeqCst), kReplyNoMatch);
    assert_eq!(rig.b().friends.0[0].outSlot.load(SeqCst), -1);
    check_sweep_never_heard(&mut rig);
}

#[test]
fn a_track_that_took_over_the_slot_while_the_hub_waited_is_not_paired() {
    let mut rig = ProbeRig::new(true, 290.0, 600.0, 0.0, RATE);
    rig.run(1.5);
    rig.r.music = false;
    rig.run(0.5);
    let end = rig.end_slot;
    let mut swapped = false;
    rig.probe(|b, _, since| {
        if since >= (0.6 * RATE as f64) as i64 && !swapped {
            swapped = true; // after the answer, before the unmute
            b.slots[end].claimNs.fetch_add(12345, SeqCst);
        }
    });
    assert!(swapped);
    assert_eq!(rig.b().feeders[rig.fidx].reply.load(SeqCst), kReplyNoMatch);
    assert_eq!(rig.b().friends.0[0].outSlot.load(SeqCst), -1);
    assert!(rig.r.hub.measurer().pair_memory().is_empty());
}

#[test]
fn line_up_that_starts_during_a_probe_waits_until_the_probe_is_over() {
    let mut rig = ProbeRig::new(true, 290.0, 0.0, 0.0, RATE); // Line up is off
    rig.run(1.5);
    rig.r.music = false;
    rig.run(0.5);
    let m = rig.r.hub.measurer();
    m.set_line_up(true, 600.0);
    let before = m.line_up_ms();
    rig.probe(|_, _, _| {});
    assert!(before.abs() < 0.01);
    assert!(m.line_up_ms().abs() < 0.01); // not while the sweep may sit in the delay line
    rig.b().friends.0[0]
        .route
        .store(FriendRoute::kRouteDaw, SeqCst);
    rig.run(1.5);
    assert!(m.line_up_ms() > 250.0); // and then it does
    assert!(sweep_in(&rig.r.stream, rig.start_out, rig.sweep()) < 0.05);
}

#[test]
fn line_up_does_not_grow_during_a_probe_when_a_friends_delay_grows() {
    let mut rig = ProbeRig::new(true, 290.0, 600.0, 0.0, RATE);
    rig.run(1.5);
    rig.r.music = false;
    rig.run(0.5);
    let m = rig.r.hub.measurer();
    let d = m.line_up_ms();
    assert!(d > 250.0);
    let mut grown = false;
    rig.probe(|b, hub, since| {
        if since >= (0.1 * RATE as f64) as i64 && !grown {
            grown = true; // a worse network, measured now: D wants to grow, still under the 600 ms limit
            b.friends.0[0].delayBits.store(float_bits(500.0), SeqCst);
        }
        assert!((hub.measurer().line_up_ms() - d).abs() < 0.01);
    });
    assert!(grown);
    check_sweep_never_heard(&mut rig);
    assert!(m.line_up_ms() > d + 150.0); // and it did want to: it follows once the probe is over
}

#[test]
fn nobody_hears_the_probe_sweep_at_44k1() {
    let mut rig = ProbeRig::new(true, 290.0, 600.0, 450.0, 44100);
    rig.run(1.5);
    rig.r.hub.measurer().measure_latencies();
    rig.r.music = false;
    rig.run(0.5);
    rig.probe(|_, _, _| {});
    assert!(rig.replied);
    assert_eq!(rig.b().feeders[rig.fidx].reply.load(SeqCst), 1);
    check_sweep_never_heard(&mut rig);
}

#[test]
fn the_probe_is_refused_at_96k_before_anything_is_muted_or_played() {
    let mut rig = ProbeRig::new(true, 40.0, 0.0, 0.0, 96000);
    rig.run(1.0);
    let seq = rig.b().feeders[rig.fidx].hubCommandSeq.load(SeqCst);
    let m = rig.r.hub.measurer();
    assert!(!m.start_probe(rig.fidx));
    assert!(!m.probing());
    rig.run(0.5);
    assert_eq!(rig.b().feeders[rig.fidx].hubCommand.load(SeqCst), 0);
    assert_eq!(rig.b().feeders[rig.fidx].hubCommandSeq.load(SeqCst), seq);
    assert!(rig.daw.sweep_from < 0);
    assert!(tail_peak(&rig.r.stream) > 0.05); // the Stream Mix goes on
    assert!(tail_peak(&rig.r.friend_mix) > 0.05);
}

#[test]
fn a_probe_the_hub_ended_takes_the_sweep_command_back() {
    let mut rig = ProbeRig::new(true, 290.0, 600.0, 0.0, RATE);
    rig.run(1.5);
    rig.probe(|_, _, _| {});
    assert_eq!(rig.b().feeders[rig.fidx].hubCommand.load(SeqCst), 0);
}

#[test]
fn speaker_leak_detection_activates_when_the_friend_mic_receives_the_friend_mix() {
    let mut r = Room::new("leak", RATE);
    r.open_friend(0, VOICE, 40.0);
    // friend 0's microphone receives the music 50 ms (2400 frames) later, gain 0.8
    r.friends[0].custom = Some(|t| 0.8 * noise_at(t - 2400, MUSIC));
    r.run(1.2, &mut none);
    let m = r.hub.measurer();
    m.measure_latencies();
    assert_eq!(r.hub.friend_info(0).leak_detected.load(SeqCst), 1);
    // the leak stops: friend 0 sends a clean voice
    r.friends[0].custom = None;
    r.run(1.2, &mut none);
    m.measure_latencies();
    assert_eq!(r.hub.friend_info(0).leak_detected.load(SeqCst), 0);
}

#[test]
fn echo_in_master_detection_activates_when_the_friend_voice_enters_the_master_while_route_is_direct()
 {
    let mut r = Room::new("echo_mast", RATE);
    r.music = false;
    r.open_friend(0, VOICE, 40.0);
    assert_eq!(
        r.b().friends.0[0].route.load(SeqCst),
        FriendRoute::kRouteDirect
    );
    r.run(0.5, &mut none); // let the jitter buffer prime and settle
    // friend 0's voice leaks into the master bus (DAW track monitoring with 5 ms DAW latency)
    r.run(1.0, &mut |_, hub, t0, l| {
        let tms = bits_float(hub.friend_info(0).total_ms_bits.load(SeqCst));
        let tot = (tms * 48.0 + 0.5) as i64;
        for (i, x) in l.iter_mut().enumerate() {
            *x += 0.8 * noise_at(t0 + i as i64 - tot - 240, VOICE);
        }
    });
    let m = r.hub.measurer();
    m.measure_latencies();
    assert_eq!(r.hub.friend_info(0).echo_in_master.load(SeqCst), 1);
    // with the route on DAW the friend in the master is intended, not an unwanted echo warning
    r.b().friends.0[0]
        .route
        .store(FriendRoute::kRouteDaw, SeqCst);
    m.measure_latencies();
    assert_eq!(r.hub.friend_info(0).echo_in_master.load(SeqCst), 0);
    // Direct again but the leak stopped: the warning clears
    r.b().friends.0[0]
        .route
        .store(FriendRoute::kRouteDirect, SeqCst);
    r.run(1.0, &mut none);
    m.measure_latencies();
    assert_eq!(r.hub.friend_info(0).echo_in_master.load(SeqCst), 0);
}

#[test]
fn direct_monitor_check_turns_off_vocal_monitor_to_prevent_comb_filtering() {
    let r = Room::new("direct_mon", RATE);
    let b = r.b();
    let sidx = claim_slot(b, &make_uuid(), RATE as u32, 2).unwrap();
    set_slot_identity(&b.slots[sidx], "Lead Vocal", 0);
    b.slots[sidx]
        .flags
        .store(SlotFlags::kFlagMon | SlotFlags::kFlagStr, SeqCst);
    post_command(&b.slots[sidx], ParamId::Mon, 0.0); // the Hub's direct-monitor checklist action
    let mut cursor = 0;
    let delivered = poll_commands(&b.slots[sidx], &mut cursor, |id, val| {
        if id == ParamId::Mon {
            if val > 0.5 {
                b.slots[sidx].flags.fetch_or(SlotFlags::kFlagMon, SeqCst);
            } else {
                b.slots[sidx].flags.fetch_and(!SlotFlags::kFlagMon, SeqCst);
            }
        }
    });
    assert_eq!(delivered, 1);
    assert_eq!(b.slots[sidx].flags.load(SeqCst) & SlotFlags::kFlagMon, 0);
}

#[test]
fn preparing_the_hub_during_a_probe_ends_it_instead_of_keeping_the_viewers_muted() {
    let mut rig = ProbeRig::new(true, 290.0, 0.0, 0.0, RATE);
    rig.run(1.5);
    let m = rig.r.hub.measurer();
    let seq = rig.b().feeders[rig.fidx].replySeq.load(SeqCst);
    assert!(m.start_probe(rig.fidx));
    for _ in 0..30 {
        let daw = &mut rig.daw;
        rig.r
            .cycle(&mut |b, _, t, mm| daw.feeder_and_tracks(b, t, mm));
        m.probe_step();
    }
    assert!(m.probing());
    rig.r.hub.prepare(RATE as f64, 1024); // the host changes the block size mid-probe
    assert!(!m.probing());
    assert_ne!(rig.b().feeders[rig.fidx].replySeq.load(SeqCst), seq);
    assert_eq!(rig.b().feeders[rig.fidx].reply.load(SeqCst), kReplyFailed);
    rig.run(0.5);
    assert!(tail_peak(&rig.r.stream) > 0.05); // the viewers hear the stream again at once
}
