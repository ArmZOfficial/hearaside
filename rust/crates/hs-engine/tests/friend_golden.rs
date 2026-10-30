//! FriendReader against the C++ captures: `compat/golden/core/friend/*` (LCG jitter, a 120 ms stall at
//! 44.1 kHz, a 500 ms outage at 48 kHz) and `compat/golden/netsim/friend/*` (arrivals recorded through the
//! real net-sim TCP proxy). Scenarios re-stated from `tools/compat-golden/main.cpp::runFriend`.
use hs_bus::protocol::ChannelRing;
use hs_bus::ring::ring_write;
use hs_engine::friend_reader::FriendReader;
use std::alloc::{Layout, alloc_zeroed};
use std::path::PathBuf;
use std::sync::atomic::AtomicU64;

const RATE: u32 = 48000;
const BLOCK: usize = 256;
const SEED: u32 = 0x1357_9BDF;
const PI: f64 = std::f64::consts::PI;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../compat/golden")
}

fn golden(rel: &str) -> Vec<f32> {
    let p = root().join(rel);
    let b = std::fs::read(&p).unwrap_or_else(|e| panic!("{p:?}: {e}"));
    b.as_chunks::<4>()
        .0
        .iter()
        .map(|c| f32::from_le_bytes(*c))
        .collect()
}

fn assert_close(name: &str, got: &[f32], want: &[f32], tol: f32) {
    assert_eq!(got.len(), want.len(), "{name}: length");
    let (mut worst, mut at) = (0.0f32, 0);
    for (i, (a, b)) in got.iter().zip(want).enumerate() {
        let d = (a - b).abs();
        if d > worst || d.is_nan() {
            (worst, at) = (if d.is_nan() { f32::INFINITY } else { d }, i);
        }
    }
    assert!(
        worst <= tol,
        "{name}: max error {worst:e} at {at} (got {}, want {})",
        got[at],
        want[at]
    );
}

fn random_sample(state: &mut u32) -> f32 {
    *state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
    (*state >> 8) as f32 / 8_388_608.0 - 1.0
}

fn sine(frame: u64, rate: u32) -> f32 {
    (2.0 * PI * 440.0 * (frame % u64::from(rate)) as f64 / f64::from(rate)).sin() as f32
}

struct Run {
    captured: Vec<f32>,
    trace: Vec<f32>,
    reprimes: u32,
    end_audible: bool,
}

/// Packets of 10 ms of 400 ms singing / 200 ms pause arrive at `arrival(index)` ms; three 0.5 s output
/// windows are kept (48 kHz output-frame coordinates).
fn run_friend(
    source_rate: u32,
    blocks: usize,
    mut arrival: impl FnMut(i32) -> f64,
    windows: [u64; 3],
    audible_from: u64,
) -> Run {
    // SAFETY: floats; all-zero is valid.
    let ring: Box<ChannelRing> =
        unsafe { Box::from_raw(alloc_zeroed(Layout::new::<ChannelRing>()).cast()) };
    let write = AtomicU64::new(0);
    let mut reader = FriendReader::new();
    reader.prepare(f64::from(RATE), BLOCK);
    let packet_frames = (source_rate / 100) as usize;
    let mut packet = vec![0.0f32; packet_frames];
    let (mut l, mut r) = (vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]);
    let mut run = Run {
        captured: vec![],
        trace: vec![],
        reprimes: 0,
        end_audible: false,
    };
    let mut next = 0i32;
    let mut cached = arrival(0);
    let mut last_arrival = 0.0f64;
    for block in 0..blocks {
        let now = (block * BLOCK) as f64 * 1000.0 / f64::from(RATE);
        while last_arrival.max(cached) <= now {
            for (i, x) in packet.iter_mut().enumerate() {
                let frame = next as u64 * packet_frames as u64 + i as u64;
                *x = if (frame / u64::from(source_rate / 10)) % 6 < 4 {
                    0.5 * sine(frame, source_rate)
                } else {
                    0.0
                };
            }
            ring_write(&ring, &write, &[&packet], packet_frames as u32);
            last_arrival = last_arrival.max(cached);
            next += 1;
            cached = arrival(next);
        }
        reader.pull(&ring, &write, source_rate, [&mut l, &mut r], BLOCK);
        let start = (block * BLOCK) as u64;
        for i in 0..BLOCK {
            let frame = start + i as u64;
            if windows.iter().any(|&w| frame >= w && frame < w + 24000) {
                run.captured.extend([l[i], r[i]]);
            }
            if frame >= audible_from && l[i].abs() > 0.1 {
                run.end_audible = true;
            }
        }
        run.trace.extend([
            reader.buffer_ms() as f32,
            reader.jitter_p95_ms() as f32,
            reader.underruns() as f32,
            reader.reprimes() as f32,
            reader.concealed_ms() as f32,
            if reader.primed() { 1.0 } else { 0.0 },
        ]);
    }
    run.reprimes = reader.reprimes();
    run
}

fn friend_golden(source_rate: u32, outage: bool) {
    let mut seed = SEED.wrapping_add(source_rate);
    let arrival = |index: i32| {
        let due = 40.0 + f64::from(index) * (10.0 / (1.0 + 200.0e-6));
        let jitter = f64::from(random_sample(&mut seed)) * 5.0; // drawn for every packet, as the C++ does
        if outage && (10000.0..10500.0).contains(&due) {
            return 10500.0;
        }
        if !outage && (10000.0..10120.0).contains(&due) {
            return 10120.0;
        }
        due + jitter
    };
    let run = run_friend(
        source_rate,
        4875,
        arrival,
        [0, 480_000, 1_200_000],
        1_200_000,
    );
    assert!(run.reprimes >= 1, "the scenario did not exercise a reprime");
    assert!(run.end_audible);
    let name = if outage {
        "outage_48000"
    } else {
        "stall_44100"
    };
    assert_close(
        &format!("{name} status"),
        &run.trace,
        &golden(&format!("core/friend/{name}_status.f32")),
        1e-4,
    );
    assert_close(
        name,
        &run.captured,
        &golden(&format!("core/friend/{name}.f32")),
        1e-6,
    );
}

#[test]
fn stall_44k1_matches_golden() {
    friend_golden(44100, false);
}

#[test]
fn outage_48k_matches_golden() {
    friend_golden(48000, true);
}

fn netsim(name: &str) {
    let text = std::fs::read_to_string(root().join(format!("netsim/traces/{name}.trace"))).unwrap();
    let arrival_ms: Vec<f64> = text
        .lines()
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .map(|l| l.split_whitespace().nth(1).unwrap().parse::<f64>().unwrap() / 1000.0)
        .collect();
    assert_eq!(arrival_ms.len(), 2000);
    let arrival = |i: i32| {
        arrival_ms
            .get(i as usize)
            .map_or(f64::INFINITY, |a| 40.0 + a)
    };
    let run = run_friend(RATE, 3937, arrival, [0, 456_000, 912_000], 912_000);
    assert!(run.end_audible);
    assert_close(
        &format!("{name} status"),
        &run.trace,
        &golden(&format!("netsim/friend/{name}_status.f32")),
        1e-4,
    );
    assert_close(
        name,
        &run.captured,
        &golden(&format!("netsim/friend/{name}.f32")),
        1e-6,
    );
}

#[test]
fn netsim_good_matches_golden() {
    netsim("good");
}

#[test]
fn netsim_home_matches_golden() {
    netsim("home");
}

#[test]
fn netsim_mobile_matches_golden() {
    netsim("mobile");
}
