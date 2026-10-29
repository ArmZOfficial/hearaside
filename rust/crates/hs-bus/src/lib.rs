//! HEARASIDE shared-memory bus, the Rust port of `libs/ssbus` (PLAN R1).
//!
//! [`protocol`] is the v14 segment layout, byte for byte the same as `protocol.h`, so Rust and C++
//! processes share one bus during the hybrid phase.

pub mod protocol;
