//! Opens or creates the bus segment the way `libs/ssbus/src/shm_*.cpp` does, so a Rust process and
//! a C++ process land in the same memory. Segment names, sizes and the init handshake are part of
//! the compatibility contract.
use crate::protocol::{BeaconHeader, BusLayout, kLayoutSize, kMagic, kProtocolVersion};
use std::fmt;
use std::ptr::addr_of_mut;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

#[cfg(unix)]
#[path = "sys_posix.rs"]
mod sys;
#[cfg(windows)]
#[path = "sys_win.rs"]
mod sys;

pub use sys::{current_pid, now_ns, process_alive};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    CreateFailed,
    MapFailed,
    InitTimeout,
    VersionMismatch,
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Status::CreateFailed => "cannot create shared memory",
            Status::MapFailed => "cannot map shared memory",
            Status::InitTimeout => "shared memory was never initialised",
            Status::VersionMismatch => "protocol version mismatch",
        })
    }
}

impl std::error::Error for Status {}

const INIT_WAIT: Duration = Duration::from_millis(200);

/// The `BusLayout` mapping of one bus. Dropping unmaps it; the segment itself stays for other processes.
pub struct SharedMemory {
    map: sys::Map,
    bus_name: String,
}

// SAFETY: the mapping is plain shared memory. Concurrently written fields are atomics or `Shared`
// (UnsafeCell), which are only touched through raw pointers.
unsafe impl Send for SharedMemory {}
// SAFETY: see above; `layout` hands out only shared references.
unsafe impl Sync for SharedMemory {}

impl SharedMemory {
    /// Opens the segment, creating it when missing. Not for audio threads.
    pub fn open(bus_name: &str) -> Result<Self, Status> {
        Self::open_impl(bus_name, true)
    }

    /// Opens only an existing segment (consumers such as OBS poll with this).
    pub fn open_existing(bus_name: &str) -> Result<Self, Status> {
        Self::open_impl(bus_name, false)
    }

    pub fn segment_name(bus_name: &str) -> String {
        sys::bus_segment_name(bus_name)
    }

    pub fn bus_name(&self) -> &str {
        &self.bus_name
    }

    pub fn layout(&self) -> &BusLayout {
        // SAFETY: the mapping is `size_of::<BusLayout>()` bytes, page aligned, and lives as long as `self`.
        unsafe { &*self.map.base().cast::<BusLayout>() }
    }

    fn open_impl(bus_name: &str, create: bool) -> Result<Self, Status> {
        let name = sys::bus_segment_name(bus_name);
        let (map, created) = sys::map(&name, kLayoutSize, create, true)?;
        let shm = SharedMemory {
            map,
            bus_name: bus_name.to_owned(),
        };
        let base = shm.map.base().cast::<BusLayout>();
        if created {
            // A fresh mapping is zero filled; zero bits are valid for every atomic.
            // SAFETY: nobody else reads these two plain fields before `magic` is published below.
            unsafe {
                addr_of_mut!((*base).header.version).write(kProtocolVersion);
                addr_of_mut!((*base).header.layoutSize).write(kLayoutSize as u32);
            }
            let l = shm.layout();
            for slot in &l.slots {
                slot.stemIndex.store(-1, Ordering::Relaxed);
            }
            l.header.magic.store(kMagic, Ordering::Release);
        } else {
            let l = shm.layout();
            let deadline = Instant::now() + INIT_WAIT;
            while l.header.magic.load(Ordering::Acquire) != kMagic {
                if Instant::now() > deadline {
                    return Err(Status::InitTimeout);
                }
                std::thread::sleep(Duration::from_millis(2));
            }
            if l.header.version != kProtocolVersion || l.header.layoutSize != kLayoutSize as u32 {
                return Err(Status::VersionMismatch);
            }
        }
        Ok(shm)
    }
}

/// The 64-byte beacon segment: its name and layout never change between versions.
pub struct BeaconMap {
    map: sys::Map,
}

// SAFETY: as for `SharedMemory`.
unsafe impl Send for BeaconMap {}
// SAFETY: as for `SharedMemory`.
unsafe impl Sync for BeaconMap {}

impl BeaconMap {
    /// Creates the beacon when missing (the Hub).
    pub fn open(bus_name: &str) -> Option<Self> {
        Self::open_impl(bus_name, true)
    }

    /// Readers.
    pub fn open_existing(bus_name: &str) -> Option<Self> {
        Self::open_impl(bus_name, false)
    }

    pub fn segment_name(bus_name: &str) -> String {
        sys::beacon_segment_name(bus_name)
    }

    pub fn header(&self) -> &BeaconHeader {
        // SAFETY: the mapping is at least `size_of::<BeaconHeader>()` bytes and page aligned.
        unsafe { &*self.map.base().cast::<BeaconHeader>() }
    }

    fn open_impl(bus_name: &str, create: bool) -> Option<Self> {
        let name = sys::beacon_segment_name(bus_name);
        let (map, _) = sys::map(&name, size_of::<BeaconHeader>(), create, false).ok()?;
        Some(BeaconMap { map })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU32;

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    fn unique(tag: &str) -> String {
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        format!("rs-test-{tag}-{}-{n}", current_pid())
    }

    #[test]
    fn segment_names_match_cpp() {
        let n = SharedMemory::segment_name("abc");
        let b = BeaconMap::segment_name("abc");
        #[cfg(windows)]
        {
            assert_eq!(n, "Local\\HEARASIDE_v14_abc");
            assert_eq!(b, "Local\\HEARASIDE_beacon_abc");
        }
        #[cfg(unix)]
        {
            // FNV-1a("abc") = 0x1A47E90B
            assert_eq!(n, "/hrsd14_1a47e90b");
            assert_eq!(b, "/hrsdbc_1a47e90b");
        }
    }

    #[test]
    fn open_existing_fails_when_missing() {
        assert_eq!(
            SharedMemory::open_existing(&unique("missing")).err(),
            Some(Status::CreateFailed)
        );
        assert!(BeaconMap::open_existing(&unique("missing-b")).is_none());
    }

    #[test]
    fn create_initialises_and_second_open_shares_memory() {
        let name = unique("share");
        let a = SharedMemory::open(&name).unwrap();
        let l = a.layout();
        assert_eq!(l.header.magic.load(Ordering::Acquire), kMagic);
        assert_eq!(l.header.version, kProtocolVersion);
        assert_eq!(l.header.layoutSize as usize, kLayoutSize);
        assert!(
            l.slots
                .iter()
                .all(|s| s.stemIndex.load(Ordering::Relaxed) == -1)
        );

        let b = SharedMemory::open_existing(&name).unwrap();
        a.layout().header.hubOwnerPid.store(1234, Ordering::Relaxed);
        assert_eq!(b.layout().header.hubOwnerPid.load(Ordering::Relaxed), 1234);
        // open() on an existing segment must not re-initialise it.
        let c = SharedMemory::open(&name).unwrap();
        assert_eq!(c.layout().header.hubOwnerPid.load(Ordering::Relaxed), 1234);
        assert_eq!(c.bus_name(), name);
    }

    #[test]
    fn beacon_roundtrip() {
        let name = unique("beacon");
        let a = BeaconMap::open(&name).unwrap();
        a.header().hubPid.store(77, Ordering::Relaxed);
        let b = BeaconMap::open_existing(&name).unwrap();
        assert_eq!(b.header().hubPid.load(Ordering::Relaxed), 77);
    }

    #[test]
    fn process_helpers() {
        assert!(process_alive(current_pid()));
        assert!(!process_alive(0));
        let a = now_ns();
        assert!(now_ns() >= a);
    }

    /// Manual interop check: `HS_BUS_HOLD=<bus> cargo test -p hs-bus -- --ignored hold_bus`, then run
    /// `bus-inspector <bus> --once` (C++) while it sleeps.
    #[test]
    #[ignore]
    fn hold_bus_for_cpp_inspector() {
        let name = std::env::var("HS_BUS_HOLD").expect("set HS_BUS_HOLD to a bus name");
        let a = SharedMemory::open(&name).unwrap();
        a.layout()
            .header
            .hubSampleRate
            .store(48000, Ordering::Relaxed);
        // something to look at in `bus-inspector`: a named Track, Hub flags and an App Audio source
        let l = a.layout();
        let i = crate::bus::claim_slot(l, "demo", 44100, 2).unwrap();
        crate::bus::set_slot_identity(
            &l.slots[i],
            "เสียงร้องนำ Vocal ที่ยาวมากเกินยี่สิบตัวอักษร",
            0xFF00_FF00,
        );
        l.slots[i]
            .strGainBits
            .store((-3.5f32).to_bits(), Ordering::Relaxed);
        l.slots[i].stemIndex.store(2, Ordering::Relaxed);
        l.header.hubFlags.store(2 | 16, Ordering::Relaxed);
        let j = crate::bus::claim_source(l).unwrap();
        crate::bus::set_source_identity(&l.sources[j], "เพลง", "chrome.exe", 1);
        std::thread::sleep(Duration::from_secs(8));
    }
}
