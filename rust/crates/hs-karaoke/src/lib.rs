//! `hs-karaoke`: HAS Karaoke in Rust (R5). R5.3 brings the parts under the UI: the vocal chain, the music player, the
//! pre-flight leak check and the update check (ports of `apps/karaoke/{VocalChain,MusicPlayer,PreflightChecker,
//! UpdateChecker}`). R5.4 adds hosting a room (`room`) and joining one (`join`). The window comes in R5.5.
//!
//! JUCE delivered work to the message thread with `MessageManager::callAsync`; here each part whose work finishes on
//! another thread has a `poll()` for the owner's UI timer (the convention of `hs_win::asio::Device::poll_restart`).
pub mod join;
pub mod player;
pub mod preflight;
pub mod room;
pub mod update;
pub mod vocal;

use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

/// An `f32` in an atomic (the C++ `std::atomic<float>`, used with relaxed ordering there too).
pub(crate) struct AtomicF32(AtomicU32);

impl AtomicF32 {
    pub(crate) fn new(v: f32) -> Self {
        Self(AtomicU32::new(v.to_bits()))
    }
    pub(crate) fn load(&self) -> f32 {
        f32::from_bits(self.0.load(Relaxed))
    }
    pub(crate) fn store(&self, v: f32) {
        self.0.store(v.to_bits(), Relaxed);
    }
}

/// `juce::roundToInt`: round half to even (the 1.5 · 2^52 trick).
pub(crate) fn round_to_int(v: f64) -> i32 {
    v.round_ties_even() as i32
}
