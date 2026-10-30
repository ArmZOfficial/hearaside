//! FriendReader's jitter buffer (port of `tests/unit/test_jitter.cpp`): measured arrival jitter, short
//! gaps hidden with a fade instead of a re-prime, a long outage re-primes and plays again. Simulated, no
//! threads: packets are written into a ChannelRing when their arrival time passes, blocks are pulled.
use hs_bus::protocol::ChannelRing;
use hs_bus::ring::ring_write;
use hs_engine::friend_reader::FriendReader;
use std::alloc::{Layout, alloc_zeroed};
use std::sync::atomic::AtomicU64;

const BLOCK: usize = 256;
const PI: f64 = std::f64::consts::PI;

fn ring() -> Box<ChannelRing> {
    // SAFETY: floats; all-zero is valid.
    unsafe { Box::from_raw(alloc_zeroed(Layout::new::<ChannelRing>()).cast::<ChannelRing>()) }
}

/// [0, 1)
fn rnd(s: &mut u32) -> f64 {
    *s = s.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
    f64::from(*s >> 8) / f64::from(1u32 << 24)
}

#[derive(Default)]
struct Res {
    out: Vec<f32>,
    avg_buf_ms: f64,
    p95: f64,
    concealed_ms: f64,
    gaps10: i32,
    underruns: i32,
    /// seen from outside (primed() went false)
    reprimes: i32,
    reprimes_counted: i32,
    /// largest sample-to-sample step after the first second
    max_step: f64,
    playing_at_end: bool,
}

/// `arrive(i)` = arrival time (ms) of packet i (>= 1e17 = lost); `sig(frame)` = what the friend sends.
/// Packets keep their order (TCP).
fn simulate(
    seconds: f64,
    mut arrive: impl FnMut(i32) -> f64,
    sig: fn(u64, u32) -> f32,
    keep: bool,
    block: usize,
    rate: u32,
    pkt_frames: usize,
) -> Res {
    let pkt = if pkt_frames > 0 {
        pkt_frames
    } else {
        (rate / 100) as usize
    }; // 10 ms packets unless asked
    let ring = ring();
    let wp = AtomicU64::new(0);
    let mut r = FriendReader::new();
    r.prepare(48000.0, block);
    let block_ms = block as f64 * 1000.0 / 48000.0;
    let mut res = Res::default();
    let (mut l, mut rr, mut pk) = (vec![0.0f32; block], vec![0.0f32; block], vec![0.0f32; pkt]);
    let blocks = (seconds * 1000.0 / block_ms) as i32;
    let (mut next_pkt, mut drawn) = (0i32, -1i32);
    let (mut last_arrival, mut buf_sum, mut raw_now) = (0.0f64, 0.0f64, 0.0f64);
    let (mut buf_n, mut quiet) = (0, 0);
    let (mut started, mut was_primed) = (false, false);
    let mut prev = 0.0f32;
    for k in 0..blocks {
        let now = f64::from(k) * block_ms;
        loop {
            if drawn != next_pkt {
                raw_now = arrive(next_pkt); // one random draw per packet, not per block
                drawn = next_pkt;
            }
            let raw = raw_now;
            let a = last_arrival.max(raw);
            if raw < 1.0e17 && a > now {
                break;
            }
            if raw < 1.0e17 {
                // a huge arrival time = the packet is dropped (lost, not delayed)
                for (i, x) in pk.iter_mut().enumerate() {
                    *x = sig(next_pkt as u64 * pkt as u64 + i as u64, rate);
                }
                ring_write(&ring, &wp, &[&pk], pkt as u32);
                last_arrival = a;
            }
            next_pkt += 1;
        }
        r.pull(&ring, &wp, rate, [&mut l, &mut rr], block);
        if k > 0 && was_primed && !r.primed() {
            res.reprimes += 1;
        }
        was_primed = r.primed();
        for &x in &l {
            if x.abs() > 0.02 {
                started = true;
            }
            if started {
                if x.abs() < 0.005 {
                    quiet += 1;
                    if quiet == 480 {
                        res.gaps10 += 1;
                    }
                } else {
                    quiet = 0;
                }
                if now > 1000.0 {
                    res.max_step = res.max_step.max(f64::from((x - prev).abs()));
                }
            }
            prev = x;
            if keep {
                res.out.push(x);
            }
        }
        if now > 30000.0 && r.primed() {
            buf_sum += r.buffer_ms();
            buf_n += 1;
        }
    }
    res.avg_buf_ms = if buf_n > 0 {
        buf_sum / f64::from(buf_n)
    } else {
        0.0
    };
    res.p95 = r.jitter_p95_ms();
    res.underruns = r.underruns() as i32;
    res.reprimes_counted = r.reprimes() as i32;
    res.concealed_ms = r.concealed_ms();
    res.playing_at_end = quiet < 480 && started;
    res
}

/// packet i is due at i * 10 ms + 40 ms; jitter uniform in +-j ms
fn jittery(j: f64, seed: u32) -> impl FnMut(i32) -> f64 {
    let mut s = seed;
    move |i| 40.0 + f64::from(i) * 10.0 + (rnd(&mut s) * 2.0 - 1.0) * j
}

fn dc(_: u64, _: u32) -> f32 {
    0.5
}

fn sine(f: u64, rate: u32) -> f32 {
    0.5 * (2.0 * PI * 440.0 * f as f64 / f64::from(rate)).sin() as f32
}

#[test]
fn the_measured_p95_follows_a_20ms_spread_of_arrival_times() {
    let r = simulate(60.0, jittery(10.0, 1), dc, false, BLOCK, 48000, 0);
    println!("p95 {:.1} ms (uniform +-10 ms)", r.p95);
    assert!((15.0..=25.0).contains(&r.p95), "{}", r.p95);
    let steady = simulate(20.0, jittery(0.0, 1), dc, false, BLOCK, 48000, 0);
    assert!(steady.p95 <= 8.0, "{}", steady.p95); // the pull block alone, 5.3 ms
    assert_eq!(steady.underruns, 0);
}

/// jitter +-10 ms for ten minutes: gaps <= once a minute, and the buffer is the jitter plus the block
/// plus ~8 ms (2 ms safety + half a packet)
fn ten_minutes(block: usize, rate: u32) {
    let r = simulate(600.0, jittery(10.0, 7), dc, false, block, rate, 0);
    let block_ms = block as f64 * 1000.0 / 48000.0;
    println!(
        "10 min block {block} {rate}: gaps {} hidden {} reprimes {} concealed {:.0} buffer {:.1} p95 {:.1}",
        r.gaps10, r.underruns, r.reprimes_counted, r.concealed_ms, r.avg_buf_ms, r.p95
    );
    assert!(r.gaps10 <= 10, "gaps {}", r.gaps10);
    assert!(r.reprimes_counted <= 1, "reprimes {}", r.reprimes_counted);
    assert!(
        r.avg_buf_ms <= r.p95 + block_ms + 8.0,
        "buffer {} p95 {}",
        r.avg_buf_ms,
        r.p95
    );
    assert!(r.playing_at_end);
}

#[test]
fn ten_minutes_block_256() {
    ten_minutes(256, 48000);
}

#[test]
fn ten_minutes_block_32() {
    ten_minutes(32, 48000);
}

#[test]
fn ten_minutes_block_2048() {
    ten_minutes(2048, 48000);
}

#[test]
fn ten_minutes_from_a_44k1_friend_resampler_path() {
    ten_minutes(256, 44100);
}

#[test]
fn a_line_with_3_percent_spikes_of_up_to_40ms_learns_its_headroom_and_gaps_rarely() {
    let mut s = 3u32;
    let spiky = move |i: i32| {
        let base = 40.0 + f64::from(i) * 10.0 + rnd(&mut s) * 6.0;
        if rnd(&mut s) < 0.03 {
            base + rnd(&mut s) * 40.0
        } else {
            base
        }
    };
    let r = simulate(600.0, spiky, dc, false, BLOCK, 48000, 0);
    assert!(r.gaps10 <= 10, "gaps {}", r.gaps10);
    assert_eq!(r.reprimes_counted, 0);
    assert!(r.playing_at_end);
}

#[test]
fn stalls_of_60_80ms_on_2_percent_of_the_packets_are_learned_not_repeated() {
    let mut s = 5u32;
    let stalls = move |i: i32| {
        let base = 40.0 + f64::from(i) * 10.0;
        if rnd(&mut s) < 0.02 {
            base + 60.0 + rnd(&mut s) * 20.0
        } else {
            base
        }
    };
    let r = simulate(600.0, stalls, dc, false, BLOCK, 48000, 0);
    assert!(r.gaps10 <= 10, "gaps {}", r.gaps10);
    assert!(r.p95 <= 30.0, "p95 {}", r.p95);
    assert!(
        (1..=10).contains(&r.reprimes_counted),
        "reprimes {}",
        r.reprimes_counted
    ); // the long-gap path really ran
    assert!(r.playing_at_end);
}

#[test]
fn four_80ms_stalls_3s_apart_gap_at_most_twice() {
    let stall = |i: i32| {
        let due = 40.0 + f64::from(i) * 10.0;
        for at in [10000.0, 13000.0, 16000.0, 19000.0] {
            if due >= at && due < at + 80.0 {
                return at + 80.0;
            }
        }
        due
    };
    let r = simulate(25.0, stall, dc, false, BLOCK, 48000, 0);
    assert!(r.reprimes_counted >= 1);
    assert!(r.gaps10 <= 2, "gaps {}", r.gaps10); // without learning from long gaps all four gap
    assert!(r.playing_at_end);
}

#[test]
fn a_cold_start_on_a_line_with_20ms_jitter_gaps_at_most_twice() {
    let r = simulate(5.0, jittery(20.0, 11), dc, false, BLOCK, 48000, 0);
    assert!(r.gaps10 <= 2, "gaps {}", r.gaps10);
    assert!(r.reprimes_counted <= 1);
    assert!(r.playing_at_end);
}

#[test]
fn a_single_15ms_late_packet_is_faded_over_not_reprimed_and_does_not_click() {
    let late = |i: i32| 40.0 + f64::from(i) * 10.0 + if i == 1500 { 15.0 } else { 0.0 }; // packet 1500 arrives at 15 s
    let r = simulate(25.0, late, sine, false, BLOCK, 48000, 0);
    assert!(r.underruns >= 1);
    assert_eq!(r.reprimes, 0);
    assert!(r.max_step <= 0.1, "step {}", r.max_step);
    assert!(r.concealed_ms < 30.0, "{}", r.concealed_ms);
    assert!(r.playing_at_end);
}

/// DC at block 32 / 64: a fade-in longer than a block must not step between blocks
fn double_late(block: usize) {
    let late =
        |i: i32| 40.0 + f64::from(i) * 10.0 + if i == 1500 || i == 1502 { 15.0 } else { 0.0 };
    let r = simulate(25.0, late, dc, false, block, 48000, 0);
    assert!(r.max_step <= 0.03, "block {block}: step {}", r.max_step);
    assert_eq!(r.reprimes, 0);
    assert!(r.playing_at_end);
}

#[test]
fn two_late_packets_in_a_row_do_not_click_at_block_32() {
    double_late(32);
}

#[test]
fn two_late_packets_in_a_row_do_not_click_at_block_64() {
    double_late(64);
}

#[test]
fn packets_of_2048_frames_with_5ms_jitter_at_block_256_do_not_gap() {
    let mut s = 13u32;
    let step = 2048.0 * 1000.0 / 48000.0;
    let big = move |i: i32| 100.0 + f64::from(i) * step + (rnd(&mut s) * 2.0 - 1.0) * 5.0;
    let r = simulate(60.0, big, dc, false, BLOCK, 48000, 2048);
    assert_eq!(r.gaps10, 0);
    assert!(r.underruns <= 2, "{}", r.underruns);
    assert!(r.playing_at_end);
}

#[test]
fn a_500ms_outage_reprimes_and_then_plays_again() {
    for dropped in [false, true] {
        // false: TCP delivers the backlog afterwards, true: nothing arrives meanwhile
        let outage = move |i: i32| {
            let due = 40.0 + f64::from(i) * 10.0;
            if !(10000.0..10500.0).contains(&due) {
                due
            } else if dropped {
                1.0e18
            } else {
                10500.0
            }
        };
        let r = simulate(20.0, outage, sine, true, BLOCK, 48000, 0);
        assert!(r.reprimes >= 1 && r.reprimes_counted >= 1);
        assert!(r.playing_at_end);
        // sound again within 150 ms of the end of the outage
        let (from, to) = ((10.65 * 48000.0) as usize, (10.8 * 48000.0) as usize);
        let peak = r.out[from..to.min(r.out.len())]
            .iter()
            .fold(0.0f32, |p, x| p.max(x.abs()));
        assert!(peak > 0.3, "dropped {dropped}: {peak}");
    }
}

/// one 120 ms stall at 10 s raises the target; the extra must go within seconds, not the half minute the
/// drift's 0.5 % needs: dropped in the friend's pauses, or after 3 s anyway when they never pause
fn stall_recovers(sig: fn(u64, u32) -> f32) {
    let ring = ring();
    let wp = AtomicU64::new(0);
    let mut r = FriendReader::new();
    r.prepare(48000.0, BLOCK);
    let (mut l, mut rr, mut pk) = (vec![0.0f32; BLOCK], vec![0.0f32; BLOCK], vec![0.0f32; 480]);
    let block_ms = BLOCK as f64 * 1000.0 / 48000.0;
    let arrive = |i: i32| {
        if (1000..1012).contains(&i) {
            10120.0
        } else {
            f64::from(i) * 10.0 + 0.3 * f64::from(i % 3)
        }
    };
    let (mut next, mut reprimes) = (0i32, 0);
    let mut was_primed = false;
    let (mut before, mut after, mut peak) = (0.0f64, 0.0f64, 0.0f64);
    for k in 0..(26000.0 / block_ms) as i32 {
        let now = f64::from(k) * block_ms;
        while arrive(next) <= now {
            for (i, x) in pk.iter_mut().enumerate() {
                *x = sig(next as u64 * 480 + i as u64, 48000);
            }
            ring_write(&ring, &wp, &[&pk], 480);
            next += 1;
        }
        r.pull(&ring, &wp, 48000, [&mut l, &mut rr], BLOCK);
        if k > 0 && was_primed && !r.primed() {
            reprimes += 1;
        }
        was_primed = r.primed();
        if now < 9500.0 {
            before = r.buffer_ms();
        }
        if now > 10500.0 {
            peak = peak.max(r.buffer_ms());
        }
        after = r.buffer_ms();
    }
    println!(
        "buffer {before:.1} before, {peak:.1} at most after, {after:.1} later, reprimes {reprimes}"
    );
    assert!(peak > before + 20.0); // the stall did raise it
    assert!(after < before + 8.0, "{after} vs {before}"); // and it came back down
    assert!(reprimes <= 1);
}

fn phrases(f: u64, rate: u32) -> f32 {
    // 400 ms sung, 200 ms pause
    if (f / u64::from(rate / 10)) % 6 < 4 {
        sine(f, rate)
    } else {
        0.0
    }
}

#[test]
fn after_a_120ms_stall_the_extra_buffer_is_dropped_in_the_friends_pauses() {
    stall_recovers(phrases);
}

#[test]
fn after_a_120ms_stall_with_no_pauses_at_all_the_extra_still_goes() {
    stall_recovers(sine);
}
