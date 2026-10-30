//! Auto sync under realistic sound (port of `tests/unit/test_autosync.cpp`): music with a steady beat
//! and a microphone that only hears a quiet, treble-heavy bleed of it from the headphones, with room
//! noise. `measure_lag` must find the same lag in every reading, like `HubProcessor::steadyLag` asks
//! (five readings 0.4 s apart, within 4 ms of each other).
use hs_bus::bus::{claim_source, set_source_identity};
use hs_bus::protocol::*;
use hs_bus::ring::ring_write;
use hs_bus::shm::{current_pid, now_ns};
use hs_engine::hub_engine::{HubEngine, HubParams};
use hs_engine::track_publisher::{Mirror, TrackPublisher};
use std::sync::atomic::Ordering::SeqCst;

const RATE: f64 = 48000.0;
const PI: f64 = std::f64::consts::PI;

fn hash_noise(t: i64, seed: u32) -> f32 {
    let mut h = (t as u32).wrapping_mul(2_654_435_761) ^ seed.wrapping_mul(40503);
    h ^= h >> 15;
    h = h.wrapping_mul(2_246_822_519);
    h ^= h >> 13;
    h = h.wrapping_mul(3_266_489_917);
    h ^= h >> 16;
    (h & 0xffff) as f32 / 65535.0 - 0.5
}

/// A loop at 120 bpm: kick on every beat, snare on 2 and 4, closed hi-hat on eighths, a bass line that
/// changes every beat and a chord pad that changes every bar.
fn music(t: i64) -> f32 {
    if t < 0 {
        return 0.0;
    }
    let s = t as f64 / RATE;
    let beat = 0.5;
    let in_beat = s % beat;
    let in_eighth = s % (beat / 2.0);
    let beat_no = (s / beat) as i64;
    let mut v = 0.0f64;
    v += 0.6
        * (2.0 * PI * (50.0 + 60.0 * (-in_beat / 0.03).exp()) * in_beat).sin()
        * (-in_beat / 0.12).exp(); // kick
    if beat_no % 2 == 1 {
        v += 0.35 * f64::from(hash_noise(t, 3)) * (-in_beat / 0.07).exp(); // snare
    }
    v += 0.12 * f64::from(hash_noise(t, 9) - hash_noise(t - 1, 9)) * (-in_eighth / 0.015).exp(); // hi-hat
    const BASS: [f64; 8] = [55.0, 55.0, 65.4, 73.4, 49.0, 49.0, 61.7, 73.4];
    v += 0.25 * (2.0 * PI * BASS[(beat_no % 8) as usize] * s).sin();
    const PAD: [[f64; 3]; 4] = [
        [220.0, 277.2, 329.6],
        [196.0, 246.9, 293.7],
        [174.6, 220.0, 261.6],
        [196.0, 246.9, 311.1],
    ];
    for f in PAD[((beat_no / 4) % 4) as usize] {
        v += 0.05 * (2.0 * PI * f * s).sin();
    }
    v as f32
}

fn mirror(mon: bool) -> Mirror {
    Mirror {
        mon,
        str_: true,
        solo: false,
        gain_db: 0.0,
        pan: 0.0,
        delay_ms: 0.0,
        trim_db: 0.0,
        stem: -1,
        app: false,
    }
}

struct Rig {
    hub: HubEngine,
    music_track: TrackPublisher,
    mic_track: TrackPublisher,
    t: i64,
    /// the microphone: headphone bleed of what the Hub played, `round_trip` frames later
    round_trip: i64,
    bleed: f32,
    noise: f32,
    /// music that reaches the master another way (a channel without HEARASIDE Track)
    leak: f32,
    /// in the music step: a second copy of the music, `second_lag` frames later (drifting)
    second: f32,
    second_lag: i64,
    /// room / reverb: a copy 23 ms later
    echo: f32,
}

impl Rig {
    fn new(bus: &str) -> Self {
        let mut hub = HubEngine::new();
        hub.connect(bus);
        hub.prepare(RATE, 1024);
        hub.maintain();
        let (mut music_track, mut mic_track) = (TrackPublisher::new(), TrackPublisher::new());
        music_track.connect(bus, "", 48000, 1);
        music_track.prepare(48000, 1);
        mic_track.connect(bus, "", 48000, 1);
        mic_track.prepare(48000, 1);
        music_track.mirror(&mirror(true));
        mic_track.mirror(&mirror(false));
        Rig {
            hub,
            music_track,
            mic_track,
            t: 100_000,
            round_trip: 0,
            bleed: 0.02,
            noise: 0.002,
            leak: 0.0,
            second: 0.0,
            second_lag: 0,
            echo: 0.0,
        }
    }

    fn mic(&self, at: i64) -> f32 {
        let s = at - self.round_trip;
        let treble = music(s) - music(s - 1);
        let treble2 = music(s - 1104) - music(s - 1105);
        self.bleed * 4.0 * (treble + self.echo * treble2) + self.noise * hash_noise(at, 77)
    }

    /// `music_to_master`: phase 1 (only the music reaches the master) or phase 2 (only the mic)
    fn run(&mut self, seconds: f64, music_to_master: bool) {
        const N: usize = 256;
        let (mut a, mut m, mut l, mut r) = (
            vec![0.0f32; N],
            vec![0.0f32; N],
            vec![0.0f32; N],
            vec![0.0f32; N],
        );
        let mut done = 0i64;
        while done < (seconds * RATE) as i64 {
            for i in 0..N {
                a[i] = music(self.t + i as i64);
                m[i] = self.mic(self.t + i as i64);
            }
            self.music_track
                .process(&[&a], N, self.t, true, false, false);
            self.mic_track.process(&[&m], N, self.t, true, false, false);
            for i in 0..N {
                let ti = self.t + i as i64;
                l[i] = if music_to_master {
                    a[i] + self.second * music(ti - self.second_lag - ti / 4800)
                } else {
                    m[i] + self.leak * a[i]
                };
                r[i] = l[i];
            }
            self.hub.process(
                &mut [&mut l, &mut r],
                &HubParams::default(),
                self.t,
                true,
                false,
            );
            self.t += N as i64;
            done += N as i64;
        }
    }

    /// `HubProcessor::serviceAutoSync`: five readings 0.4 s apart after a settling time
    fn readings(
        &mut self,
        music_phase: bool,
        slot: usize,
        remove_ms: Option<f64>,
    ) -> Vec<Option<f64>> {
        self.run(2.0, music_phase);
        let measurer = self.hub.measurer();
        let mut out = vec![];
        for k in 0..5 {
            if k > 0 {
                self.run(0.4, music_phase);
            }
            let (v, score) = measurer.measure_lag(
                slot,
                if music_phase { 0.3 } else { 0.1 },
                !music_phase,
                false,
                true,
                remove_ms,
            );
            println!(
                "    {} reading {k}: {v:?} (score {score:.2})",
                if music_phase { "music" } else { "mic  " }
            );
            out.push(v);
        }
        out
    }
}

/// what `steadyLag` accepts: at least 3 of 5 within 4 ms of their median
fn steady(rs: &[Option<f64>]) -> Option<f64> {
    let mut f: Vec<f64> = rs.iter().flatten().copied().collect();
    if f.len() < 3 {
        return None;
    }
    f.sort_by(f64::total_cmp);
    let med = f[f.len() / 2];
    let near: Vec<f64> = f
        .iter()
        .copied()
        .filter(|v| (v - med).abs() <= 4.0)
        .collect();
    (near.len() >= 3).then(|| near.iter().sum::<f64>() / near.len() as f64)
}

fn bus(tag: &str) -> String {
    format!("autosync_{tag}_{}", current_pid())
}

#[test]
fn music_with_a_beat_mic_hears_a_quiet_bleed_from_the_headphones() {
    for rt_ms in [12i64, 40, 95] {
        let mut r = Rig::new(&bus(&format!("beat_{rt_ms}")));
        r.round_trip = rt_ms * 48;
        let slot = r.music_track.slot_index().unwrap();
        let reference = steady(&r.readings(true, slot, None)).expect("music step");
        let mic = steady(&r.readings(false, slot, Some(reference))).expect("mic step");
        assert!(
            (mic - reference - rt_ms as f64).abs() < 1.0,
            "rt {rt_ms}: {}",
            mic - reference
        );
    }
}

#[test]
fn music_that_reaches_the_master_another_way_does_not_outvote_the_mic() {
    // an FX return or a channel without a HEARASIDE Track isn't muted while measuring
    for leak in [0.003f32, 0.01, 0.1, 0.5] {
        let mut r = Rig::new(&bus(&format!("leak_{}", (leak * 1000.0) as i32)));
        r.round_trip = 40 * 48;
        r.bleed = 0.01;
        r.leak = leak;
        let slot = r.music_track.slot_index().unwrap();
        let reference = steady(&r.readings(true, slot, None)).expect("music step");
        let mic = steady(&r.readings(false, slot, Some(reference))).expect("mic step");
        assert!(
            (mic - reference - 40.0).abs() < 1.0,
            "leak {leak}: {}",
            mic - reference
        );
    }
}

#[test]
fn room_echo_and_a_mic_that_hears_nothing_gives_no_number_rather_than_a_wrong_one() {
    {
        let mut r = Rig::new(&bus("echo"));
        r.round_trip = 40 * 48;
        r.bleed = 0.01;
        r.echo = 0.8;
        let slot = r.music_track.slot_index().unwrap();
        let reference = steady(&r.readings(true, slot, None)).expect("music step");
        let mic = steady(&r.readings(false, slot, Some(reference))).expect("mic step");
        assert!((mic - reference - 40.0).abs() < 1.0, "{}", mic - reference);
    }
    {
        let mut r = Rig::new(&bus("deaf"));
        r.round_trip = 40 * 48;
        r.bleed = 0.001;
        r.noise = 0.02;
        let slot = r.music_track.slot_index().unwrap();
        let reference = steady(&r.readings(true, slot, None)).expect("music step");
        assert!(steady(&r.readings(false, slot, Some(reference))).is_none());
    }
}

/// App Audio written in small blocks on its own schedule (Studio One runs a monitored input channel on
/// the low-latency path, the master in 2048-sample blocks): when the Hub reads, App Audio has written a
/// varying amount more than the master block needs. The Hub's copy of the App signal must still line up
/// with the master at one fixed lag, or the music step of auto sync would wander.
#[test]
fn app_audio_running_on_another_thread_than_the_hub_studio_one_input_channel() {
    let mut hub = HubEngine::new();
    hub.connect(&bus("app_async"));
    hub.prepare(RATE, 2048);
    hub.maintain();
    let measurer = hub.measurer();
    let shm_bus = hub.bus().unwrap();
    let idx = claim_source(shm_bus).expect("source slot");
    let src = &shm_bus.sources[idx];
    set_source_identity(src, "music", "brave.exe", 0);
    src.flags.store(SourceFlags::kSrcOn, SeqCst);
    const N: usize = 2048;
    let d: i64 = 4800; // the App Audio output reaches the master 100 ms after App Audio wrote it
    let mut rng = 99u32;
    let mut written = 0i64; // App Audio's ring position = its own timeline
    let (mut chunk, mut l, mut r) = (vec![0.0f32; 16], vec![0.0f32; N], vec![0.0f32; N]);
    let mut h = 200_000i64; // the master timeline
    let mut cycle = |hub: &mut HubEngine| {
        // App Audio has written what this master block needs, plus 0..2500 frames more
        rng = rng.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        let want = h + N as i64 - d + i64::from((rng >> 8) % 2500);
        if written == 0 {
            written = want - 4096;
        }
        while written < want {
            for (i, c) in chunk.iter_mut().enumerate() {
                *c = music(written + i as i64);
            }
            let b = hub.bus().unwrap();
            ring_write(
                &b.sourceAudio.0[idx],
                &b.sources[idx].writePos.0,
                &[&chunk],
                16,
            );
            written += 16;
        }
        b_heartbeat(hub, idx);
        for i in 0..N {
            l[i] = music(h + i as i64 - d);
            r[i] = l[i];
        }
        hub.process(&mut [&mut l, &mut r], &HubParams::default(), h, true, false);
        h += N as i64;
    };
    let settle = (1.5 * RATE) as i64;
    let mut f = 0;
    while f < settle {
        cycle(&mut hub);
        f += N as i64;
    }
    let mut rs = vec![];
    for k in 0..5 {
        if k > 0 {
            let mut f = 0;
            while f < (0.4 * RATE) as i64 {
                cycle(&mut hub);
                f += N as i64;
            }
        }
        let (v, score) = measurer.measure_lag(idx, 0.3, false, true, false, None);
        println!("    App Audio reading {k}: {v:?} (score {score:.2})");
        assert!(score > 0.9, "score {score}");
        rs.push(v);
    }
    assert!(steady(&rs).is_some());
    let first = rs[0].expect("first reading");
    for v in rs.iter().flatten() {
        assert!((v - first).abs() < 0.5, "{v} vs {first}");
    }
}

fn b_heartbeat(hub: &HubEngine, idx: usize) {
    hub.bus().unwrap().sources[idx]
        .heartbeatNs
        .store(now_ns(), SeqCst);
}

#[test]
fn a_second_copy_of_the_music_in_the_master_does_not_pull_the_music_step_apart() {
    // the program also reaches the master another way, drifting (it played straight to the interface and
    // came back through a loopback): a user's autosync.log read 210 / 195 / 213 / 130 / 211 ms
    for sec in [0.5f32, 0.8, 1.0] {
        let mut r = Rig::new(&bus(&format!("second_{}", (sec * 10.0) as i32)));
        r.second = sec;
        r.second_lag = 150 * 48;
        let slot = r.music_track.slot_index().unwrap();
        let rs = r.readings(true, slot, None);
        let reference = steady(&rs).expect("music step");
        assert!(reference.abs() < 0.5, "second {sec}: {reference}");
        for v in &rs {
            let v = v.expect("every reading found");
            assert!(
                (v - reference).abs() < 1.0,
                "second {sec}: {v} vs {reference}"
            );
        }
    }
}
