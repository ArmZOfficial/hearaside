//! Consumer side of the bus (OBS source, Bridge): reads a stream output from shared memory,
//! compensates clock drift and converts the sample rate with a variable-ratio resampler.
//! Port of `ssdsp::StreamConsumer` (`libs/ssdsp/src/consumer.cpp`).
//!
//! Known gap kept from C++ (PLAN R2.4): the first `pull` after `configure` opens the shared memory and
//! builds the resampler, which allocates. A host that cannot tolerate that must call `pull` once from a
//! non-critical thread first. `VarResampler::set_correction_ppm` can also make Speex reallocate its filter
//! when downsampling (for example DAW 48 kHz to OBS 44.1 kHz) as the correction moves; that is inherited too.
use crate::dsp::DriftController;
use crate::resampler::VarResampler;
use hs_bus::bus::{hub_alive, read_stream_name};
use hs_bus::protocol::{kGuardFrames, kNameBytes, kNumStreamOuts, kRingFrames};
use hs_bus::ring::{ReadResult, ring_read_at};
use hs_bus::shm::{SharedMemory, Status, now_ns};
use std::sync::atomic::Ordering::{Acquire, Relaxed};

#[derive(Debug, Clone, Default)]
pub struct ConsumerStatus {
    /// shared memory open
    pub connected: bool,
    /// Hub heartbeat fresh
    pub hub_alive: bool,
    /// output currently active
    pub streaming: bool,
    pub daw_rate: u32,
    /// current (filtered) buffer fill
    pub buffer_ms: f64,
    pub target_ms: f64,
    /// target chosen from measured timing
    pub auto_buffer: bool,
    /// current correction
    pub drift_ppm: f64,
    pub underruns: u64,
    pub overruns: u64,
    pub resyncs: u64,
    pub hub_flags: u32,
    pub output_name: String,
    pub error: String,
}

pub struct StreamConsumer {
    bus_name: String,
    output: usize,
    buffer_ms: f64,
    auto: bool,
    /// current adaptive target
    auto_ms: f64,
    /// worst margin (frames) in the current window
    min_margin: f64,
    window_sec: f64,
    lower_votes: i32,
    out_rate: u32,
    report_to_hub: bool,

    shm: Option<SharedMemory>,
    rs: Option<VarResampler>,
    drift: DriftController,
    cursor: u64,
    daw_rate: u32,
    need_resync: bool,
    reconnect_timer: f64,
    fill_filtered: f64,
    /// 0..1 output fade (for disconnect / hub loss)
    fade: f32,
    in_buf: [Vec<f32>; 2],
    name: [u8; kNameBytes],
    last_error: Option<Status>,
    st: ConsumerStatus,
}

fn silence(out: &mut [&mut [f32]; 2], frames: usize) {
    for o in out.iter_mut() {
        o[..frames].fill(0.0);
    }
}

impl Default for StreamConsumer {
    fn default() -> Self {
        Self::new()
    }
}

impl StreamConsumer {
    pub fn new() -> Self {
        StreamConsumer {
            bus_name: "Main".into(),
            output: 0,
            buffer_ms: 30.0,
            auto: false,
            auto_ms: 20.0,
            min_margin: 1.0e9,
            window_sec: 0.0,
            lower_votes: 0,
            out_rate: 48000,
            report_to_hub: true,
            shm: None,
            rs: None,
            drift: DriftController::default(),
            cursor: 0,
            daw_rate: 0,
            need_resync: true,
            reconnect_timer: 0.0,
            fill_filtered: 0.0,
            fade: 0.0,
            in_buf: [
                vec![0.0; kRingFrames as usize / 4],
                vec![0.0; kRingFrames as usize / 4],
            ],
            name: [0; kNameBytes],
            last_error: None,
            st: ConsumerStatus::default(),
        }
    }

    /// Not thread safe with `pull`; call from the consumer thread or while stopped.
    /// `buffer_ms <= 0` selects the adaptive buffer: the target follows the measured worst-case margin.
    pub fn configure(&mut self, bus_name: &str, output_index: i32, buffer_ms: f64, out_rate: u32) {
        let bus_changed = bus_name != self.bus_name;
        self.bus_name = if bus_name.is_empty() {
            "Main".into()
        } else {
            bus_name.into()
        };
        self.output = output_index.clamp(0, kNumStreamOuts as i32 - 1) as usize;
        self.auto = buffer_ms <= 0.0;
        self.buffer_ms = if self.auto {
            20.0
        } else {
            buffer_ms.clamp(5.0, 500.0)
        };
        self.auto_ms = 20.0;
        self.st.auto_buffer = self.auto;
        if out_rate != self.out_rate && out_rate != 0 {
            self.rs = None;
        }
        self.out_rate = if out_rate == 0 { 48000 } else { out_rate };
        if bus_changed {
            self.shm = None;
        }
        self.need_resync = true;
        self.reconnect_timer = 1.0e9; // connect on next pull
    }

    /// Off: don't stamp `consumerHeartbeatNs` / `consumerBufferMs`, so the Hub's "OBS connected" and
    /// "delay to OBS" keep meaning OBS (the Hub's own virtual mic reads the bus too).
    pub fn set_report_to_hub(&mut self, on: bool) {
        self.report_to_hub = on;
    }

    /// Snapshot (copy); call from the same thread as `pull`.
    pub fn status(&self) -> ConsumerStatus {
        let mut s = self.st.clone();
        let n = self.name.iter().position(|&c| c == 0).unwrap_or(kNameBytes);
        s.output_name = String::from_utf8_lossy(&self.name[..n]).into_owned();
        s.error = self.last_error.map(|e| e.to_string()).unwrap_or_default();
        s
    }

    /// `status()` without the copy: the numbers only (`output_name` and `error` stay empty, `status()` fills them).
    /// Allocates nothing, so a real-time thread can publish it every block (the OBS source does).
    pub fn status_ref(&self) -> &ConsumerStatus {
        &self.st
    }

    fn try_connect(&mut self) {
        match SharedMemory::open_existing(&self.bus_name) {
            Ok(s) => {
                self.shm = Some(s);
                self.last_error = None;
            }
            Err(e) => {
                self.shm = None;
                self.st.error = if e == Status::CreateFailed {
                    String::new()
                } else {
                    e.to_string()
                };
            }
        }
        self.st.connected = self.shm.is_some();
        self.need_resync = true;
    }

    fn resync(&mut self, daw_rate: u32) {
        let Some(shm) = self.shm.as_ref() else { return };
        let l = shm.layout();
        let fresh = !matches!(&self.rs, Some(r) if r.in_rate() == daw_rate && r.out_rate() == self.out_rate);
        if fresh {
            self.rs = VarResampler::new(daw_rate, self.out_rate, 5).ok();
        } else if let Some(r) = self.rs.as_mut() {
            r.reset();
        }
        if let Some(r) = self.rs.as_mut() {
            r.set_correction_ppm(0);
        }
        self.daw_rate = daw_rate;

        let hub_block = f64::from(l.header.hubBlockSize.load(Relaxed));
        let rate = f64::from(daw_rate);
        let target = if self.auto {
            (self.auto_ms * 0.001 * rate).max(hub_block * 1.25)
        } else {
            (self.buffer_ms * 0.001 * rate).max(hub_block * 1.5 + 0.012 * rate)
        };
        self.min_margin = 1.0e9;
        self.window_sec = 0.0;
        self.lower_votes = 0;
        self.drift.reset(target);
        let w = l.streamHeader.writePos.0[self.output].load(Acquire);
        self.cursor = w.saturating_sub(target as u64);
        self.fill_filtered = target;
        self.need_resync = false;
        self.st.resyncs += 1;
    }

    /// Produces exactly `frames` frames at the output rate into `out[0]`, `out[1]` (planar). Always
    /// fills the buffers (silence when disconnected). `dt_sec`: time since the previous call.
    pub fn pull(&mut self, mut out: [&mut [f32]; 2], frames: u32, dt_sec: f64) {
        // never write past the caller's buffers
        let frames = frames.min(out[0].len().min(out[1].len()).min(u32::MAX as usize) as u32);
        let n = frames as usize;
        if self.shm.is_none() {
            self.reconnect_timer += dt_sec;
            if self.reconnect_timer >= 1.0 {
                self.reconnect_timer = 0.0;
                self.try_connect();
            }
            if self.shm.is_none() {
                self.st.connected = false;
                self.st.hub_alive = false;
                self.st.streaming = false;
                silence(&mut out, n);
                return;
            }
        }

        let (hub_ok, mut daw_rate, flags) = {
            let l = self.shm.as_ref().expect("checked above").layout();
            if self.report_to_hub {
                l.header.consumerHeartbeatNs.store(now_ns(), Relaxed);
                let ms = if self.auto {
                    self.auto_ms
                } else {
                    self.buffer_ms
                };
                l.header.consumerBufferMs.store(ms.round() as u32, Relaxed);
            }
            (
                hub_alive(l, 500_000_000) && l.streamHeader.active.load(Acquire) != 0,
                l.streamHeader.sampleRate.load(Acquire),
                l.header.hubFlags.load(Relaxed),
            )
        };
        self.st.connected = true;
        self.st.hub_alive = hub_ok;
        self.st.hub_flags = flags;
        self.st.daw_rate = daw_rate;

        let rate_ok = (8000..=384_000).contains(&daw_rate);
        if (!hub_ok || !rate_ok) && !(self.fade > 0.0 && self.rs.is_some() && !self.need_resync) {
            // Fade out whatever is left (10 ms), then go silent and resync when the Hub comes back.
            self.fade = 0.0;
            self.need_resync = true;
            self.st.streaming = false;
            silence(&mut out, n);
            return;
        }
        let use_rate = if hub_ok && rate_ok {
            daw_rate
        } else {
            self.daw_rate
        };
        if self.need_resync || use_rate != self.daw_rate {
            self.resync(use_rate);
            self.fade = 0.0;
        }
        daw_rate = use_rate;
        if self.rs.is_none() {
            // the resampler could not be built (unusable rate): stay silent and try again next time
            self.need_resync = true;
            self.st.streaming = false;
            silence(&mut out, n);
            return;
        }

        let output = self.output;
        let w = self
            .shm
            .as_ref()
            .expect("connected")
            .layout()
            .streamHeader
            .writePos
            .0[output]
            .load(Acquire);
        let fill = w.wrapping_sub(self.cursor) as i64 as f64;
        if fill < 0.0 || fill > f64::from(kRingFrames - kGuardFrames) {
            self.st.overruns += 1;
            self.resync(daw_rate);
            self.fade = 0.0;
            silence(&mut out, n);
            return;
        }

        let c = self.drift.update(fill, dt_sec);
        let rs_need = {
            let rs = self.rs.as_mut().expect("resampler built by resync");
            rs.set_correction_ppm((c * 1.0e6).round() as i32);
            rs.input_for(frames)
        };
        self.fill_filtered += (fill - self.fill_filtered) * (1.0 - (-dt_sec / 0.5).exp());
        if self.auto {
            self.adapt(fill - f64::from(rs_need), dt_sec);
        }

        // Resample until the block is full. One pass is normally enough; a second one covers the
        // fractional phase of the resampler. Never pad with zeros mid-stream (= audible click).
        let mut made = 0u32;
        let mut pass = 0;
        while pass < 3 && made < frames {
            pass += 1;
            let need = self
                .rs
                .as_ref()
                .expect("resampler")
                .input_for(frames - made)
                .min(self.in_buf[0].len() as u32);
            let mut avail = 0u32;
            let res = {
                let l = self.shm.as_ref().expect("connected").layout();
                let [b0, b1] = &mut self.in_buf;
                ring_read_at(
                    &l.streamAudio.0[output],
                    &l.streamHeader.writePos.0[output],
                    self.cursor,
                    &mut [&mut b0[..need as usize], &mut b1[..need as usize]],
                    need,
                    Some(&mut avail),
                )
            };
            if res == ReadResult::Overrun {
                self.st.overruns += 1;
                self.resync(daw_rate);
                self.fade = 0.0;
                silence(&mut out, n);
                return;
            }
            if avail == 0 {
                break;
            }
            let (mut in_used, mut out_made) = (avail, frames - made);
            {
                let [b0, b1] = &self.in_buf;
                let [o0, o1] = &mut out;
                let m = made as usize;
                self.rs.as_mut().expect("resampler").process(
                    [&b0[..avail as usize], &b1[..avail as usize]],
                    &mut in_used,
                    [&mut o0[m..n], &mut o1[m..n]],
                    &mut out_made,
                );
            }
            self.cursor += u64::from(in_used);
            made += out_made;
            if out_made == 0 && in_used == 0 {
                break;
            }
        }
        if made < frames {
            // Not enough data: the DAW stalled or the buffer is too small. Rebuild the buffer.
            self.st.underruns += 1;
            if self.auto {
                self.auto_ms = (self.auto_ms + 5.0).min(120.0); // be safer from now on
            }
            self.need_resync = true;
            silence(&mut out, n);
            self.st.streaming = false;
            return;
        }

        // 10 ms fades for connect / disconnect
        let fade_target = if hub_ok { 1.0f32 } else { 0.0 };
        let step = 1.0 / (0.010 * self.out_rate as f32);
        if self.fade != fade_target || fade_target < 1.0 {
            let [o0, o1] = &mut out;
            for (a, b) in o0[..n].iter_mut().zip(&mut o1[..n]) {
                self.fade = if fade_target > self.fade {
                    fade_target.min(self.fade + step)
                } else {
                    fade_target.max(self.fade - step)
                };
                *a *= self.fade;
                *b *= self.fade;
            }
        }
        if !hub_ok && self.fade <= 0.0 {
            self.need_resync = true;
        }

        self.st.streaming = hub_ok;
        self.st.buffer_ms = self.fill_filtered * 1000.0 / f64::from(daw_rate);
        self.st.target_ms = self.drift.target() * 1000.0 / f64::from(daw_rate);
        self.st.drift_ppm = c * 1.0e6;

        // output name (kept in a fixed buffer: no allocation here)
        if let Some(nm) = read_stream_name(
            &self.shm.as_ref().expect("connected").layout().streamHeader,
            output,
        ) {
            self.name = nm;
        }
    }

    /// Adaptive buffer: every 2 s look at the worst margin (fill minus what the next read needs) and
    /// move the target so that this worst case keeps ~3 ms of headroom. Up immediately, down by at
    /// most 2 ms per window and only once the fill has settled on the current target.
    fn adapt(&mut self, margin_frames: f64, dt_sec: f64) {
        self.min_margin = self.min_margin.min(margin_frames);
        self.window_sec += dt_sec; // stream time, not wall time
        if self.window_sec < 2.0 {
            return;
        }
        let fpm = (f64::from(self.daw_rate) * 0.001).max(8.0); // frames per ms
        let margin_ms = self.min_margin / fpm;
        let fill_ms = self.fill_filtered / fpm;
        const SAFETY_MS: f64 = 3.0;
        let mut next = self.auto_ms;
        if margin_ms < SAFETY_MS {
            next = self.auto_ms.max(fill_ms + (SAFETY_MS - margin_ms) + 1.0);
            self.lower_votes = 0;
        } else if margin_ms > SAFETY_MS + 1.0 && (fill_ms - self.auto_ms).abs() < 1.5 {
            self.lower_votes += 1;
            if self.lower_votes >= 3 {
                next = (fill_ms - (margin_ms - SAFETY_MS)).max(self.auto_ms - 2.0);
            }
        } else {
            self.lower_votes = 0;
        }
        next = next.clamp(4.0, 120.0);
        if (next - self.auto_ms).abs() > 0.05 {
            self.auto_ms = next;
            self.drift.set_target(self.auto_ms * fpm);
        }
        self.min_margin = 1.0e9;
        self.window_sec = 0.0;
    }
}
