//! Sample conversions of HAS Direct. Port of `karaoke: ASIO host sample formats round-trip through float`
//! (tests/unit/test_karaoke.cpp) plus golden vectors made by the C++ itself (`tests/golden/asio_convert.golden`,
//! regenerate with `tests/golden/run_golden.ps1`; the inputs below must stay in step with gen_asio_convert.cpp).

use hs_win::asio::*;

const ALL: [i32; 9] = [
    INT16_LSB,
    INT24_LSB,
    INT32_LSB,
    FLOAT32_LSB,
    FLOAT64_LSB,
    INT32_LSB16,
    INT32_LSB18,
    INT32_LSB20,
    INT32_LSB24,
];

#[test]
fn asio_host_sample_formats_round_trip_through_float() {
    let input = [0.0f32, 0.5, -0.5, 0.999, -1.0, 1.5]; // 1.5 must clip, not wrap
    for ty in ALL {
        let bytes = bytes_per_sample(ty);
        assert!(bytes > 0);
        let mut raw = vec![0u8; bytes * 6];
        let mut back = [0.0f32; 6];
        assert!(from_float(ty, &input, &mut raw));
        assert!(to_float(ty, &raw, &mut back));
        let tol = if bit_depth(ty) <= 16 { 1.0e-4 } else { 1.0e-5 };
        for i in 0..5 {
            assert!(
                (back[i] - input[i]).abs() < tol,
                "type {ty} sample {i}: {} vs {}",
                back[i],
                input[i]
            );
        }
        assert!(
            back[5] > 0.99 && back[5] <= 1.0,
            "type {ty}: the over came back as {}",
            back[5]
        );
    }
    // 24-bit packed: a negative sample keeps its sign
    let neg = [0x00u8, 0x00, 0x80]; // -2^23 = -1.0
    let mut v = [0.0f32];
    assert!(to_float(INT24_LSB, &neg, &mut v));
    assert!((v[0] + 1.0).abs() < 1.0e-6);
    assert_eq!(bytes_per_sample(3), 0); // big-endian / unknown types are refused
    let mut dummy = [0.0f32];
    assert!(!to_float(3, &neg, &mut dummy));
}

#[test]
fn sizes_and_depths_match_the_cpp_tables() {
    let table = [
        (INT16_LSB, 2, 16),
        (INT24_LSB, 3, 24),
        (INT32_LSB, 4, 32),
        (FLOAT32_LSB, 4, 32),
        (FLOAT64_LSB, 8, 64),
        (INT32_LSB16, 4, 16),
        (INT32_LSB18, 4, 18),
        (INT32_LSB20, 4, 20),
        (INT32_LSB24, 4, 24),
    ];
    for (ty, bytes, depth) in table {
        assert_eq!(
            (bytes_per_sample(ty), bit_depth(ty)),
            (bytes, depth),
            "type {ty}"
        );
    }
    // ASIO type numbers this host does not take: Int16MSB 0, Int24MSB 1, Int32MSB 2, Float32MSB 3, Float64MSB 4, DSD 32+, junk
    for ty in [
        0, 1, 2, 3, 4, 8, 9, 10, 11, 21, 22, 23, 28, 32, 33, 34, -1, 1000,
    ] {
        assert_eq!((bytes_per_sample(ty), bit_depth(ty)), (0, 0), "type {ty}");
    }
}

#[test]
fn known_values() {
    let enc = |ty: i32, x: f32| {
        let mut b = vec![0u8; bytes_per_sample(ty)];
        assert!(from_float(ty, &[x], &mut b));
        b
    };
    // scale is full scale - 1 going out (+1.0 is the largest value, never wraps), full scale coming in
    assert_eq!(enc(INT16_LSB, 1.0), 32767i16.to_le_bytes());
    assert_eq!(enc(INT16_LSB, -1.0), (-32767i16).to_le_bytes());
    assert_eq!(enc(INT16_LSB, 0.5), 16384i16.to_le_bytes()); // 16383.5 rounds away from zero
    assert_eq!(enc(INT16_LSB, -0.5), (-16384i16).to_le_bytes());
    assert_eq!(enc(INT24_LSB, 1.0), [0xff, 0xff, 0x7f]);
    assert_eq!(enc(INT24_LSB, -1.0), [0x01, 0x00, 0x80]);
    assert_eq!(enc(INT32_LSB, 1.0), i32::MAX.to_le_bytes());
    assert_eq!(enc(INT32_LSB, -1.0), (-i32::MAX).to_le_bytes());
    assert_eq!(enc(INT32_LSB16, 1.0), 32767i32.to_le_bytes()); // 16 bits in the low half of the container
    assert_eq!(enc(INT32_LSB24, -1.0), (-8_388_607i32).to_le_bytes());
    assert_eq!(enc(FLOAT32_LSB, 2.5), 1.0f32.to_le_bytes());
    assert_eq!(enc(FLOAT64_LSB, -7.0), (-1.0f64).to_le_bytes());
    // inf clips like any over
    assert_eq!(enc(INT16_LSB, f32::INFINITY), 32767i16.to_le_bytes());
    assert_eq!(enc(INT16_LSB, f32::NEG_INFINITY), (-32767i16).to_le_bytes());

    let mut v = [0.0f32];
    assert!(to_float(INT16_LSB, &i16::MIN.to_le_bytes(), &mut v));
    assert_eq!(v[0], -1.0);
    assert!(to_float(INT32_LSB, &i32::MIN.to_le_bytes(), &mut v));
    assert_eq!(v[0], -1.0);
    assert!(to_float(
        INT32_LSB24,
        &(-8_388_608i32).to_le_bytes(),
        &mut v
    ));
    assert_eq!(v[0], -1.0);
    assert!(to_float(FLOAT64_LSB, &0.25f64.to_le_bytes(), &mut v));
    assert_eq!(v[0], 0.25);
    // a float32 buffer is copied bit for bit, NaN payload included
    let nan = f32::from_bits(0x7fc0_1234);
    assert!(to_float(FLOAT32_LSB, &nan.to_le_bytes(), &mut v));
    assert_eq!(v[0].to_bits(), 0x7fc0_1234);
}

/// C++ bug, fixed in the port: `fmin(fmax(NaN, -1), 1)` is -1, so a NaN went to the interface as full-scale negative.
#[test]
fn nan_is_silence_not_full_scale_negative() {
    for ty in ALL {
        let mut raw = vec![0xAAu8; bytes_per_sample(ty)];
        assert!(from_float(ty, &[f32::NAN], &mut raw));
        let mut back = [1.0f32];
        assert!(to_float(ty, &raw, &mut back));
        assert_eq!(back[0], 0.0, "type {ty}");
    }
}

#[test]
fn short_buffers_are_refused_and_left_alone() {
    for ty in ALL {
        let bytes = bytes_per_sample(ty);
        let mut dst = vec![7.0f32; 4];
        assert!(!to_float(ty, &vec![0u8; bytes * 4 - 1], &mut dst));
        assert_eq!(dst, [7.0; 4]);
        let mut raw = vec![0x55u8; bytes * 4 - 1];
        assert!(!from_float(ty, &[0.5; 4], &mut raw));
        assert!(raw.iter().all(|b| *b == 0x55));
        // a longer buffer is fine: only `n` samples are touched
        let mut long = vec![0x55u8; bytes * 5];
        assert!(from_float(ty, &[0.5; 4], &mut long));
        assert!(long[bytes * 4..].iter().all(|b| *b == 0x55));
    }
    assert!(!to_float(99, &[0; 8], &mut [0.0; 1]));
    assert!(!from_float(99, &[0.0; 1], &mut [0; 8]));
    assert!(to_float(INT16_LSB, &[], &mut [])); // n = 0 is a success, as in the C++
}

// --- golden: output of the C++ conversions ---------------------------------------------------------------------

struct Lcg(u64);

impl Lcg {
    fn u32(&mut self) -> u32 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (self.0 >> 32) as u32
    }
    fn unit(&mut self) -> f64 {
        f64::from(self.u32()) / 4294967296.0
    }
}

const RAW: usize = 512;

fn float_inputs(rng: &mut Lcg) -> Vec<f32> {
    let mut f = vec![
        0.0f32,
        -0.0,
        1.0,
        -1.0,
        0.5,
        -0.5,
        0.999,
        1.5,
        -1.5,
        2.0,
        1.0e-9,
        -1.0e-9,
        0.999_999_9,
        -0.999_999_9,
        3.4e38,
        -3.4e38,
        f32::INFINITY,
        f32::NEG_INFINITY,
    ];
    for bits in [16, 18, 20, 24] {
        let fs = f64::from(1i32 << (bits - 1));
        for k in [0, 1, 2, 3, -1, -2, 100, -100, 12345, -12345] {
            for d in [-0.5, 0.0, 0.5, 0.25, -0.25, 0.4999, 0.5001] {
                f.push(((f64::from(k) + d) / (fs - 1.0)) as f32);
            }
        }
    }
    for _ in 0..1024 {
        f.push((rng.unit() * 2.4 - 1.2) as f32);
    }
    f
}

fn raw_inputs(rng: &mut Lcg, ty: i32) -> Vec<u8> {
    let mut r = vec![0u8; bytes_per_sample(ty) * RAW];
    match ty {
        FLOAT32_LSB => {
            for i in 0..RAW {
                let v = ((rng.unit() * 2.0 - 1.0) * if i % 7 == 0 { 3.0e30 } else { 1.3 }) as f32;
                r[4 * i..4 * i + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        FLOAT64_LSB => {
            for i in 0..RAW {
                let v = (rng.unit() * 2.0 - 1.0)
                    * if i % 7 == 0 { 1.0e300 } else { 1.3 }
                    * if i % 11 == 0 { 1.0e-3 } else { 1.0 };
                r[8 * i..8 * i + 8].copy_from_slice(&v.to_le_bytes());
            }
        }
        _ => r.iter_mut().for_each(|b| *b = (rng.u32() >> 24) as u8),
    }
    r
}

#[test]
fn conversions_match_the_cpp_bit_for_bit() {
    let golden = include_bytes!("golden/asio_convert.golden");
    let mut at = 0usize;
    let mut take = |n: usize| {
        let s = &golden[at..at + n];
        at += n;
        s
    };
    let mut rng = Lcg(0x1234_5678_8765_4321);
    let input = float_inputs(&mut rng);
    for ty in ALL {
        let n = u32::from_le_bytes(take(4).try_into().unwrap()) as usize;
        let expect_enc = take(n).to_vec();
        let mut enc = vec![0u8; bytes_per_sample(ty) * input.len()];
        assert!(from_float(ty, &input, &mut enc));
        assert_eq!(enc.len(), expect_enc.len());
        if let Some(i) = (0..enc.len()).find(|i| enc[*i] != expect_enc[*i]) {
            let s = i / bytes_per_sample(ty);
            panic!(
                "type {ty} from_float: sample {s} (input {:e}) differs from the C++",
                input[s]
            );
        }

        let raw = raw_inputs(&mut rng, ty);
        let n = u32::from_le_bytes(take(4).try_into().unwrap()) as usize;
        assert_eq!(n, RAW);
        let expect_dec: Vec<u32> = take(4 * n)
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| u32::from_le_bytes(*c))
            .collect();
        let mut dec = vec![0.0f32; RAW];
        assert!(to_float(ty, &raw, &mut dec));
        for (i, (d, e)) in dec.iter().zip(&expect_dec).enumerate() {
            assert_eq!(
                d.to_bits(),
                *e,
                "type {ty} to_float: sample {i}: {d:e} vs {:e}",
                f32::from_bits(*e)
            );
        }
    }
    assert_eq!(at, golden.len(), "unread golden data");
}
