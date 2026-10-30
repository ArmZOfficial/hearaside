//! `hs-net`: the network side of HEARASIDE (PLAN-rust-migration R3), a port of `libs/ssnet`.
//!
//! - [`wire`]: the HRA1/2/3 packet formats sent to friends (byte for byte the C++ `wire.h`).
//! - [`opus`]: Opus encoder / decoder over libopus built from `external/opus`.
//! - [`aec`]: `FriendEcho`, the echo canceller for friends on speakers (SpeexDSP MDF + preprocessor).
//!
//! - [`rtc`]: WebRTC DataChannel with UDP ICE / STUN, driven by a network worker.
//!
//! - [`share`]: the ShareServer (R3.3): listen / send-in links over HTTP + WebSocket + WebRTC.
//! - [`control`], [`rest`], [`json`]: the Hub's REST API (R3.4): the HTTP server on 127.0.0.1:47800–47809, its routes against
//!   the [`rest::HubControl`] trait, and the JUCE-style JSON the answers are written in.
//!
//! No UI or plug-in framework here (MIT).
//!
//! Real time: after `init`, `OpusEnc::encode`, `OpusDec::decode` and `FriendEcho::process` / `push_reference` make
//! no Rust allocation (`tests/rt_safety.rs`); the C++ versions could grow their scratch buffers on the audio thread.

pub mod aec;
pub mod control;
mod ffi;
pub mod json;
pub mod opus;
pub mod rest;
pub mod rtc;
pub mod share;
pub mod wire;
