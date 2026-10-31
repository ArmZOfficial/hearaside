//! `obs/hearaside-obs/src/remote.{h,cpp}` (`hearaside::BusClient`): the commands the two OBS hotkeys send to the
//! Hub through the bus remote queue (`post_remote` with `RemoteParam::Panic` / `Preview`, target -1 = the Hub).
//!
//! Runs on OBS's hotkey thread, never on the feed thread. It keeps one mapping per bus name, opens the bus
//! only if it exists (the DAW is running) and retries at most once a second, exactly like the C++ class. The
//! queue itself is `hs_bus::bus::post_remote`; nothing here touches the layout.
use hs_bus::bus::post_remote;
use hs_bus::protocol::{BusLayout, HubFlags, RemoteParam};
use hs_bus::shm::{SharedMemory, now_ns};
use std::collections::BTreeMap;
use std::sync::Mutex;
use std::sync::atomic::Ordering::Relaxed;

/// Only retry opening a missing bus once per second (the DAW may not be running yet).
const RETRY_NS: u64 = 1_000_000_000;

/// `target` of a command addressed to the Hub itself (not a Track slot).
const TARGET_HUB: i32 = -1;

#[derive(Default)]
struct Maps {
    maps: BTreeMap<String, SharedMemory>,
    last_attempt_ns: BTreeMap<String, u64>,
}

/// One read/write mapping per bus name; thread safe.
pub struct BusClient {
    inner: Mutex<Maps>,
}

static CLIENT: BusClient = BusClient::new();

impl BusClient {
    pub const fn new() -> Self {
        Self {
            inner: Mutex::new(Maps {
                maps: BTreeMap::new(),
                last_attempt_ns: BTreeMap::new(),
            }),
        }
    }

    /// Runs `f` with the bus layout if the bus exists (DAW running). Returns false otherwise.
    pub fn with(&self, bus_name: &str, f: impl FnOnce(&BusLayout)) -> bool {
        self.with_at(bus_name, now_ns(), f)
    }

    /// `with` with the clock passed in, so the retry limit can be tested without waiting.
    fn with_at(&self, bus_name: &str, now: u64, f: impl FnOnce(&BusLayout)) -> bool {
        let bus = if bus_name.is_empty() {
            "Main"
        } else {
            bus_name
        };
        // A panic under this lock would only have interrupted a map insert; the data is still sound.
        let mut guard = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let inner = &mut *guard;
        if !inner.maps.contains_key(bus) {
            let last = inner.last_attempt_ns.entry(bus.to_owned()).or_insert(0);
            if *last != 0 && now.wrapping_sub(*last) < RETRY_NS {
                return false;
            }
            *last = now;
            let Ok(shm) = SharedMemory::open_existing(bus) else {
                return false;
            };
            inner.maps.insert(bus.to_owned(), shm);
        }
        f(inner.maps[bus].layout());
        true
    }

    /// Toggle against the Hub's current state: `hubFlags` has the bit set -> post 0, otherwise post 1.
    fn toggle(&self, bus_name: &str, flag: u32, param: u32) {
        self.with(bus_name, |layout| {
            let on = layout.header.hubFlags.load(Relaxed) & flag != 0;
            post_remote(layout, TARGET_HUB, param, if on { 0.0 } else { 1.0 });
        });
    }

    pub fn toggle_panic(&self, bus_name: &str) {
        self.toggle(bus_name, HubFlags::kHubPanic, RemoteParam::Panic);
    }

    pub fn toggle_preview(&self, bus_name: &str) {
        self.toggle(bus_name, HubFlags::kHubPreview, RemoteParam::Preview);
    }

    /// Drops every mapping (the retry times stay, like the C++ class).
    pub fn release_all(&self) {
        let mut guard = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        guard.maps.clear();
    }
}

impl Default for BusClient {
    fn default() -> Self {
        Self::new()
    }
}

/// Hotkey `hearaside.mute_stream`: toggle the Hub's panic (mute the viewers' stream) on bus `bus`.
pub fn toggle_panic(bus: &str) {
    CLIENT.toggle_panic(bus);
}

/// Hotkey `hearaside.preview`: toggle "hear the viewers' mix" in the Hub on bus `bus`.
pub fn toggle_preview(bus: &str) {
    CLIENT.toggle_preview(bus);
}

/// `obs_module_unload`: drop the bus mappings `BusClient` keeps open.
pub fn release_all() {
    CLIENT.release_all();
}

#[cfg(test)]
mod tests;
