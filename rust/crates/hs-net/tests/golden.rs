//! `compat/golden/net` (written by the C++ `ss_net_golden`): the same Opus packets, decoded audio (with loss
//! concealment and FEC) and FriendEcho output as the C++ ssnet, from the same input files.
//!
//! The goldens were recorded with MSVC on Windows x64 (`compat/golden/provenance.json`), the compiler the plug-ins
//! ship with; another compiler may round libopus' float code differently, so the comparison runs there only.
#![cfg(all(windows, target_env = "msvc", target_arch = "x86_64"))]

use hs_net::aec::{AecMode, FriendEcho};
use hs_net::opus::{App, OpusDec, OpusEnc};
use std::path::PathBuf;

fn dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../compat/golden/net")
}

fn read(name: &str) -> Vec<u8> {
    std::fs::read(dir().join(name)).unwrap_or_else(|e| panic!("{name}: {e}"))
}

fn f32s(name: &str) -> Vec<f32> {
    read(name)
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| f32::from_le_bytes(*b))
        .collect()
}

fn packets(name: &str) -> Vec<Vec<u8>> {
    let b = read(name);
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let n = usize::from(u16::from_le_bytes([b[i], b[i + 1]]));
        out.push(b[i + 2..i + 2 + n].to_vec());
        i += 2 + n;
    }
    out
}

/// Bit-equal floats, or the first index and both values that differ.
fn same(a: &[f32], b: &[f32]) -> Result<(), String> {
    if a.len() != b.len() {
        return Err(format!("length {} vs {}", a.len(), b.len()));
    }
    match a
        .iter()
        .zip(b)
        .position(|(x, y)| x.to_bits() != y.to_bits())
    {
        None => Ok(()),
        Some(i) => Err(format!("sample {i}: {} vs {}", a[i], b[i])),
    }
}

/// libopus picks AVX2 / FMA code at run time; the goldens come from such a CPU (see provenance.json), and other
/// paths round differently. Both the C++ and the Rust side would change together on another CPU.
fn opus_cpu_matches() -> bool {
    let ok = is_x86_feature_detected!("avx2") && is_x86_feature_detected!("fma");
    if !ok {
        eprintln!("skipped: the Opus goldens were recorded on a CPU with AVX2 + FMA");
    }
    ok
}

fn opus_case(name: &str, app: App, chans: usize, bitrate: i32, pkt: usize, input: &[Vec<f32>]) {
    if !opus_cpu_matches() {
        return;
    }
    let mut enc = OpusEnc::new();
    let mut dec = OpusDec::new();
    assert!(enc.init(48000, chans as i32, app) && dec.init(48000, chans as i32));
    if bitrate > 0 {
        enc.set_bitrate(bitrate);
    }
    let count = input[0].len() / pkt;
    let mut mine = Vec::new();
    let mut buf = [0u8; 1500];
    for p in 0..count {
        let ch: Vec<&[f32]> = input.iter().map(|c| &c[p * pkt..][..pkt]).collect();
        let n = enc.encode(&ch, pkt, &mut buf);
        assert!(n > 0);
        mine.push(buf[..n as usize].to_vec());
    }
    let golden = packets(&format!("{name}.pkt"));
    assert_eq!(mine.len(), golden.len(), "{name}: packet count");
    for (p, (m, g)) in mine.iter().zip(&golden).enumerate() {
        assert_eq!(m, g, "{name}: packet {p} differs from the C++");
    }

    let mut out = Vec::new();
    let (mut l, mut r) = (vec![0.0f32; pkt], vec![0.0f32; pkt]);
    for p in 0..count {
        let mut ch: [&mut [f32]; 2] = [&mut l, &mut r];
        let ch = &mut ch[..chans];
        let got = if p % 10 == 9 {
            dec.decode_plc(ch, pkt)
        } else if p == 25 {
            dec.decode(Some(&golden[26]), ch, pkt, true)
        } else {
            dec.decode(Some(&golden[p]), ch, pkt, false)
        };
        assert_eq!(got, pkt as i32);
        for i in 0..pkt {
            out.push(l[i]);
            if chans == 2 {
                out.push(r[i]);
            }
        }
    }
    same(&out, &f32s(&format!("{name}_dec.f32"))).unwrap_or_else(|e| panic!("{name} decoded: {e}"));
}

#[test]
fn opus_voip_mono_matches_cpp() {
    opus_case(
        "opus_voip",
        App::Voip,
        1,
        64000,
        480,
        &[f32s("input_mono.f32")],
    );
}

#[test]
fn opus_audio_stereo_matches_cpp() {
    let st = f32s("input_stereo.f32");
    let l = st.iter().step_by(2).copied().collect();
    let r = st.iter().skip(1).step_by(2).copied().collect();
    opus_case("opus_audio_stereo", App::Audio, 2, 128_000, 960, &[l, r]);
}

#[test]
fn opus_lowdelay_matches_cpp() {
    opus_case(
        "opus_lowdelay",
        App::LowDelay,
        1,
        0,
        480,
        &[f32s("input_mono.f32")],
    );
}

#[test]
fn friend_echo_matches_cpp() {
    const FRAME: usize = 480;
    const DELAY: usize = 960;
    let reference = f32s("aec_ref.f32");
    let mic = f32s("aec_mic.f32");
    let frames = mic.len() / FRAME;
    let mut aec = FriendEcho::new();
    assert!(aec.init(48000, FRAME as i32, 200));
    aec.set_mode(AecMode::On);
    let mut out = vec![0.0f32; mic.len()];
    let mut erle = Vec::new();
    for f in 0..frames {
        aec.push_reference(&reference[DELAY + f * FRAME..][..FRAME]);
        aec.process(
            &mic[f * FRAME..][..FRAME],
            &mut out[f * FRAME..][..FRAME],
            (DELAY - 40) as i32,
        );
        erle.push(aec.erle_db());
    }
    same(&out, &f32s("aec_out.f32")).unwrap_or_else(|e| panic!("FriendEcho output: {e}"));
    close_erle(&erle, &f32s("aec_erle.f32"));
}

/// ERLE goes through log10f, which may differ by an ulp between C runtimes.
fn close_erle(mine: &[f32], golden: &[f32]) {
    assert_eq!(mine.len(), golden.len(), "ERLE frame count");
    for (f, (a, b)) in mine.iter().zip(golden).enumerate() {
        assert!(
            (a - b).abs() <= 1e-4 * b.abs().max(1.0),
            "ERLE frame {f}: {a} vs {b}"
        );
    }
}

#[test]
fn friend_echo_in_place_240_matches_cpp() {
    // as ShareServer runs it: 240-frame steps in place, clipping input, a delay change and a reset
    const FRAME: usize = 240;
    let reference = f32s("aec240_ref.f32");
    let mut out = f32s("aec240_mic.f32");
    let mut aec = FriendEcho::new();
    assert!(aec.init(48000, FRAME as i32, 200));
    aec.set_mode(AecMode::On);
    let mut erle = Vec::new();
    for f in 0..out.len() / FRAME {
        if f == 300 {
            aec.reset();
        }
        aec.push_reference(&reference[2000 + f * FRAME..][..FRAME]);
        aec.process_in_place(
            &mut out[f * FRAME..][..FRAME],
            if f < 150 { 1300 } else { 1700 },
        );
        erle.push(aec.erle_db());
    }
    same(&out, &f32s("aec240_out.f32")).unwrap_or_else(|e| panic!("FriendEcho 240 output: {e}"));
    close_erle(&erle, &f32s("aec240_erle.f32"));
}

#[test]
fn opus_interleaved_matches_cpp() {
    if !opus_cpu_matches() {
        return;
    }
    let stereo = f32s("input_stereo.f32");
    let mut enc = OpusEnc::new();
    let mut dec = OpusDec::new();
    assert!(enc.init(48000, 2, App::Voip) && dec.init(48000, 2));
    enc.set_bitrate(96000);
    let golden = packets("opus_interleaved.pkt");
    let mut out = Vec::new();
    let mut pcm = vec![0.0f32; 960];
    let mut buf = [0u8; 1500];
    for p in 0..stereo.len() / 960 {
        let n = enc.encode_interleaved(&stereo[p * 960..][..960], 480, &mut buf);
        assert!(n > 0);
        assert_eq!(
            &buf[..n as usize],
            golden[p].as_slice(),
            "interleaved packet {p} differs from the C++"
        );
        let got = if p % 10 == 9 {
            dec.decode_plc_interleaved(&mut pcm, 480)
        } else {
            dec.decode_interleaved(Some(&buf[..n as usize]), &mut pcm, 480, false)
        };
        assert_eq!(got, 480);
        out.extend_from_slice(&pcm);
    }
    assert_eq!(golden.len(), stereo.len() / 960);
    same(&out, &f32s("opus_interleaved_dec.f32"))
        .unwrap_or_else(|e| panic!("interleaved decoded: {e}"));
}
