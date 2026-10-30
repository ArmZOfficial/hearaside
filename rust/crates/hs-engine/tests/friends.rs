//! The friends room in the Hub engine (port of `tests/unit/test_friends.cpp`, audio and pairing cases):
//! friends reach the headphones and the Stream Mix, Line up delays the Stream Mix so every voice lands
//! on the beat, panic and the route decide who hears what; S7 pairing by name, by hand and from memory.
#![allow(clippy::needless_range_loop)]

use hs_bus::bus::{
    bits_float, claim_feeder, claim_slot, float_bits, make_uuid, release_feeder, release_slot,
    set_feeder_identity, set_slot_identity,
};
use hs_bus::protocol::*;
use hs_bus::ring::{ring_read_at, ring_write};
use hs_bus::shm::{current_pid, now_ns};
use hs_dsp::dsp::db_to_gain;
use hs_engine::hub_engine::{HubEngine, HubParams};
use hs_engine::measure::{FRIEND_MON, FRIEND_STR};
use hs_engine::track_publisher::TrackPublisher;
use std::sync::atomic::Ordering::SeqCst;

const MUSIC: u32 = 11;
const VOICE: u32 = 29;
const VOICE2: u32 = 47;
const RATE: i64 = 48000;

fn unique_bus(tag: &str) -> String {
    format!("fr_{tag}_{}_{}", current_pid(), now_ns() % 1_000_000)
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
    id: u32,
    seed: u32,
    /// network + browser: a finished 20 ms packet reaches the Hub this much later
    net_frames: i64,
    /// friend frames written so far
    sent: i64,
    open: bool,
}

/// A DAW cycle by cycle: the master bus carries `music`, friends write packets like a browser does.
struct Room {
    bus: String,
    sr: i64,
    hub: HubEngine,
    params: HubParams,
    t: i64,
    block: usize,
    music: bool,
    phones: Vec<f32>,
    stream: Vec<f32>,
    friend_mix: Vec<f32>,
    stream_read: u64,
    mix_read: u64,
    read_init: bool,
    friends: [Friend; kMaxFriends],
    /// a HEARASIDE Track at the end of a DAW channel that carries a friend: content = noise(t - lag, seed)
    carrier: Option<TrackPublisher>,
    carrier_lag: i64,
    carrier_seed: u32,
}

impl Room {
    fn new(tag: &str) -> Self {
        let bus = unique_bus(tag);
        let mut hub = HubEngine::new();
        hub.connect(&bus);
        hub.prepare(RATE as f64, 1024);
        hub.maintain();
        hub.bus()
            .unwrap()
            .streamHeader
            .sampleRate
            .store(RATE as u32, SeqCst);
        Room {
            bus,
            sr: RATE,
            hub,
            params: HubParams::default(),
            t: 0,
            block: 256,
            music: true,
            phones: vec![],
            stream: vec![],
            friend_mix: vec![],
            stream_read: 0,
            mix_read: 0,
            read_init: false,
            friends: std::array::from_fn(|_| Friend::default()),
            carrier: None,
            carrier_lag: 0,
            carrier_seed: 0,
        }
    }

    fn b(&self) -> &BusLayout {
        self.hub.bus().unwrap()
    }

    /// the friend sings along with the music: the voice at friend time i belongs to music time i
    fn open_friend(&mut self, j: usize, seed: u32, net_ms: f64) {
        let f = &mut self.friends[j];
        f.id = 100 + j as u32;
        f.seed = seed;
        f.net_frames = (net_ms * self.sr as f64 / 1000.0 + 0.5) as i64;
        f.open = true;
        f.sent = (self.t / 960) * 960;
        let measured_ms = net_ms + 10.0;
        let h = &self.hub.bus().unwrap().friends.0[j];
        h.id.store(f.id, SeqCst);
        h.sampleRate.store(self.sr as u32, SeqCst);
        h.delayBits.store(float_bits(measured_ms as f32), SeqCst);
        h.route.store(FriendRoute::kRouteDirect, SeqCst);
        h.state.store(FriendState::kFriendLive, SeqCst);
    }

    fn close_friend(&mut self, j: usize) {
        self.friends[j].open = false;
        self.b().friends.0[j]
            .state
            .store(FriendState::kFriendOffline, SeqCst);
    }

    fn cycle(&mut self) {
        let block = self.block;
        {
            let b = self.hub.bus().unwrap();
            // friends: 20 ms packets as the clock passes
            for j in 0..kMaxFriends {
                let f = &mut self.friends[j];
                if !f.open {
                    continue;
                }
                let h = &b.friends.0[j];
                h.heartbeatNs.store(now_ns(), SeqCst);
                while f.sent + 960 + f.net_frames <= self.t {
                    let pk: Vec<f32> = (0..960).map(|i| noise_at(f.sent + i, f.seed)).collect();
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
        if let Some(c) = &self.carrier {
            let cs: Vec<f32> = (0..block)
                .map(|i| noise_at(self.t + i as i64 - self.carrier_lag, self.carrier_seed))
                .collect();
            c.process(&[&cs], block, self.t, true, false, false);
            // the DAW plays the Track's output into the master, unless it sends the viewers' side to the Hub
            if c.slot().unwrap().flags.load(SeqCst) & SlotFlags::kFlagViewersViaHub == 0 {
                for (x, y) in l.iter_mut().zip(&cs) {
                    *x += y;
                }
            }
        }
        let mut r = l.clone();
        self.hub
            .process(&mut [&mut l, &mut r], &self.params, self.t, true, false);
        self.phones.extend_from_slice(&l);
        // capture the Stream Mix and what friends hear
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

    fn run(&mut self, seconds: f64) {
        for _ in 0..(seconds * self.sr as f64 / self.block as f64) as usize {
            self.cycle();
        }
    }

    /// normalised correlation of x[from, from+len) with noise(seed) delayed by `lag` frames
    fn corr(x: &[f32], offset: i64, from: usize, len: usize, seed: u32, lag: i64) -> f64 {
        let (mut xy, mut xx, mut yy) = (0.0, 0.0, 0.0);
        for i in from..(from + len).min(x.len()) {
            let a = f64::from(x[i]);
            let y = f64::from(noise_at(offset + i as i64 - lag, seed));
            xy += a * y;
            xx += a * a;
            yy += y * y;
        }
        if xx > 0.0 && yy > 0.0 {
            xy / (xx * yy).sqrt()
        } else {
            0.0
        }
    }

    /// lag (frames) in [lo, hi] where x best matches noise(seed), -1 when nothing correlates
    fn best_lag(
        x: &[f32],
        offset: i64,
        from: usize,
        len: usize,
        seed: u32,
        lo: i64,
        hi: i64,
    ) -> (i64, f64) {
        let (mut best, mut bs) = (-1, 0.15);
        for k in lo..=hi {
            let c = Self::corr(x, offset, from, len, seed, k);
            if c > bs {
                bs = c;
                best = k;
            }
        }
        (best, bs)
    }

    fn ms(frames: i64) -> f64 {
        frames as f64 * 1000.0 / RATE as f64
    }

    fn peak(v: &[f32], from: usize) -> f32 {
        v[from.min(v.len())..]
            .iter()
            .fold(0.0, |p, x| p.max(x.abs()))
    }
}

fn info_f(
    r: &Room,
    j: usize,
    f: impl Fn(&hs_engine::measure::FriendInfo) -> &std::sync::atomic::AtomicU32,
) -> f64 {
    f64::from(bits_float(f(r.hub.friend_info(j)).load(SeqCst)))
}

#[test]
fn a_live_friend_reaches_the_headphones_and_the_stream_mix_the_headphones_are_not_delayed() {
    let mut r = Room::new("basic");
    r.music = false;
    r.open_friend(0, VOICE, 90.0);
    r.run(2.0);
    assert_eq!(r.hub.friend_info(0).playing.load(SeqCst), 1);
    let (from, len) = ((1.2 * RATE as f64) as usize, (0.06 * RATE as f64) as usize);
    let (lag_p, sp) = Room::best_lag(
        &r.phones,
        0,
        from,
        len,
        VOICE,
        0,
        (0.6 * RATE as f64) as i64,
    );
    let (lag_s, sh) = Room::best_lag(
        &r.stream,
        0,
        from,
        len,
        VOICE,
        0,
        (0.6 * RATE as f64) as i64,
    );
    assert!(lag_p >= 0 && lag_s >= 0);
    // with Line up off both hear the friend as it arrives: 90 ms of network + the Hub's jitter buffer
    let buf = info_f(&r, 0, |i| &i.buffer_ms_bits);
    assert!(
        (Room::ms(lag_p) - (90.0 + 10.0 + buf)).abs() < 7.0,
        "{} vs {}",
        Room::ms(lag_p),
        100.0 + buf
    );
    assert!((Room::ms(lag_s) - Room::ms(lag_p)).abs() < 2.0);
    assert!(sp > 0.8 && sh > 0.8, "{sp} {sh}");
}

#[test]
fn you_hear_viewers_hear_panic_are_separate() {
    let mut r = Room::new("gates");
    r.music = false;
    r.open_friend(0, VOICE, 40.0);
    r.run(1.5);
    let m0 = r.phones.len();
    r.hub.friend_control(0).flags.store(FRIEND_STR, SeqCst); // viewers only
    r.run(1.0);
    assert!(Room::peak(&r.phones, m0 + 8000) < 1e-4);
    assert!(Room::peak(&r.stream, m0 + 8000) > 0.05);

    let m1 = r.phones.len();
    r.hub
        .friend_control(0)
        .flags
        .store(FRIEND_MON | FRIEND_STR, SeqCst);
    r.params.panic = true;
    r.run(1.0);
    assert!(Room::peak(&r.phones, m1 + 8000) > 0.05); // panic never touches the headphones
    assert!(Room::peak(&r.stream, m1 + 8000) < 1e-4); // and cuts the viewers at once
}

#[test]
fn gain_and_pan_apply_to_the_viewers_side() {
    let mut r = Room::new("pan");
    r.music = false;
    r.open_friend(0, VOICE, 30.0);
    r.hub
        .friend_control(0)
        .gain_bits
        .store(float_bits(-6.0), SeqCst);
    r.hub
        .friend_control(0)
        .pan_bits
        .store(float_bits(1.0), SeqCst); // hard right
    r.run(2.0);
    let b = r.b();
    let (mut l, mut rr) = (vec![0.0f32; 4096], vec![0.0f32; 4096]);
    let w = b.streamHeader.writePos.0[0].load(SeqCst);
    ring_read_at(
        &b.streamAudio.0[0],
        &b.streamHeader.writePos.0[0],
        w - 4096,
        &mut [&mut l, &mut rr],
        4096,
        None,
    );
    assert!(Room::peak(&l, 0) < 1e-4);
    assert!(Room::peak(&rr, 0) > 0.05);
}

#[test]
fn line_up_puts_the_music_and_every_friend_on_the_same_beat() {
    let mut r = Room::new("lineup");
    r.open_friend(0, VOICE, 100.0);
    r.open_friend(1, VOICE2, 250.0); // the slowest
    r.hub.set_line_up(true, 600.0);
    r.run(4.0);
    let m = r.hub.measurer();
    let d = f64::from(m.line_up_ms());
    let slowest_tot = info_f(&r, 1, |i| &i.total_ms_bits);
    assert_eq!(m.line_up_slowest(), 1);
    assert!((d - slowest_tot).abs() < 3.0, "{d} vs {slowest_tot}");
    assert!(d > 250.0);

    // the stream: music delayed by D, both friends delayed so they land on the same beat as the music
    let (from, len) = ((3.0 * RATE as f64) as usize, (0.06 * RATE as f64) as usize);
    let lag = (d * RATE as f64 / 1000.0 + 0.5) as i64;
    let (lo, hi) = (lag - 600, lag + 600);
    let (lag_m, _) = Room::best_lag(&r.stream, 0, from, len, MUSIC, lo, hi);
    let (lag_a, _) = Room::best_lag(&r.stream, 0, from, len, VOICE, lo, hi);
    let (lag_b, _) = Room::best_lag(&r.stream, 0, from, len, VOICE2, lo, hi);
    assert!(lag_m >= 0 && lag_a >= 0 && lag_b >= 0);
    assert!((Room::ms(lag_m) - d).abs() < 2.0);
    assert!((Room::ms(lag_a) - Room::ms(lag_m)).abs() < 7.0); // each singer is as late as the music
    assert!((Room::ms(lag_b) - Room::ms(lag_m)).abs() < 7.0);

    // the headphones are never delayed and what friends hear is the live mix
    let (lag_ph, _) = Room::best_lag(
        &r.phones,
        0,
        from,
        len,
        VOICE,
        0,
        (0.3 * RATE as f64) as i64,
    );
    assert!(lag_ph >= 0);
    let buf = info_f(&r, 0, |i| &i.buffer_ms_bits);
    assert!((Room::ms(lag_ph) - (110.0 + buf)).abs() < 8.0);
    let (lag_fm, _) = Room::best_lag(&r.friend_mix, 0, from, len, MUSIC, 0, 400);
    assert!(lag_fm >= 0 && Room::ms(lag_fm) < 3.0);
}

#[test]
fn line_up_off_means_live_again_the_change_is_a_crossfade_not_a_jump() {
    let mut r = Room::new("lineoff");
    r.open_friend(0, VOICE, 200.0);
    r.hub.set_line_up(true, 600.0);
    r.run(3.0);
    let m = r.hub.measurer();
    assert!(m.line_up_ms() > 150.0);
    let changes = m.line_up_changes();
    let mark = r.stream.len();
    r.hub.set_line_up(false, 600.0);
    r.run(2.0);
    assert_eq!(m.line_up_ms(), 0.0);
    assert!(m.line_up_changes() > changes);
    // after the switch the music is live again (no delay)
    let (lag, _) = Room::best_lag(
        &r.stream,
        0,
        mark + (1.2 * RATE as f64) as usize,
        (0.4 * RATE as f64) as usize,
        MUSIC,
        0,
        (0.6 * RATE as f64) as i64,
    );
    assert!(lag >= 0 && Room::ms(lag) < 3.0);
    // no click: the largest sample step stays in the range of the material itself
    let max_step = (mark + 1..r.stream.len())
        .fold(0.0f32, |p, i| p.max((r.stream[i] - r.stream[i - 1]).abs()));
    assert!(max_step < 1.3, "{max_step}");
}

#[test]
fn a_friend_slower_than_the_limit_is_not_waited_for() {
    let mut r = Room::new("limit");
    r.open_friend(0, VOICE, 100.0);
    r.open_friend(1, VOICE2, 900.0); // over a 400 ms limit
    r.hub.set_line_up(true, 400.0);
    r.run(4.0);
    assert_eq!(r.hub.friend_info(1).over_limit.load(SeqCst), 1);
    assert_eq!(r.hub.friend_info(0).over_limit.load(SeqCst), 0);
    assert_eq!(r.hub.measurer().line_up_slowest(), 0);
    assert!(r.hub.measurer().line_up_ms() < 250.0);
}

#[test]
fn the_route_decides_who_plays_the_friend_never_both() {
    let mut r = Room::new("route");
    r.music = false;
    r.open_friend(0, VOICE, 40.0);
    r.run(1.5);
    r.b().friends.0[0]
        .route
        .store(FriendRoute::kRouteDaw, SeqCst); // a DAW track carries this friend now
    r.run(1.0);
    let n = r.phones.len() - 20000;
    assert!(Room::peak(&r.phones, n) < 1e-4); // out of the headphones...
    assert!(Room::peak(&r.stream, n) < 1e-4); // ...and out of the Stream Mix: the DAW track is the only way
    r.b().friends.0[0]
        .route
        .store(FriendRoute::kRouteDirect, SeqCst);
    r.run(1.0);
    let n = r.phones.len() - 20000;
    assert!(Room::peak(&r.phones, n) > 0.05);
    assert!(Room::peak(&r.stream, n) > 0.05);
}

#[test]
fn a_friend_who_stops_is_silent_within_a_second_and_does_not_hang_the_hub() {
    let mut r = Room::new("gone");
    r.music = false;
    r.open_friend(0, VOICE, 40.0);
    r.run(1.5);
    r.close_friend(0);
    r.run(1.5);
    assert!(Room::peak(&r.phones, r.phones.len() - 20000) < 1e-4);
    assert_eq!(r.hub.friend_info(0).playing.load(SeqCst), 0);
    r.open_friend(0, VOICE, 40.0); // and comes back
    r.run(1.5);
    assert_eq!(r.hub.friend_info(0).playing.load(SeqCst), 1);
}

#[test]
fn eight_friends_at_once_stay_under_the_streams_limiter() {
    let mut r = Room::new("eight");
    for j in 0..kMaxFriends {
        r.open_friend(j, VOICE + j as u32 * 7, 60.0 + 30.0 * j as f64);
    }
    r.hub.set_line_up(true, 600.0);
    r.params.ceiling_db = -3.0;
    r.run(4.0);
    let peak = Room::peak(&r.stream, (2.5 * RATE as f64) as usize);
    assert!(peak > 0.05);
    assert!(peak < db_to_gain(-3.0) + 1e-3, "{peak}"); // the limiter sees the final sum, friends included
    for j in 0..kMaxFriends {
        assert_eq!(r.hub.friend_info(j).playing.load(SeqCst), 1, "friend {j}");
    }
}

/// a friend input (feeder) linked to friend `j` with id `fid`
fn add_feeder(b: &BusLayout, j: usize, fid: u32, name: &str) -> usize {
    b.friends.0[j].id.store(fid, SeqCst);
    b.friends.0[j].state.store(FriendState::kFriendLive, SeqCst);
    let fidx = claim_feeder(b).expect("feeder");
    set_feeder_identity(&b.feeders[fidx], name, 0);
    b.feeders[fidx].friendId.store(fid, SeqCst);
    b.feeders[fidx]
        .status
        .store(FeederStatus::kFeederFlowing, SeqCst);
    b.feeders[fidx].heartbeatNs.store(now_ns(), SeqCst);
    b.friends.0[j].feeder.store(fidx as i32, SeqCst);
    fidx
}

fn add_slot(b: &BusLayout, uuid: &str, name: &str) -> usize {
    let s = claim_slot(b, uuid, RATE as u32, 2).expect("slot");
    set_slot_identity(&b.slots[s], name, 0);
    s
}

#[test]
fn s7_feeder_pairing_by_track_identity_li_measurement_and_line_up_route() {
    let r = Room::new("s7_pair");
    let b = r.b();
    let m = r.hub.measurer();
    let fidx = add_feeder(b, 0, 101, "Lead Vocal");
    let sidx = add_slot(b, &make_uuid(), "Lead Vocal");
    b.slots[sidx]
        .chainLatencyBits
        .store(float_bits(12.5), SeqCst);

    m.measure_latencies();
    assert_eq!(b.slots[sidx].fedBy.load(SeqCst), 101);
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), sidx as i32);
    assert!((bits_float(b.slots[sidx].fxLatencyBits.load(SeqCst)) - 12.5).abs() < 0.1);

    // a paired Track always sends the viewers' side through the Hub, Line up or not
    for on in [true, false] {
        m.set_line_up(on, 400.0);
        m.measure_latencies();
        assert_ne!(
            b.slots[sidx].flags.load(SeqCst) & SlotFlags::kFlagViewersViaHub,
            0
        );
    }

    // the friend input goes: the Track is unpaired
    b.friends.0[0].feeder.store(-1, SeqCst);
    m.measure_latencies();
    assert_eq!(b.slots[sidx].fedBy.load(SeqCst), 0);
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1);
    assert_eq!(
        b.slots[sidx].flags.load(SeqCst) & SlotFlags::kFlagViewersViaHub,
        0
    );
    release_slot(b, sidx);
    release_feeder(b, fidx);
}

#[test]
fn a_pair_chosen_by_hand_wins_over_the_names_is_refused_for_bad_tracks_and_ends_with_its_track() {
    let r = Room::new("manual_pair");
    let b = r.b();
    let m = r.hub.measurer();
    let fidx = add_feeder(b, 0, 101, "Friend 1"); // what a host that sends no track names gives
    let sidx = add_slot(b, &make_uuid(), "Track 3");
    let aidx = add_slot(b, &make_uuid(), "");
    let other = add_slot(b, &make_uuid(), "Track 4");
    b.slots[aidx].flags.fetch_or(SlotFlags::kFlagApp, SeqCst);
    b.slots[sidx]
        .chainLatencyBits
        .store(float_bits(12.5), SeqCst);

    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1); // names differ: nothing pairs

    assert!(!m.pair_with(fidx, aidx as u32)); // an App Audio
    assert!(!m.pair_with(fidx, kMaxSlots as u32 - 1)); // not a Track
    assert!(!m.pair_with(fidx + 1, sidx as u32)); // not a friend input
    b.slots[other].fedBy.store(555, SeqCst);
    assert!(!m.pair_with(fidx, other as u32)); // carries another friend
    b.slots[other].fedBy.store(0, SeqCst);
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1);

    assert!(m.pair_with(fidx, sidx as u32));
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), sidx as i32);
    assert_eq!(b.slots[sidx].fedBy.load(SeqCst), 101);
    assert_ne!(
        b.slots[sidx].flags.load(SeqCst) & SlotFlags::kFlagViewersViaHub,
        0
    );
    assert!((bits_float(b.slots[sidx].fxLatencyBits.load(SeqCst)) - 12.5).abs() < 0.1);
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), sidx as i32); // the names still differ: it stays

    assert!(m.pair_with(fidx, other as u32)); // changing one's mind
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), other as i32);
    assert_eq!(b.slots[sidx].fedBy.load(SeqCst), 0);

    // the Track goes away and another one takes its index before the Hub looks again: not paired by mistake
    release_slot(b, other);
    let again = claim_slot(b, &make_uuid(), RATE as u32, 2).unwrap();
    assert_eq!(again, other);
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1);
    assert_eq!(b.slots[again].fedBy.load(SeqCst), 0);
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1);

    assert!(m.pair_with(fidx, sidx as u32));
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), sidx as i32);
    assert!(m.pair_with(fidx, kPairAutomatic)); // back to automatic: the names differ, so unpaired
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1);
    assert_eq!(b.slots[sidx].fedBy.load(SeqCst), 0);

    assert!(m.pair_with(fidx, sidx as u32));
    m.measure_latencies();
    b.friends.0[0].feeder.store(-1, SeqCst); // the friend input goes away
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1);
    b.friends.0[0].feeder.store(fidx as i32, SeqCst);
    m.measure_latencies();
    assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1); // and it does not come back by itself
}

#[test]
fn a_track_chosen_by_hand_for_one_friend_input_is_not_taken_by_another_with_the_same_name() {
    let r = Room::new("reserved");
    let b = r.b();
    let m = r.hub.measurer();
    let f0 = add_feeder(b, 0, 101, "Lead");
    let f1 = add_feeder(b, 1, 102, "Lead"); // both channels are called the same
    let lead = add_slot(b, &make_uuid(), "Lead");
    assert!(m.pair_with(f1, lead as u32)); // the second input is the one that was chosen by hand
    for _ in 0..3 {
        m.measure_latencies();
        assert_eq!(b.friends.0[1].outSlot.load(SeqCst), lead as i32);
        assert_eq!(b.friends.0[0].outSlot.load(SeqCst), -1); // the same name is not enough
        assert_eq!(b.slots[lead].fedBy.load(SeqCst), 102);
    }
    let _ = f0;
}

#[test]
fn a_remembered_pair_is_applied_again_when_the_friend_input_and_the_track_are_both_back() {
    let mut r = Room::new("pair_memory");
    let m = r.hub.measurer();
    let (fidx, uuid, mut sidx);
    {
        let b = r.b();
        fidx = add_feeder(b, 0, 101, "Friend 1");
        uuid = make_uuid();
        sidx = add_slot(b, &uuid, "Track 3");
    }
    assert!(m.pair_memory().is_empty());
    assert!(m.pair_with(fidx, sidx as u32));
    let saved = m.pair_memory(); // what the project would store
    assert_eq!(saved.len(), 1);
    assert_eq!(saved[0].feeder, "Friend 1");
    assert_eq!(saved[0].slot_uuid, uuid);
    m.measure_latencies();
    assert_eq!(r.b().friends.0[0].outSlot.load(SeqCst), sidx as i32);

    // the project is closed and opened again: the Track comes back first on another index, the input later
    let filler;
    {
        let b = r.b();
        release_slot(b, sidx);
        b.friends.0[0].feeder.store(-1, SeqCst);
    }
    m.measure_latencies();
    assert_eq!(r.b().friends.0[0].outSlot.load(SeqCst), -1);
    {
        let b = r.b();
        filler = claim_slot(b, &make_uuid(), RATE as u32, 2).unwrap();
        sidx = add_slot(b, &uuid, "Track 3");
        b.friends.0[0].feeder.store(fidx as i32, SeqCst);
        b.feeders[fidx].heartbeatNs.store(now_ns(), SeqCst);
    }
    r.hub.maintain();
    m.measure_latencies();
    assert_eq!(r.b().friends.0[0].outSlot.load(SeqCst), sidx as i32);
    assert_eq!(r.b().slots[filler].fedBy.load(SeqCst), 0);

    // a new Hub gets the saved pairs from the project
    m.pair_with(fidx, kPairAutomatic);
    assert!(m.pair_memory().is_empty()); // choosing Automatic forgets it
    m.measure_latencies();
    assert_eq!(r.b().friends.0[0].outSlot.load(SeqCst), -1);
    m.set_pair_memory(saved);
    r.b().feeders[fidx].heartbeatNs.store(now_ns(), SeqCst);
    r.hub.maintain();
    m.measure_latencies();
    assert_eq!(r.b().friends.0[0].outSlot.load(SeqCst), sidx as i32);
}

#[test]
fn a_friend_mixed_in_a_daw_track_never_hears_themselves_and_is_delayed_once_not_twice() {
    let mut r = Room::new("s7_minus");
    r.params.sync_safety = 0;
    r.open_friend(0, VOICE, 100.0);
    r.open_friend(1, VOICE2, 250.0); // the friend a DAW track carries
    r.b().friends.0[1]
        .route
        .store(FriendRoute::kRouteDaw, SeqCst);
    let mut carrier = TrackPublisher::new();
    carrier.connect(&r.bus, "", RATE as u32, 1);
    carrier.prepare(RATE as u32, 1);
    let sh = carrier.slot().unwrap();
    sh.fedBy.store(101, SeqCst);
    sh.flags.fetch_or(SlotFlags::kFlagViewersViaHub, SeqCst); // paired (what measure_latencies does)
    r.carrier = Some(carrier);
    r.carrier_seed = VOICE2;
    r.carrier_lag = (0.300 * RATE as f64) as i64; // the voice reaches the DAW track 300 ms after its beat
    r.hub.set_line_up(true, 600.0);
    r.run(4.0);

    let (from, len) = ((3.0 * RATE as f64) as usize, (0.06 * RATE as f64) as usize);
    // what friends hear: the music, never a friend (direct or carried by the DAW)
    assert!(
        Room::best_lag(
            &r.friend_mix,
            0,
            from,
            len,
            VOICE2,
            0,
            (1.2 * RATE as f64) as i64
        )
        .0 < 0
    );
    assert!(
        Room::best_lag(
            &r.friend_mix,
            0,
            from,
            len,
            VOICE,
            0,
            (1.2 * RATE as f64) as i64
        )
        .0 < 0
    );
    assert!(Room::best_lag(&r.friend_mix, 0, from, len, MUSIC, 0, 400).0 >= 0);

    // the viewers: the carried friend joins after the Line-up delay, placed at D - its own delay
    let d = f64::from(r.hub.measurer().line_up_ms());
    let tot1 = info_f(&r, 1, |i| &i.total_ms_bits);
    assert!(d > 250.0);
    let (lag_c, _) = Room::best_lag(
        &r.stream,
        0,
        from,
        len,
        VOICE2,
        0,
        (1.2 * RATE as f64) as i64,
    );
    assert!(lag_c >= 0);
    assert!(
        (Room::ms(lag_c) - (300.0 + d - tot1)).abs() < 8.0,
        "{} vs {}",
        Room::ms(lag_c),
        300.0 + d - tot1
    );
}

#[test]
fn friends_hear_each_other_as_soon_as_each_voice_arrives_line_up_or_not() {
    let mut r = Room::new("hear_each");
    r.music = false;
    r.open_friend(0, VOICE, 100.0);
    r.open_friend(1, VOICE2, 250.0);
    r.hub.set_line_up(true, 600.0);
    r.run(4.0);
    let b = r.b();
    let w = b.friendVoiceWrite.0[0].load(SeqCst);
    assert_eq!(w, b.friendMixWrite.0.load(SeqCst)); // same positions as the mix friends hear
    let len = (0.5 * RATE as f64) as u32;
    let (mut v0, mut v1) = (vec![0.0f32; len as usize], vec![0.0f32; len as usize]);
    ring_read_at(
        &b.friendVoiceAudio.0[0],
        &b.friendVoiceWrite.0[0],
        w - u64::from(len),
        &mut [&mut v0, &mut v1],
        len,
        None,
    );
    // position == DAW time here; each voice is only as late as the line + the Hub's jitter buffer (not D)
    let from = (w - u64::from(len)) as i64;
    let win = (0.06 * RATE as f64) as usize;
    let (lag0, _) = Room::best_lag(&v0, from, 0, win, VOICE, 0, (0.5 * RATE as f64) as i64);
    let (lag1, _) = Room::best_lag(&v1, from, 0, win, VOICE2, 0, (0.5 * RATE as f64) as i64);
    assert!(lag0 >= 0 && lag1 >= 0);
    let buf = info_f(&r, 0, |i| &i.buffer_ms_bits);
    assert!((Room::ms(lag0) - (110.0 + buf)).abs() < 8.0);
    let d = f64::from(r.hub.measurer().line_up_ms());
    assert!(Room::ms(lag1) < d - 1.0 + 30.0);
    assert!(d > 250.0);
    // the base mix still carries no friend (the share server adds everyone but the listener)
    assert!(
        Room::best_lag(
            &r.friend_mix,
            0,
            (3.0 * RATE as f64) as usize,
            win,
            VOICE,
            0,
            (0.6 * RATE as f64) as i64
        )
        .0 < 0
    );
}

#[test]
fn a_track_flagged_via_hub_with_no_friend_never_feeds_an_empty_friend_entry() {
    // fedBy 0 with ViewersViaHub still set (unpairing in progress, or a crashed Hub): the C++ matched the first
    // empty friend entry (id 0) and sent the Track into that friend's voice
    let mut r = Room::new("fid0");
    let mut carrier = TrackPublisher::new();
    carrier.connect(&r.bus, "", RATE as u32, 1);
    carrier.prepare(RATE as u32, 1);
    let sh = carrier.slot().unwrap();
    sh.fedBy.store(0, SeqCst);
    sh.flags.fetch_or(SlotFlags::kFlagViewersViaHub, SeqCst);
    r.carrier = Some(carrier);
    r.carrier_seed = VOICE;
    r.run(1.0);
    let b = r.b();
    let w = b.friendVoiceWrite.0[0].load(SeqCst);
    let len = 24000u32;
    let (mut v0, mut v1) = (vec![0.0f32; len as usize], vec![0.0f32; len as usize]);
    ring_read_at(
        &b.friendVoiceAudio.0[0],
        &b.friendVoiceWrite.0[0],
        w - u64::from(len),
        &mut [&mut v0, &mut v1],
        len,
        None,
    );
    assert!(
        Room::peak(&v0, 0) == 0.0 && Room::peak(&v1, 0) == 0.0,
        "an empty friend entry got the Track's audio"
    );
}
