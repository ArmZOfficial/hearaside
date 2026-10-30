//! Port of "resampler: ratio control and steady output length" from `tests/unit/test_dsp.cpp`.
use hs_dsp::resampler::VarResampler;

#[test]
fn ratio_control_and_steady_output_length() {
    let mut rs = VarResampler::new(44100, 48000, 5).unwrap();
    let input: Vec<f32> = (0..4096).map(|i| (i as f32 * 0.01).sin()).collect();
    let (mut a, mut b) = (vec![0.0f32; 480], vec![0.0f32; 480]);
    let mut run = |rs: &mut VarResampler, blocks: u32| -> f64 {
        let mut consumed = 0u64;
        for _ in 0..blocks {
            let mut in_n = rs.input_for(480);
            let mut out_n = 480;
            rs.process([&input, &input], &mut in_n, [&mut a, &mut b], &mut out_n);
            assert_eq!(out_n, 480);
            consumed += u64::from(in_n);
        }
        consumed as f64 / (f64::from(blocks) * 480.0)
    };
    assert!((run(&mut rs, 200) - 44100.0 / 48000.0).abs() < 0.002);
    rs.set_correction_ppm(5000);
    assert!((run(&mut rs, 400) - 44100.0 / 48000.0 * 1.005).abs() < 0.001);
}

#[test]
fn short_slices_are_never_overrun() {
    let mut rs = VarResampler::new(48000, 48000, 5).unwrap();
    let input = vec![0.1f32; 16];
    let (mut a, mut b) = (vec![0.0f32; 8], vec![0.0f32; 8]);
    let (mut i, mut o) = (1000, 1000); // far more than the slices hold
    rs.process([&input, &input], &mut i, [&mut a, &mut b], &mut o);
    assert!(i <= 16 && o <= 8);
}
