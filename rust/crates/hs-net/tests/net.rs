//! Port of `tests/unit/test_net.cpp` (all cases except the RtcPeer loopback, which comes with WebRTC in R3.2),
//! plus the C++ bugs fixed in Rust.
#![allow(clippy::needless_range_loop)]

use hs_net::aec::{AecMode, FriendEcho, LEAK_LATCH_TICKS};
use hs_net::opus::{App, OpusDec, OpusEnc};
use hs_net::wire::*;
use std::f32::consts::PI;

#[test]
fn hra3_header_packing_and_parsing() {
    let hdr = Hra3Header {
        magic: MAGIC3,
        codec: CODEC_OPUS,
        chans: 1,
        flags: FLAG_FEC,
        seq: 1042,
        pos: 960_000,
        frames: 480,
    };
    let payload = [1u8, 2, 3, 4, 5];
    let mut packet = [0u8; HRA3_HEADER_BYTES + 5];
    assert_eq!(write_hra3(&mut packet, &hdr, &payload), Some(packet.len()));
    // the C++ packed struct, byte for byte
    assert_eq!(&packet[..4], b"HRA3");
    assert_eq!(&packet[8..12], &1042u32.to_le_bytes());

    let (parsed, body) = parse_hra3(&packet).expect("parses");
    assert_eq!(parsed, hdr);
    assert_eq!(body, &payload);

    packet[0] = b'X'; // bad magic
    assert!(parse_hra3(&packet).is_none());
    packet[0] = b'H';
    assert!(parse_hra3(&packet[..HRA3_HEADER_BYTES - 1]).is_none()); // too small
    packet[5] = 3; // three channels
    assert!(parse_hra3(&packet).is_none());
}

#[test]
fn write_hra3_refuses_a_short_buffer() {
    // C++ bug: writeHra3 wrote past the end of a buffer too small for header + payload
    let mut small = [0u8; HRA3_HEADER_BYTES + 3];
    assert_eq!(
        write_hra3(&mut small, &Hra3Header::default(), &[0; 4]),
        None
    );
    assert_eq!(small, [0u8; HRA3_HEADER_BYTES + 3]);
}

#[test]
fn hra3_seq_gap_in_order_lost_late_repeated_across_the_wrap() {
    assert_eq!(seq_gap(6, 5), 0);
    assert_eq!(seq_gap(8, 5), 2);
    assert_eq!(seq_gap(4, 5), -2);
    assert_eq!(seq_gap(5, 5), -1);
    assert_eq!(seq_gap(0, 0xFFFF_FFFF), 0);
    assert_eq!(seq_gap(1, 0xFFFF_FFFF), 1);
    assert_eq!(seq_gap(0xFFFF_FFFF, 1), -3);

    // 1,2,4,3,5 over an unordered line: 3 arrives after 4 and must not be played or reset the count
    let (mut last, mut played, mut concealed) = (1u32, 1, 0);
    for s in [2u32, 4, 3, 5] {
        let g = seq_gap(s, last);
        if g < 0 {
            continue;
        }
        concealed += g;
        played += 1;
        last = s;
    }
    assert_eq!((played, concealed, last), (4, 1, 5));
}

#[test]
fn seq_gap_half_way_round_is_late_not_two_billion_lost() {
    // C++ bug: int32_t(0x80000000) - 1 overflowed to INT32_MAX ("2^31 - 1 packets lost")
    assert!(seq_gap(0x8000_0000, 0) < 0);
    assert!(seq_gap(5u32.wrapping_add(0x8000_0000), 5) < 0);
    assert_eq!(seq_gap(0x7FFF_FFFF, 0), 0x7FFF_FFFE);
}

#[test]
fn friend_echo_auto_latches_after_two_seconds_until_reset() {
    let mut aec = FriendEcho::new();
    assert!(aec.init(48000, 480, 200));
    for _ in 0..LEAK_LATCH_TICKS - 1 {
        aec.set_leak_detected(true);
    }
    aec.set_leak_detected(false); // one quiet reading starts the count again
    for _ in 0..LEAK_LATCH_TICKS - 1 {
        aec.set_leak_detected(true);
    }
    assert!(!aec.leak_detected());
    aec.set_leak_detected(true);
    assert!(aec.leak_detected());
    aec.set_leak_detected(false); // the canceller hides the leak from the detector: stay on
    assert!(aec.leak_detected());
    aec.reset();
    assert!(!aec.leak_detected());
}

fn sine(n: usize, amp: f32, hz: f32) -> Vec<f32> {
    (0..n)
        .map(|i| amp * (2.0 * PI * hz * i as f32 / 48000.0).sin())
        .collect()
}

#[test]
fn opus_mono_encode_decode_snr() {
    let mut enc = OpusEnc::new();
    assert!(enc.init(48000, 1, App::Voip));
    enc.set_bitrate(64000);
    let mut dec = OpusDec::new();
    assert!(dec.init(48000, 1));

    const PKT: usize = 480;
    const N: usize = PKT * 20;
    let original = sine(N, 0.5, 440.0);
    let mut decoded = vec![0.0f32; N];
    let mut buf = [0u8; 1500];
    for p in 0..20 {
        let bytes = enc.encode(&[&original[p * PKT..][..PKT]], PKT, &mut buf);
        assert!(bytes > 0 && bytes < 300, "{bytes}");
        let got = dec.decode(
            Some(&buf[..bytes as usize]),
            &mut [&mut decoded[p * PKT..][..PKT]],
            PKT,
            false,
        );
        assert_eq!(got, PKT as i32);
    }

    let (mut best_lag, mut max_corr) = (0usize, -1.0f64);
    for lag in 0..240 {
        let corr: f64 = (480..N - 240)
            .map(|i| f64::from(original[i]) * f64::from(decoded[i + lag]))
            .sum();
        if corr > max_corr {
            max_corr = corr;
            best_lag = lag;
        }
    }
    assert!(best_lag > 0 && best_lag < 200, "{best_lag}");
    let (mut sig, mut noise) = (0.0f64, 0.0f64);
    for i in 480 * 4..480 * 18 {
        let s = f64::from(original[i]);
        let d = f64::from(decoded[i + best_lag]);
        sig += s * s;
        noise += (s - d) * (s - d);
    }
    assert!(noise > 0.0);
    let snr = 10.0 * (sig / noise).log10();
    println!("Opus 64 kbps mono SNR: {snr:.1} dB (lag {best_lag})");
    assert!(snr > 25.0, "{snr}");
}

#[test]
fn opus_plc_on_a_dropped_packet() {
    let mut enc = OpusEnc::new();
    assert!(enc.init(48000, 1, App::Voip));
    enc.set_bitrate(64000);
    let mut dec = OpusDec::new();
    assert!(dec.init(48000, 1));
    const F: usize = 480;
    let pcm = sine(F, 0.4, 500.0);
    let (mut p1, mut p2) = ([0u8; 1500], [0u8; 1500]);
    let b1 = enc.encode(&[&pcm], F, &mut p1);
    let b2 = enc.encode(&[&pcm], F, &mut p2);
    assert!(b1 > 0 && b2 > 0);

    let (mut out1, mut plc, mut out2) = (vec![0.0f32; F], vec![0.0f32; F], vec![0.0f32; F]);
    assert_eq!(
        dec.decode(Some(&p1[..b1 as usize]), &mut [&mut out1], F, false),
        F as i32
    );
    assert_eq!(dec.decode_plc(&mut [&mut plc], F), F as i32);
    let max = plc.iter().fold(0.0f32, |m, v| {
        assert!(v.is_finite());
        m.max(v.abs())
    });
    assert!(max > 0.01 && max < 1.0, "{max}");
    assert!((plc[0] - out1[F - 1]).abs() < 0.3);
    assert_eq!(
        dec.decode(Some(&p2[..b2 as usize]), &mut [&mut out2], F, false),
        F as i32
    );
}

#[test]
fn opus_stereo_keeps_the_channels_apart() {
    let mut enc = OpusEnc::new();
    assert!(enc.init(48000, 2, App::Audio));
    enc.set_bitrate(128_000);
    let mut dec = OpusDec::new();
    assert!(dec.init(48000, 2));
    const F: usize = 480 * 10;
    let left = sine(F, 0.5, 300.0);
    let right = vec![0.0f32; F];
    let (mut lo, mut ro) = (vec![0.0f32; F], vec![0.0f32; F]);
    let mut pkt = [0u8; 1500];
    for p in 0..10 {
        let bytes = enc.encode(
            &[&left[p * 480..][..480], &right[p * 480..][..480]],
            480,
            &mut pkt,
        );
        assert!(bytes > 0);
        let got = dec.decode(
            Some(&pkt[..bytes as usize]),
            &mut [&mut lo[p * 480..][..480], &mut ro[p * 480..][..480]],
            480,
            false,
        );
        assert_eq!(got, 480);
    }
    let el: f64 = lo[960..].iter().map(|&v| f64::from(v * v)).sum();
    let er: f64 = ro[960..].iter().map(|&v| f64::from(v * v)).sum();
    assert!(el > 10.0, "{el}");
    let isolation = 10.0 * (el / er.max(1e-9)).log10();
    println!("Opus stereo separation: {isolation:.1} dB");
    assert!(isolation > 20.0, "{isolation}");
}

#[test]
fn opus_refuses_short_slices_and_overlong_frames() {
    // C++ bugs: encode/decode read and wrote `frames` samples whatever the buffers held, and grew their scratch
    // buffer on the audio thread for any frame count
    let mut enc = OpusEnc::new();
    assert!(enc.init(48000, 2, App::Audio));
    let short = vec![0.1f32; 100];
    let mut out = [0u8; 1500];
    assert!(enc.encode(&[&short, &short], 480, &mut out) < 0);
    let long = vec![0.1f32; 48000];
    assert!(enc.encode(&[&long, &long], 48000, &mut out) < 0); // 1 s: longer than any Opus frame
    assert!(enc.encode_interleaved(&short, 480, &mut out) < 0);
    assert!(enc.encode(&[], 480, &mut out) < 0);

    let mut dec = OpusDec::new();
    assert!(dec.init(48000, 2));
    let mut l = vec![0.0f32; 100];
    assert!(dec.decode_plc(&mut [&mut l], 480) < 0);
    assert!(dec.decode_plc(&mut [], 480) < 0);
    let mut pcm = vec![0.0f32; 480];
    assert!(dec.decode_plc_interleaved(&mut pcm, 480) < 0); // stereo needs 960 samples
    // concealment longer than 120 ms (the frame count comes off the wire): all of it, in 120 ms steps, as the C++
    // gave (which grew its buffer for it)
    let (mut bl, mut br) = (vec![1.0f32; 9600], vec![1.0f32; 9600]);
    assert_eq!(dec.decode_plc(&mut [&mut bl, &mut br], 9600), 9600);
    assert!(bl.iter().chain(&br).all(|v| v.abs() < 1.0)); // every sample written
    let mut mono_dec = OpusDec::new();
    assert!(mono_dec.init(48000, 1));
    assert_eq!(mono_dec.decode_plc(&mut [&mut bl], 9600), 9600);
    // a mono codec ignores a second channel, however short (as the C++ did)
    let mut unused = vec![0.0f32; 3];
    assert_eq!(
        mono_dec.decode_plc(&mut [&mut bl[..480], &mut unused], 480),
        480
    );
    let mut mono_enc = OpusEnc::new();
    assert!(mono_enc.init(48000, 1, App::Voip));
    assert!(mono_enc.encode(&[&[0.1; 480], &[0.1; 3]], 480, &mut out) > 0);

    // a stereo encoder given one channel sends it on both (the C++ ch[1] == nullptr case)
    let mono = sine(480, 0.5, 300.0);
    assert!(enc.encode(&[&mono], 480, &mut out) > 0);
}

#[test]
fn uninitialised_codecs_fail_cleanly() {
    let mut enc = OpusEnc::new();
    let mut dec = OpusDec::new();
    let mut out = [0u8; 100];
    assert!(!enc.is_valid() && !dec.is_valid());
    assert!(enc.encode(&[&[0.0; 480]], 480, &mut out) < 0);
    assert!(dec.decode_plc(&mut [&mut [0.0; 480]], 480) < 0);
    assert!(!enc.init(12345, 1, App::Voip)); // not an Opus rate
    assert!(!dec.init(48000, 3));
    enc.reset();
    dec.reset();
}

#[test]
fn friend_echo_survives_absurd_arguments() {
    // C++ bugs: int overflow in init for huge rates / tails, std::clamp(lo > hi) for a frame longer than the ring
    let mut aec = FriendEcho::new();
    assert!(aec.init(i32::MAX, 64, i32::MAX));
    assert_eq!(aec.sample_rate(), 384_000);
    let mut aec = FriendEcho::new();
    assert!(aec.init(8000, 8000, 50));
    aec.set_mode(AecMode::On);
    let mic = vec![0.1f32; 8000];
    let mut out = vec![0.0f32; 8000];
    aec.push_reference(&mic);
    aec.process(&mic, &mut out, i32::MAX);
    aec.process(&mic, &mut out, -5);
    assert!(out.iter().all(|v| v.is_finite()));
    // output shorter than the input: refused, nothing written
    let mut short = vec![7.0f32; 10];
    aec.process(&mic, &mut short, 0);
    assert!(short.iter().all(|&v| v == 7.0));
    // NaN in the mic reaches Speex as 0 (the C++ lround(NaN) was UB) and leaves the ERLE alone
    let before = aec.erle_db();
    let mut nan = vec![0.0f32; 8000];
    nan[3] = f32::NAN;
    aec.process(&nan, &mut out, 0);
    assert!(out.iter().all(|v| v.is_finite()));
    assert_eq!(aec.erle_db().to_bits(), before.to_bits());
}

#[test]
fn friend_echo_in_place_equals_process() {
    let mut a = FriendEcho::new();
    let mut b = FriendEcho::new();
    assert!(a.init(48000, 240, 200) && b.init(48000, 240, 200));
    a.set_mode(AecMode::On);
    b.set_mode(AecMode::On);
    let mut seed = 3u32;
    let mut noise = || {
        seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        ((seed >> 16) as i32 - 32768) as f32 / 65536.0
    };
    for _ in 0..50 {
        let r: Vec<f32> = (0..240).map(|_| noise()).collect();
        let mic: Vec<f32> = r.iter().map(|v| v * 0.5 + noise() * 0.1).collect();
        a.push_reference(&r);
        b.push_reference(&r);
        let mut out = vec![0.0f32; 240];
        a.process(&mic, &mut out, 0);
        let mut io = mic.clone();
        b.process_in_place(&mut io, 0);
        assert_eq!(out, io);
    }
    assert_eq!(a.erle_db(), b.erle_db());
    // off: in place leaves the buffer as it is
    b.set_mode(AecMode::Off);
    let mut io = vec![0.25f32; 240];
    b.process_in_place(&mut io, 0);
    assert!(io.iter().all(|&v| v == 0.25));
}

#[test]
fn friend_echo_passes_through_when_off_or_not_a_frame() {
    let mut aec = FriendEcho::new();
    assert!(aec.init(48000, 480, 200));
    let mic: Vec<f32> = (0..480).map(|i| (i as f32 * 0.01).sin()).collect();
    let mut out = vec![0.0f32; 480];
    aec.set_mode(AecMode::Off);
    aec.process(&mic, &mut out, 0);
    assert_eq!(out, mic);
    aec.set_mode(AecMode::Auto); // no leak yet
    aec.process(&mic, &mut out, 0);
    assert_eq!(out, mic);
    aec.set_mode(AecMode::On);
    let mut part = vec![0.0f32; 100];
    aec.process(&mic[..100], &mut part, 0); // not a whole frame
    assert_eq!(part, &mic[..100]);
}

#[test]
fn friend_echo_erle_and_double_talk() {
    const RATE: usize = 48000;
    const FRAME: usize = 480;
    const DELAY: usize = 3840; // 80 ms
    const IR_LEN: usize = 5760; // 120 ms
    let mut aec = FriendEcho::new();
    assert!(aec.init(RATE as i32, FRAME as i32, 200));
    aec.set_mode(AecMode::On);

    // room impulse response: early reflections + decaying diffuse tail
    let mut ir = vec![0.0f32; IR_LEN];
    ir[0] = 0.5;
    ir[240] = -0.25;
    ir[600] = 0.18;
    ir[1200] = -0.12;
    let mut ir_seed: u32 = 1234;
    for k in 0..IR_LEN {
        ir_seed = ir_seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        let n = ((ir_seed >> 16) as i32 - 32768) as f32 / 32768.0;
        let t = k as f32 / RATE as f32;
        ir[k] += 0.04 * (-t / 0.030).exp() * n;
    }
    let ir_energy: f64 = ir.iter().map(|&v| f64::from(v) * f64::from(v)).sum();
    let ir_scale = (0.15 / ir_energy).sqrt() as f32;
    for v in &mut ir {
        *v *= ir_scale;
    }

    const FRAMES: usize = 500;
    const TOTAL: usize = FRAMES * FRAME;
    const PRE: usize = DELAY + IR_LEN + 480;
    let mut seed: u32 = 42;
    let reference: Vec<f32> = (0..TOTAL + PRE)
        .map(|_| {
            seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            0.4 * (((seed >> 16) as i32 - 32768) as f32 / 32768.0)
        })
        .collect();
    let mic_echo: Vec<f32> = (0..TOTAL)
        .map(|i| {
            let start = PRE + i - DELAY;
            (0..IR_LEN)
                .map(|k| f64::from(reference[start - k]) * f64::from(ir[k]))
                .sum::<f64>() as f32
        })
        .collect();

    let mut clean = vec![0.0f32; FRAME];
    let (mut echo_in, mut echo_out) = (0.0f64, 0.0f64);
    for f in 0..350 {
        let mic = &mic_echo[f * FRAME..][..FRAME];
        aec.push_reference(&reference[PRE + f * FRAME..][..FRAME]);
        aec.process(mic, &mut clean, DELAY as i32);
        if f >= 250 {
            echo_in += mic
                .iter()
                .map(|&v| f64::from(v) * f64::from(v))
                .sum::<f64>();
            echo_out += clean
                .iter()
                .map(|&v| f64::from(v) * f64::from(v))
                .sum::<f64>();
        }
    }
    assert!(echo_in > 0.0 && echo_out > 0.0);
    let erle = 10.0 * (echo_in / echo_out).log10();
    println!("FriendEcho ERLE after 3.0 s: {erle:.1} dB (target >= 25)");
    assert!(erle >= 25.0, "{erle}");

    // double talk: a sung voice with vibrato over the echo (frames 350..450)
    let mut all_near = Vec::new();
    let mut all_clean = Vec::new();
    for f in 350..450 {
        let near: Vec<f32> = (0..FRAME)
            .map(|i| {
                let t = (f * FRAME + i) as f32 / RATE as f32;
                let vib = 1.0 + 0.03 * (2.0 * PI * 5.5 * t).sin();
                0.30 * (2.0 * PI * 260.0 * vib * t).sin()
                    + 0.18 * (2.0 * PI * 520.0 * vib * t).sin()
                    + 0.08 * (2.0 * PI * 780.0 * vib * t).sin()
            })
            .collect();
        let dt: Vec<f32> = (0..FRAME)
            .map(|i| mic_echo[f * FRAME + i] + near[i])
            .collect();
        aec.push_reference(&reference[PRE + f * FRAME..][..FRAME]);
        aec.process(&dt, &mut clean, DELAY as i32);
        all_near.extend_from_slice(&near);
        all_clean.extend_from_slice(&clean);
    }
    // the same voice through a canceller with no echo: the filter's own effect
    let mut pure = FriendEcho::new();
    assert!(pure.init(RATE as i32, FRAME as i32, 200));
    pure.set_mode(AecMode::On);
    let silence = vec![0.0f32; FRAME];
    let mut pure_clean = Vec::new();
    let mut out = vec![0.0f32; FRAME];
    for f in 0..100 {
        pure.push_reference(&silence);
        pure.process(&all_near[f * FRAME..][..FRAME], &mut out, 0);
        pure_clean.extend_from_slice(&out);
    }

    let max_lag = FRAME * 2;
    let eval_start = FRAME * 30;
    let eval_len = all_near.len() - eval_start - max_lag - 1;
    let (mut best_lag, mut max_corr) = (0usize, -1e9f64);
    for lag in 0..=max_lag {
        let corr: f64 = (0..eval_len)
            .map(|i| {
                f64::from(all_clean[eval_start + lag + i]) * f64::from(all_near[eval_start + i])
            })
            .sum();
        if corr > max_corr {
            max_corr = corr;
            best_lag = lag;
        }
    }
    let (mut speech, mut dist) = (0.0f64, 0.0f64);
    for i in 0..eval_len {
        let s = f64::from(pure_clean[eval_start + best_lag + i]);
        let o = f64::from(all_clean[eval_start + best_lag + i]);
        speech += s * s;
        dist += (o - s) * (o - s);
    }
    assert!(dist > 0.0);
    let snr = 10.0 * (speech / dist).log10();
    println!("FriendEcho double-talk speech SNR: {snr:.1} dB (target >= 15)");
    assert!(snr >= 15.0, "{snr}");
}
