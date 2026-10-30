//! `FriendEcho`: echo cancellation and residual suppression for a friend on speakers (port of
//! `libs/ssnet/src/aec.cpp`, SpeexDSP MDF + preprocessor).
//!
//! Differences from the C++ (see the handoff ledger): absurd `init` arguments are clamped instead of overflowing
//! `int` (rate ≤ 384 kHz, frame ≤ 1 s, filter ≤ 10 s); a frame longer than the reference ring no longer reaches
//! `std::clamp` with `lo > hi` (undefined behaviour); output shorter than the input is refused.
#![allow(clippy::needless_range_loop)] // the frame loops index four buffers at once, as the C++ did

use crate::ffi::*;
use std::ffi::{c_int, c_void};
use std::ptr;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum AecMode {
    Off = 0,
    On = 1,
    /// on after a leak was detected for `LEAK_LATCH_TICKS` readings in a row
    #[default]
    Auto = 2,
}

/// 2 s of leak readings at 20 Hz
pub const LEAK_LATCH_TICKS: u32 = 40;

pub struct FriendEcho {
    sample_rate: i32,
    frame_size: usize,
    filter_length: usize,
    mode: AecMode,
    leak_detected: bool,
    leak_run: u32,
    erle_db: f32,
    echo: *mut SpeexEchoState,
    prep: *mut SpeexPreprocessState,
    /// far-end reference (3 s)
    ref_ring: Vec<f32>,
    ref_write: usize,
    /// 16-bit buffers SpeexDSP works on
    spx_rec: Vec<i16>,
    spx_play: Vec<i16>,
    spx_out: Vec<i16>,
}

// SAFETY: the Speex states are owned by this value alone and keep no thread-bound state.
unsafe impl Send for FriendEcho {}

impl Default for FriendEcho {
    fn default() -> Self {
        FriendEcho {
            sample_rate: 48000,
            frame_size: 480,
            filter_length: 9600,
            mode: AecMode::Auto,
            leak_detected: false,
            leak_run: 0,
            erle_db: 0.0,
            echo: ptr::null_mut(),
            prep: ptr::null_mut(),
            ref_ring: Vec::new(),
            ref_write: 0,
            spx_rec: Vec::new(),
            spx_play: Vec::new(),
            spx_out: Vec::new(),
        }
    }
}

impl Drop for FriendEcho {
    fn drop(&mut self) {
        self.cleanup();
    }
}

fn to_i16(x: f32) -> i16 {
    // the C++ `int16_t(lround(clamp(x) * 32767))`: round half away from zero; NaN becomes 0 (as MSVC gave)
    (x.clamp(-1.0, 1.0) * 32767.0).round() as i16
}

impl FriendEcho {
    pub fn new() -> Self {
        Self::default()
    }

    fn cleanup(&mut self) {
        if !self.prep.is_null() {
            // SAFETY: created by `speex_preprocess_state_init`, destroyed once (before the echo state it points to).
            unsafe { speex_preprocess_state_destroy(self.prep) };
            self.prep = ptr::null_mut();
        }
        if !self.echo.is_null() {
            // SAFETY: created by `speex_echo_state_init`, destroyed once.
            unsafe { speex_echo_state_destroy(self.echo) };
            self.echo = ptr::null_mut();
        }
    }

    /// `sample_rate` typically 48000, `frame_size` samples per `process` call (480 = 10 ms), `filter_length_ms` the
    /// room tail (150–250 ms, default 200).
    pub fn init(&mut self, sample_rate: i32, frame_size: i32, filter_length_ms: i32) -> bool {
        self.cleanup();
        self.sample_rate = sample_rate.clamp(8000, 384_000);
        let rate = self.sample_rate as usize;
        self.frame_size = (frame_size.max(64) as usize).min(rate);
        let tail = rate * filter_length_ms.clamp(50, 10_000) as usize / 1000;
        let filter = tail.max(self.frame_size * 2);
        self.filter_length = filter.div_ceil(self.frame_size) * self.frame_size;

        // SAFETY: plain constructors; the arguments are positive and fit `int`.
        self.echo =
            unsafe { speex_echo_state_init(self.frame_size as c_int, self.filter_length as c_int) };
        if self.echo.is_null() {
            return false;
        }
        let mut r: c_int = self.sample_rate;
        // SAFETY: a live echo state; the request takes a pointer to `int`.
        unsafe {
            speex_echo_ctl(
                self.echo,
                SPEEX_ECHO_SET_SAMPLING_RATE,
                (&raw mut r).cast::<c_void>(),
            )
        };

        // SAFETY: plain constructor.
        self.prep =
            unsafe { speex_preprocess_state_init(self.frame_size as c_int, self.sample_rate) };
        if self.prep.is_null() {
            self.cleanup();
            return false;
        }
        let set = |req: c_int, mut v: c_int| {
            // SAFETY: a live preprocess state; these requests take a pointer to `int`.
            unsafe { speex_preprocess_ctl(self.prep, req, (&raw mut v).cast::<c_void>()) };
        };
        // SAFETY: a live preprocess state; SET_ECHO_STATE takes the echo state pointer, which outlives it.
        unsafe { speex_preprocess_ctl(self.prep, SPEEX_PREPROCESS_SET_ECHO_STATE, self.echo) };
        set(SPEEX_PREPROCESS_SET_DENOISE, 0); // keep singing harmonics intact (karaoke)
        set(SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, -45);
        set(SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, 0); // transparent voice during double talk (no gating)

        self.ref_ring.clear();
        self.ref_ring.resize(rate * 3, 0.0);
        self.ref_write = 0;
        for b in [&mut self.spx_rec, &mut self.spx_play, &mut self.spx_out] {
            b.clear();
            b.resize(self.frame_size, 0);
        }
        self.erle_db = 0.0;
        self.leak_detected = false;
        self.leak_run = 0;
        true
    }

    /// Resets the filter and the reference without reallocating.
    pub fn reset(&mut self) {
        if !self.echo.is_null() {
            // SAFETY: a live echo state.
            unsafe { speex_echo_state_reset(self.echo) };
        }
        self.ref_ring.fill(0.0);
        self.ref_write = 0;
        self.erle_db = 0.0;
        self.leak_detected = false;
        self.leak_run = 0;
    }

    pub fn set_mode(&mut self, mode: AecMode) {
        self.mode = mode;
    }

    pub fn mode(&self) -> AecMode {
        self.mode
    }

    /// Far-end reference: what the Hub sends to this friend, downmixed to mono.
    pub fn push_reference(&mut self, r: &[f32]) {
        let cap = self.ref_ring.len();
        if r.is_empty() || cap == 0 {
            return;
        }
        for (i, &v) in r.iter().enumerate() {
            self.ref_ring[(self.ref_write + i) % cap] = v;
        }
        self.ref_write = (self.ref_write + r.len()) % cap;
    }

    /// Removes the echo from the friend's mic (`mic` → `out`, mono). `delay_samples` is the measured acoustic round
    /// trip. Runs only for a whole frame in mode On, or Auto after a leak; otherwise copies `mic` through.
    pub fn process(&mut self, mic: &[f32], out: &mut [f32], delay_samples: i32) {
        let n = mic.len();
        if n == 0 || out.len() < n {
            return;
        }
        if !self.active(n) {
            out[..n].copy_from_slice(mic);
            return;
        }
        let in_power = self.load(mic, delay_samples);
        self.cancel(&mut out[..n], in_power);
    }

    /// `process` on one buffer (the C++ callers pass the same pointer as input and output).
    pub fn process_in_place(&mut self, buf: &mut [f32], delay_samples: i32) {
        if buf.is_empty() || !self.active(buf.len()) {
            return;
        }
        let in_power = self.load(buf, delay_samples);
        self.cancel(buf, in_power);
    }

    fn active(&self, n: usize) -> bool {
        let run = self.mode == AecMode::On || (self.mode == AecMode::Auto && self.leak_detected);
        run && !self.echo.is_null()
            && !self.prep.is_null()
            && n == self.frame_size
            && !self.ref_ring.is_empty()
    }

    /// Converts the mic frame and the reference `delay_samples` back to 16 bit; returns the mic power.
    fn load(&mut self, mic: &[f32], delay_samples: i32) -> f32 {
        let n = mic.len();
        let cap = self.ref_ring.len();
        let max_delay = cap.saturating_sub(n + 1);
        let d = (delay_samples.max(0) as usize).min(max_delay);
        let start = (self.ref_write as i64 - d as i64 - n as i64).rem_euclid(cap as i64) as usize;
        let mut in_power = 0.0f32;
        for i in 0..n {
            let r = self.ref_ring[(start + i) % cap];
            let m = mic[i];
            in_power += m * m;
            self.spx_play[i] = to_i16(r);
            self.spx_rec[i] = to_i16(m);
        }
        in_power
    }

    /// Runs the canceller + preprocessor on the loaded frame, writes `out` and updates the ERLE.
    fn cancel(&mut self, out: &mut [f32], in_power: f32) {
        // SAFETY: live states; the three buffers hold `frame_size` samples each, as both states were made for.
        unsafe {
            speex_echo_cancellation(
                self.echo,
                self.spx_rec.as_ptr(),
                self.spx_play.as_ptr(),
                self.spx_out.as_mut_ptr(),
            );
            speex_preprocess_run(self.prep, self.spx_out.as_mut_ptr());
        }
        let mut out_power = 0.0f32;
        for i in 0..out.len() {
            out[i] = f32::from(self.spx_out[i]) / 32768.0;
            out_power += out[i] * out[i];
        }
        if in_power > 1e-6 {
            let curr = 10.0 * ((in_power + 1e-9) / (out_power + 1e-9)).log10();
            self.erle_db = if self.erle_db == 0.0 {
                curr
            } else {
                self.erle_db * 0.95 + curr * 0.05
            };
        }
    }
    /// Echo return loss enhancement (dB), smoothed over frames with far-end signal.
    pub fn erle_db(&self) -> f32 {
        self.erle_db
    }

    pub fn leak_detected(&self) -> bool {
        self.leak_detected
    }

    /// Called by the Hub at 20 Hz. Auto switches on after `LEAK_LATCH_TICKS` leak readings in a row and stays on
    /// until `init` / `reset`: the detector hears the mic after cancelling, so a working canceller would switch itself
    /// off again.
    pub fn set_leak_detected(&mut self, detected: bool) {
        self.leak_run = if detected {
            self.leak_run.saturating_add(1)
        } else {
            0
        };
        if self.leak_run >= LEAK_LATCH_TICKS {
            self.leak_detected = true;
        }
    }

    pub fn sample_rate(&self) -> i32 {
        self.sample_rate
    }

    pub fn frame_size(&self) -> usize {
        self.frame_size
    }
}
