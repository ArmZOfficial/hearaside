//! `VocalChain`: the port of `karaoke: VocalChain gate, EQ, compressor, reverb, echo, and routing` from
//! `tests/unit/test_karaoke.cpp`, and the 20 C++ captures of `compat/golden/vocal` (made by `tools/compat-vocal-golden`
//! from the unchanged 1.0.10 source). The scenarios are re-stated from that tool's `settings[]`; outputs must match
//! within 1e-6 absolute (-120 dBFS), the bound of PLAN section 9.
use hs_karaoke::vocal::VocalChain;
use std::path::PathBuf;

fn golden(name: &str) -> Vec<f32> {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../compat/golden/vocal")
        .join(name);
    let b = std::fs::read(&p).unwrap_or_else(|e| panic!("{p:?}: {e}"));
    b.as_chunks::<4>()
        .0
        .iter()
        .map(|c| f32::from_le_bytes(*c))
        .collect()
}

fn peak(x: &[f32]) -> f32 {
    x.iter().fold(0.0f32, |m, v| m.max(v.abs()))
}

fn block(chain: &mut VocalChain, mic: &[f32], l: &mut [f32], r: &mut [f32], net: &mut [f32]) {
    chain.process(Some(mic), Some(l), Some(r), Some(net), mic.len());
}

#[test]
fn karaoke_vocal_chain_gate_eq_compressor_reverb_echo_and_routing() {
    let mut chain = VocalChain::new();
    chain.prepare(48000.0, 256);
    let p = chain.params().clone();
    let mut mic = vec![0.0f32; 256];
    let (mut l, mut r, mut net) = (vec![0.0f32; 256], vec![0.0f32; 256], vec![0.0f32; 256]);

    // 1. quiet noise under the -45 dBFS gate
    mic.fill(0.001);
    for _ in 0..20 {
        block(&mut chain, &mic, &mut l, &mut r, &mut net);
    }
    assert!(peak(&net) < 0.0005, "gate attenuates under the threshold");

    // 2. 1 kHz at -10 dBFS opens it
    for (i, m) in mic.iter_mut().enumerate() {
        *m = 0.316
            * (2.0f32 * std::f32::consts::PI * 1000.0 // C++: 3.14159265f, the same f32
             * i as f32
                / 48000.0)
                .sin();
    }
    for _ in 0..10 {
        block(&mut chain, &mic, &mut l, &mut r, &mut net);
    }
    assert!(peak(&net) > 0.1, "gate opened");

    // 3. muted = strictly silent
    p.set_muted(true);
    block(&mut chain, &mic, &mut l, &mut r, &mut net);
    assert_eq!(peak(&l).max(peak(&net)), 0.0);

    // 4. dry network send, wet headphones
    p.set_muted(false);
    p.set_send_wet(false);
    p.set_reverb_level(0.8);
    p.set_echo_level(0.8);
    for _ in 0..10 {
        block(&mut chain, &mic, &mut l, &mut r, &mut net);
    }
    mic.fill(0.0);
    for _ in 0..10 {
        block(&mut chain, &mic, &mut l, &mut r, &mut net);
    }
    assert!(peak(&net) < 0.001, "dry send drops with dry input");
    assert!(
        peak(&l) > 0.01,
        "headphones still ring with the reverb tail"
    );
}

struct Setting {
    name: &'static str,
    reverb: f32,
    echo: f32,
    gate_db: f32,
    send_wet: bool,
    muted: bool,
}

const SETTINGS: [Setting; 5] = [
    Setting {
        name: "default-dry-send",
        reverb: 0.25,
        echo: 0.20,
        gate_db: -45.0,
        send_wet: false,
        muted: false,
    },
    Setting {
        name: "ambience-off",
        reverb: 0.0,
        echo: 0.0,
        gate_db: -45.0,
        send_wet: false,
        muted: false,
    },
    Setting {
        name: "wet-send",
        reverb: 0.65,
        echo: 0.55,
        gate_db: -36.0,
        send_wet: true,
        muted: false,
    },
    Setting {
        name: "gate-closed",
        reverb: 0.0,
        echo: 0.0,
        gate_db: 6.0,
        send_wet: false,
        muted: false,
    },
    Setting {
        name: "muted",
        reverb: 0.65,
        echo: 0.55,
        gate_db: -36.0,
        send_wet: true,
        muted: true,
    },
];

#[test]
fn golden_vocal_chain_matches_cpp() {
    const BLOCK: usize = 256;
    for stimulus in ["sine", "noise", "impulse", "synthetic-music"] {
        let input = golden(&format!("input-{stimulus}.f32"));
        assert_eq!(input.len(), 32768);
        for s in &SETTINGS {
            let mut chain = VocalChain::new();
            chain.prepare(48000.0, BLOCK);
            let p = chain.params().clone();
            if s.name == "default-dry-send" {
                // the defaults themselves are under test here, like the C++ tool's `require`
                assert!(p.reverb_level() == s.reverb && p.echo_level() == s.echo);
                assert!(!p.is_send_wet() && !p.is_muted());
            } else {
                p.set_reverb_level(s.reverb);
                p.set_echo_level(s.echo);
                p.set_gate_threshold_db(s.gate_db);
                p.set_send_wet(s.send_wet);
                p.set_muted(s.muted);
            }
            let mut got = Vec::with_capacity(input.len() * 3);
            let (mut l, mut r, mut net) = ([0.0f32; BLOCK], [0.0f32; BLOCK], [0.0f32; BLOCK]);
            for mic in input.chunks(BLOCK) {
                block(&mut chain, mic, &mut l, &mut r, &mut net);
                for i in 0..BLOCK {
                    got.extend([l[i], r[i], net[i]]);
                }
            }
            let name = format!("{stimulus}-{}.f32", s.name);
            let want = golden(&name);
            assert_eq!(got.len(), want.len(), "{name}: length");
            let (mut worst, mut at) = (0.0f32, 0);
            for (i, (a, b)) in got.iter().zip(&want).enumerate() {
                let d = (a - b).abs();
                if d > worst || d.is_nan() {
                    (worst, at) = (d, i);
                }
            }
            assert!(
                worst <= 1e-6,
                "{name}: max error {worst:e} at sample {at} (frame {}, channel {}; got {}, want {})",
                at / 3,
                at % 3,
                got[at],
                want[at]
            );
            if s.gate_db > 0.0 {
                assert!(p.current_gate_gain() < 1.0e-6, "closed gate settles");
            }
        }
    }
}

#[test]
fn a_block_longer_than_prepared_is_processed_whole() {
    // C++ processed only the first max(maxBlock, 2048) frames and left the rest of the outputs untouched
    let mut chain = VocalChain::new();
    chain.prepare(48000.0, 256);
    let n = 5000;
    let mic: Vec<f32> = (0..n).map(|i| 0.3 * (i as f32 * 0.05).sin()).collect();
    let (mut l, mut r, mut net) = (vec![9.0f32; n], vec![9.0f32; n], vec![9.0f32; n]);
    block(&mut chain, &mic, &mut l, &mut r, &mut net);
    assert!(l.iter().chain(&r).chain(&net).all(|v| v.abs() < 2.0));
    assert!(peak(&net[4096..]) > 0.05);

    // same result as the same input in 256-frame blocks
    let mut other = VocalChain::new();
    other.prepare(48000.0, 256);
    let mut net2 = vec![0.0f32; n];
    let (mut a, mut b) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    for (m, o) in mic.chunks(256).zip(net2.chunks_mut(256)) {
        let k = m.len();
        other.process(Some(m), Some(&mut a[..k]), Some(&mut b[..k]), Some(o), k);
    }
    assert_eq!(net, net2);
}

#[test]
fn no_mic_or_unprepared_is_silent() {
    let mut chain = VocalChain::new();
    let mic = [0.5f32; 64];
    let mut out = [1.0f32; 64];
    chain.process(Some(&mic), Some(&mut out), None, None, 64);
    assert_eq!(peak(&out), 0.0, "not prepared");
    chain.prepare(48000.0, 64);
    out.fill(1.0);
    chain.process(None, None, None, Some(&mut out), 64);
    assert_eq!(peak(&out), 0.0, "no mic");
}
