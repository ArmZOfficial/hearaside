//! Windows-only pieces of HEARASIDE (PLAN-rust-migration §4.1, §8 item 5: the unsafe OS / COM code lives here).
//!
//! R5.2: [`asio`], HAS Karaoke's own ASIO host ("HAS Direct"). The sample conversions and the device logic are
//! plain Rust and build everywhere (so they run in the Linux CI); only the registry scan and `CoCreateInstance` are
//! Windows code. WASAPI is not here: the C++ app gets it from JUCE, so there is no matching C++ layer to port, and
//! the Rust app takes it from `cpal` (no `asio` feature) where the audio device manager is built (R5.4).

pub mod asio;
