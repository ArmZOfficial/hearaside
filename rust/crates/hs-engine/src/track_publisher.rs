//! `TrackPublisher`: lives in every HEARASIDE Track instance. Port of `libs/ssengine/src/track_publisher.cpp`.
//!
//! Threading follows the C++: `connect` / `disconnect` / `prepare` / `set_identity` on the message thread,
//! `mirror` from any thread, `process` on the audio thread. The audio thread reaches the bus through raw
//! pointers that are published with release stores. `connect` / `disconnect` take `&mut self`, so they
//! cannot overlap a `process(&self)` call; old mappings are kept (see `retired`).
use hs_bus::bus::{
    bits_float, claim_slot, float_bits, hub_alive, make_uuid, release_slot, set_slot_identity,
    write_tag,
};
use hs_bus::protocol::*;
use hs_bus::ring::ring_write;
use hs_bus::shm::{SharedMemory, now_ns};
use std::sync::atomic::Ordering::{AcqRel, Acquire, Relaxed, Release};
use std::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32};

/// The sentinel for "the host gives no playhead".
pub const NO_TIME: i64 = i64::MIN;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PublisherStatus {
    Disconnected,
    Connected,
    SlotsFull,
    ShmError,
}

/// What the Track mirrors to the bus for the Hub to read.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Mirror {
    pub mon: bool,
    pub str_: bool,
    pub solo: bool,
    pub gain_db: f32,
    pub pan: f32,
    pub delay_ms: f32,
    pub trim_db: f32,
    pub stem: i32,
    /// an App Audio's slot
    pub app: bool,
}

impl Default for Mirror {
    fn default() -> Self {
        Mirror {
            mon: true,
            str_: true,
            solo: false,
            gain_db: 0.0,
            pan: 0.0,
            delay_ms: 0.0,
            trim_db: 0.0,
            stem: -1,
            app: false,
        }
    }
}

/// `Mirror` kept in atomics so any thread can set it and `connect` can replay it.
struct MirrorCell {
    bits: AtomicU32, // mon | str << 1 | solo << 2 | app << 3
    gain: AtomicU32,
    pan: AtomicU32,
    delay: AtomicU32,
    trim: AtomicU32,
    stem: AtomicI32,
}

impl MirrorCell {
    fn new() -> Self {
        let m = Mirror::default();
        let c = MirrorCell {
            bits: AtomicU32::new(0),
            gain: AtomicU32::new(0),
            pan: AtomicU32::new(0),
            delay: AtomicU32::new(0),
            trim: AtomicU32::new(0),
            stem: AtomicI32::new(0),
        };
        c.set(&m);
        c
    }

    fn set(&self, m: &Mirror) {
        let bits = u32::from(m.mon)
            | u32::from(m.str_) << 1
            | u32::from(m.solo) << 2
            | u32::from(m.app) << 3;
        self.bits.store(bits, Relaxed);
        self.gain.store(float_bits(m.gain_db), Relaxed);
        self.pan.store(float_bits(m.pan), Relaxed);
        self.delay.store(float_bits(m.delay_ms), Relaxed);
        self.trim.store(float_bits(m.trim_db), Relaxed);
        self.stem.store(m.stem, Relaxed);
    }

    fn get(&self) -> Mirror {
        let bits = self.bits.load(Relaxed);
        Mirror {
            mon: bits & 1 != 0,
            str_: bits & 2 != 0,
            solo: bits & 4 != 0,
            app: bits & 8 != 0,
            gain_db: bits_float(self.gain.load(Relaxed)),
            pan: bits_float(self.pan.load(Relaxed)),
            delay_ms: bits_float(self.delay.load(Relaxed)),
            trim_db: bits_float(self.trim.load(Relaxed)),
            stem: self.stem.load(Relaxed),
        }
    }
}

pub struct TrackPublisher {
    bus: AtomicPtr<BusLayout>,
    slot: AtomicPtr<SlotHeader>,
    shm: Option<SharedMemory>,
    /// old mappings stay mapped: another thread may still hold a slot / bus pointer into them (the C++ kept them
    /// too). One per bus name: reconnecting to a bus reuses its mapping, so this never grows past the buses used.
    retired: Vec<(String, SharedMemory)>,
    bus_name: String,
    uuid: String,
    slot_index: Option<usize>,
    status: PublisherStatus,
    sample_rate: AtomicU32,
    last_mirror: MirrorCell,
}

// SAFETY: the raw pointers point into the mapping owned by `shm` (old ones stay in `retired`);
// everything the audio thread writes is atomic (see the module docs).
unsafe impl Send for TrackPublisher {}
// SAFETY: as above; `process` / `mirror` / the getters only use atomics and shared references.
unsafe impl Sync for TrackPublisher {}

impl Default for TrackPublisher {
    fn default() -> Self {
        Self::new()
    }
}

impl TrackPublisher {
    pub fn new() -> Self {
        TrackPublisher {
            bus: AtomicPtr::new(std::ptr::null_mut()),
            slot: AtomicPtr::new(std::ptr::null_mut()),
            shm: None,
            retired: Vec::new(),
            bus_name: String::new(),
            uuid: String::new(),
            slot_index: None,
            status: PublisherStatus::Disconnected,
            sample_rate: AtomicU32::new(48000),
            last_mirror: MirrorCell::new(),
        }
    }

    fn bus_ref(&self) -> Option<&BusLayout> {
        // SAFETY: published by `connect` from a mapping that stays alive as long as `self`.
        unsafe { self.bus.load(Acquire).as_ref() }
    }

    fn slot_ref(&self) -> Option<&SlotHeader> {
        // SAFETY: as above.
        unsafe { self.slot.load(Acquire).as_ref() }
    }

    /// Opens the bus and claims a slot. If the uuid is already used by another live slot (a duplicated
    /// track) a fresh uuid is generated; check `uuid()` afterwards. Message thread.
    pub fn connect(
        &mut self,
        bus_name: &str,
        uuid: &str,
        sample_rate: u32,
        num_channels: u32,
    ) -> PublisherStatus {
        self.disconnect();
        self.bus_name = if bus_name.is_empty() {
            "Main".into()
        } else {
            bus_name.into()
        };
        self.sample_rate.store(sample_rate, Relaxed);
        let mut uuid = if uuid.is_empty() {
            make_uuid()
        } else {
            uuid.to_owned()
        };

        let reuse = self.retired.iter().position(|(n, _)| *n == self.bus_name);
        let opened = match reuse {
            Some(i) => Ok(self.retired.swap_remove(i).1),
            None => SharedMemory::open(&self.bus_name),
        };
        let Ok(shm) = opened else {
            self.status = PublisherStatus::ShmError;
            self.uuid = uuid;
            return self.status;
        };
        let mut claimed = claim_slot(shm.layout(), &uuid, sample_rate, num_channels);
        if claimed == Err(true) {
            // duplicated track: pick a new identity
            uuid = make_uuid();
            claimed = claim_slot(shm.layout(), &uuid, sample_rate, num_channels);
        }
        self.uuid = uuid;
        let Ok(idx) = claimed else {
            self.status = PublisherStatus::SlotsFull;
            self.retired.push((self.bus_name.clone(), shm));
            return self.status;
        };
        let l = shm.layout();
        let b = std::ptr::from_ref(l).cast_mut();
        let s = std::ptr::from_ref(&l.slots[idx]).cast_mut();
        self.shm = Some(shm);
        self.slot_index = Some(idx);
        self.bus.store(b, Release);
        self.slot.store(s, Release);
        self.status = PublisherStatus::Connected;
        let m = self.last_mirror.get();
        self.mirror(&m);
        self.status
    }

    pub fn disconnect(&mut self) {
        let s = self.slot.swap(std::ptr::null_mut(), AcqRel);
        let b = self.bus.swap(std::ptr::null_mut(), AcqRel);
        if let (false, false, Some(idx)) = (s.is_null(), b.is_null(), self.slot_index) {
            // SAFETY: `b` was published from the live mapping in `self.shm`, still owned here.
            release_slot(unsafe { &*b }, idx);
        }
        self.slot_index = None;
        if let Some(shm) = self.shm.take() {
            self.retired.push((self.bus_name.clone(), shm));
        }
        self.status = PublisherStatus::Disconnected;
    }

    /// Sample-rate / layout change.
    pub fn prepare(&self, sample_rate: u32, num_channels: u32) {
        self.sample_rate.store(sample_rate, Relaxed);
        if let Some(s) = self.slot_ref() {
            s.sampleRate.store(sample_rate, Relaxed);
            s.numChannels.store(num_channels, Relaxed);
            if num_channels == 1 {
                s.flags.fetch_or(SlotFlags::kFlagMono, Relaxed);
            } else {
                s.flags.fetch_and(!SlotFlags::kFlagMono, Relaxed);
            }
            s.epoch.fetch_add(1, Release);
        }
    }

    pub fn set_identity(&self, name: &str, color_argb: u32) {
        if let Some(s) = self.slot_ref() {
            set_slot_identity(s, name, color_argb);
        }
    }

    pub fn status(&self) -> PublisherStatus {
        self.status
    }

    pub fn uuid(&self) -> &str {
        &self.uuid
    }

    pub fn bus_name(&self) -> &str {
        &self.bus_name
    }

    pub fn slot_index(&self) -> Option<usize> {
        self.slot_index
    }

    pub fn bus(&self) -> Option<&BusLayout> {
        self.bus_ref()
    }

    pub fn slot(&self) -> Option<&SlotHeader> {
        self.slot_ref()
    }

    pub fn hub_present(&self) -> bool {
        self.bus_ref().is_some_and(|b| hub_alive(b, 1_000_000_000))
    }

    /// Whether any track on the bus is soloed for the viewers.
    pub fn solo_active(&self) -> bool {
        let Some(b) = self.bus_ref() else {
            return false;
        };
        b.slots.iter().any(|sh| {
            if sh.state.load(Acquire) != SlotState::kSlotActive {
                return false;
            }
            let f = sh.flags.load(Relaxed);
            f & SlotFlags::kFlagSolo != 0 && f & SlotFlags::kFlagStr != 0
        })
    }

    /// Any thread.
    pub fn mirror(&self, m: &Mirror) {
        self.last_mirror.set(m);
        let Some(s) = self.slot_ref() else { return };
        let own =
            SlotFlags::kFlagMon | SlotFlags::kFlagStr | SlotFlags::kFlagSolo | SlotFlags::kFlagApp;
        let set = if m.mon { SlotFlags::kFlagMon } else { 0 }
            | if m.str_ { SlotFlags::kFlagStr } else { 0 }
            | if m.solo { SlotFlags::kFlagSolo } else { 0 }
            | if m.app { SlotFlags::kFlagApp } else { 0 };
        s.strGainBits.store(float_bits(m.gain_db), Relaxed);
        s.strPanBits.store(float_bits(m.pan), Relaxed);
        s.strDelayBits.store(float_bits(m.delay_ms), Relaxed);
        s.monTrimBits.store(float_bits(m.trim_db), Relaxed);
        s.stemIndex.store(m.stem, Relaxed);
        // Change only our own bits, atomically: the audio thread (bypass / offline) and the Hub
        // (ViewersViaHub) write other bits of the same word at the same time. The C++ loaded, merged
        // and stored, which could lose either update.
        let _ = s
            .flags
            .try_update(Release, Relaxed, |f| Some((f & !own) | set));
    }

    /// Audio thread: publishes the post-FX block. `time_samples` = `NO_TIME` when the host gives no
    /// playhead. No allocation, no locks. `ch` holds one or two channels; every slice needs `n` frames
    /// (a shorter one cuts `n`).
    pub fn process(
        &self,
        ch: &[&[f32]],
        n: usize,
        time_samples: i64,
        playing: bool,
        offline: bool,
        bypassed: bool,
    ) {
        let (Some(s), Some(b)) = (self.slot_ref(), self.bus_ref()) else {
            return;
        };
        let n = ch.iter().map(|c| c.len()).fold(n, usize::min);
        if n == 0 || ch.is_empty() {
            return;
        }

        s.heartbeatNs.store(now_ns(), Relaxed);

        // status bits that only the audio thread knows
        // (single-bit atomic updates: see `mirror`)
        if bypassed {
            s.flags.fetch_or(SlotFlags::kFlagBypassed, Relaxed);
        } else {
            s.flags.fetch_and(!SlotFlags::kFlagBypassed, Relaxed);
        }
        if offline {
            s.flags.fetch_or(SlotFlags::kFlagOffline, Relaxed);
        } else {
            s.flags.fetch_and(!SlotFlags::kFlagOffline, Relaxed);
        }

        if offline {
            return; // never send an export/bounce to the viewers
        }

        // index of this slot in the bus
        let Some(idx) = self.slot_index else { return };
        let w = s.writePos.0.load(Relaxed);
        write_tag(
            s,
            &b.slotTags[idx],
            w,
            if time_samples == NO_TIME {
                0
            } else {
                time_samples
            },
            n as u32,
            playing && time_samples != NO_TIME,
        );
        let used = &ch[..ch.len().min(kMaxChannels)];
        ring_write(&b.slotAudio.0[idx], &s.writePos.0, used, n as u32);

        // meters: peak with ~300 ms decay
        let sr = self.sample_rate.load(Relaxed).max(8000);
        let decay = (-(n as f32) / (0.3 * sr as f32)).exp();
        for c in 0..kMaxChannels {
            let x = ch[c.min(ch.len() - 1)];
            let p = hs_dsp::dsp::block_peak(&x[..n]);
            let prev = bits_float(s.peakInBits[c].load(Relaxed));
            s.peakInBits[c].store(float_bits(p.max(prev * decay)), Relaxed);
        }
    }
}

impl Drop for TrackPublisher {
    fn drop(&mut self) {
        self.disconnect();
    }
}
