//! Everything of `obs/hearaside-obs/src/plugin.cpp` that does not call libobs: names and defaults of the source,
//! the texts, settings mapping, status line, the 5 ms feed loop body and the Streamlabs struct layout. Kept apart
//! from `module.rs` so it builds and tests without OBS.
use hs_bus::protocol::HubFlags;
use hs_dsp::consumer::{ConsumerStatus, StreamConsumer};
use std::ffi::CStr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

// ---- names the user's scene collections and hotkey bindings refer to (PLAN section 3.4 item 6) ------------

pub const SOURCE_ID: &CStr = c"hearaside_source";
pub const SOURCE_NAME: &CStr = c"HEARASIDE";
pub const HOTKEY_MUTE_STREAM: &CStr = c"hearaside.mute_stream";
pub const HOTKEY_PREVIEW: &CStr = c"hearaside.preview";
pub const MODULE_AUTHOR: &CStr = c"HEARASIDE";
pub const MODULE_DESCRIPTION: &CStr = c"Receives the HEARASIDE viewers' mix from your DAW.";

/// Keys of the source's saved settings.
pub mod key {
    use std::ffi::CStr;
    pub const BUS: &CStr = c"bus";
    pub const OUTPUT: &CStr = c"output";
    pub const BUFFER_MS: &CStr = c"buffer_ms";
    /// not a property: remembers that the first-use Audio Monitoring "off" was applied
    pub const MONITORING_CHECKED: &CStr = c"monitoring_checked";
}

/// Names of the entries of the properties panel.
pub mod prop {
    use std::ffi::CStr;
    pub const MONITORING_WARNING: &CStr = c"monitoring_warning";
    pub const MONITOR_OFF: &CStr = c"monitor_off";
    pub const STATUS_INFO: &CStr = c"status_info";
    pub const REFRESH: &CStr = c"refresh";
}

pub const DEFAULT_BUS: &str = "Main";
pub const DEFAULT_OUTPUT: i64 = 0;
/// 0 = adaptive
pub const DEFAULT_BUFFER_MS: i64 = 0;

/// The Buffer list: (text, value in ms).
pub const BUFFER_CHOICES: [(Text, i64); 4] = [
    (T_BUFFER_AUTO, 0),
    (T_BUFFER_LOW, 15),
    (T_BUFFER_NORMAL, 30),
    (T_BUFFER_SAFE, 60),
];

// ---- texts: the same pairs as the C++ `T(en, th)` calls (OBS's own locale decides, not the HEARASIDE setting) ---

/// English and Thai text of one label.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Text {
    pub en: &'static CStr,
    pub th: &'static CStr,
}

impl Text {
    const fn new(en: &'static CStr, th: &'static CStr) -> Self {
        Text { en, th }
    }

    pub fn pick(&self, thai: bool) -> &'static CStr {
        if thai { self.th } else { self.en }
    }

    pub fn str(&self, thai: bool) -> &'static str {
        // built from `c"..."` literals of `&str` source: always UTF-8
        self.pick(thai).to_str().unwrap_or("")
    }
}

/// `thai()`: OBS runs in a Thai locale.
pub fn is_thai(locale: Option<&str>) -> bool {
    locale.is_some_and(|l| l.starts_with("th"))
}

pub const T_WAITING: Text = Text::new(
    c"Waiting for HEARASIDE Hub (open your DAW project with the Hub on the master bus)",
    c"รอ HEARASIDE Hub (เปิดโปรเจกต์ DAW ที่มี Hub อยู่ที่ Master)",
);
pub const T_BYPASSED: Text = Text::new(
    c"Hub is bypassed - viewers hear nothing",
    c"Hub ถูก bypass อยู่ คนดูจะไม่ได้ยินอะไร",
);
pub const T_NOT_RUNNING: Text = Text::new(
    c"Connected, but the Hub is not running (DAW stopped audio?)",
    c"เชื่อมแล้ว แต่ Hub ไม่ทำงาน (DAW ปิดเสียงอยู่หรือเปล่า)",
);
pub const T_CONNECTED: Text = Text::new(c"Connected", c"เชื่อมแล้ว");
pub const T_AUTO_SUFFIX: Text = Text::new(c" (auto)", c" (อัตโนมัติ)");
pub const T_MUTED_IN_HUB: Text = Text::new(c" · stream muted in Hub", c" · ตัดเสียงคนดูอยู่ใน Hub");
pub const T_HOTKEY_MUTE: Text = Text::new(
    c"HEARASIDE: Mute / unmute stream",
    c"HEARASIDE: ตัด / คืนเสียงคนดู",
);
pub const T_HOTKEY_PREVIEW: Text = Text::new(
    c"HEARASIDE: Hear viewers' mix on / off",
    c"HEARASIDE: ฟังแบบคนดู เปิด / ปิด",
);
pub const T_BUS_NAME: Text = Text::new(c"Bus name", c"ชื่อ Bus");
pub const T_OUTPUT: Text = Text::new(c"Output", c"เสียงที่ส่ง");
pub const T_STREAM_MIX: Text = Text::new(
    c"Stream Mix (what viewers hear)",
    c"Stream Mix (เสียงที่คนดูได้ยิน)",
);
pub const T_BUFFER: Text = Text::new(c"Buffer", c"บัฟเฟอร์");
pub const T_BUFFER_AUTO: Text = Text::new(
    c"Auto - lowest safe (recommended)",
    c"อัตโนมัติ - ต่ำสุดที่ปลอดภัย (แนะนำ)",
);
pub const T_BUFFER_LOW: Text = Text::new(c"Low (15 ms)", c"ต่ำ (15 ms)");
pub const T_BUFFER_NORMAL: Text = Text::new(c"Normal (30 ms)", c"ปกติ (30 ms)");
pub const T_BUFFER_SAFE: Text = Text::new(c"Safe (60 ms)", c"ปลอดภัย (60 ms)");
pub const T_MONITORING_WARNING: Text = Text::new(
    c"Audio Monitoring is on: you hear the DAW twice (the second time late). Viewers are not affected.",
    c"Audio Monitoring เปิดอยู่: คุณได้ยินเสียง DAW 2 รอบ (รอบสองช้ากว่า) คนดูไม่ได้รับผลกระทบ",
);
pub const T_MONITOR_OFF: Text = Text::new(c"Turn Audio Monitoring off", c"ปิด Audio Monitoring ให้");
pub const T_STATUS_HELP: Text = Text::new(
    c"Set this source's Audio Monitoring to \"Monitor Off\" - you already hear your DAW. If the DAW also plays through Desktop Audio, mute Desktop Audio or viewers hear everything twice.",
    c"ตั้ง Audio Monitoring ของ source นี้เป็น \"Monitor Off\" เพราะคุณฟังจาก DAW อยู่แล้ว และถ้า DAW เล่นเสียงผ่าน Desktop Audio ด้วย ให้ปิด Desktop Audio ไม่งั้นคนดูจะได้ยินซ้ำ 2 ชั้น",
);
pub const T_REFRESH: Text = Text::new(c"Refresh status", c"อัปเดตสถานะ");

/// Every `T(...)` pair of plugin.cpp (the compat test checks this list against the C++ file both ways).
pub const ALL_TEXTS: [Text; 20] = [
    T_WAITING,
    T_BYPASSED,
    T_NOT_RUNNING,
    T_CONNECTED,
    T_AUTO_SUFFIX,
    T_MUTED_IN_HUB,
    T_HOTKEY_MUTE,
    T_HOTKEY_PREVIEW,
    T_BUS_NAME,
    T_OUTPUT,
    T_STREAM_MIX,
    T_BUFFER,
    T_BUFFER_AUTO,
    T_BUFFER_LOW,
    T_BUFFER_NORMAL,
    T_BUFFER_SAFE,
    T_MONITORING_WARNING,
    T_MONITOR_OFF,
    T_STATUS_HELP,
    T_REFRESH,
];

// ---- settings ----------------------------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Config {
    pub bus: String,
    pub output: i32,
    pub buffer_ms: i32,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            bus: DEFAULT_BUS.into(),
            output: 0,
            buffer_ms: 0,
        }
    }
}

impl Config {
    /// `ss_update`: an empty bus name means "Main"; a nonzero buffer under 5 ms means adaptive (0). The C++ narrows
    /// the 64-bit settings with `int(...)` (wraps); this saturates, so a hand-edited huge number cannot land on
    /// another value.
    pub fn from_settings(bus: Option<&str>, output: i64, buffer_ms: i64) -> Self {
        let narrow = |v: i64| v.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32;
        let mut buffer_ms = narrow(buffer_ms);
        if buffer_ms != 0 && buffer_ms < 5 {
            buffer_ms = 0;
        }
        Config {
            bus: bus.filter(|b| !b.is_empty()).unwrap_or(DEFAULT_BUS).into(),
            output: narrow(output),
            buffer_ms,
        }
    }
}

// ---- status ------------------------------------------------------------------------------------------------

/// The numbers `statusText` uses, as a plain copy.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StatusSnap {
    pub connected: bool,
    pub hub_alive: bool,
    pub hub_flags: u32,
    pub daw_rate: u32,
    pub buffer_ms: f64,
    pub auto_buffer: bool,
    pub drift_ppm: f64,
    pub underruns: u64,
}

/// The feed thread's status for the properties panel. The C++ copies a `ConsumerStatus` (two `std::string`s) under a
/// mutex every 5 ms; here the thread stores plain atomics, so the feed loop takes no lock and allocates nothing.
/// Fields are read one by one: a panel refresh can show numbers from two neighbouring ticks, which is harmless.
#[derive(Default)]
pub struct StatusCell {
    flags: AtomicU32,
    hub_flags: AtomicU32,
    daw_rate: AtomicU32,
    buffer_ms: AtomicU64,
    drift_ppm: AtomicU64,
    underruns: AtomicU64,
}

const F_CONNECTED: u32 = 1;
const F_HUB_ALIVE: u32 = 2;
const F_AUTO: u32 = 4;

impl StatusCell {
    pub fn store(&self, s: &ConsumerStatus) {
        let bit = |on: bool, mask: u32| if on { mask } else { 0 };
        let flags = bit(s.connected, F_CONNECTED)
            | bit(s.hub_alive, F_HUB_ALIVE)
            | bit(s.auto_buffer, F_AUTO);
        self.flags.store(flags, Ordering::Relaxed);
        self.hub_flags.store(s.hub_flags, Ordering::Relaxed);
        self.daw_rate.store(s.daw_rate, Ordering::Relaxed);
        self.buffer_ms
            .store(s.buffer_ms.to_bits(), Ordering::Relaxed);
        self.drift_ppm
            .store(s.drift_ppm.to_bits(), Ordering::Relaxed);
        self.underruns.store(s.underruns, Ordering::Relaxed);
    }

    pub fn load(&self) -> StatusSnap {
        let flags = self.flags.load(Ordering::Relaxed);
        StatusSnap {
            connected: flags & F_CONNECTED != 0,
            hub_alive: flags & F_HUB_ALIVE != 0,
            hub_flags: self.hub_flags.load(Ordering::Relaxed),
            daw_rate: self.daw_rate.load(Ordering::Relaxed),
            buffer_ms: f64::from_bits(self.buffer_ms.load(Ordering::Relaxed)),
            auto_buffer: flags & F_AUTO != 0,
            drift_ppm: f64::from_bits(self.drift_ppm.load(Ordering::Relaxed)),
            underruns: self.underruns.load(Ordering::Relaxed),
        }
    }
}

/// `statusText`: the line under the properties.
pub fn status_text(s: &StatusSnap, thai: bool) -> String {
    if !s.connected {
        return T_WAITING.str(thai).into();
    }
    if !s.hub_alive {
        if s.hub_flags & HubFlags::kHubBypassed != 0 {
            return T_BYPASSED.str(thai).into();
        }
        return T_NOT_RUNNING.str(thai).into();
    }
    format!(
        "{} · DAW {:.1} kHz · buffer {:.1} ms{} · drift {:+.0} ppm · underrun {}{}",
        T_CONNECTED.str(thai),
        f64::from(s.daw_rate) / 1000.0,
        s.buffer_ms,
        if s.auto_buffer {
            T_AUTO_SUFFIX.str(thai)
        } else {
            ""
        },
        s.drift_ppm,
        s.underruns,
        if s.hub_flags & HubFlags::kHubPanic != 0 {
            T_MUTED_IN_HUB.str(thai)
        } else {
            ""
        },
    )
}

/// An entry of the Output list: "Stem 3", or "Stem 3 - <name the Hub published>".
pub fn stem_label(index: usize, hub_name: &str) -> String {
    let mut label = format!("Stem {index}");
    if !hub_name.is_empty() && hub_name != label {
        label.push_str(" - ");
        label.push_str(hub_name);
    }
    label
}

// ---- shared state of one source ------------------------------------------------------------------------------

/// What the OBS threads (settings, panel, hotkeys) and the feed thread share.
pub struct Shared {
    cfg: Mutex<Config>,
    /// bumped by every settings change (`cfgDirty`); starts dirty
    cfg_seq: AtomicU32,
    pub status: StatusCell,
    pub running: AtomicBool,
}

impl Default for Shared {
    fn default() -> Self {
        Shared {
            cfg: Mutex::new(Config::default()),
            cfg_seq: AtomicU32::new(1),
            status: StatusCell::default(),
            running: AtomicBool::new(true),
        }
    }
}

impl Shared {
    pub fn set_config(&self, cfg: Config) {
        let mut g = self
            .cfg
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        *g = cfg;
        self.cfg_seq.fetch_add(1, Ordering::Release);
    }

    pub fn bus(&self) -> String {
        self.cfg
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .bus
            .clone()
    }
}

// ---- the feed loop ---------------------------------------------------------------------------------------------

/// One tick of the feed thread, in ns.
pub const TICK_NS: u64 = 5_000_000;
/// If the thread is later than this after a sleep (system stall) it does not try to catch up.
pub const STALL_NS: u64 = 200_000_000;

/// Timestamps for the blocks. A stamp in the past makes OBS grow its global audio buffering (and it never shrinks
/// while OBS runs), so every block is stamped as audio that just finished arriving (start = now - duration), pulled
/// gently towards that and never backwards; OBS smooths small jitter itself.
pub struct Stamper {
    ts: u64,
}

impl Stamper {
    pub fn new(now_ns: u64) -> Self {
        Stamper { ts: now_ns }
    }

    /// The stamp of the block that ends at `now_ns` and lasts `dur_ns`.
    pub fn stamp(&mut self, now_ns: u64, dur_ns: u64) -> u64 {
        let fresh = if now_ns > dur_ns {
            now_ns - dur_ns
        } else {
            now_ns
        };
        let diff = (self.ts as i64).wrapping_sub(fresh as i64);
        if !(-20_000_000..=20_000_000).contains(&diff) {
            self.ts = fresh; // re-anchor after a stall
        } else if fresh > self.ts {
            self.ts += (fresh - self.ts) / 8;
        }
        let stamp = self.ts;
        self.ts += dur_ns;
        stamp
    }
}

/// A block ready for `obs_source_output_audio`.
pub struct Block<'a> {
    pub left: &'a [f32],
    pub right: &'a [f32],
    pub frames: u32,
    pub rate: u32,
    pub timestamp: u64,
}

/// The body of `Source::run`: pulls 5 ms of audio from the bus at OBS's sample rate. Everything is allocated in
/// `new`; `apply_config` allocates only when the settings changed (and `StreamConsumer::pull` once, when it first
/// connects: the gap documented in hs-dsp, same as C++).
pub struct Feeder {
    consumer: StreamConsumer,
    left: Vec<f32>,
    right: Vec<f32>,
    rate: u32,
    frames: u32,
    last: u64,
    stamper: Stamper,
    applied: u32,
}

impl Feeder {
    /// `rate`: OBS's audio sample rate. A rate of 0 (no audio output yet) would divide by zero in the C++; it falls
    /// back to 48 kHz here and OBS resamples from the rate each block states.
    pub fn new(rate: u32, now_ns: u64) -> Self {
        let rate = if rate == 0 { 48_000 } else { rate };
        let frames = rate / 200; // 5 ms per tick: less waiting between DAW block and OBS
        Feeder {
            consumer: StreamConsumer::new(),
            left: vec![0.0; frames as usize],
            right: vec![0.0; frames as usize],
            rate,
            frames,
            last: now_ns,
            stamper: Stamper::new(now_ns),
            applied: 0,
        }
    }

    /// Hands changed settings to the consumer (`cfgDirty`). Takes the lock only when the settings changed.
    pub fn apply_config(&mut self, shared: &Shared) {
        let seq = shared.cfg_seq.load(Ordering::Acquire);
        if seq != self.applied {
            let cfg = shared
                .cfg
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            self.consumer
                .configure(&cfg.bus, cfg.output, f64::from(cfg.buffer_ms), self.rate);
            self.applied = seq;
        }
    }

    /// Pulls one block (silence when the Hub is away) and stamps it. `now_ns` is `os_gettime_ns()` read after the sleep.
    pub fn pull_block(&mut self, now_ns: u64, shared: &Shared) -> Block<'_> {
        let dt = now_ns.wrapping_sub(self.last) as f64 * 1.0e-9;
        self.last = now_ns;
        self.consumer.pull(
            [&mut self.left[..], &mut self.right[..]],
            self.frames,
            if dt > 0.0 && dt < 1.0 { dt } else { 0.010 },
        );
        let dur = u64::from(self.frames) * 1_000_000_000 / u64::from(self.rate);
        let timestamp = self.stamper.stamp(now_ns, dur);
        shared.status.store(self.consumer.status_ref());
        Block {
            left: &self.left,
            right: &self.right,
            frames: self.frames,
            rate: self.rate,
            timestamp,
        }
    }
}

// ---- Streamlabs Desktop ------------------------------------------------------------------------------------------

/// OBS's `obs_source_info` in the Streamlabs fork's layout (a libobs 31.x fork): one null slot inserted at `insert`
/// (`enum_all_sources`, after `audio_render`), and the last two slots (`message`, `get_messages` there, the icon
/// callbacks in OBS) left null. `info` is the OBS layout, `tail` the offset of `get_dark_icon`.
pub fn streamlabs_source_info(info: &[u8], insert: usize, tail: usize) -> Vec<u8> {
    const SLOT: usize = std::mem::size_of::<usize>();
    let mut out = vec![0u8; info.len() + SLOT];
    out[..insert].copy_from_slice(&info[..insert]);
    out[insert + SLOT..insert + SLOT + (tail - insert)].copy_from_slice(&info[insert..tail]);
    out
}

#[cfg(test)]
mod tests;
