//! C ABI over the Rust core for the hybrid phase (PLAN R2.5): the JUCE plug-ins, the OBS source and HAS
//! Karaoke can call the Rust `TrackPublisher`, `HubEngine`, `StreamConsumer` and `FriendReader` through
//! `include/hs_core.h`. Removed in R9 together with the C++.
//!
//! Threading follows the C++ classes: one message thread (connect, prepare, maintain, ...), one audio
//! thread (process), any thread for the read-only queries. The Rust types take `&mut self` for changes,
//! so every handle carries a small gate: a message-thread change closes it and waits for an audio call in
//! progress to leave; an audio call that finds it closed does nothing for that block (the Hub then leaves
//! the block as it came in, like a secondary Hub). Read-only queries never touch the engine itself: they go
//! through atomics and the shared measuring state.
//!
//! Every entry point catches a Rust panic and returns a neutral value instead of unwinding into C++.
#![allow(clippy::missing_safety_doc)]

use hs_bus::bus::set_stream_name;
use hs_bus::protocol::{BusLayout, SlotHeader, kNumStreamOuts};
use hs_bus::shm::SharedMemory;
use hs_dsp::consumer::StreamConsumer;
use hs_engine::friend_reader::FriendReader;
use hs_engine::hub_engine::{HubEngine, HubParams, Role};
use hs_engine::measure::{FriendControl, FriendInfo, HubMeasurer, PairMemory};
use hs_engine::track_publisher::{Mirror, PublisherStatus, TrackPublisher};
use std::cell::UnsafeCell;
use std::ffi::{CStr, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::{null, null_mut};
use std::sync::Arc;
use std::sync::atomic::Ordering::{Acquire, Release, SeqCst};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU32};

fn guard<T>(default: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(default)
}

/// # Safety
/// `s` is null or a NUL-terminated string.
unsafe fn text(s: *const c_char) -> String {
    if s.is_null() {
        return String::new();
    }
    // SAFETY: caller contract.
    unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned()
}

/// Copies `s` into `buf[..cap]` NUL-terminated; returns the full length of `s` in bytes.
fn copy_out(s: &str, buf: *mut c_char, cap: usize) -> usize {
    if !buf.is_null() && cap > 0 {
        let n = s.len().min(cap - 1);
        // SAFETY: caller provides `cap` writable bytes at `buf`.
        unsafe {
            std::ptr::copy_nonoverlapping(s.as_ptr().cast::<c_char>(), buf, n);
            *buf.add(n) = 0;
        }
    }
    s.len()
}

/// Message thread vs audio thread (see the crate docs).
#[derive(Default)]
struct Gate {
    closed: AtomicBool,
    active: AtomicU32,
    /// hosts call prepare / set-state / timers from different threads: one change at a time
    changer: std::sync::Mutex<()>,
}

/// Reopens the gate when a change ends, also when it panicked (else the audio side would stay shut out).
struct Reopen<'a>(&'a AtomicBool);

impl Drop for Reopen<'_> {
    fn drop(&mut self) {
        self.0.store(false, Release);
    }
}

impl Gate {
    /// Audio side: `false` = a change is in progress, skip this block.
    fn enter(&self) -> bool {
        self.active.fetch_add(1, SeqCst);
        if self.closed.load(SeqCst) {
            self.active.fetch_sub(1, Release);
            return false;
        }
        true
    }

    fn leave(&self) {
        self.active.fetch_sub(1, Release);
    }

    /// Audio side for a block that must not be skipped (an offline render): waits for a change to end.
    fn enter_waiting(&self) {
        while !self.enter() {
            std::thread::yield_now();
        }
    }

    /// Message side: waits for the audio call in progress (a block at most), then runs `f` alone.
    fn exclusive<R>(&self, f: impl FnOnce() -> R) -> R {
        let _one = self
            .changer
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        self.closed.store(true, SeqCst);
        let _reopen = Reopen(&self.closed);
        while self.active.load(SeqCst) != 0 {
            std::thread::yield_now();
        }
        f()
    }
}

// ================================================================================================
// TrackPublisher

pub struct HsTrack {
    gate: Gate,
    inner: UnsafeCell<TrackPublisher>,
    /// a mirror that arrived while a change held the gate (applied when it ends)
    pending: std::sync::Mutex<Option<Mirror>>,
}

// SAFETY: access to `inner` follows the gate: `&mut` only inside `exclusive`, shared otherwise.
unsafe impl Sync for HsTrack {}

/// `HsMirror` = `TrackPublisher::Mirror` (C layout).
#[repr(C)]
pub struct HsMirror {
    pub mon: bool,
    pub str_: bool,
    pub solo: bool,
    pub gain_db: f32,
    pub pan: f32,
    pub delay_ms: f32,
    pub trim_db: f32,
    pub stem: i32,
    pub app: bool,
}

impl HsTrack {
    fn get(&self) -> &TrackPublisher {
        // SAFETY: shared access; `&mut` exists only inside the gate, which also excludes this call's
        // callers on the message thread (they are the same thread) and audio callers (gate).
        unsafe { &*self.inner.get() }
    }

    fn change<R>(&self, f: impl FnOnce(&mut TrackPublisher) -> R) -> R {
        self.gate.exclusive(|| {
            // SAFETY: the gate excludes every other user of `inner` (audio calls and mirrors).
            let t = unsafe { &mut *self.inner.get() };
            let r = f(t);
            let late = self
                .pending
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .take();
            if let Some(m) = late {
                t.mirror(&m);
            }
            r
        })
    }
}

fn status_code(s: PublisherStatus) -> i32 {
    match s {
        PublisherStatus::Disconnected => 0,
        PublisherStatus::Connected => 1,
        PublisherStatus::SlotsFull => 2,
        PublisherStatus::ShmError => 3,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hs_track_new() -> *mut HsTrack {
    guard(null_mut(), || {
        Box::into_raw(Box::new(HsTrack {
            gate: Gate::default(),
            inner: UnsafeCell::new(TrackPublisher::new()),
            pending: std::sync::Mutex::new(None),
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_free(t: *mut HsTrack) {
    if !t.is_null() {
        // SAFETY: `t` came from `hs_track_new` and is freed once; no other call may run on it any more.
        guard((), || drop(unsafe { Box::from_raw(t) }));
    }
}

/// Returns the status: 0 disconnected, 1 connected, 2 slots full, 3 shared-memory error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_connect(
    t: *const HsTrack,
    bus: *const c_char,
    uuid: *const c_char,
    sample_rate: u32,
    num_channels: u32,
) -> i32 {
    // SAFETY: valid handle and strings (caller contract).
    let (t, bus, uuid) = unsafe { (&*t, text(bus), text(uuid)) };
    guard(3, || {
        status_code(t.change(|p| p.connect(&bus, &uuid, sample_rate, num_channels)))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_disconnect(t: *const HsTrack) {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard((), || t.change(TrackPublisher::disconnect));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_prepare(t: *const HsTrack, sample_rate: u32, num_channels: u32) {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard((), || t.get().prepare(sample_rate, num_channels));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_set_identity(
    t: *const HsTrack,
    name: *const c_char,
    color_argb: u32,
) {
    // SAFETY: valid handle and string.
    let (t, name) = unsafe { (&*t, text(name)) };
    guard((), || t.get().set_identity(&name, color_argb));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_status(t: *const HsTrack) -> i32 {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard(0, || status_code(t.get().status()))
}

/// Copies the uuid into `buf[..cap]`; returns its full length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_uuid(t: *const HsTrack, buf: *mut c_char, cap: usize) -> usize {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard(0, || copy_out(t.get().uuid(), buf, cap))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_slot_index(t: *const HsTrack) -> i32 {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard(-1, || t.get().slot_index().map_or(-1, |i| i as i32))
}

/// The bus mapping (valid until the next connect / disconnect), or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_bus(t: *const HsTrack) -> *mut BusLayout {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard(null_mut(), || {
        t.get()
            .bus()
            .map_or(null_mut(), |b| std::ptr::from_ref(b).cast_mut())
    })
}

/// This Track's slot (valid until the next connect / disconnect), or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_slot(t: *const HsTrack) -> *mut SlotHeader {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard(null_mut(), || {
        t.get()
            .slot()
            .map_or(null_mut(), |s| std::ptr::from_ref(s).cast_mut())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_hub_present(t: *const HsTrack) -> bool {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard(false, || t.get().hub_present())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_solo_active(t: *const HsTrack) -> bool {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    guard(false, || t.get().solo_active())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_mirror(t: *const HsTrack, m: *const HsMirror) {
    // SAFETY: valid handle and struct.
    let (t, m) = unsafe { (&*t, &*m) };
    let m = Mirror {
        mon: m.mon,
        str_: m.str_,
        solo: m.solo,
        gain_db: m.gain_db,
        pan: m.pan,
        delay_ms: m.delay_ms,
        trim_db: m.trim_db,
        stem: m.stem,
        app: m.app,
    };
    guard((), || {
        // the Track mirrors from the audio thread too: it must not touch the publisher during a change
        if t.gate.enter() {
            t.get().mirror(&m);
            t.gate.leave();
        } else if let Ok(mut p) = t.pending.try_lock() {
            *p = Some(m); // applied when the change ends (no wait on the audio thread)
        }
    });
}

/// Audio thread. `ch[0..num_ch]` each hold `n` frames.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_track_process(
    t: *const HsTrack,
    ch: *const *const f32,
    num_ch: i32,
    n: i32,
    time_samples: i64,
    playing: bool,
    offline: bool,
    bypassed: bool,
) {
    // SAFETY: valid handle.
    let t = unsafe { &*t };
    if n <= 0 || num_ch <= 0 || ch.is_null() || !t.gate.enter() {
        return;
    }
    let (n, k) = (n as usize, (num_ch as usize).min(2));
    // SAFETY: caller provides `num_ch` channel pointers of `n` floats each.
    let chans: [&[f32]; 2] = unsafe {
        let c0 = std::slice::from_raw_parts(*ch, n);
        [
            c0,
            if k > 1 {
                std::slice::from_raw_parts(*ch.add(1), n)
            } else {
                c0
            },
        ]
    };
    guard((), || {
        t.get()
            .process(&chans[..k], n, time_samples, playing, offline, bypassed)
    });
    t.gate.leave();
}

// ================================================================================================
// HubEngine

pub struct HsHub {
    gate: Gate,
    inner: UnsafeCell<HubEngine>,
    measurer: HubMeasurer,
    /// the mapping the message thread reads through (kept with every old one: pointers handed out stay valid)
    mappings: UnsafeCell<Vec<Arc<SharedMemory>>>,
    bus: AtomicPtr<BusLayout>,
    latency: AtomicI32,
    active_mask: std::sync::atomic::AtomicU64,
}

// SAFETY: `inner` and `mappings` change only inside the gate (message thread); the audio thread gets `&mut
// HubEngine` only through `enter`, which the gate makes exclusive with those changes; every query uses
// atomics or the measurer.
unsafe impl Sync for HsHub {}

/// `HsHubParams` = `HubParams` (C layout).
#[repr(C)]
pub struct HsHubParams {
    pub master_db: f32,
    pub limiter_on: bool,
    pub ceiling_db: f32,
    pub preview: bool,
    pub panic: bool,
    pub sync_safety: i32,
    pub bypassed: bool,
    pub silence: bool,
}

#[repr(C)]
#[derive(Default)]
pub struct HsFxLatency {
    pub master_ms: f64,
    pub tracks_ms: f64,
}

#[repr(C)]
#[derive(Default)]
pub struct HsSourceReadStats {
    pub lead_min: i64,
    pub lead_max: i64,
    pub jumps: u32,
    pub blocks: u32,
}

impl HsHub {
    fn change<R>(&self, f: impl FnOnce(&mut HubEngine) -> R) -> R {
        self.gate.exclusive(|| {
            // SAFETY: exclusive by the gate (see the Sync impl).
            let e = unsafe { &mut *self.inner.get() };
            let r = f(e);
            self.publish(e);
            r
        })
    }

    /// After a change: what the queries read.
    fn publish(&self, e: &HubEngine) {
        self.latency.store(e.latency_frames(), Release);
        let shm = self.measurer.shared_memory();
        // SAFETY: inside the gate (message thread).
        let maps = unsafe { &mut *self.mappings.get() };
        let ptr = shm
            .as_ref()
            .map_or(null_mut(), |m| std::ptr::from_ref(m.layout()).cast_mut());
        if let Some(m) = shm
            && !maps.iter().any(|o| Arc::ptr_eq(o, &m))
        {
            maps.push(m);
        }
        self.bus.store(ptr, Release);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hs_hub_new() -> *mut HsHub {
    guard(null_mut(), || {
        let engine = HubEngine::new();
        let measurer = engine.measurer();
        Box::into_raw(Box::new(HsHub {
            gate: Gate::default(),
            inner: UnsafeCell::new(engine),
            measurer,
            mappings: UnsafeCell::new(Vec::new()),
            bus: AtomicPtr::new(null_mut()),
            latency: AtomicI32::new(0),
            active_mask: std::sync::atomic::AtomicU64::new(0),
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_free(h: *mut HsHub) {
    if !h.is_null() {
        // SAFETY: from `hs_hub_new`, freed once.
        guard((), || drop(unsafe { Box::from_raw(h) }));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_connect(h: *const HsHub, bus: *const c_char) -> bool {
    // SAFETY: valid handle and string.
    let (h, bus) = unsafe { (&*h, text(bus)) };
    guard(false, || h.change(|e| e.connect(&bus)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_disconnect(h: *const HsHub) {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    guard((), || h.change(HubEngine::disconnect));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_maintain(h: *const HsHub) {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    // no gate: the work runs on the shared state, so the audio side never skips a block for it
    guard((), || h.measurer.maintain());
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_prepare(h: *const HsHub, sample_rate: f64, max_block: i32) {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    guard((), || {
        h.change(|e| e.prepare(sample_rate, max_block.max(0) as usize))
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_set_output_name(h: *const HsHub, output: i32, name: *const c_char) {
    // SAFETY: valid handle and string.
    let (h, name) = unsafe { (&*h, text(name)) };
    guard((), || {
        if let (Some(m), Ok(o)) = (h.measurer.shared_memory(), usize::try_from(output))
            && o < kNumStreamOuts
        {
            set_stream_name(&m.layout().streamHeader, o, &name);
        }
    });
}

/// 0 disconnected, 1 owner, 2 secondary.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_role(h: *const HsHub) -> i32 {
    // SAFETY: valid handle.
    match unsafe { &*h }.measurer.role() {
        Role::Disconnected => 0,
        Role::Owner => 1,
        Role::Secondary => 2,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_bus(h: *const HsHub) -> *mut BusLayout {
    // SAFETY: valid handle.
    unsafe { &*h }.bus.load(Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_latency_frames(h: *const HsHub) -> i32 {
    // SAFETY: valid handle.
    unsafe { &*h }.latency.load(Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_last_block_size(h: *const HsHub) -> i32 {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    guard(256, || h.measurer.last_block_size())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_history_frames(h: *const HsHub) -> u64 {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    guard(0, || h.measurer.history_frames())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_measure_latencies(h: *const HsHub, out: *mut HsFxLatency) {
    // SAFETY: valid handle and out-pointer.
    let (h, out) = unsafe { (&*h, &mut *out) };
    let r = guard(None, || Some(h.measurer.measure_latencies()));
    let r = r.unwrap_or_default();
    *out = HsFxLatency {
        master_ms: r.master_ms,
        tracks_ms: r.tracks_ms,
    };
}

/// `true` and `*lag_ms` when found. `score` may be null. `remove_ms` NaN = none (any finite value is a
/// delay, negative ones too: a faint reading can be early).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_measure_lag(
    h: *const HsHub,
    index: i32,
    min_score: f64,
    any_polarity: bool,
    source: bool,
    faint: bool,
    remove_ms: f64,
    lag_ms: *mut f64,
    score: *mut f64,
) -> bool {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    let Ok(index) = usize::try_from(index) else {
        return false;
    };
    let rm = (!remove_ms.is_nan()).then_some(remove_ms);
    let (lag, s) = guard((None, 0.0), || {
        h.measurer
            .measure_lag(index, min_score, any_polarity, source, faint, rm)
    });
    if !score.is_null() {
        // SAFETY: caller's out-pointer.
        unsafe { *score = s };
    }
    if let (Some(l), false) = (lag, lag_ms.is_null()) {
        // SAFETY: caller's out-pointer.
        unsafe { *lag_ms = l };
    }
    lag.is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_take_source_read_stats(
    h: *const HsHub,
    index: i32,
    out: *mut HsSourceReadStats,
) {
    // SAFETY: valid handle and out-pointer.
    let (h, out) = unsafe { (&*h, &mut *out) };
    let s = usize::try_from(index)
        .ok()
        .map(|i| guard(Default::default(), || h.measurer.take_source_read_stats(i)))
        .unwrap_or_default();
    *out = HsSourceReadStats {
        lead_min: s.lead_min,
        lead_max: s.lead_max,
        jumps: s.jumps,
        blocks: s.blocks,
    };
}

/// The friend's control block (C layout = `HubEngine::FriendControl`), or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_friend_control(h: *const HsHub, j: i32) -> *const FriendControl {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    usize::try_from(j)
        .ok()
        .filter(|&j| j < 8)
        .map_or(null(), |j| std::ptr::from_ref(h.measurer.friend_control(j)))
}

/// The friend's info block (C layout = `HubEngine::FriendInfo`), or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_friend_info(h: *const HsHub, j: i32) -> *const FriendInfo {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    usize::try_from(j)
        .ok()
        .filter(|&j| j < 8)
        .map_or(null(), |j| std::ptr::from_ref(h.measurer.friend_info(j)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_set_line_up(h: *const HsHub, on: bool, limit_ms: f32) {
    // SAFETY: valid handle.
    unsafe { &*h }.measurer.set_line_up(on, limit_ms);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_line_up_ms(h: *const HsHub) -> f32 {
    // SAFETY: valid handle.
    unsafe { &*h }.measurer.line_up_ms()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_line_up_slowest(h: *const HsHub) -> i32 {
    // SAFETY: valid handle.
    unsafe { &*h }.measurer.line_up_slowest()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_line_up_changes(h: *const HsHub) -> u32 {
    // SAFETY: valid handle.
    unsafe { &*h }.measurer.line_up_changes()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_pair_with(h: *const HsHub, feeder: i32, arg: u32) -> bool {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    usize::try_from(feeder).is_ok_and(|f| guard(false, || h.measurer.pair_with(f, arg)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_start_probe(h: *const HsHub, feeder: i32) -> bool {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    usize::try_from(feeder).is_ok_and(|f| guard(false, || h.measurer.start_probe(f)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_probing(h: *const HsHub) -> bool {
    // SAFETY: valid handle.
    unsafe { &*h }.measurer.probing()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_probe_step(h: *const HsHub) {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    guard((), || h.measurer.probe_step());
}

/// The pair memory as text, one pair per line: `feeder TAB slot-uuid LF`. Copies into `buf[..cap]`
/// (NUL-terminated) and returns the full length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_pair_memory(
    h: *const HsHub,
    buf: *mut c_char,
    cap: usize,
) -> usize {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    guard(0, || {
        let s: String = h
            .measurer
            .pair_memory()
            .iter()
            .map(|m| format!("{}\t{}\n", m.feeder, m.slot_uuid))
            .collect();
        copy_out(&s, buf, cap)
    })
}

/// Sets the pair memory from text in the `hs_hub_pair_memory` format.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_set_pair_memory(h: *const HsHub, textp: *const c_char) {
    // SAFETY: valid handle and string.
    let (h, t) = unsafe { (&*h, text(textp)) };
    guard((), || {
        let mem = t
            .lines()
            .filter_map(|l| l.split_once('\t'))
            .map(|(f, u)| PairMemory {
                feeder: f.to_owned(),
                slot_uuid: u.to_owned(),
            })
            .collect();
        h.measurer.set_pair_memory(mem);
    });
}

/// `void tap(void* user, int friendIndex, const float* const* ch, int n, int64_t timeSamples, bool playing)`,
/// called on the audio thread; must not allocate or lock.
pub type HsFriendTap =
    Option<unsafe extern "C" fn(*mut c_void, i32, *const *const f32, i32, i64, bool)>;

struct TapUser(*mut c_void);

impl TapUser {
    fn get(&self) -> *mut c_void {
        self.0
    }
}
// SAFETY: the C++ side owns `user` and promises it may be used from the audio thread.
unsafe impl Send for TapUser {}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_set_friend_tap(
    h: *const HsHub,
    tap: HsFriendTap,
    user: *mut c_void,
) {
    // SAFETY: valid handle.
    let h = unsafe { &*h };
    let user = TapUser(user);
    guard((), || {
        h.change(|e| {
            e.set_friend_tap(tap.map(|f| {
                let u = user;
                Box::new(move |j: usize, ch: [&[f32]; 2], time: i64, playing: bool| {
                    let ptrs = [ch[0].as_ptr(), ch[1].as_ptr()];
                    // SAFETY: the callback's contract (see `HsFriendTap`).
                    unsafe {
                        f(
                            u.get(),
                            j as i32,
                            ptrs.as_ptr(),
                            ch[0].len() as i32,
                            time,
                            playing,
                        )
                    };
                }) as hs_engine::hub_engine::FriendTap
            }))
        })
    });
}

/// Audio thread. `io[0..num_ch]` hold `n` frames: in, the master bus; out, the headphone mix.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_process(
    h: *const HsHub,
    io: *const *mut f32,
    num_ch: i32,
    n: i32,
    p: *const HsHubParams,
    time_samples: i64,
    playing: bool,
    offline: bool,
) {
    // SAFETY: valid handle and params.
    let (h, p) = unsafe { (&*h, &*p) };
    if n <= 0 || num_ch <= 0 || io.is_null() {
        return;
    }
    if offline {
        h.gate.enter_waiting(); // an export must not lose a block to a change on another thread
    } else if !h.gate.enter() {
        return; // connect / prepare in progress: the block stays as it came in
    }
    let params = HubParams {
        master_db: p.master_db,
        limiter_on: p.limiter_on,
        ceiling_db: p.ceiling_db,
        preview: p.preview,
        panic: p.panic,
        sync_safety: p.sync_safety,
        bypassed: p.bypassed,
        silence: p.silence,
    };
    let n = n as usize;
    guard((), || {
        // SAFETY: exclusive with every change (gate); the caller provides `num_ch` buffers of `n` floats.
        let e = unsafe { &mut *h.inner.get() };
        // SAFETY: see the comment at the top of this function (caller-provided buffers).
        unsafe {
            if num_ch >= 2 {
                let a = std::slice::from_raw_parts_mut(*io, n);
                let b = std::slice::from_raw_parts_mut(*io.add(1), n);
                e.process(&mut [a, b], &params, time_samples, playing, offline);
            } else {
                let a = std::slice::from_raw_parts_mut(*io, n);
                e.process(&mut [a], &params, time_samples, playing, offline);
            }
        }
        h.active_mask.store(e.active_slots_mask(), Release);
    });
    h.gate.leave();
}

// ================================================================================================
// StreamConsumer (one thread: the OBS audio thread)

#[repr(C)]
pub struct HsConsumerStatus {
    pub connected: bool,
    pub hub_alive: bool,
    pub streaming: bool,
    pub daw_rate: u32,
    pub buffer_ms: f64,
    pub target_ms: f64,
    pub auto_buffer: bool,
    pub drift_ppm: f64,
    pub underruns: u64,
    pub overruns: u64,
    pub resyncs: u64,
    pub hub_flags: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn hs_consumer_new() -> *mut StreamConsumer {
    guard(null_mut(), || {
        Box::into_raw(Box::new(StreamConsumer::new()))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_consumer_free(c: *mut StreamConsumer) {
    if !c.is_null() {
        // SAFETY: from `hs_consumer_new`, freed once.
        guard((), || drop(unsafe { Box::from_raw(c) }));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_consumer_configure(
    c: *mut StreamConsumer,
    bus: *const c_char,
    output: i32,
    buffer_ms: f64,
    out_rate: u32,
) {
    // SAFETY: valid handle and string; one thread.
    let (c, bus) = unsafe { (&mut *c, text(bus)) };
    guard((), || c.configure(&bus, output, buffer_ms, out_rate));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_consumer_set_report_to_hub(c: *mut StreamConsumer, on: bool) {
    // SAFETY: valid handle; one thread.
    unsafe { &mut *c }.set_report_to_hub(on);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_consumer_pull(
    c: *mut StreamConsumer,
    out: *const *mut f32,
    frames: u32,
    dt_sec: f64,
) {
    // SAFETY: valid handle; two output buffers of `frames` floats.
    let c = unsafe { &mut *c };
    guard((), || {
        // SAFETY: see the comment at the top of this function (caller-provided buffers).
        let (a, b) = unsafe {
            (
                std::slice::from_raw_parts_mut(*out, frames as usize),
                std::slice::from_raw_parts_mut(*out.add(1), frames as usize),
            )
        };
        c.pull([a, b], frames, dt_sec);
    });
}

/// Fills `out`; the output name is copied into `name[..cap]`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_consumer_status(
    c: *const StreamConsumer,
    out: *mut HsConsumerStatus,
    name: *mut c_char,
    cap: usize,
) {
    // SAFETY: valid handle and out-pointer.
    let (c, out) = unsafe { (&*c, &mut *out) };
    let s = c.status();
    copy_out(&s.output_name, name, cap);
    *out = HsConsumerStatus {
        connected: s.connected,
        hub_alive: s.hub_alive,
        streaming: s.streaming,
        daw_rate: s.daw_rate,
        buffer_ms: s.buffer_ms,
        target_ms: s.target_ms,
        auto_buffer: s.auto_buffer,
        drift_ppm: s.drift_ppm,
        underruns: s.underruns,
        overruns: s.overruns,
        resyncs: s.resyncs,
        hub_flags: s.hub_flags,
    };
}

// ================================================================================================
// FriendReader (HEARASIDE Track in "friend input" mode; one audio thread, prepare on the message thread
// while the audio is stopped)

#[unsafe(no_mangle)]
pub extern "C" fn hs_reader_new() -> *mut FriendReader {
    guard(null_mut(), || Box::into_raw(Box::new(FriendReader::new())))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_free(r: *mut FriendReader) {
    if !r.is_null() {
        // SAFETY: from `hs_reader_new`, freed once.
        guard((), || drop(unsafe { Box::from_raw(r) }));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_prepare(r: *mut FriendReader, sample_rate: f64, max_block: i32) {
    // SAFETY: valid handle.
    let r = unsafe { &mut *r };
    guard((), || r.prepare(sample_rate, max_block.max(0) as usize));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_reset(r: *mut FriendReader) {
    // SAFETY: valid handle.
    unsafe { &mut *r }.reset();
}

/// Audio thread. `ring` / `write_pos` are a friend's ring and its write position in the bus.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_pull(
    r: *mut FriendReader,
    ring: *const hs_bus::protocol::ChannelRing,
    write_pos: *const std::sync::atomic::AtomicU64,
    rate: u32,
    out: *const *mut f32,
    n: i32,
) -> bool {
    if n <= 0 {
        return false;
    }
    // SAFETY: valid handle, ring, write position and two output buffers of `n` floats.
    let (r, ring, wp) = unsafe { (&mut *r, &*ring, &*write_pos) };
    guard(false, || {
        // SAFETY: see the comment at the top of this function (caller-provided buffers).
        let (a, b) = unsafe {
            (
                std::slice::from_raw_parts_mut(*out, n as usize),
                std::slice::from_raw_parts_mut(*out.add(1), n as usize),
            )
        };
        r.pull(ring, wp, rate, [a, b], n as usize)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_buffer_ms(r: *const FriendReader) -> f64 {
    // SAFETY: valid handle.
    unsafe { &*r }.buffer_ms()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_primed(r: *const FriendReader) -> bool {
    // SAFETY: valid handle.
    unsafe { &*r }.primed()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_underruns(r: *const FriendReader) -> u32 {
    // SAFETY: valid handle.
    unsafe { &*r }.underruns()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_reprimes(r: *const FriendReader) -> u32 {
    // SAFETY: valid handle.
    unsafe { &*r }.reprimes()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_concealed_ms(r: *const FriendReader) -> f64 {
    // SAFETY: valid handle.
    unsafe { &*r }.concealed_ms()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_reader_jitter_p95_ms(r: *const FriendReader) -> f64 {
    // SAFETY: valid handle.
    unsafe { &*r }.jitter_p95_ms()
}

/// The probe sweep at `sample_rate`: copies up to `cap` samples into `out` and returns the full length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_probe_sweep(sample_rate: f64, out: *mut f32, cap: usize) -> usize {
    guard(0, || {
        let s = hs_engine::hub_engine::probe_sweep(sample_rate);
        if !out.is_null() {
            let n = s.len().min(cap);
            // SAFETY: caller provides `cap` writable floats.
            unsafe { std::ptr::copy_nonoverlapping(s.as_ptr(), out, n) };
        }
        s.len()
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn hs_hub_active_slots_mask(h: *const HsHub) -> u64 {
    // SAFETY: valid handle.
    unsafe { &*h }.active_mask.load(Acquire)
}
