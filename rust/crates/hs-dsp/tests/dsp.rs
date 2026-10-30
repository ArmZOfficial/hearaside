//! Port of the non-resampler cases of `tests/unit/test_dsp.cpp`.
use hs_dsp::dsp::*;

/// Small deterministic uniform generator (the C++ test uses mt19937; only the distribution matters).
struct Rng(u64);
impl Rng {
    fn unit(&mut self) -> f64 {
        self.0 = self
            .0
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        (self.0 >> 11) as f64 / (1u64 << 53) as f64
    }
    fn range(&mut self, lo: f64, hi: f64) -> f64 {
        lo + (hi - lo) * self.unit()
    }
}

#[test]
fn smoother_ramps_linearly_without_overshoot() {
    let mut s = Smoother::default();
    s.prepare(48000.0, 10.0); // 480 samples
    s.snap(0.0);
    s.set_target(1.0);
    let (mut prev, mut max_step) = (0.0f32, 0.0f32);
    for _ in 0..480 {
        let v = s.next_value();
        max_step = max_step.max(v - prev);
        assert!(v >= prev && v <= 1.0);
        prev = v;
    }
    assert_eq!(prev, 1.0);
    assert!((f64::from(max_step) - 1.0 / 480.0).abs() < 1e-5);
    assert!(!s.is_smoothing());
}

#[test]
fn smoother_apply_block() {
    let mut s = Smoother::default();
    s.prepare(48000.0, 1.0);
    s.snap(1.0);
    let mut a = vec![1.0f32; 8];
    assert!(s.apply(&mut [&mut a], 8)); // unity: untouched
    assert_eq!(a, vec![1.0; 8]);
    s.snap(0.0);
    assert!(!s.apply(&mut [&mut a], 8)); // exact zero gain
    assert_eq!(a, vec![0.0; 8]);
}

#[test]
fn equal_power_pan() {
    assert_eq!(pan_gains(0.0), (1.0, 1.0));
    let (l, r) = pan_gains(1.0);
    assert!(l.abs() < 1e-6 && (f64::from(r) - std::f64::consts::SQRT_2).abs() < 1e-5);
    let (_, r) = pan_gains(-1.0);
    assert!(r.abs() < 1e-6);
    let (l, r) = pan_gains(0.5);
    assert!((f64::from(l * l + r * r) - 2.0).abs() < 1e-5);
    assert_eq!(pan_gains(5.0), pan_gains(1.0)); // clamped
}

#[test]
fn limiter_never_exceeds_ceiling_and_is_transparent_below_it() {
    let mut lim = Limiter::default();
    lim.prepare(48000.0, 1.0, 100.0);
    let ceiling = db_to_gain(-1.0);
    lim.set_ceiling(ceiling);
    let mut rng = Rng(1);
    let (mut l, mut r) = (vec![0.0f32; 512], vec![0.0f32; 512]);
    let mut max_out = 0.0f32;
    for b in 0..400 {
        for i in 0..512 {
            l[i] = rng.range(-4.0, 4.0) as f32 * if b % 7 == 0 { 1.0 } else { 0.3 };
            r[i] = rng.range(-4.0, 4.0) as f32 * 0.5;
        }
        lim.process(&mut l, &mut r, 512, true);
        max_out = max_out.max(block_peak(&l)).max(block_peak(&r));
    }
    assert!(max_out <= ceiling + 1e-6);

    // quiet signal passes unchanged (just delayed by the lookahead)
    let mut q = Limiter::default();
    q.prepare(48000.0, 1.0, 100.0);
    q.set_ceiling(ceiling);
    let input: Vec<f32> = (0..2048).map(|i| 0.5 * (i as f32 * 0.05).sin()).collect();
    let (mut ql, mut qr) = (input.clone(), input.clone());
    q.process(&mut ql, &mut qr, 2048, true);
    let la = q.latency() as usize;
    let err = (la..2048)
        .map(|i| (ql[i] - input[i - la]).abs())
        .fold(0.0f32, f32::max);
    assert!(err < 1e-6, "err {err}");
}

#[test]
fn loudness_of_1khz_sine_at_0dbfs_in_both_channels_is_about_0_lufs() {
    let mut m = LoudnessMeter::default();
    m.prepare(48000.0);
    let x: Vec<f32> = (0..48000 * 4)
        .map(|i| (2.0 * std::f32::consts::PI * 1000.0 * i as f32 / 48000.0).sin())
        .collect();
    m.process(&x, &x, x.len());
    // BS.1770: a 0 dBFS 1 kHz sine in one channel reads -3.01 LUFS, so two channels read ~0.
    assert!(f64::from(m.momentary()).abs() < 0.1, "{}", m.momentary());
    assert!(f64::from(m.short_term()).abs() < 0.1, "{}", m.short_term());
}

#[test]
fn loudness_at_44k1_minus_20dbfs_stereo_sine() {
    let mut m = LoudnessMeter::default();
    m.prepare(44100.0);
    let x: Vec<f32> = (0..44100 * 4)
        .map(|i| 0.1 * (2.0 * std::f32::consts::PI * 1000.0 * i as f32 / 44100.0).sin())
        .collect();
    m.process(&x, &x, x.len());
    assert!(
        (f64::from(m.short_term()) + 20.0).abs() < 0.1,
        "{}",
        m.short_term()
    );
}

/// DAW (audio-interface clock, irregular blocks) -> consumer (system clock, 10 ms ticks with jitter)
/// for 4 hours at the given drift; returns (worst filtered buffer error in ms, underruns).
fn simulate_drift(ppm: f64, daw_block: u32, daw_rate: u32, out_rate: u32) -> (f64, u32) {
    let mut ctl = DriftController::default();
    // same minimum as StreamConsumer::resync(): large host blocks need room for the sawtooth
    let target =
        (0.030 * f64::from(daw_rate)).max(1.5 * f64::from(daw_block) + 0.012 * f64::from(daw_rate));
    ctl.reset(target);
    let mut fill = target;
    let mut rng = Rng(42);
    let producer_period = f64::from(daw_block) / (f64::from(daw_rate) * (1.0 + ppm * 1e-6));
    let (mut t_prod, mut t_cons, mut last_cons) = (0.0f64, 0.010f64, 0.0f64);
    let (mut frac, mut filtered) = (0.0f64, target);
    let (mut worst, mut under) = (0.0f64, 0u32);
    while t_cons < 4.0 * 3600.0 {
        if t_prod <= t_cons {
            fill += f64::from(daw_block);
            t_prod += producer_period;
            continue;
        }
        let now = t_cons + rng.range(-0.002, 0.002) * 0.25;
        let dt = now - last_cons;
        last_cons = now;
        let c = ctl.update(fill, dt.max(1e-4));
        let ppm_q = (c * 1e6).round();
        let ratio = f64::from(daw_rate) / f64::from(out_rate) * (1.0 + ppm_q * 1e-6);
        let want = 480.0 * f64::from(out_rate) / 48000.0 * ratio + frac;
        let take = want.floor();
        frac = want - take;
        filtered += (fill - filtered) * (1.0 - (-dt / 2.0).exp());
        if fill < take {
            under += 1;
        }
        fill -= take;
        if t_cons > 120.0 {
            worst = worst.max((filtered - target).abs() * 1000.0 / f64::from(daw_rate));
        }
        t_cons += 0.010;
    }
    (worst, under)
}

#[test]
fn drift_pi_controller_holds_buffer_within_2ms_over_4h_plus_200ppm_256_blocks() {
    let (err, under) = simulate_drift(200.0, 256, 48000, 48000);
    assert!(err < 2.0 + 256.0 / 48.0 / 2.0, "err {err}"); // filtered fill still holds half a block of sawtooth
    assert_eq!(under, 0);
}

#[test]
fn drift_minus_200ppm_44k1_to_48k_1024_blocks() {
    let (err, under) = simulate_drift(-200.0, 1024, 44100, 48000);
    assert!(err < 2.0 + 1024.0 / 44.1 / 2.0, "err {err}");
    assert_eq!(under, 0);
}

#[test]
fn vector_operations_match_scalar_reference_across_odd_lengths() {
    for n in [1usize, 3, 4, 7, 16, 19, 64, 255, 256] {
        let a: Vec<f32> = (0..n).map(|i| i as f32 * 1.5 - 10.0).collect();
        let b: Vec<f32> = (0..n).map(|i| (i * 3 + 2) as f32 * 0.25).collect();
        let scale = 0.5f32;
        let close = |x: &[f32], y: Vec<f32>| {
            assert!(x.iter().zip(&y).all(|(p, q)| (p - q).abs() < 1e-6));
        };
        let mut t = a.clone();
        vector_add(&mut t, &b);
        close(&t, a.iter().zip(&b).map(|(p, q)| p + q).collect());
        let mut t = a.clone();
        vector_add_scaled(&mut t, &b, scale);
        close(&t, a.iter().zip(&b).map(|(p, q)| p + q * scale).collect());
        let mut t = a.clone();
        vector_multiply(&mut t, &b);
        close(&t, a.iter().zip(&b).map(|(p, q)| p * q).collect());
        let mut t = a.clone();
        vector_scale(&mut t, scale);
        close(&t, a.iter().map(|p| p * scale).collect());
        let mut t = vec![0.0; n];
        vector_copy(&mut t, &a);
        assert_eq!(t, a);
        vector_zero(&mut t);
        assert!(t.iter().all(|&x| x == 0.0));
    }
}

#[test]
fn db_conversions() {
    assert_eq!(db_to_gain(-60.0), 0.0);
    assert!((db_to_gain(0.0) - 1.0).abs() < 1e-7);
    assert!((gain_to_db(db_to_gain(-6.0)) + 6.0).abs() < 1e-4);
    assert_eq!(gain_to_db(0.0), -200.0);
}

#[test]
fn short_buffers_and_unprepared_limiter_never_panic() {
    let mut lim = Limiter::default();
    let (mut l, mut r) = (vec![0.5f32; 4], vec![0.5f32; 4]);
    lim.process(&mut l, &mut r, 4, true); // before prepare: untouched
    assert_eq!(l, vec![0.5; 4]);
    lim.prepare(48000.0, 1.0, 100.0);
    lim.process(&mut l, &mut r, 1000, true); // n larger than the slices
    let mut m = LoudnessMeter::default();
    m.prepare(48000.0);
    m.process(&l, &r, 1000);
    let mut s = Smoother::default();
    s.snap(0.5);
    let mut a = vec![1.0f32; 3];
    assert!(s.apply(&mut [&mut a], 100));
    assert_eq!(a, vec![0.5; 3]);
}
