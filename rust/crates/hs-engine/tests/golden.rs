//! Compares hs-engine with `compat/golden/core/engine/*` (from `tools/compat-golden`,
//! `engineGoldens`): two mono Tracks and a Hub on a private bus, panic / preview / stop / seek /
//! suspend / epoch events. Outputs must match the C++ within 1e-6.
use hs_bus::bus::random_token;
use hs_bus::protocol::*;
use hs_bus::ring::{ReadResult, ring_read_at};
use hs_bus::shm::current_pid;
use hs_dsp::dsp::{db_to_gain, pan_gains};
use hs_engine::hub_engine::{HubEngine, HubParams, Role};
use hs_engine::track_publisher::{Mirror, PublisherStatus, TrackPublisher};
use std::path::PathBuf;

const RATE: u32 = 48000;
const BLOCK: usize = 256;
const PI: f64 = std::f64::consts::PI;

fn golden(name: &str) -> Vec<f32> {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../compat/golden/core")
        .join(name);
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
        if d > worst {
            (worst, at) = (d, i);
        }
    }
    assert!(
        worst <= tol,
        "{name}: max error {worst:e} at sample {at} (got {}, want {})",
        got[at],
        want[at]
    );
}

fn sine(frame: u64, rate: u32, hz: f64) -> f32 {
    (2.0 * PI * hz * (frame % u64::from(rate)) as f64 / f64::from(rate)).sin() as f32
}

fn music(frame: u64, rate: u32) -> f32 {
    const NOTES: [f64; 4] = [220.0, 330.0, 275.0, 440.0];
    let note = ((frame / u64::from(rate / 4)) % 4) as usize;
    let envelope =
        0.65 + 0.35 * (2.0 * PI * (frame % u64::from(rate)) as f64 / f64::from(rate)).sin();
    envelope as f32 * (0.22 * sine(frame, rate, NOTES[note]) + 0.10 * sine(frame, rate, 110.0))
}

fn stereo(out: &mut Vec<f32>, l: &[f32], r: &[f32]) {
    for (a, b) in l.iter().zip(r) {
        out.extend([*a, *b]);
    }
}

fn mirror(mon: bool, gain: f32, pan: f32, trim: f32, stem: i32) -> Mirror {
    Mirror {
        mon,
        str_: true,
        solo: false,
        gain_db: gain,
        pan,
        delay_ms: 0.0,
        trim_db: trim,
        stem,
        app: false,
    }
}

#[test]
fn engine_matches_golden() {
    let bus = format!("golden_{}_{}", current_pid(), random_token());
    let mut hub = HubEngine::new();
    assert!(hub.connect(&bus));
    hub.prepare(f64::from(RATE), BLOCK);
    hub.maintain();
    assert_eq!(hub.role(), Role::Owner);
    let (mut a, mut b) = (TrackPublisher::new(), TrackPublisher::new());
    assert_eq!(
        a.connect(&bus, "golden-a", RATE, 1),
        PublisherStatus::Connected
    );
    assert_eq!(
        b.connect(&bus, "golden-b", RATE, 1),
        PublisherStatus::Connected
    );
    a.prepare(RATE, 1);
    b.prepare(RATE, 1);
    a.mirror(&mirror(true, -3.0, -0.4, 0.0, 0));
    b.mirror(&mirror(true, 0.0, 0.4, -3.0, 1));
    let mut params = HubParams::default();

    let (mut phones, mut stream, mut stem) = (vec![], vec![], vec![]);
    let (mut l, mut r) = (vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]);
    let (mut x, mut y) = (vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]);
    let (mut sl, mut sr) = (vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]);
    let (mut read, mut timeline) = (0u64, 100_000i64);
    for block in 0..200 {
        if block == 40 {
            a.mirror(&mirror(false, -3.0, -0.4, 0.0, 0));
        }
        if block == 80 {
            params.panic = true;
        }
        if block == 100 {
            params.panic = false;
            params.preview = true;
        }
        if block == 120 {
            params.preview = false;
        }
        if block == 140 {
            timeline += 10_000;
        }
        if block == 180 {
            a.prepare(RATE, 1);
        }
        let playing = !(120..140).contains(&block);
        let suspend = (160..180).contains(&block);
        for i in 0..BLOCK {
            x[i] = music((timeline + i as i64) as u64, RATE);
            y[i] = 0.2 * sine((timeline + i as i64) as u64, RATE, 440.0);
        }
        if !suspend {
            a.process(&[&x], BLOCK, timeline, playing, false, false);
        }
        b.process(&[&y], BLOCK, timeline, playing, false, false);
        let (agl, agr) = pan_gains(-0.4);
        let (bgl, bgr) = pan_gains(0.4);
        for i in 0..BLOCK {
            let av = if suspend {
                0.0
            } else {
                x[i] * db_to_gain(-3.0)
            };
            l[i] = av * agl + y[i] * bgl;
            r[i] = av * agr + y[i] * bgr;
        }
        hub.process(
            &mut [&mut l, &mut r],
            &params,
            if playing { timeline } else { i64::MIN },
            playing,
            false,
        );
        stereo(&mut phones, &l, &r);
        let busl = hub.bus().unwrap();
        let res = ring_read_at(
            &busl.streamAudio.0[0],
            &busl.streamHeader.writePos.0[0],
            read,
            &mut [&mut sl, &mut sr],
            BLOCK as u32,
            None,
        );
        assert_eq!(res, ReadResult::Ok, "stream read failed at block {block}");
        stereo(&mut stream, &sl, &sr);
        let res = ring_read_at(
            &busl.streamAudio.0[1],
            &busl.streamHeader.writePos.0[1],
            read,
            &mut [&mut sl, &mut sr],
            BLOCK as u32,
            None,
        );
        assert_eq!(res, ReadResult::Ok, "stem read failed at block {block}");
        stereo(&mut stem, &sl, &sr);
        read += BLOCK as u64;
        timeline += BLOCK as i64;
    }
    assert_close("phones", &phones, &golden("engine/phones.f32"), 1e-6);
    assert_close("stream", &stream, &golden("engine/stream.f32"), 1e-6);
    assert_close("stem0", &stem, &golden("engine/stem0.f32"), 1e-6);
    let _ = kMaxSlots;
}
