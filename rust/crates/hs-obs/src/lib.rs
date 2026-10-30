//! HEARASIDE OBS Source in Rust (R4.1): port of `obs/hearaside-obs/src/plugin.cpp`. Delivers the HEARASIDE Stream
//! Mix (or a stem) to OBS from the shared-memory bus, without a virtual audio cable. Loads as `hearaside-obs.dll`
//! into OBS Studio and Streamlabs Desktop. The remote hotkey commands (`remote.cpp`) are R4.2, see [`remote`].
//!
//! `module` holds everything that calls libobs and is left out of unit tests (it imports `obs.dll`, which a
//! test executable cannot load); everything else is plain Rust.
pub mod abi;
pub mod logic;
pub mod remote;

#[cfg(not(test))]
mod module;
