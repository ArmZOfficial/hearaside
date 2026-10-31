//! HAS Direct: HAS Karaoke's own ASIO host. Port of apps/karaoke/HasAsio.{h,cpp}.
//!
//! It loads the ASIO drivers listed under `HKLM\SOFTWARE\ASIO` through their COM interface, written from the public
//! driver ABI (the vtable in [`abi`]), without the Steinberg SDK and without `cpal`'s `asio` feature (which pulls the
//! SDK in). The C++ was a JUCE device type; here it is a [`DeviceType`] (the driver list) and a [`Device`] (one
//! driver) that run an [`AudioCallback`] on the driver's thread.
//!
//! Real-time rules (PLAN §8): the driver's buffer switch converts to float, runs the callback and converts back
//! without allocating, locking, logging or making a system call of its own. The C++ held a `CriticalSection` around the
//! callback; here the audio thread only *tries* to take the callback slot and plays silence for that one block if
//! `start` / `stop` has it at that moment.

pub mod abi;
pub mod convert;
mod device;
mod win;

pub use convert::{
    FLOAT32_LSB, FLOAT64_LSB, INT16_LSB, INT24_LSB, INT32_LSB, INT32_LSB16, INT32_LSB18,
    INT32_LSB20, INT32_LSB24, bit_depth, bytes_per_sample, from_float, to_float,
};
pub use device::{AudioCallback, Block, Device, DeviceType, OpenError, StreamInfo};

/// The name people see: our own, the drivers keep the names their makers gave them.
pub const TYPE_NAME: &str = "HAS Direct";
