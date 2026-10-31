//! `PreflightChecker`: the port of `karaoke: PreflightChecker acoustic leak detector (closed headphones, open
//! headphones, speakers)` from `tests/unit/test_karaoke.cpp`, plus the audio / UI hand-over the C++ did not test.
use hs_karaoke::preflight::{LeakStatus, chirp, evaluate, preflight};

const SR: f64 = 48000.0;
const CHIRP_LEN: usize = 2400; // 50 ms
const TOTAL: usize = 12000; // 250 ms

/// The test's own chirp, as the C++ test builds it (not the checker's), so `chirp()` is checked too.
fn reference() -> Vec<f32> {
    let (f0, f1, t) = (500.0, 2500.0, CHIRP_LEN as f64 / SR);
    let k = (f1 - f0) / t;
    let two_pi = 2.0 * std::f64::consts::PI;
    (0..CHIRP_LEN)
        .map(|i| {
            let t = i as f64 / SR;
            let phase = two_pi * (f0 * t + 0.5 * k * t * t);
            let win = 0.5 * (1.0 - (two_pi * i as f64 / CHIRP_LEN as f64).cos());
            (0.125 * win * phase.sin()) as f32
        })
        .collect()
}

#[test]
fn karaoke_preflight_checker_acoustic_leak_detector() {
    let chirp_ref = reference();
    assert_eq!(chirp(SR), chirp_ref);

    // 1. closed headphones: -55 dB room noise, no chirp
    let mut seed = 12345u32;
    let closed: Vec<f32> = (0..TOTAL)
        .map(|_| {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            0.0018 * (((seed >> 16) as i32 - 32768) as f32 / 32768.0)
        })
        .collect();
    let res = evaluate(&closed, &chirp_ref, 35.0, 4.0, false);
    assert_eq!(res.leak_status, LeakStatus::HeadphonesClean);
    assert!(res.correlation_peak < 0.08);
    assert!(res.is_ready);
    assert_eq!(res.summary, "พร้อมร้อง ✓");

    // 2. open-back headphones: chirp at -42 dB, 10 ms late
    let mut open = vec![0.0f32; TOTAL];
    for i in 0..CHIRP_LEN {
        open[480 + i] = chirp_ref[i] * 0.008;
    }
    let res = evaluate(&open, &chirp_ref, 40.0, 6.0, false);
    assert_ne!(res.leak_status, LeakStatus::SpeakerLeakDetected);
    assert!(res.is_ready);
    assert_eq!(res.summary, "พร้อมร้อง ✓");

    // 3. speakers: -18 dB, 12 ms, two wall reflections
    let mut speaker = vec![0.0f32; TOTAL];
    let d = 576;
    for i in 0..CHIRP_LEN {
        speaker[d + i] += chirp_ref[i] * 0.35;
        if d + i + 144 < TOTAL {
            speaker[d + i + 144] -= chirp_ref[i] * 0.12;
        }
        if d + i + 384 < TOTAL {
            speaker[d + i + 384] += chirp_ref[i] * 0.06;
        }
    }
    let no_aec = evaluate(&speaker, &chirp_ref, 40.0, 6.0, false);
    assert_eq!(no_aec.leak_status, LeakStatus::SpeakerLeakDetected);
    assert!(no_aec.correlation_peak >= 0.20);
    assert!(!no_aec.is_ready);
    assert!(no_aec.advice.contains("AEC"));

    let with_aec = evaluate(&speaker, &chirp_ref, 40.0, 6.0, true);
    assert_eq!(with_aec.leak_status, LeakStatus::SpeakerLeakDetected);
    assert!(with_aec.is_ready);
    assert_eq!(with_aec.summary, "พร้อมร้อง ✓");
}

#[test]
fn slow_network_and_silent_mic_are_issues() {
    let chirp_ref = reference();
    let res = evaluate(&vec![0.0; TOTAL], &chirp_ref, 150.4, 5.0, false);
    assert!(!res.mic_detected && !res.is_ready);
    assert_eq!(res.summary, "ต้องปรับตั้งค่าก่อนร้อง");
    assert!(res.advice.contains("(RTT 150 ms)"), "{}", res.advice);
    assert!(res.advice.contains(" · "));
    // C++ read past the end of a recording shorter than the chirp (lag 0 always ran)
    let short = evaluate(&[0.1; 100], &chirp_ref, -1.0, -1.0, false);
    assert_eq!(short.correlation_peak, 0.0);
    assert_eq!(
        evaluate(&[], &chirp_ref, -1.0, -1.0, false).leak_status,
        LeakStatus::Untested
    );
}

/// Runs the audio half with `mic` fed back from its own output (`leak` times the chirp, `delay` frames late).
fn run_check(leak: f32, delay: usize, block: usize) -> hs_karaoke::preflight::PreflightResult {
    let (mut ui, mut audio) = preflight(SR);
    ui.start_check(-1.0, -1.0, false);
    assert!(ui.is_testing());
    let mut played: Vec<f32> = Vec::new();
    let (mut l, mut r) = (vec![0.0f32; block], vec![0.0f32; block]);
    let mut blocks = 0;
    while ui.is_testing() {
        let start = played.len();
        let mic: Vec<f32> = (start..start + block)
            .map(|i| {
                i.checked_sub(delay)
                    .and_then(|j| played.get(j))
                    .copied()
                    .unwrap_or(0.0)
                    * leak
            })
            .collect();
        l.fill(0.0);
        r.fill(0.0);
        audio.process(Some(&mic), Some(&mut l), Some(&mut r), block);
        played.extend_from_slice(&l);
        blocks += 1;
        assert!(blocks < 1000);
    }
    assert!(ui.poll());
    assert!(!ui.poll(), "one result per check");
    ui.result().clone()
}

#[test]
fn audio_half_plays_the_chirp_and_records_the_mic() {
    let speakers = run_check(0.35, 576, 256);
    assert_eq!(speakers.leak_status, LeakStatus::SpeakerLeakDetected);
    let clean = run_check(0.0, 0, 480);
    assert_eq!(clean.leak_status, LeakStatus::HeadphonesClean);
    assert!(!clean.mic_detected);
}

#[test]
fn restart_and_cancel_during_a_check() {
    let (mut ui, mut audio) = preflight(SR);
    let mic = vec![0.2f32; 256];
    let mut l = vec![0.0f32; 256];
    ui.start_check(-1.0, -1.0, false);
    assert_eq!(ui.result().leak_status, LeakStatus::Testing);
    for _ in 0..20 {
        audio.process(Some(&mic), Some(&mut l), None, 256);
    }
    // restarting mid-way rewinds: the chirp plays again from its start
    ui.start_check(-1.0, -1.0, false);
    l.fill(0.0);
    audio.process(Some(&mic), Some(&mut l), None, 256);
    assert_eq!(l[..256], ui.chirp()[..256]);

    ui.cancel();
    assert!(!ui.is_testing());
    for _ in 0..60 {
        l.fill(0.0);
        audio.process(Some(&mic), Some(&mut l), None, 256);
        assert!(l.iter().all(|v| *v == 0.0), "cancelled: no chirp");
    }
    assert!(!ui.poll(), "a cancelled check leaves no result");
    assert_eq!(ui.result().leak_status, LeakStatus::Testing);
}
