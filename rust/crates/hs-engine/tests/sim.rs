//! Host simulations for the Track publisher + Hub engine (port of `tests/unit/test_engine.cpp`):
//! null tests under the host behaviours of plan sections 4.3 / 6.4 / 8 (block sizes, ahead
//! processing, ordering, seek...). The latency-measuring case moves with `measureLatencies` (R2.2b);
//! the consumer end-to-end cases that need the Hub are here too.
use hs_bus::bus::bits_float;
use hs_bus::protocol::*;
use hs_bus::ring::ring_read_at;
use hs_bus::shm::current_pid;
use hs_dsp::consumer::StreamConsumer;
use hs_dsp::dsp::db_to_gain;
use hs_engine::hub_engine::{HubEngine, HubParams, Role};
use hs_engine::track_publisher::{Mirror, NO_TIME, PublisherStatus, TrackPublisher};
use std::sync::atomic::Ordering::SeqCst;

fn unique_bus(tag: &str) -> String {
    // a counter, not the clock: two tests starting in the same tick shared a bus (the second Hub was not the owner)
    static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    format!("sim_{tag}_{}_{n}", current_pid())
}

/// Deterministic programme material as a function of the timeline sample.
fn signal_at(t: i64) -> f32 {
    let x = t as f64;
    (0.25 * (x * 0.0131).sin() + 0.15 * (x * 0.0577 + 1.0).sin() + 0.05 * (x * 0.31).sin()) as f32
}

/// Deterministic noise (music correlates like noise, not like a few sines).
fn noise_at(t: i64, seed: u32) -> f32 {
    let mut h = (t as u32).wrapping_mul(2_654_435_761) ^ seed.wrapping_mul(40503);
    h ^= h >> 15;
    h = h.wrapping_mul(2_246_822_519);
    h ^= h >> 13;
    h = h.wrapping_mul(3_266_489_917);
    h ^= h >> 16;
    ((h & 0xffff) as f32 / 65535.0 - 0.5) * 0.5
}

fn mirror(mon: bool, str_: bool, solo: bool, gain: f32, delay_ms: f32, stem: i32) -> Mirror {
    Mirror {
        mon,
        str_,
        solo,
        gain_db: gain,
        pan: 0.0,
        delay_ms,
        trim_db: 0.0,
        stem,
        app: false,
    }
}

struct SimTrack {
    publisher: TrackPublisher,
    sign: f32,
    /// content = sign * signal(t - content_delay)
    content_delay: i64,
    /// latency of the plug-ins before the Track (the DAW lines the master up)
    chain: i64,
    /// != 0: noise material instead of `signal_at`
    seed: u32,
    /// render cycle whose first sample is NaN (a broken plug-in before the Track)
    nan_at: Option<usize>,
    /// host renders this many cycles ahead (anticipative FX / ASIO-Guard)
    ahead_blocks: i64,
    /// processed after the master bus in each cycle
    after_hub: bool,
    suspended: bool,
    rendered_cycles: i64,
}

impl SimTrack {
    fn at(&self, t: i64) -> f32 {
        self.sign
            * if self.seed != 0 {
                noise_at(t, self.seed)
            } else {
                signal_at(t)
            }
    }
}

struct Sim {
    bus: String,
    hub: HubEngine,
    tracks: Vec<SimTrack>,
    params: HubParams,
    sizes: Vec<usize>,
    times: Vec<i64>,
    playing: bool,
    stream_read: u64,
    stream_cursor_init: bool,
    out_l: Vec<f32>,
    out_r: Vec<f32>,
    stream_l: Vec<f32>,
    stream_r: Vec<f32>,
    master_out: Vec<f32>,
    stem_to_capture: Option<usize>,
    stem_l: Vec<f32>,
    /// cycle whose first master-bus sample is NaN
    master_nan_at: Option<usize>,
    /// a plug-in on the master bus above the Hub: latency in frames...
    master_fx_delay: i64,
    /// ...and gain
    master_fx_gain: f32,
}

/// Tiny deterministic generator for the block sizes (the C++ uses mt19937; only the spread matters).
struct Rng(u64);
impl Rng {
    fn range(&mut self, lo: usize, hi: usize) -> usize {
        self.0 = self
            .0
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        lo + ((self.0 >> 33) as usize) % (hi - lo + 1)
    }
}

impl Sim {
    fn new(tag: &str, cycles: usize) -> Self {
        Self::with_blocks(tag, cycles, 256, 256, 7)
    }

    fn with_blocks(
        tag: &str,
        cycles: usize,
        block_min: usize,
        block_max: usize,
        seed: u64,
    ) -> Self {
        let bus = unique_bus(tag);
        let mut hub = HubEngine::new();
        assert!(hub.connect(&bus), "hub connect failed");
        hub.prepare(48000.0, 1024);
        hub.maintain();
        let mut rng = Rng(seed);
        let (mut sizes, mut times) = (vec![], vec![]);
        let mut t = 100_000i64;
        for _ in 0..cycles + 64 {
            let n = rng.range(block_min, block_max);
            sizes.push(n);
            times.push(t);
            t += n as i64;
        }
        Sim {
            bus,
            hub,
            tracks: vec![],
            params: HubParams::default(),
            sizes,
            times,
            playing: true,
            stream_read: 0,
            stream_cursor_init: false,
            out_l: vec![],
            out_r: vec![],
            stream_l: vec![],
            stream_r: vec![],
            master_out: vec![],
            stem_to_capture: None,
            stem_l: vec![],
            master_nan_at: None,
            master_fx_delay: 0,
            master_fx_gain: 1.0,
        }
    }

    fn add_track(&mut self, sign: f32, ahead: i64, after_hub: bool) -> usize {
        let mut publisher = TrackPublisher::new();
        publisher.connect(&self.bus, "", 48000, 1);
        publisher.prepare(48000, 1);
        self.tracks.push(SimTrack {
            publisher,
            sign,
            content_delay: 0,
            chain: 0,
            seed: 0,
            nan_at: None,
            ahead_blocks: ahead,
            after_hub,
            suspended: false,
            rendered_cycles: 0,
        });
        self.tracks.len() - 1
    }

    /// Seek: the host restarts the timeline at `new_time` from cycle `from` on; ahead-rendered audio is
    /// discarded by the host and re-rendered.
    fn seek(&mut self, from: usize, new_time: i64) {
        let mut t = new_time;
        for c in from..self.times.len() {
            self.times[c] = t;
            t += self.sizes[c] as i64;
        }
        for tr in &mut self.tracks {
            tr.rendered_cycles = tr.rendered_cycles.min(from as i64);
        }
    }

    fn render_track(&mut self, k: usize, c: usize) {
        let playing = self.playing;
        let tr = &mut self.tracks[k];
        if tr.suspended {
            tr.rendered_cycles = tr.rendered_cycles.max(c as i64 + 1);
            return;
        }
        let until = c as i64 + 1 + tr.ahead_blocks;
        while tr.rendered_cycles < until {
            let cyc = tr.rendered_cycles as usize;
            let n = self.sizes[cyc];
            let mut buf: Vec<f32> = (0..n)
                .map(|i| tr.at(self.times[cyc] + i as i64 - tr.content_delay - tr.chain))
                .collect();
            if tr.nan_at == Some(cyc) {
                buf[0] = f32::NAN;
            }
            tr.publisher
                .process(&[&buf], n, self.times[cyc], playing, false, false);
            tr.rendered_cycles += 1;
        }
    }

    fn run_cycle(&mut self, c: usize) {
        for k in 0..self.tracks.len() {
            if !self.tracks[k].after_hub {
                self.render_track(k, c);
            }
        }
        let n = self.sizes[c];
        // master bus input: every Track sends what viewers hear (Viewers Hear / Level / Delay)
        let mut l = vec![0.0f32; n];
        let slowest = self.tracks.iter().map(|t| t.chain).max().unwrap_or(0);
        for tr in &self.tracks {
            let Some(sh) = tr.publisher.slot() else {
                continue;
            };
            if tr.suspended || sh.flags.load(SeqCst) & SlotFlags::kFlagStr == 0 {
                continue;
            }
            let g = db_to_gain(bits_float(sh.strGainBits.load(SeqCst)));
            let d = (bits_float(sh.strDelayBits.load(SeqCst)) * 48.0 + 0.5) as i64;
            for (i, x) in l.iter_mut().enumerate() {
                *x += self.master_fx_gain
                    * g
                    * tr.at(self.times[c] + i as i64
                        - tr.content_delay
                        - d
                        - slowest
                        - self.master_fx_delay);
            }
        }
        if self.master_nan_at == Some(c) {
            l[0] = f32::NAN;
        }
        let mut r = l.clone();
        self.hub.process(
            &mut [&mut l, &mut r],
            &self.params,
            if self.playing { self.times[c] } else { NO_TIME },
            self.playing,
            false,
        );
        self.master_out.extend_from_slice(&l);
        self.out_l.extend_from_slice(&l);
        self.out_r.extend_from_slice(&r);

        for k in 0..self.tracks.len() {
            if self.tracks[k].after_hub {
                self.render_track(k, c);
            }
        }

        // capture what the Hub wrote this cycle
        let b = self.hub.bus().unwrap();
        let w = b.streamHeader.writePos.0[0].load(SeqCst);
        if !self.stream_cursor_init {
            self.stream_read = w - n as u64;
            self.stream_cursor_init = true;
        }
        let len = (w - self.stream_read) as usize;
        let (mut a, mut bb) = (vec![0.0f32; len], vec![0.0f32; len]);
        ring_read_at(
            &b.streamAudio.0[0],
            &b.streamHeader.writePos.0[0],
            self.stream_read,
            &mut [&mut a, &mut bb],
            len as u32,
            None,
        );
        if let Some(stem) = self.stem_to_capture {
            let (mut s0, mut s1) = (vec![0.0f32; len], vec![0.0f32; len]);
            ring_read_at(
                &b.streamAudio.0[1 + stem],
                &b.streamHeader.writePos.0[1 + stem],
                self.stream_read,
                &mut [&mut s0, &mut s1],
                len as u32,
                None,
            );
            self.stem_l.extend_from_slice(&s0);
        }
        self.stream_read = w;
        self.stream_l.extend_from_slice(&a);
        self.stream_r.extend_from_slice(&bb);
    }

    fn run(&mut self, from: usize, to: usize) {
        for c in from..to {
            self.run_cycle(c);
        }
    }

    fn peak(v: &[f32], from: usize, to: usize) -> f32 {
        v[from.min(v.len())..to.min(v.len())]
            .iter()
            .fold(0.0, |p, x| p.max(x.abs()))
    }

    fn stream_peak_after(&self, skip: usize) -> f32 {
        Self::peak(&self.stream_l, skip, usize::MAX).max(Self::peak(
            &self.stream_r,
            skip,
            usize::MAX,
        ))
    }

    /// peak of the headphone output after skipping `skip` frames
    fn peak_after(&self, skip: usize) -> f32 {
        Self::peak(&self.out_l, skip, usize::MAX).max(Self::peak(&self.out_r, skip, usize::MAX))
    }

    fn peak_range(&self, from: usize, to: usize) -> f32 {
        Self::peak(&self.out_l, from, to).max(Self::peak(&self.out_r, from, to))
    }
}

/// -90 dBFS
const NULL: f32 = 3.2e-5;

/// A null test passes for the wrong reason if the Hub outputs silence (the two tracks cancel on every
/// output, so the mixes cannot tell): the Hub's own per-track viewers meter shows the track is mixed.
fn assert_stream_alive(s: &Sim) {
    let sh = s.tracks[0].publisher.slot().unwrap();
    let p = bits_float(sh.peakStreamBits[0].load(SeqCst));
    assert!(p > 0.05, "track 0 never reached the Hub's mix: meter {p}");
}

#[test]
fn null_test_two_realtime_tracks_fixed_256_blocks() {
    let mut s = Sim::new("null", 400);
    s.add_track(1.0, 0, false);
    s.add_track(-1.0, 0, false);
    s.run(0, 400);
    assert!(s.peak_after(2048) < NULL, "{}", s.peak_after(2048));
    assert_stream_alive(&s);
}

#[test]
fn polarity_check_same_sign_doubles() {
    let mut s = Sim::new("sum", 200);
    s.add_track(1.0, 0, false);
    s.add_track(1.0, 0, false);
    s.run(0, 200);
    assert!(s.peak_after(4800) > 0.5);
}

#[test]
fn null_test_with_irregular_block_sizes_fl_studio_style() {
    let mut s = Sim::with_blocks("irregular", 600, 31, 900, 3);
    s.add_track(1.0, 0, false);
    s.add_track(-1.0, 0, false);
    s.run(0, 600);
    assert!(s.peak_after(4096) < NULL, "{}", s.peak_after(4096));
    assert_stream_alive(&s);
}

#[test]
fn ahead_processing_8_blocks_aligned_through_timeline_tags() {
    let mut s = Sim::new("ahead", 500);
    s.add_track(1.0, 8, false); // backing rendered ahead
    s.add_track(-1.0, 0, false); // live input, realtime
    s.run(0, 500);
    assert!(s.peak_after(4096) < NULL, "{}", s.peak_after(4096));
    assert_stream_alive(&s);
}

#[test]
fn ahead_processing_with_irregular_blocks_and_a_seek() {
    let mut s = Sim::with_blocks("aheadseek", 900, 64, 600, 11);
    s.add_track(1.0, 5, false);
    s.add_track(-1.0, 0, false);
    s.run(0, 400);
    let mark = s.out_l.len();
    s.seek(400, 5_000_000);
    s.run(400, 900);
    assert!(s.peak_range(4096, mark) < NULL);
    assert!(
        s.peak_after(mark + 4096) < NULL,
        "re-locked after the seek: {}",
        s.peak_after(mark + 4096)
    );
    assert_stream_alive(&s);
}

#[test]
fn track_processed_after_the_master_needs_sync_safety_1() {
    {
        let mut s = Sim::new("after0", 300);
        s.add_track(1.0, 0, false);
        s.add_track(-1.0, 0, true);
        s.run(0, 300);
        assert!(s.peak_after(4096) > 1e-3); // documented 1-block offset without sync safety
    }
    {
        let mut s = Sim::new("after1", 300);
        s.params.sync_safety = 1;
        s.add_track(1.0, 0, false);
        s.add_track(-1.0, 0, true);
        s.run(0, 300);
        assert!(s.peak_after(4096) < NULL, "{}", s.peak_after(4096));
        assert_stream_alive(&s);
    }
}

#[test]
fn transport_stopped_no_timeline_still_nulls_for_tracks_before_the_master() {
    let mut s = Sim::new("stopped", 300);
    s.playing = false;
    s.add_track(1.0, 0, false);
    s.add_track(-1.0, 0, false);
    s.run(0, 300);
    assert!(s.peak_after(4096) < NULL, "{}", s.peak_after(4096));
    assert_stream_alive(&s);
}

#[test]
fn suspended_track_is_reanchored_when_it_resumes() {
    let mut s = Sim::new("suspend", 600);
    s.add_track(1.0, 0, false);
    let b = s.add_track(-1.0, 0, false);
    s.run(0, 200);
    s.tracks[b].suspended = true;
    s.run(200, 300);
    s.tracks[b].suspended = false;
    let mark = s.out_l.len();
    s.run(300, 600);
    assert!(
        s.peak_after(mark + 4096) < NULL,
        "{}",
        s.peak_after(mark + 4096)
    );
    assert!(s.peak_range(mark - 20000, mark) > 0.1); // while suspended only A was audible
    assert_stream_alive(&s);
}

#[test]
fn you_hear_off_viewers_hear_panic_and_solo() {
    let mut s = Sim::new("gates", 600);
    let a = s.add_track(1.0, 0, false);
    let b = s.add_track(1.0, 0, false);
    s.run(0, 100);
    assert!(s.peak_after(s.out_l.len() - 10000) > 0.5); // you hear both
    s.tracks[a]
        .publisher
        .mirror(&mirror(false, true, false, 0.0, 0.0, -1)); // You Hear off: viewers only
    s.tracks[b]
        .publisher
        .mirror(&mirror(false, true, false, 0.0, 0.0, -1));
    s.run(100, 200);
    assert!(s.peak_after(s.out_l.len() - 15000) < NULL);
    assert!(s.stream_peak_after(s.stream_l.len() - 10000) > 0.5); // ...and viewers still hear them

    s.tracks[a]
        .publisher
        .mirror(&mirror(true, true, false, 0.0, 0.0, -1));
    s.params.panic = true;
    s.run(200, 300);
    assert!(s.stream_peak_after(s.stream_l.len() - 15000) < NULL);
    assert!(s.peak_after(s.out_l.len() - 10000) > 0.1); // panic never touches the headphones
    s.params.panic = false;

    assert!(!s.tracks[a].publisher.solo_active());
    s.tracks[b]
        .publisher
        .mirror(&mirror(false, true, true, 0.0, 0.0, -1)); // the Tracks mute themselves for the viewers' solo
    assert!(s.tracks[a].publisher.solo_active());
}

#[test]
fn gain_is_smoothed_no_step_larger_than_the_material_allows() {
    let mut s = Sim::new("ramp", 300);
    let a = s.add_track(1.0, 0, false);
    s.run(0, 100);
    let m = s.out_l.len();
    s.tracks[a]
        .publisher
        .mirror(&mirror(false, true, false, 0.0, 0.0, -1)); // You Hear off: headphones fade out
    s.run(100, 300);
    let max_jump =
        (m..s.out_l.len()).fold(0.0f32, |p, i| p.max((s.out_l[i] - s.out_l[i - 1]).abs()));
    assert!(max_jump < 0.05, "{max_jump}"); // material slope ~0.03/sample; a hard cut would jump ~0.45
}

#[test]
fn stems_carry_only_their_tracks() {
    let mut s = Sim::new("stems", 200);
    let a = s.add_track(1.0, 0, false);
    s.add_track(-1.0, 0, false); // B cancels A on the main mix but is not on the stem
    s.tracks[a]
        .publisher
        .mirror(&mirror(true, true, false, 0.0, 0.0, 2));
    s.stem_to_capture = Some(2);
    s.run(0, 200);
    assert!(s.peak_after(4096) < NULL);
    let stem_peak = s.stem_l[4096..].iter().fold(0.0f32, |p, x| p.max(x.abs()));
    assert!(stem_peak > 0.2, "{stem_peak}");
}

#[test]
fn sample_rate_mismatch_excludes_the_track_and_reports_it() {
    let mut s = Sim::new("rate", 200);
    s.add_track(1.0, 0, false);
    let b = s.add_track(1.0, 0, false);
    s.tracks[b].publisher.prepare(44100, 1);
    s.run(0, 200);
    let st = s.tracks[b].publisher.slot().unwrap().hubStatus.load(SeqCst);
    assert!(st & SlotHubStatus::kHubStatusRateMismatch != 0);
    assert!((f64::from(s.peak_after(4096)) - 0.45).abs() < 0.06); // only A
}

#[test]
fn limiter_keeps_the_stream_under_the_ceiling() {
    let mut s = Sim::new("limit", 300);
    for _ in 0..6 {
        s.add_track(1.0, 0, false); // ~2.7 peak
    }
    s.params.ceiling_db = -1.0;
    s.run(0, 300);
    assert!(s.stream_peak_after(4096) > 0.8);
    assert!(s.stream_peak_after(0) <= db_to_gain(-1.0) + 1e-5);
}

#[test]
fn preview_swaps_the_headphone_mix_for_the_stream_mix() {
    let mut s = Sim::new("preview", 300);
    let a = s.add_track(1.0, 0, false);
    s.tracks[a]
        .publisher
        .mirror(&mirror(false, true, false, 0.0, 0.0, -1)); // viewers only
    s.run(0, 100);
    let m = Sim::peak(&s.master_out, 4096, usize::MAX);
    assert_eq!(m, 0.0);
    s.params.preview = true;
    s.run(100, 300);
    let m = Sim::peak(&s.master_out, s.master_out.len() - 10000, usize::MAX);
    assert!(m > 0.2, "{m}");
}

#[test]
fn second_hub_on_the_same_bus_is_secondary_and_passes_audio_through() {
    let s = Sim::new("twohubs", 50);
    let mut second = HubEngine::new();
    second.connect(&s.bus);
    second.prepare(48000.0, 1024);
    second.maintain();
    assert_eq!(s.hub.role(), Role::Owner);
    assert_eq!(second.role(), Role::Secondary);
    let (mut l, mut r) = (vec![0.3f32; 256], vec![0.3f32; 256]);
    let p = HubParams {
        preview: true,
        ..HubParams::default()
    };
    second.process(&mut [&mut l, &mut r], &p, 0, true, false);
    assert_eq!(l[100], 0.3);
}

#[test]
fn duplicated_track_gets_a_fresh_uuid() {
    let bus = unique_bus("dup");
    let mut hub = HubEngine::new();
    hub.connect(&bus);
    let (mut a, mut b) = (TrackPublisher::new(), TrackPublisher::new());
    a.connect(&bus, "same-uuid", 48000, 2);
    b.connect(&bus, "same-uuid", 48000, 2);
    assert_eq!(a.uuid(), "same-uuid");
    assert!(b.uuid() != "same-uuid" && b.status() == PublisherStatus::Connected);
}

/// Drives a Track + Hub on the DAW clock (slightly off from nominal) and pulls a consumer on its own
/// clock, as the two end-to-end cases of the C++ file do.
#[allow(clippy::too_many_arguments)]
fn e2e(
    bus: &str,
    hub_rate: u32,
    daw_rate: f64,
    amp: f32,
    cons: &mut StreamConsumer,
    out_n: usize,
    tick: f64,
    end: f64,
    warm: f64,
) -> (Vec<f32>, u64) {
    let mut hub = HubEngine::new();
    hub.connect(bus);
    hub.prepare(f64::from(hub_rate), 512);
    hub.maintain();
    let mut tr = TrackPublisher::new();
    tr.connect(bus, "", hub_rate, 1);
    tr.prepare(hub_rate, 1);
    let block = 256usize;
    let (mut t_daw, mut t_obs) = (0.0f64, 0.0f64);
    let mut timeline = 0i64;
    let (mut inp, mut l, mut r) = (
        vec![0.0f32; block],
        vec![0.0f32; block],
        vec![0.0f32; block],
    );
    let (mut ol, mut orr) = (vec![0.0f32; out_n], vec![0.0f32; out_n]);
    let mut captured = vec![];
    let mut last_under = 0;
    while t_obs < end {
        if t_daw <= t_obs {
            // 441 Hz has an exact 100-sample period at 44.1 kHz; reducing the phase keeps float precision
            for (i, x) in inp.iter_mut().enumerate() {
                *x = amp
                    * (((timeline + i as i64) % 100) as f64 * 2.0 * std::f64::consts::PI / 100.0)
                        .sin() as f32;
            }
            tr.process(&[&inp], block, timeline, true, false, false);
            l.copy_from_slice(&inp);
            r.copy_from_slice(&inp); // master bus = the Track
            hub.process(
                &mut [&mut l, &mut r],
                &HubParams::default(),
                timeline,
                true,
                false,
            );
            timeline += block as i64;
            t_daw += block as f64 / daw_rate;
            continue;
        }
        cons.pull([&mut ol, &mut orr], out_n as u32, tick);
        if t_obs > warm {
            captured.extend_from_slice(&ol);
        }
        if t_obs > warm && t_obs < warm + 2.0 * tick {
            last_under = cons.status().underruns;
        }
        t_obs += tick;
    }
    (captured, last_under)
}

#[test]
fn e2e_hub_to_consumer_44k1_to_48k_with_drift_no_underruns() {
    let bus = unique_bus("e2e");
    let mut cons = StreamConsumer::new();
    cons.configure(&bus, 0, 30.0, 48000);
    let (captured, last_under) = e2e(
        &bus,
        44100,
        44100.0 * (1.0 + 150e-6),
        0.5,
        &mut cons,
        480,
        0.010,
        120.0,
        10.0,
    );
    let st = cons.status();
    assert!(st.connected && st.hub_alive && st.streaming);
    assert_eq!(st.underruns, last_under);
    assert!(
        (st.buffer_ms - st.target_ms).abs() < 3.0,
        "{} vs {}",
        st.buffer_ms,
        st.target_ms
    );
    assert!((st.drift_ppm - 150.0).abs() < 60.0, "{}", st.drift_ppm);
    // output should be a clean sine of amplitude 0.5 (limiter ceiling -1 dBFS does not touch it)
    let pk = captured.iter().fold(0.0f32, |p, x| p.max(x.abs()));
    let max_step = captured
        .windows(2)
        .fold(0.0f64, |p, w| p.max(f64::from((w[1] - w[0]).abs())));
    assert!((f64::from(pk) - 0.5).abs() < 0.02, "{pk}");
    assert!(
        max_step < 0.5 * 2.0 * std::f64::consts::PI * 441.0 / 48000.0 * 1.2,
        "clicks: {max_step}"
    );
}

#[test]
fn consumer_the_hubs_virtual_mic_does_not_count_as_obs() {
    let bus = unique_bus("vmic");
    let mut hub = HubEngine::new();
    hub.connect(&bus);
    hub.prepare(48000.0, 256);
    hub.maintain();
    let (mut l, mut r) = (vec![0.0f32; 240], vec![0.0f32; 240]);

    let mut mic = StreamConsumer::new();
    mic.set_report_to_hub(false);
    mic.configure(&bus, 0, 30.0, 48000);
    for _ in 0..300 {
        mic.pull([&mut l, &mut r], 240, 0.005); // 1.5 s: past the 1 s reconnect timer
    }
    assert!(mic.status().connected);
    assert_eq!(
        hub.bus().unwrap().header.consumerHeartbeatNs.load(SeqCst),
        0
    );

    let mut obs = StreamConsumer::new(); // default: reports itself like the OBS source
    obs.configure(&bus, 0, 30.0, 48000);
    for _ in 0..300 {
        obs.pull([&mut l, &mut r], 240, 0.005);
    }
    assert_ne!(
        hub.bus().unwrap().header.consumerHeartbeatNs.load(SeqCst),
        0
    );
}

#[test]
fn e2e_adaptive_buffer_settles_low_without_underruns() {
    let bus = unique_bus("auto");
    let mut cons = StreamConsumer::new();
    cons.configure(&bus, 0, 0.0, 48000); // 0 = adaptive
    let (_, under_at_warm) = e2e(
        &bus,
        48000,
        48000.0 * (1.0 - 80e-6),
        0.3,
        &mut cons,
        240,
        0.005,
        180.0,
        100.0,
    );
    let st = cons.status();
    println!(
        "adaptive target {:.1} ms, fill {:.1} ms, underruns {} (at 100 s: {under_at_warm})",
        st.target_ms, st.buffer_ms, st.underruns
    );
    assert!(st.auto_buffer);
    assert_eq!(st.underruns, under_at_warm);
    assert!(st.target_ms < 18.0, "{}", st.target_ms);
    assert!((st.buffer_ms - st.target_ms).abs() < 2.0);
}

#[test]
fn engine_active_slot_bitmask_tracks_active_tracks_and_clears_deactivated_slots() {
    let bus = unique_bus("bitmask");
    let mut hub = HubEngine::new();
    assert!(hub.connect(&bus));
    hub.prepare(48000.0, 256);
    hub.maintain();

    let mut buf = vec![0.0f32; 256];
    let mut buf2 = vec![0.0f32; 256];
    let p = HubParams::default();
    // initially no slots active
    hub.process(&mut [&mut buf, &mut buf2], &p, 0, true, false);
    assert_eq!(hub.active_slots_mask(), 0);

    let mut t1 = TrackPublisher::new();
    assert_eq!(
        t1.connect(&bus, "track-1-uuid", 48000, 1),
        PublisherStatus::Connected
    );
    t1.prepare(48000, 1);
    let s1 = t1.slot_index().unwrap();
    assert!(s1 < 64);
    hub.maintain();
    hub.process(&mut [&mut buf, &mut buf2], &p, 256, true, false);
    assert_ne!(hub.active_slots_mask() & (1 << s1), 0);

    let mut t2 = TrackPublisher::new();
    assert_eq!(
        t2.connect(&bus, "track-2-uuid", 48000, 1),
        PublisherStatus::Connected
    );
    t2.prepare(48000, 1);
    let s2 = t2.slot_index().unwrap();
    assert_ne!(s1, s2);
    hub.maintain();
    hub.process(&mut [&mut buf, &mut buf2], &p, 512, true, false);
    assert_eq!(hub.active_slots_mask(), (1 << s1) | (1 << s2));

    t1.disconnect();
    // the next process clears s1 from the mask at once
    hub.process(&mut [&mut buf, &mut buf2], &p, 768, true, false);
    assert_eq!(hub.active_slots_mask() & (1 << s1), 0);
    assert_ne!(hub.active_slots_mask() & (1 << s2), 0);

    t2.disconnect();
    hub.process(&mut [&mut buf, &mut buf2], &p, 1024, true, false);
    assert_eq!(hub.active_slots_mask(), 0);
}

#[test]
fn host_blocks_larger_than_max_block_are_chunked() {
    // prepare(.., 1024) but the host delivers 1500..3000 frames per cycle
    let mut s = Sim::with_blocks("bigblocks", 120, 1500, 3000, 5);
    s.add_track(1.0, 0, false);
    s.add_track(-1.0, 0, false);
    s.run(0, 120);
    assert!(s.peak_after(8192) < NULL, "{}", s.peak_after(8192));
    assert_stream_alive(&s);
}

#[test]
fn flag_updates_from_different_threads_do_not_overwrite_each_other() {
    // Solo set by `mirror` must survive the audio thread's bypass/offline update and vice versa
    // (the C++ loaded, merged and stored, losing one of the two).
    let bus = unique_bus("flags");
    let mut t = TrackPublisher::new();
    t.connect(&bus, "", 48000, 1);
    t.prepare(48000, 1);
    let sh = t.slot().unwrap();
    let buf = vec![0.1f32; 64];
    t.process(&[&buf], 64, NO_TIME, false, false, true); // host bypass
    t.mirror(&mirror(true, true, true, 0.0, 0.0, -1)); // solo on
    let f = sh.flags.load(SeqCst);
    assert!(
        f & SlotFlags::kFlagBypassed != 0 && f & SlotFlags::kFlagSolo != 0,
        "{f:#x}"
    );
    t.process(&[&buf], 64, NO_TIME, false, false, false); // bypass off, solo must stay
    let f = sh.flags.load(SeqCst);
    assert!(
        f & SlotFlags::kFlagBypassed == 0 && f & SlotFlags::kFlagSolo != 0,
        "{f:#x}"
    );
    // a bit set by the Hub survives mirror and process
    sh.flags.fetch_or(SlotFlags::kFlagViewersViaHub, SeqCst);
    t.mirror(&mirror(true, true, false, 0.0, 0.0, -1));
    t.process(&[&buf], 64, NO_TIME, false, true, false);
    let f = sh.flags.load(SeqCst);
    assert!(
        f & SlotFlags::kFlagViewersViaHub != 0 && f & SlotFlags::kFlagOffline != 0,
        "{f:#x}"
    );
    assert_eq!(f & SlotFlags::kFlagSolo, 0);
}

#[test]
fn reconnecting_publishers_do_not_keep_old_mappings() {
    let bus = unique_bus("reconnect");
    let mut hub = HubEngine::new();
    hub.connect(&bus);
    let mut t = TrackPublisher::new();
    for _ in 0..5 {
        assert_eq!(
            t.connect(&bus, "re-uuid", 48000, 1),
            PublisherStatus::Connected
        );
        t.disconnect();
    }
    assert_eq!(t.status(), PublisherStatus::Disconnected);
    assert!(t.slot().is_none());
}

#[test]
fn measures_track_and_master_plugin_latency() {
    for master in [0i64, 960] {
        // master plug-ins: none / 20 ms
        let mut s = Sim::new("lat", 600);
        let backing = s.add_track(1.0, 0, false);
        s.tracks[backing].seed = 11; // no plug-in latency
        let vocal = s.add_track(1.0, 0, false);
        s.tracks[vocal].seed = 22;
        s.tracks[vocal].chain = 1440; // vocal plug-ins: 30 ms
        s.master_fx_delay = master;
        s.master_fx_gain = 0.5;
        s.run(0, 400);
        let m = s.hub.measurer();
        let r = m.measure_latencies();
        assert!(
            (r.master_ms - master as f64 / 48.0).abs() < 0.3,
            "master {} vs {}",
            r.master_ms,
            master as f64 / 48.0
        );
        assert!((r.tracks_ms - 30.0).abs() < 0.3, "{}", r.tracks_ms);
        let chain = |k: usize| {
            bits_float(
                s.tracks[k]
                    .publisher
                    .slot()
                    .unwrap()
                    .chainLatencyBits
                    .load(SeqCst),
            )
        };
        assert!((f64::from(chain(vocal)) - 30.0).abs() < 0.3);
        assert!(f64::from(chain(backing)).abs() < 0.3);

        // live: the backing stops, only the vocal sounds -> its remembered chain latency still counts
        s.tracks[backing]
            .publisher
            .mirror(&mirror_full(true, false, 0.0));
        s.run(400, 520);
        let live = m.measure_latencies();
        assert!(
            (live.master_ms - master as f64 / 48.0).abs() < 0.3,
            "live master {}",
            live.master_ms
        );
        assert!((live.tracks_ms - 30.0).abs() < 0.3);
    }
    {
        // a Viewers Delay (sync offset) on the backing is not plug-in latency of the other tracks
        let mut s = Sim::new("latdelay", 600);
        let backing = s.add_track(1.0, 0, false);
        s.tracks[backing].seed = 11;
        s.tracks[backing]
            .publisher
            .mirror(&mirror_full(true, true, 40.0));
        let vocal = s.add_track(1.0, 0, false);
        s.tracks[vocal].seed = 22;
        s.tracks[vocal].chain = 1440;
        s.run(0, 400);
        let r = s.hub.measurer().measure_latencies();
        assert!(r.master_ms.abs() < 0.3, "{}", r.master_ms);
        assert!((r.tracks_ms - 30.0).abs() < 0.3);
        let chain = |k: usize| {
            bits_float(
                s.tracks[k]
                    .publisher
                    .slot()
                    .unwrap()
                    .chainLatencyBits
                    .load(SeqCst),
            )
        };
        assert!((f64::from(chain(vocal)) - 30.0).abs() < 0.3);
        assert!(f64::from(chain(backing)).abs() < 0.3);
    }
    {
        // one track, plug-ins on the master only
        let mut s = Sim::new("latone", 400);
        let t = s.add_track(1.0, 0, false);
        s.tracks[t].seed = 5;
        s.master_fx_delay = 4321; // 90 ms
        s.run(0, 400);
        let r = s.hub.measurer().measure_latencies();
        assert!((r.master_ms - 4321.0 / 48.0).abs() < 0.3, "{}", r.master_ms);
        assert!(r.tracks_ms.abs() < 0.3);
    }
    let mut quiet = Sim::new("latquiet", 100);
    quiet.run(0, 100);
    assert!(quiet.hub.measurer().measure_latencies().master_ms < 0.0); // nothing to compare: no estimate
}

fn mirror_full(mon: bool, str_: bool, delay_ms: f32) -> Mirror {
    Mirror {
        mon,
        str_,
        solo: false,
        gain_db: 0.0,
        pan: 0.0,
        delay_ms,
        trim_db: 0.0,
        stem: -1,
        app: false,
    }
}

#[test]
fn a_nan_sample_never_becomes_a_measured_latency() {
    // A NaN in a Track's history is neutralised by the energy checks, but a NaN on the master bus made
    // every correlation score NaN; the C++ then took the first lag as a match (0 ms).
    let mut s = Sim::new("latnan", 400);
    let t = s.add_track(1.0, 0, false);
    s.tracks[t].seed = 11;
    s.tracks[t].nan_at = Some(380); // inside the analysed window (Track history)
    s.master_nan_at = Some(390); // and on the master bus
    s.master_fx_delay = 960;
    s.run(0, 400);
    let m = s.hub.measurer();
    let r = m.measure_latencies();
    assert!(r.master_ms.is_finite() && r.tracks_ms.is_finite(), "{r:?}");
    let slot = s.tracks[t].publisher.slot_index().unwrap();
    for faint in [false, true] {
        let (lag, score) = m.measure_lag(slot, 0.3, false, false, faint, None);
        assert!(
            lag.is_none(),
            "faint {faint}: NaN history gave a lag {lag:?} (score {score})"
        );
    }
}

#[test]
fn reconnecting_the_hub_drops_the_old_bus_history() {
    let mut s = Sim::new("latreconnect", 400);
    let t = s.add_track(1.0, 0, false);
    s.tracks[t].seed = 5;
    s.master_fx_delay = 960;
    s.run(0, 400);
    let m = s.hub.measurer();
    assert!(m.measure_latencies().master_ms >= 0.0);
    // the Hub moves to another bus: until new audio is analysed there is nothing to measure
    let other = unique_bus("latreconnect_b");
    assert!(s.hub.connect(&other));
    assert_eq!(m.history_frames(), 0);
    assert!(m.measure_latencies().master_ms < 0.0);
}
