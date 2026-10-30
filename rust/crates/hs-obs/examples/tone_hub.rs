//! A stand-in Hub for trying the OBS module by hand: owns bus `Main` (or argv[1]) and feeds a quiet 440 Hz tone
//! through `HubEngine` in real time at a DAW's pace (48 kHz, 256-frame blocks) for argv[2] seconds (default 10).
//! `tools/obs_smoke.c` with `--hub` expects to hear it.
use hs_engine::hub_engine::{HubEngine, HubParams};
use std::time::{Duration, Instant};

fn main() {
    let mut args = std::env::args().skip(1);
    let bus = args.next().unwrap_or_else(|| "Main".into());
    let seconds: f64 = args.next().and_then(|s| s.parse().ok()).unwrap_or(10.0);
    let mut hub = HubEngine::new();
    hub.connect(&bus);
    hub.prepare(48000.0, 256);
    hub.maintain();
    let start = Instant::now();
    let (mut l, mut r) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let mut t = 0i64;
    while start.elapsed().as_secs_f64() < seconds {
        for (i, s) in l.iter_mut().enumerate() {
            *s = 0.2 * (std::f32::consts::TAU * 440.0 * (t + i as i64) as f32 / 48000.0).sin();
        }
        r.copy_from_slice(&l);
        hub.process(&mut [&mut l, &mut r], &HubParams::default(), t, true, false);
        t += 256;
        // pace: block n is due at n * 256 / 48000 s
        let due = Duration::from_secs_f64(t as f64 / 48000.0);
        if let Some(wait) = due.checked_sub(start.elapsed()) {
            std::thread::sleep(wait);
        }
        if (t / 256) % 128 == 0 {
            hub.maintain();
        }
    }
}
