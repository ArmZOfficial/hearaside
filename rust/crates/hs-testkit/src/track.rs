//! `TestTrack`: what the REST and friends checks need from a HEARASIDE Track plug-in: a `TrackPublisher` on the bus, the ten
//! parameters as plain values, and the mailbox commands the Hub posts (`TrackProcessor::applyCommand`). The real Rust Track
//! plug-in is R6; this stands in for it, so the checks can see "the Track took the command within a block".
use hs_bus::bus::{bits_float, poll_commands};
use hs_bus::protocol::ParamId;
use hs_engine::track_publisher::{Mirror, NO_TIME, PublisherStatus, TrackPublisher};
use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

/// The parameters the Hub can change, with the ranges of `TrackProcessor::createLayout`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TrackParams {
    pub mon: bool,
    pub str_: bool,
    pub solo: bool,
    pub str_gain_db: f32,
    pub str_pan: f32,
    pub str_delay_ms: f32,
    pub mon_trim_db: f32,
    pub stem: i32,
}

impl Default for TrackParams {
    fn default() -> Self {
        TrackParams {
            mon: true,
            str_: true,
            solo: false,
            str_gain_db: 0.0,
            str_pan: 0.0,
            str_delay_ms: 0.0,
            mon_trim_db: 0.0,
            stem: -1,
        }
    }
}

struct State {
    p: TrackParams,
    cursor: u32,
}

pub struct TestTrack {
    publisher: TrackPublisher,
    state: Mutex<State>,
    block: AtomicU32,
    /// 0-based position in the demo scene: the sine it plays
    index: usize,
}

impl TestTrack {
    /// A Track on `bus` named `name`. `None` when the bus has no free slot.
    pub fn connect(bus: &str, name: &str, index: usize, p: TrackParams) -> Option<TestTrack> {
        let mut publisher = TrackPublisher::new();
        if publisher.connect(bus, "", 48_000, 2) != PublisherStatus::Connected {
            return None;
        }
        publisher.prepare(48_000, 2);
        publisher.set_identity(name, 0xFF40_80C0);
        let cursor = publisher
            .slot()
            .map_or(0, |s| s.cmdWrite.load(std::sync::atomic::Ordering::Acquire));
        let t = TestTrack {
            publisher,
            state: Mutex::new(State { p, cursor }),
            block: AtomicU32::new(0),
            index,
        };
        t.mirror(&p);
        Some(t)
    }

    fn mirror(&self, p: &TrackParams) {
        self.publisher.mirror(&Mirror {
            mon: p.mon,
            str_: p.str_,
            solo: p.solo,
            gain_db: p.str_gain_db,
            pan: p.str_pan,
            delay_ms: p.str_delay_ms,
            trim_db: p.mon_trim_db,
            stem: p.stem,
            app: false,
        });
    }

    pub fn params(&self) -> TrackParams {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).p
    }

    /// Changes the parameters the way a click in the Track does.
    pub fn update(&self, f: impl FnOnce(&mut TrackParams)) {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        f(&mut s.p);
        self.mirror(&s.p);
    }

    pub fn slot(&self) -> Option<usize> {
        self.publisher.slot_index()
    }

    /// The Track's timer: takes the commands the Hub posted (`applyCommand`) and the Hub's rename request.
    pub fn service(&self) {
        let Some(slot) = self.publisher.slot() else {
            return;
        };
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let mut cursor = s.cursor;
        let mut p = s.p;
        poll_commands(slot, &mut cursor, |id, v| match id {
            ParamId::Mon => p.mon = v > 0.5,
            ParamId::Str => p.str_ = v > 0.5,
            ParamId::StrGainDb => p.str_gain_db = v.clamp(-60.0, 12.0),
            ParamId::StrPan => p.str_pan = (v * 100.0).clamp(-100.0, 100.0),
            ParamId::StrDelayMs => p.str_delay_ms = v.clamp(0.0, 500.0),
            ParamId::MonTrimDb => p.mon_trim_db = v.clamp(-60.0, 6.0),
            ParamId::StrSolo => p.solo = v > 0.5,
            ParamId::StemIndex => p.stem = (v + 0.5).floor() as i32,
            _ => {}
        });
        s.cursor = cursor;
        if p != s.p {
            s.p = p;
            self.mirror(&p);
        }
    }

    /// One audio block of the demo scene: a sine per Track (`0.3 / (i + 1)`, `0.03 * (i + 1)` rad per sample), published to the
    /// bus; `out` gets the same samples (what the Track hands on to the master).
    pub fn process_demo(&self, out: &mut [Vec<f32>; 2]) {
        let b = u64::from(self.block.fetch_add(1, Relaxed));
        let k = (self.index + 1) as f64;
        let n = out[0].len();
        let [left, right] = &mut *out;
        for (s, (l, r)) in left.iter_mut().zip(right.iter_mut()).enumerate() {
            // f64: the phase grows for hours and an f32 one loses its steps
            let v = (0.3 / k * (((b * n as u64 + s as u64) as f64) * 0.03 * k).sin()) as f32;
            *l = v;
            *r = v;
        }
        self.publisher
            .process(&[&out[0], &out[1]], n, NO_TIME, false, false, false);
    }

    /// Meter: the louder input channel.
    pub fn peak_in(&self) -> f32 {
        self.publisher.slot().map_or(0.0, |s| {
            bits_float(s.peakInBits[0].load(Relaxed)).max(bits_float(s.peakInBits[1].load(Relaxed)))
        })
    }
}
