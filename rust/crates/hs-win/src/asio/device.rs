//! One ASIO driver as a device (`Device` in HasAsio.cpp) and the list of installed drivers (`Type`).

use super::abi::{
    BufferInfo, Callbacks, ChannelInfo, Drv, Guid, K_BUFFER_SIZE_CHANGE, K_ENGINE_VERSION,
    K_LATENCIES_CHANGED, K_RESET_REQUEST, K_RESYNC_REQUEST, K_SELECTOR_SUPPORTED,
};
use super::convert::{bit_depth, bytes_per_sample, from_float, to_float};
use super::win;
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::fmt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::null_mut;
use std::sync::atomic::Ordering::{Acquire, Relaxed, Release, SeqCst};
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicUsize};

/// Text from a fixed C buffer: up to the first NUL (`strnlen` in the C++).
pub(super) fn c_text(buf: &[u8]) -> String {
    let n = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[..n]).into_owned()
}

// --- what the app plugs in ---------------------------------------------------------------------------------

/// What the stream looks like once a callback starts (`audioDeviceAboutToStart`).
#[derive(Clone, Debug, PartialEq)]
pub struct StreamInfo {
    pub sample_rate: f64,
    pub buffer_size: usize,
    pub num_inputs: usize,
    pub num_outputs: usize,
    pub input_latency: i32,
    pub output_latency: i32,
    pub bit_depth: i32,
}

/// One buffer's worth of audio, planar: channel `c` is `frames` consecutive samples. Only the switched-on channels
/// are in it, in order.
pub struct Block<'a> {
    input: &'a [f32],
    output: &'a mut [f32],
    frames: usize,
    num_inputs: usize,
    num_outputs: usize,
}

impl Block<'_> {
    pub fn frames(&self) -> usize {
        self.frames
    }
    pub fn num_inputs(&self) -> usize {
        self.num_inputs
    }
    pub fn num_outputs(&self) -> usize {
        self.num_outputs
    }
    /// Input channel `ch` (empty if there is no such channel).
    pub fn input(&self, ch: usize) -> &[f32] {
        self.input
            .get(ch * self.frames..(ch + 1) * self.frames)
            .unwrap_or(&[])
    }
    /// Output channel `ch`, silent when the block starts (empty if there is no such channel).
    pub fn output(&mut self, ch: usize) -> &mut [f32] {
        self.output
            .get_mut(ch * self.frames..(ch + 1) * self.frames)
            .unwrap_or(&mut [])
    }
}

/// `juce::AudioIODeviceCallback`. `process` runs on the driver's thread: no allocation, locks, logging or system
/// calls (PLAN §8). A panic in it is caught: the device goes silent and `Device::faulted` turns true.
pub trait AudioCallback: Send {
    /// On the thread that calls `Device::start`, before the first block.
    fn about_to_start(&mut self, _info: &StreamInfo) {}
    fn process(&mut self, io: &mut Block<'_>);
    /// On the thread that calls `Device::stop` / `close`, after the last block.
    fn stopped(&mut self) {}
}

// --- errors (the C++ strings, word for word) ------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
pub enum OpenError {
    /// the driver could not be loaded or started (its own message when it gave one)
    Load(String),
    Rate(f64),
    NoChannels,
    AnotherRunning,
    Buffers(i32),
    Format(i32),
    NotStarted,
}

impl fmt::Display for OpenError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Load(m) if m.is_empty() => {
                f.write_str("The interface driver could not be loaded")
            }
            Self::Load(m) => f.write_str(m),
            Self::Rate(r) => write!(f, "The driver can't run at {r:.0} Hz"),
            Self::NoChannels => f.write_str("No channels are switched on"),
            Self::AnotherRunning => f.write_str("Another interface driver is already running"),
            Self::Buffers(n) => write!(f, "The driver couldn't make its buffers ({n} samples)"),
            Self::Format(t) => write!(f, "Unsupported sample format from the driver ({t})"),
            Self::NotStarted => f.write_str("The driver didn't start"),
        }
    }
}

impl std::error::Error for OpenError {}

// --- the audio side ----------------------------------------------------------------------------------------------

/// ASIO callbacks carry no context: one running driver at a time.
static ACTIVE: AtomicPtr<Inner> = AtomicPtr::new(null_mut());
/// Callbacks inside `with_active`, so `close` can wait until none of them can still touch the device.
static IN_FLIGHT: AtomicUsize = AtomicUsize::new(0);

/// Everything the driver thread touches while a device is open. Written only while the driver is stopped.
#[derive(Default)]
struct AudioState {
    drv: Option<Drv>,
    infos: Vec<BufferInfo>,
    types: Vec<i32>,
    in_buf: Vec<f32>,
    out_buf: Vec<f32>,
    frames: usize,
    num_inputs: usize,
    num_outputs: usize,
    post_output: bool,
}

struct Inner {
    open: AtomicBool,
    /// the audio state is complete and the driver's buffers exist: true from just before `drv.start()` until `close`
    /// has waited out the last callback. Between `ready` and `open` a buffer switch only silences the outputs.
    ready: AtomicBool,
    /// the driver asked for a reset (`kAsioResetRequest` etc.); `Device::poll_restart` acts on it
    restart: AtomicBool,
    /// the callback panicked: silence from then on
    faulted: AtomicBool,
    playing: AtomicBool,
    /// guards `cb`: the audio thread only tries it, `start` / `stop` spin on it
    busy: AtomicBool,
    cb: UnsafeCell<Option<Box<dyn AudioCallback>>>,
    audio: UnsafeCell<AudioState>,
}

impl Inner {
    fn new() -> Self {
        Self {
            open: AtomicBool::new(false),
            ready: AtomicBool::new(false),
            restart: AtomicBool::new(false),
            faulted: AtomicBool::new(false),
            playing: AtomicBool::new(false),
            busy: AtomicBool::new(false),
            cb: UnsafeCell::new(None),
            audio: UnsafeCell::new(AudioState::default()),
        }
    }

    /// Control thread: put `new` in the callback slot, get the old one out. Waits at most one callback.
    fn swap_callback(&self, new: Option<Box<dyn AudioCallback>>) -> Option<Box<dyn AudioCallback>> {
        while self
            .busy
            .compare_exchange_weak(false, true, Acquire, Relaxed)
            .is_err()
        {
            std::thread::yield_now();
        }
        // SAFETY: `busy` is ours until the store below; the audio thread only touches `cb` while it holds `busy` too.
        let old = std::mem::replace(unsafe { &mut *self.cb.get() }, new);
        self.busy.store(false, Release);
        old
    }

    /// Driver thread, one buffer switch: driver buffers to float, the callback, float back to the driver buffers.
    fn process(&self, index: i32) {
        let open = self.open.load(Acquire);
        if !open && !self.ready.load(Acquire) {
            return;
        }
        let Some(index) = usize::try_from(index).ok().filter(|i| *i < 2) else {
            return;
        };
        // SAFETY: `ready` is only true between the moment `open` has finished writing the audio state and the end of
        // `close` (after the last callback is out), and the state is only written outside that window; the driver never
        // runs two buffer switches at once, so this is the only reference.
        let a = unsafe { &mut *self.audio.get() };
        let AudioState {
            drv,
            infos,
            types,
            in_buf,
            out_buf,
            frames,
            num_inputs,
            num_outputs,
            post_output,
        } = a;
        let frames = *frames;

        if !open {
            // between `drv.start()` and `open` becoming true: no callback yet, but the driver may play this buffer
            // before the next switch, and it does not have to clear it
            for (info, &ty) in infos.iter().zip(types.iter()) {
                let p = info.buffers[index];
                if info.is_input == 0 && !p.is_null() {
                    // SAFETY: as for the outputs below: `frames` samples of type `ty`; all-zero bytes are silence in every type.
                    unsafe {
                        std::slice::from_raw_parts_mut(
                            p.cast::<u8>(),
                            bytes_per_sample(ty) * frames,
                        )
                    }
                    .fill(0);
                }
            }
            return;
        }

        let mut ii = 0;
        for (info, &ty) in infos.iter().zip(types.iter()) {
            if info.is_input == 0 {
                continue;
            }
            let p = info.buffers[index];
            if let (false, Some(dst)) =
                (p.is_null(), in_buf.get_mut(ii * frames..(ii + 1) * frames))
            {
                // SAFETY: createBuffers gave this channel `frames` samples of type `ty` in each half buffer, and
                // they stay valid until disposeBuffers (after `open` went false and no callback is in flight).
                let src = unsafe {
                    std::slice::from_raw_parts(p.cast::<u8>(), bytes_per_sample(ty) * frames)
                };
                to_float(ty, src, dst);
            }
            ii += 1;
        }

        out_buf.fill(0.0);
        if self
            .busy
            .compare_exchange(false, true, Acquire, Relaxed)
            .is_ok()
        {
            // SAFETY: `busy` is held until the store below.
            if let Some(cb) = unsafe { &mut *self.cb.get() }
                .as_mut()
                .filter(|_| !self.faulted.load(Relaxed))
            {
                let mut block = Block {
                    input: in_buf.get(..*num_inputs * frames).unwrap_or(&[]),
                    output: out_buf.get_mut(..*num_outputs * frames).unwrap_or(&mut []),
                    frames,
                    num_inputs: *num_inputs,
                    num_outputs: *num_outputs,
                };
                if catch_unwind(AssertUnwindSafe(|| cb.process(&mut block))).is_err() {
                    self.faulted.store(true, Relaxed);
                    out_buf.fill(0.0);
                }
            }
            self.busy.store(false, Release);
        }

        let mut oi = 0;
        for (info, &ty) in infos.iter().zip(types.iter()) {
            if info.is_input != 0 {
                continue;
            }
            let p = info.buffers[index];
            if let (false, Some(src)) = (p.is_null(), out_buf.get(oi * frames..(oi + 1) * frames)) {
                // SAFETY: as for the inputs; the driver does not read this half buffer before the switch returns.
                let dst = unsafe {
                    std::slice::from_raw_parts_mut(p.cast::<u8>(), bytes_per_sample(ty) * frames)
                };
                from_float(ty, src, dst);
            }
            oi += 1;
        }
        if *post_output && let Some(d) = drv {
            d.output_ready();
        }
    }
}

/// Runs `f` on the running device, if there is one. `close` clears `ACTIVE` and then waits for `IN_FLIGHT` to reach
/// zero before the device can go away, so the reference is good for the whole call (both sides are SeqCst: either
/// the callback sees null or `close` sees the count).
fn with_active(f: impl FnOnce(&Inner)) {
    IN_FLIGHT.fetch_add(1, SeqCst);
    let p = ACTIVE.load(SeqCst);
    if !p.is_null() {
        // SAFETY: see above.
        let inner = unsafe { &*p };
        // no panic may leave an `extern "system"` function
        let _ = catch_unwind(AssertUnwindSafe(|| f(inner)));
    }
    IN_FLIGHT.fetch_sub(1, SeqCst);
}

extern "system" fn on_buffer_switch(index: i32, _process_now: i32) {
    with_active(|d| d.process(index));
}

extern "system" fn on_buffer_switch_time_info(
    _params: *mut c_void,
    index: i32,
    _process_now: i32,
) -> *mut c_void {
    on_buffer_switch(index, 1);
    null_mut()
}

extern "system" fn on_rate_change(_rate: f64) {
    with_active(|d| d.restart.store(true, Release));
}

extern "system" fn on_message(
    selector: i32,
    value: i32,
    _message: *mut c_void,
    _opt: *mut f64,
) -> i32 {
    match selector {
        K_SELECTOR_SUPPORTED => i32::from(matches!(
            value,
            K_ENGINE_VERSION
                | K_RESET_REQUEST
                | K_BUFFER_SIZE_CHANGE
                | K_RESYNC_REQUEST
                | K_LATENCIES_CHANGED
        )),
        K_ENGINE_VERSION => 2,
        K_RESET_REQUEST | K_BUFFER_SIZE_CHANGE => {
            with_active(|d| d.restart.store(true, Release));
            1
        }
        K_RESYNC_REQUEST | K_LATENCIES_CHANGED => 1,
        _ => 0,
    }
}

// --- one device = one driver ----------------------------------------------------------------------------------

pub struct Device {
    name: String,
    clsid: Option<Guid>,
    drv: Option<Drv>,
    /// the driver loads on first real use (see `ensure_loaded`)
    loaded: bool,
    load_error: String,
    error: Option<OpenError>,
    in_names: Vec<String>,
    out_names: Vec<String>,
    min_buf: i32,
    max_buf: i32,
    pref_buf: i32,
    gran: i32,
    in_lat: i32,
    out_lat: i32,
    rate: f64,
    buf_size: i32,
    last_buf: i32,
    bit_depth: i32,
    active_in: Vec<bool>,
    active_out: Vec<bool>,
    last_in: Vec<bool>,
    last_out: Vec<bool>,
    /// the driver keeps a pointer to this while the buffers exist
    callbacks: Box<Callbacks>,
    inner: Box<Inner>,
}

impl Device {
    fn blank(name: &str, clsid: Option<Guid>, drv: Option<Drv>) -> Self {
        Self {
            name: name.to_owned(),
            clsid,
            drv,
            loaded: false,
            load_error: String::new(),
            error: None,
            in_names: Vec::new(),
            out_names: Vec::new(),
            min_buf: 0,
            max_buf: 0,
            pref_buf: 0,
            gran: 0,
            in_lat: 0,
            out_lat: 0,
            rate: 0.0,
            buf_size: 0,
            last_buf: 0,
            bit_depth: 32,
            active_in: Vec::new(),
            active_out: Vec::new(),
            last_in: Vec::new(),
            last_out: Vec::new(),
            callbacks: Box::new(Callbacks {
                buffer_switch: on_buffer_switch,
                sample_rate_did_change: on_rate_change,
                message: on_message,
                buffer_switch_time_info: on_buffer_switch_time_info,
            }),
            inner: Box::new(Inner::new()),
        }
    }

    /// A device on a driver object you already have (tests, wrappers). It takes over the COM reference: the device
    /// calls `Release` once when it is dropped (or when the driver's `init` fails).
    pub fn from_driver(name: &str, drv: Drv) -> Self {
        Self::blank(name, None, Some(drv))
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn output_channel_names(&mut self) -> &[String] {
        self.ensure_loaded();
        &self.out_names
    }

    pub fn input_channel_names(&mut self) -> &[String] {
        self.ensure_loaded();
        &self.in_names
    }

    pub fn available_sample_rates(&self) -> Vec<f64> {
        // the device manager asks every driver in the list: don't load them all
        if !self.loaded {
            return vec![44100.0, 48000.0, 88200.0, 96000.0];
        }
        let mut r: Vec<f64> = Vec::new();
        if let Some(d) = self.drv {
            r.extend(
                [44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0]
                    .into_iter()
                    .filter(|s| d.can_sample_rate(*s)),
            );
        }
        if r.is_empty() && self.rate > 0.0 {
            r.push(self.rate);
        }
        r
    }

    pub fn available_buffer_sizes(&mut self) -> Vec<i32> {
        self.ensure_loaded();
        let mut r: Vec<i32> = Vec::new();
        if self.gran == -1 {
            let mut s = self.min_buf.max(1);
            while s <= self.max_buf && r.len() < 32 {
                r.push(s);
                s = s.saturating_mul(2);
            }
        } else if self.gran > 0 {
            let mut s = self.min_buf;
            while s <= self.max_buf && r.len() < 64 {
                r.push(s);
                s = s.saturating_add(self.gran);
            }
        }
        if self.pref_buf > 0 && !r.contains(&self.pref_buf) {
            let at = r.partition_point(|x| *x < self.pref_buf);
            r.insert(at, self.pref_buf);
        }
        r
    }

    pub fn default_buffer_size(&mut self) -> i32 {
        self.ensure_loaded();
        if self.pref_buf > 0 {
            self.pref_buf
        } else {
            512
        }
    }

    /// Opens the driver with the given channels switched on (`inputs[c]` / `outputs[c]`), sample rate (0 = keep the
    /// driver's) and buffer size (0 or less = the driver's preferred). The audio callback comes with `start`.
    pub fn open(
        &mut self,
        inputs: &[bool],
        outputs: &[bool],
        rate: f64,
        buffer_size: i32,
    ) -> Result<(), OpenError> {
        self.close();
        self.error = None;
        self.ensure_loaded();
        let Some(drv) = self.drv else {
            return self.fail(OpenError::Load(self.load_error.clone()));
        };
        self.last_in = inputs.to_vec();
        self.last_out = outputs.to_vec();
        self.last_buf = buffer_size;
        self.inner.restart.store(false, Relaxed);

        let cur = drv.sample_rate();
        if rate > 0.0
            && (cur - rate).abs() > 0.5
            && !(drv.can_sample_rate(rate) && drv.set_sample_rate(rate))
        {
            return self.fail(OpenError::Rate(rate));
        }
        self.rate = drv.sample_rate();
        (self.min_buf, self.max_buf, self.pref_buf, self.gran) = drv.buffer_size();
        self.buf_size = if buffer_size > 0 {
            // juce::jlimit
            if buffer_size < self.min_buf {
                self.min_buf
            } else if self.max_buf < buffer_size {
                self.max_buf
            } else {
                buffer_size
            }
        } else {
            self.pref_buf
        };

        let on = |v: &[bool], c: usize| v.get(c).copied().unwrap_or(false);
        let mut infos: Vec<BufferInfo> = Vec::new();
        self.active_in = (0..self.in_names.len()).map(|c| on(inputs, c)).collect();
        self.active_out = (0..self.out_names.len()).map(|c| on(outputs, c)).collect();
        infos.extend(
            (0..self.active_in.len())
                .filter(|c| self.active_in[*c])
                .map(|c| BufferInfo::new(1, c as i32)),
        );
        infos.extend(
            (0..self.active_out.len())
                .filter(|c| self.active_out[*c])
                .map(|c| BufferInfo::new(0, c as i32)),
        );
        if infos.is_empty() {
            return self.fail(OpenError::NoChannels);
        }

        let me = self.inner_ptr();
        match ACTIVE.compare_exchange(null_mut(), me, SeqCst, SeqCst) {
            Ok(_) => {}
            Err(cur) if cur == me => {}
            Err(_) => return self.fail(OpenError::AnotherRunning),
        }
        if !drv.create_buffers(&mut infos, self.buf_size, &mut self.callbacks) {
            release_active(me);
            return self.fail(OpenError::Buffers(self.buf_size));
        }
        let mut types = Vec::with_capacity(infos.len());
        for b in &infos {
            let mut ci = ChannelInfo::new(b.channel_num, b.is_input);
            drv.channel_info(&mut ci);
            if bytes_per_sample(ci.kind) == 0 {
                drv.dispose_buffers();
                release_active(me);
                return self.fail(OpenError::Format(ci.kind));
            }
            types.push(ci.kind);
        }
        (self.in_lat, self.out_lat) = drv.latencies();
        self.bit_depth = types.first().map_or(32, |t| bit_depth(*t));

        let frames = self.buf_size.max(0) as usize;
        let num_inputs = self.active_in.iter().filter(|a| **a).count();
        let num_outputs = self.active_out.iter().filter(|a| **a).count();
        let state = AudioState {
            drv: Some(drv),
            infos,
            types,
            in_buf: vec![0.0; num_inputs.max(1) * frames],
            out_buf: vec![0.0; num_outputs.max(1) * frames],
            frames,
            num_inputs,
            num_outputs,
            post_output: drv.output_ready(),
        };
        // SAFETY: `ready` is still false (`close` and every failed `open` reset it, and no callback is in flight then), so a
        // buffer switch that the driver already delivers returns before it touches the audio state; this is the only
        // access until `ready` goes true on the next line. (Through the cell, not `&mut Inner`: callbacks
        // may hold `&Inner` for the atomics.)
        unsafe { *self.inner.audio.get() = state };
        self.inner.ready.store(true, Release);
        if !drv.start() {
            release_active(me);
            self.inner.ready.store(false, Release);
            drv.dispose_buffers();
            return self.fail(OpenError::NotStarted);
        }
        self.inner.open.store(true, Release);
        Ok(())
    }

    pub fn close(&mut self) {
        if !self.inner.open.load(Acquire) {
            return;
        }
        drop(self.stop());
        self.inner.open.store(false, Release);
        if let Some(d) = self.drv {
            d.stop();
            // no callback may still be reading the buffers when they go
            release_active(self.inner_ptr());
            self.inner.ready.store(false, Release);
            d.dispose_buffers();
        }
    }

    pub fn is_open(&self) -> bool {
        self.inner.open.load(Acquire)
    }

    /// Starts feeding `cb`. `false` (and `cb` dropped) if the device is not open. A callback that was already running
    /// is stopped first.
    pub fn start(&mut self, mut cb: Box<dyn AudioCallback>) -> bool {
        if !self.is_open() {
            return false;
        }
        drop(self.stop());
        cb.about_to_start(&self.stream_info());
        self.inner.faulted.store(false, Relaxed);
        drop(self.inner.swap_callback(Some(cb)));
        self.inner.playing.store(true, Release);
        true
    }

    /// Takes the callback out (calling its `stopped`) and hands it back. The audio thread may play one block of
    /// silence while this runs.
    pub fn stop(&mut self) -> Option<Box<dyn AudioCallback>> {
        self.inner.playing.store(false, Release);
        let mut old = self.inner.swap_callback(None)?;
        old.stopped();
        Some(old)
    }

    pub fn is_playing(&self) -> bool {
        self.inner.playing.load(Acquire)
    }

    /// The callback panicked since the last `start`; the device plays silence until then.
    pub fn faulted(&self) -> bool {
        self.inner.faulted.load(Relaxed)
    }

    pub fn last_error(&self) -> Option<&OpenError> {
        self.error.as_ref()
    }

    pub fn current_buffer_size(&self) -> i32 {
        self.buf_size
    }

    pub fn current_sample_rate(&self) -> f64 {
        self.rate
    }

    pub fn current_bit_depth(&self) -> i32 {
        self.bit_depth
    }

    pub fn active_output_channels(&self) -> &[bool] {
        &self.active_out
    }

    pub fn active_input_channels(&self) -> &[bool] {
        &self.active_in
    }

    pub fn output_latency(&self) -> i32 {
        self.out_lat
    }

    pub fn input_latency(&self) -> i32 {
        self.in_lat
    }

    pub fn has_control_panel(&self) -> bool {
        true
    }

    /// Shows the driver's own panel; it asks for a reset itself if something changed (see `poll_restart`).
    pub fn show_control_panel(&mut self) -> bool {
        self.ensure_loaded();
        if let Some(d) = self.drv {
            d.control_panel();
        }
        false
    }

    /// The driver changed its buffer size, rate or channels and asked for a reset: call this from the thread that owns
    /// the device (a timer on the UI thread; the C++ used `MessageManager::callAsync`). If a reset was asked for it
    /// opens again with the same choice, starts the same callback again, and returns true.
    ///
    /// If the re-open fails the callback is dropped (its `stopped()` already ran), the device stays closed, the error is
    /// left in [`Device::last_error`], and the result is still true: a reset was asked for and handled.
    pub fn poll_restart(&mut self) -> bool {
        if !self.inner.restart.swap(false, Relaxed) || !self.is_open() {
            return false;
        }
        let (inputs, outputs, buf) = (self.last_in.clone(), self.last_out.clone(), self.last_buf);
        let cb = self.stop();
        self.close();
        if self.open(&inputs, &outputs, 0.0, buf).is_ok()
            && let Some(cb) = cb
        {
            self.start(cb);
        }
        true
    }

    fn stream_info(&self) -> StreamInfo {
        StreamInfo {
            sample_rate: self.rate,
            buffer_size: self.buf_size.max(0) as usize,
            num_inputs: self.active_in.iter().filter(|a| **a).count(),
            num_outputs: self.active_out.iter().filter(|a| **a).count(),
            input_latency: self.in_lat,
            output_latency: self.out_lat,
            bit_depth: self.bit_depth,
        }
    }

    fn inner_ptr(&self) -> *mut Inner {
        let p: *const Inner = &*self.inner;
        p.cast_mut()
    }

    fn fail(&mut self, e: OpenError) -> Result<(), OpenError> {
        self.error = Some(e.clone());
        Err(e)
    }

    fn ensure_loaded(&mut self) {
        if !self.loaded {
            self.loaded = true;
            self.load();
        }
    }

    fn load(&mut self) {
        let Some(drv) = self
            .drv
            .or_else(|| self.clsid.as_ref().and_then(win::create))
        else {
            self.load_error = "The interface driver isn't installed properly".into();
            return;
        };
        // the app's window on the message thread: parent for driver dialogs
        if !drv.init(win::active_window()) {
            let msg = drv.error_message();
            self.load_error = if msg.is_empty() {
                "The interface driver didn't start (is the interface plugged in?)".into()
            } else {
                msg
            };
            drv.release();
            self.drv = None;
            return;
        }
        self.drv = Some(drv);
        let (ni, no) = drv.channels();
        let names = |count: i32, is_input: i32| -> Vec<String> {
            (0..count.max(0))
                .map(|c| {
                    let mut ci = ChannelInfo::new(c, is_input);
                    drv.channel_info(&mut ci);
                    match c_text(&ci.name) {
                        n if n.is_empty() => {
                            format!("{} {}", if is_input != 0 { "In" } else { "Out" }, c + 1)
                        }
                        n => n,
                    }
                })
                .collect()
        };
        self.in_names = names(ni, 1);
        self.out_names = names(no, 0);
        (self.min_buf, self.max_buf, self.pref_buf, self.gran) = drv.buffer_size();
        self.rate = drv.sample_rate();
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        self.close();
        if let Some(d) = self.drv.take() {
            d.release();
        }
    }
}

/// Stops the driver thread from finding this device, and waits for a callback that already has.
fn release_active(me: *mut Inner) {
    let _ = ACTIVE.compare_exchange(me, null_mut(), SeqCst, SeqCst);
    while IN_FLIGHT.load(SeqCst) != 0 {
        std::thread::yield_now();
    }
}

// --- the device type: drivers registered under HKLM\SOFTWARE\ASIO -------------------------------------------

/// The drivers installed on this machine (`Type`). Off Windows the list is empty.
pub struct DeviceType {
    entries: Vec<(String, Guid)>,
}

impl DeviceType {
    pub fn scan() -> Self {
        Self {
            entries: win::registered_drivers(),
        }
    }

    /// Reads the registry again (a driver may have been installed).
    pub fn rescan(&mut self) {
        self.entries = win::registered_drivers();
    }

    pub fn device_names(&self) -> impl Iterator<Item = &str> {
        self.entries.iter().map(|(n, _)| n.as_str())
    }

    /// There is no default driver: loading one can pop up its own dialogs (wrappers ask which device), so only the
    /// one the user picks is loaded. The driver loads on first real use of the device.
    pub fn create_device(&self, name: &str) -> Option<Device> {
        self.entries
            .iter()
            .find(|(n, _)| n == name)
            .map(|(n, clsid)| Device::blank(n, Some(*clsid), None))
    }
}
