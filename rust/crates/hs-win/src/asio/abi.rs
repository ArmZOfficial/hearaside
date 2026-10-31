//! The ASIO driver ABI as `HasAsio.cpp` declares it: a COM object whose vtable is IUnknown followed by 21 methods,
//! and the structs passed through it. Written from the public driver ABI; nothing here comes from the Steinberg SDK.
//!
//! The C++ packs the structs with `#pragma pack(4)` and `long` is 32-bit on Windows. Every field already sits on its
//! natural boundary (the pointers are at offsets 8, 0, 8, ... of 8-byte-aligned structs), so plain `repr(C)` is the
//! same layout; the asserts below pin the sizes and offsets.

use std::ffi::c_void;

#[cfg(all(windows, not(target_pointer_width = "64")))]
compile_error!("the driver interface here is the 64-bit one (HasAsio.cpp asserts the same)");

/// ASIO's `ASE_OK` and `ASE_SUCCESS`: both mean the call worked.
pub fn ok(e: i32) -> bool {
    e == 0 || e == 0x3f48_47a0
}

/// A COM GUID (same layout as `windows_sys::core::GUID`).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Guid {
    pub data1: u32,
    pub data2: u16,
    pub data3: u16,
    pub data4: [u8; 8],
}

#[repr(C)]
pub struct ClockSource {
    pub index: i32,
    pub associated_channel: i32,
    pub associated_group: i32,
    pub is_current_source: i32,
    pub name: [u8; 32],
}

#[repr(C)]
pub struct ChannelInfo {
    pub channel: i32,
    pub is_input: i32,
    pub is_active: i32,
    pub channel_group: i32,
    /// ASIO sample type (see `convert`)
    pub kind: i32,
    pub name: [u8; 32],
}

impl ChannelInfo {
    pub fn new(channel: i32, is_input: i32) -> Self {
        Self {
            channel,
            is_input,
            is_active: 0,
            channel_group: 0,
            kind: 0,
            name: [0; 32],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct BufferInfo {
    pub is_input: i32,
    pub channel_num: i32,
    /// the two half-buffers the driver fills in `createBuffers`
    pub buffers: [*mut c_void; 2],
}

impl BufferInfo {
    pub fn new(is_input: i32, channel_num: i32) -> Self {
        Self {
            is_input,
            channel_num,
            buffers: [std::ptr::null_mut(); 2],
        }
    }
}

/// `ASIOSamples` / `ASIOTimeStamp` without native 64-bit support: high and low 32 bits.
#[repr(C)]
pub struct Position {
    pub hi: u32,
    pub lo: u32,
}

/// What the host hands the driver; the driver calls these on its own threads.
#[repr(C)]
pub struct Callbacks {
    pub buffer_switch: extern "system" fn(index: i32, process_now: i32),
    pub sample_rate_did_change: extern "system" fn(rate: f64),
    pub message:
        extern "system" fn(selector: i32, value: i32, message: *mut c_void, opt: *mut f64) -> i32,
    pub buffer_switch_time_info:
        extern "system" fn(params: *mut c_void, index: i32, process_now: i32) -> *mut c_void,
}

/// `asioMessage` selectors
pub const K_SELECTOR_SUPPORTED: i32 = 1;
pub const K_ENGINE_VERSION: i32 = 2;
pub const K_RESET_REQUEST: i32 = 3;
pub const K_BUFFER_SIZE_CHANGE: i32 = 4;
pub const K_RESYNC_REQUEST: i32 = 5;
pub const K_LATENCIES_CHANGED: i32 = 6;

type Method<A, R = i32> = unsafe extern "system" fn(*mut Driver, A) -> R;

/// The driver's vtable, in the order `HasAsio.cpp` declares it (IUnknown first).
#[repr(C)]
pub struct DriverVtbl {
    pub query_interface:
        unsafe extern "system" fn(*mut Driver, *const Guid, *mut *mut c_void) -> i32,
    pub add_ref: unsafe extern "system" fn(*mut Driver) -> u32,
    pub release: unsafe extern "system" fn(*mut Driver) -> u32,
    pub init: Method<*mut c_void>,
    pub get_driver_name: Method<*mut u8, ()>,
    pub get_driver_version: unsafe extern "system" fn(*mut Driver) -> i32,
    pub get_error_message: Method<*mut u8, ()>,
    pub start: unsafe extern "system" fn(*mut Driver) -> i32,
    pub stop: unsafe extern "system" fn(*mut Driver) -> i32,
    pub get_channels: unsafe extern "system" fn(*mut Driver, *mut i32, *mut i32) -> i32,
    pub get_latencies: unsafe extern "system" fn(*mut Driver, *mut i32, *mut i32) -> i32,
    pub get_buffer_size:
        unsafe extern "system" fn(*mut Driver, *mut i32, *mut i32, *mut i32, *mut i32) -> i32,
    pub can_sample_rate: Method<f64>,
    pub get_sample_rate: Method<*mut f64>,
    pub set_sample_rate: Method<f64>,
    pub get_clock_sources:
        unsafe extern "system" fn(*mut Driver, *mut ClockSource, *mut i32) -> i32,
    pub set_clock_source: Method<i32>,
    pub get_sample_position:
        unsafe extern "system" fn(*mut Driver, *mut Position, *mut Position) -> i32,
    pub get_channel_info: Method<*mut ChannelInfo>,
    pub create_buffers:
        unsafe extern "system" fn(*mut Driver, *mut BufferInfo, i32, i32, *mut Callbacks) -> i32,
    pub dispose_buffers: unsafe extern "system" fn(*mut Driver) -> i32,
    pub control_panel: unsafe extern "system" fn(*mut Driver) -> i32,
    pub future: unsafe extern "system" fn(*mut Driver, i32, *mut c_void) -> i32,
    pub output_ready: unsafe extern "system" fn(*mut Driver) -> i32,
}

/// A COM object: a pointer to its vtable.
#[repr(C)]
pub struct Driver {
    pub vtbl: *const DriverVtbl,
}

/// A live driver. Copyable on purpose (the device and its audio side both hold one); whoever created it owns the one
/// COM reference and calls [`Drv::release`] once, after the last use.
#[derive(Clone, Copy)]
pub struct Drv(*mut Driver);

macro_rules! call {
    ($self:ident . $m:ident ( $($a:expr),* )) => {
        // SAFETY: `Drv::new` requires a live COM object whose vtable has the ASIO layout above, and the object stays
        // alive until `release`, which the owner calls after the last use; every pointer argument is a live local or
        // a buffer the caller owns for the call.
        unsafe { ((*(*$self.0).vtbl).$m)($self.0 $(, $a)*) }
    };
}

impl Drv {
    /// # Safety
    /// `p` points at a live COM object with the ASIO vtable (21 methods after IUnknown) that stays alive until
    /// [`Drv::release`] is called on one of the copies, after which no copy is used again.
    pub unsafe fn new(p: *mut Driver) -> Self {
        Self(p)
    }

    pub fn release(self) {
        call!(self.release());
    }

    /// `ASIOBool`: true = the driver started.
    pub fn init(self, sys_handle: usize) -> bool {
        call!(self.init(sys_handle as *mut c_void)) != 0
    }

    pub fn error_message(self) -> String {
        let mut msg = [0u8; 128];
        call!(self.get_error_message(msg.as_mut_ptr()));
        crate::asio::device::c_text(&msg)
    }

    pub fn channels(self) -> (i32, i32) {
        let (mut i, mut o) = (0, 0);
        call!(self.get_channels(&mut i, &mut o));
        (i, o)
    }

    pub fn latencies(self) -> (i32, i32) {
        let (mut i, mut o) = (0, 0);
        call!(self.get_latencies(&mut i, &mut o));
        (i, o)
    }

    /// (min, max, preferred, granularity)
    pub fn buffer_size(self) -> (i32, i32, i32, i32) {
        let (mut a, mut b, mut c, mut d) = (0, 0, 0, 0);
        call!(self.get_buffer_size(&mut a, &mut b, &mut c, &mut d));
        (a, b, c, d)
    }

    pub fn can_sample_rate(self, rate: f64) -> bool {
        ok(call!(self.can_sample_rate(rate)))
    }

    pub fn sample_rate(self) -> f64 {
        let mut r = 0.0;
        call!(self.get_sample_rate(&mut r));
        r
    }

    pub fn set_sample_rate(self, rate: f64) -> bool {
        ok(call!(self.set_sample_rate(rate)))
    }

    pub fn channel_info(self, info: &mut ChannelInfo) {
        call!(self.get_channel_info(info));
    }

    pub fn create_buffers(
        self,
        infos: &mut [BufferInfo],
        size: i32,
        callbacks: &mut Callbacks,
    ) -> bool {
        ok(call!(self.create_buffers(
            infos.as_mut_ptr(),
            infos.len() as i32,
            size,
            callbacks
        )))
    }

    pub fn dispose_buffers(self) {
        call!(self.dispose_buffers());
    }

    pub fn start(self) -> bool {
        ok(call!(self.start()))
    }

    pub fn stop(self) {
        call!(self.stop());
    }

    pub fn control_panel(self) {
        call!(self.control_panel());
    }

    /// `outputReady`: true = the driver wants to be told when the output is written
    pub fn output_ready(self) -> bool {
        ok(call!(self.output_ready()))
    }
}

#[cfg(target_pointer_width = "64")]
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<ClockSource>() == 48);
    assert!(size_of::<ChannelInfo>() == 52);
    assert!(offset_of!(ChannelInfo, kind) == 16 && offset_of!(ChannelInfo, name) == 20);
    assert!(size_of::<BufferInfo>() == 24 && offset_of!(BufferInfo, buffers) == 8);
    assert!(size_of::<Position>() == 8);
    assert!(size_of::<Callbacks>() == 32 && offset_of!(Callbacks, message) == 16);
    assert!(size_of::<Guid>() == 16);
    // IUnknown (3) + 21 ASIO methods, one pointer each
    assert!(size_of::<DriverVtbl>() == 24 * 8);
    // slot numbers of the C++ `struct Driver : IUnknown` declaration, method by method
    macro_rules! slot {
        ($($field:ident = $n:expr),* $(,)?) => { $(assert!(offset_of!(DriverVtbl, $field) == $n * 8);)* };
    }
    slot!(
        query_interface = 0,
        add_ref = 1,
        release = 2,
        init = 3,
        get_driver_name = 4,
        get_driver_version = 5,
        get_error_message = 6,
        start = 7,
        stop = 8,
        get_channels = 9,
        get_latencies = 10,
        get_buffer_size = 11,
        can_sample_rate = 12,
        get_sample_rate = 13,
        set_sample_rate = 14,
        get_clock_sources = 15,
        set_clock_source = 16,
        get_sample_position = 17,
        get_channel_info = 18,
        create_buffers = 19,
        dispose_buffers = 20,
        control_panel = 21,
        future = 22,
        output_ready = 23,
    );
};
