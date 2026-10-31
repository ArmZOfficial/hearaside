//! A fake ASIO driver: a COM object built from the same vtable the device calls through, so the tests run the whole
//! host (lazy load, open, buffer switches, restarts) without a sound card. Nothing here is a real driver's code.
#![allow(dead_code)]

use hs_win::asio::abi::*;
use hs_win::asio::{Device, bytes_per_sample};
use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, Ordering::SeqCst};
use std::sync::{Arc, Mutex, MutexGuard};

/// The device host runs one driver at a time (ASIO callbacks carry no context), so tests that open one take turns.
pub fn serial() -> MutexGuard<'static, ()> {
    static SERIAL: Mutex<()> = Mutex::new(());
    SERIAL.lock().unwrap_or_else(|e| e.into_inner())
}

#[derive(Clone)]
pub struct Cfg {
    /// (ASIO sample type, name) per channel
    pub inputs: Vec<(i32, &'static str)>,
    pub outputs: Vec<(i32, &'static str)>,
    pub rate: f64,
    pub rates: Vec<f64>,
    /// min, max, preferred, granularity
    pub buffer: (i32, i32, i32, i32),
    pub latencies: (i32, i32),
    pub init_ok: bool,
    pub error_message: &'static str,
    pub create_ok: bool,
    pub start_ok: bool,
    pub wants_output_ready: bool,
    /// the driver delivers buffer switches from inside `start()`, after leaving junk in the output buffers
    pub switch_in_start: bool,
}

impl Default for Cfg {
    fn default() -> Self {
        Self {
            inputs: vec![(INT24, "Mic"), (FLOAT32, "")],
            outputs: vec![(INT16, "L"), (INT32_24, "R"), (FLOAT64, "Aux")],
            rate: 44100.0,
            rates: vec![44100.0, 48000.0, 96000.0],
            buffer: (64, 256, 128, 64),
            latencies: (130, 260),
            init_ok: true,
            error_message: "",
            create_ok: true,
            start_ok: true,
            wants_output_ready: true,
            switch_in_start: false,
        }
    }
}

pub const INT16: i32 = 16;
pub const INT24: i32 = 17;
pub const FLOAT32: i32 = 19;
pub const FLOAT64: i32 = 20;
pub const INT32_24: i32 = 27;

#[derive(Default)]
pub struct Probe {
    pub calls: Mutex<Vec<String>>,
    pub output_ready_calls: AtomicU32,
    pub released: AtomicU32,
    /// the next createBuffers fails (then the flag clears)
    pub fail_next_create: std::sync::atomic::AtomicBool,
}

impl Probe {
    pub fn log(&self) -> Vec<String> {
        self.calls.lock().unwrap().clone()
    }
    pub fn count(&self, what: &str) -> usize {
        self.log().iter().filter(|c| c.starts_with(what)).count()
    }
}

/// (is_input, channel)
type Key = (i32, i32);

#[derive(Default)]
struct State {
    rate: f64,
    bufsize: i32,
    /// (is_input, channel) of each buffer made in createBuffers, with its two halves
    bufs: Vec<(Key, [Vec<u8>; 2])>,
    callbacks: usize,
}

#[repr(C)]
struct Fake {
    base: Driver,
    cfg: Cfg,
    probe: Arc<Probe>,
    state: Mutex<State>,
}

/// What a test holds to drive a fake: it stays valid while the `Device` that owns the driver is alive.
#[derive(Clone, Copy)]
pub struct Ctl(*mut Fake);

// SAFETY: only used to call the fake's thread-safe entry points from a test's "driver thread".
unsafe impl Send for Ctl {}
// SAFETY: as above.
unsafe impl Sync for Ctl {}

fn fake<'a>(this: *mut Driver) -> &'a Fake {
    // SAFETY: `this` is a `Fake` (its first field is the `Driver`) that the device keeps alive while it calls.
    unsafe { &*this.cast::<Fake>() }
}

fn note(this: *mut Driver, what: &str) {
    fake(this).probe.calls.lock().unwrap().push(what.to_owned());
}

fn code(ok: bool) -> i32 {
    if ok { 0 } else { -1000 }
}

unsafe extern "system" fn query_interface(
    _: *mut Driver,
    _: *const Guid,
    _: *mut *mut c_void,
) -> i32 {
    -1
}
unsafe extern "system" fn add_ref(_: *mut Driver) -> u32 {
    1
}
unsafe extern "system" fn release(this: *mut Driver) -> u32 {
    fake(this).probe.released.fetch_add(1, SeqCst);
    note(this, "release");
    // SAFETY: the device calls Release once, last; the box came from `make`.
    drop(unsafe { Box::from_raw(this.cast::<Fake>()) });
    0
}
unsafe extern "system" fn init(this: *mut Driver, _hwnd: *mut c_void) -> i32 {
    note(this, "init");
    i32::from(fake(this).cfg.init_ok)
}
unsafe extern "system" fn get_driver_name(_: *mut Driver, _: *mut u8) {}
unsafe extern "system" fn get_driver_version(_: *mut Driver) -> i32 {
    1
}
unsafe extern "system" fn get_error_message(this: *mut Driver, text: *mut u8) {
    note(this, "getErrorMessage");
    let m = fake(this).cfg.error_message.as_bytes();
    // SAFETY: the host passes 128 bytes.
    unsafe {
        std::ptr::copy_nonoverlapping(m.as_ptr(), text, m.len());
        *text.add(m.len()) = 0;
    }
}
unsafe extern "system" fn start(this: *mut Driver) -> i32 {
    note(this, "start");
    let f = fake(this);
    if f.cfg.switch_in_start {
        let cb = {
            let mut s = f.state.lock().unwrap();
            for (key, halves) in s.bufs.iter_mut() {
                if key.0 == 0 {
                    halves.iter_mut().for_each(|h| h.fill(0x55));
                }
            }
            s.callbacks as *const Callbacks
        };
        // SAFETY: the device's boxed Callbacks, alive while it is open.
        let cb = unsafe { &*cb };
        (cb.buffer_switch)(0, 1);
        (cb.buffer_switch)(1, 1);
    }
    code(f.cfg.start_ok)
}
unsafe extern "system" fn stop(this: *mut Driver) -> i32 {
    note(this, "stop");
    0
}
unsafe extern "system" fn get_channels(this: *mut Driver, i: *mut i32, o: *mut i32) -> i32 {
    note(this, "getChannels");
    // SAFETY: live out-pointers from the host.
    unsafe {
        *i = fake(this).cfg.inputs.len() as i32;
        *o = fake(this).cfg.outputs.len() as i32;
    }
    0
}
unsafe extern "system" fn get_latencies(this: *mut Driver, i: *mut i32, o: *mut i32) -> i32 {
    // SAFETY: live out-pointers from the host.
    unsafe {
        (*i, *o) = fake(this).cfg.latencies;
    }
    0
}
unsafe extern "system" fn get_buffer_size(
    this: *mut Driver,
    a: *mut i32,
    b: *mut i32,
    c: *mut i32,
    d: *mut i32,
) -> i32 {
    // SAFETY: live out-pointers from the host.
    unsafe {
        (*a, *b, *c, *d) = fake(this).cfg.buffer;
    }
    0
}
unsafe extern "system" fn can_sample_rate(this: *mut Driver, rate: f64) -> i32 {
    code(fake(this).cfg.rates.contains(&rate))
}
unsafe extern "system" fn get_sample_rate(this: *mut Driver, rate: *mut f64) -> i32 {
    let s = fake(this).state.lock().unwrap();
    // SAFETY: live out-pointer from the host.
    unsafe {
        *rate = if s.rate > 0.0 {
            s.rate
        } else {
            fake(this).cfg.rate
        }
    };
    0
}
unsafe extern "system" fn set_sample_rate(this: *mut Driver, rate: f64) -> i32 {
    note(this, &format!("setSampleRate {rate}"));
    fake(this).state.lock().unwrap().rate = rate;
    0
}
unsafe extern "system" fn get_clock_sources(
    _: *mut Driver,
    _: *mut ClockSource,
    _: *mut i32,
) -> i32 {
    -1
}
unsafe extern "system" fn set_clock_source(_: *mut Driver, _: i32) -> i32 {
    -1
}
unsafe extern "system" fn get_sample_position(
    _: *mut Driver,
    _: *mut Position,
    _: *mut Position,
) -> i32 {
    -1
}
unsafe extern "system" fn get_channel_info(this: *mut Driver, info: *mut ChannelInfo) -> i32 {
    let f = fake(this);
    // SAFETY: a live ChannelInfo from the host.
    let info = unsafe { &mut *info };
    let list = if info.is_input != 0 {
        &f.cfg.inputs
    } else {
        &f.cfg.outputs
    };
    let Some((ty, name)) = list.get(info.channel as usize) else {
        return -1;
    };
    info.kind = *ty;
    info.name = [0; 32];
    info.name[..name.len()].copy_from_slice(name.as_bytes());
    0
}
unsafe extern "system" fn create_buffers(
    this: *mut Driver,
    infos: *mut BufferInfo,
    count: i32,
    size: i32,
    cb: *mut Callbacks,
) -> i32 {
    note(this, &format!("createBuffers {count} {size}"));
    let f = fake(this);
    if !f.cfg.create_ok || f.probe.fail_next_create.swap(false, SeqCst) {
        return -1;
    }
    let mut s = f.state.lock().unwrap();
    s.bufsize = size;
    s.callbacks = cb as usize;
    s.bufs.clear();
    // SAFETY: `count` infos from the host.
    let infos = unsafe { std::slice::from_raw_parts_mut(infos, count as usize) };
    for info in infos.iter() {
        s.bufs
            .push(((info.is_input, info.channel_num), [Vec::new(), Vec::new()]));
    }
    for (info, (key, halves)) in infos.iter_mut().zip(s.bufs.iter_mut()) {
        let list = if key.0 != 0 {
            &f.cfg.inputs
        } else {
            &f.cfg.outputs
        };
        let bytes = bytes_per_sample(list[key.1 as usize].0) * size as usize;
        for (h, half) in halves.iter_mut().enumerate() {
            *half = vec![0u8; bytes];
            info.buffers[h] = half.as_mut_ptr().cast();
        }
    }
    0
}
unsafe extern "system" fn dispose_buffers(this: *mut Driver) -> i32 {
    note(this, "disposeBuffers");
    fake(this).state.lock().unwrap().bufs.clear();
    0
}
unsafe extern "system" fn control_panel(this: *mut Driver) -> i32 {
    note(this, "controlPanel");
    0
}
unsafe extern "system" fn future(_: *mut Driver, _: i32, _: *mut c_void) -> i32 {
    -1
}
unsafe extern "system" fn output_ready(this: *mut Driver) -> i32 {
    // the buffer-switch path calls this: no allocation, no lock
    let f = fake(this);
    if f.cfg.wants_output_ready {
        f.probe.output_ready_calls.fetch_add(1, SeqCst);
        0
    } else {
        -1
    }
}

static VTBL: DriverVtbl = DriverVtbl {
    query_interface,
    add_ref,
    release,
    init,
    get_driver_name,
    get_driver_version,
    get_error_message,
    start,
    stop,
    get_channels,
    get_latencies,
    get_buffer_size,
    can_sample_rate,
    get_sample_rate,
    set_sample_rate,
    get_clock_sources,
    set_clock_source,
    get_sample_position,
    get_channel_info,
    create_buffers,
    dispose_buffers,
    control_panel,
    future,
    output_ready,
};

/// A device on a fake driver.
pub fn device(cfg: Cfg) -> (Device, Arc<Probe>, Ctl) {
    let probe = Arc::new(Probe::default());
    let f = Box::into_raw(Box::new(Fake {
        base: Driver { vtbl: &VTBL },
        cfg,
        probe: probe.clone(),
        state: Mutex::new(State::default()),
    }));
    // SAFETY: a live object with the vtable above; the device owns the one reference and releases it on drop.
    let drv = unsafe { Drv::new(f.cast()) };
    (Device::from_driver("Fake ASIO", drv), probe, Ctl(f))
}

impl Ctl {
    fn callbacks(&self) -> &Callbacks {
        let p = fake(self.0.cast()).state.lock().unwrap().callbacks;
        // SAFETY: the device's boxed Callbacks, alive while it is open.
        unsafe { &*(p as *const Callbacks) }
    }

    /// The driver's bufferSwitch entry point, to call on a "driver thread" of the test's choosing.
    pub fn switch_fn(&self) -> extern "system" fn(i32, i32) {
        self.callbacks().buffer_switch
    }

    /// One buffer switch on the calling thread.
    pub fn switch(&self, half: i32) {
        (self.switch_fn())(half, 1);
    }

    /// `bufferSwitchTimeInfo`
    pub fn switch_time_info(&self, half: i32) -> *mut c_void {
        (self.callbacks().buffer_switch_time_info)(std::ptr::null_mut(), half, 1)
    }

    pub fn message(&self, selector: i32, value: i32) -> i32 {
        (self.callbacks().message)(selector, value, std::ptr::null_mut(), std::ptr::null_mut())
    }

    pub fn rate_changed(&self) {
        (self.callbacks().sample_rate_did_change)(48000.0);
    }

    fn with_buf<R>(&self, key: (i32, i32), half: usize, f: impl FnOnce(&mut Vec<u8>) -> R) -> R {
        let mut s = fake(self.0.cast()).state.lock().unwrap();
        let (_, halves) = s
            .bufs
            .iter_mut()
            .find(|(k, _)| *k == key)
            .expect("no such buffer");
        f(&mut halves[half])
    }

    pub fn set_input(&self, channel: i32, half: usize, bytes: &[u8]) {
        self.with_buf((1, channel), half, |b| b.copy_from_slice(bytes));
    }

    pub fn output(&self, channel: i32, half: usize) -> Vec<u8> {
        self.with_buf((0, channel), half, |b| b.clone())
    }

    pub fn set_output(&self, channel: i32, half: usize, byte: u8) {
        self.with_buf((0, channel), half, |b| b.fill(byte));
    }
}
