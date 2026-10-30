//! POSIX side of `shm`: `shm_open` + `mmap`, names hashed like `shm_posix.cpp` (macOS caps shm
//! names near 31 characters). The segment is never unlinked: other processes may still use it.
use super::Status;
use crate::protocol::kProtocolVersion;
use core::ffi::c_void;
use core::ptr::null_mut;
use rustix::fd::OwnedFd;
use rustix::fs::{fchmod, fstat, ftruncate};
use rustix::io::Errno;
use rustix::mm::{MapFlags, ProtFlags, mmap, munmap};
use rustix::shm::{self, Mode, OFlags};
use std::time::{Duration, Instant};

fn fnv1a(s: &str) -> u32 {
    s.bytes().fold(2_166_136_261u32, |h, c| {
        (h ^ u32::from(c)).wrapping_mul(16_777_619)
    })
}

pub fn bus_segment_name(bus: &str) -> String {
    format!("/hrsd{kProtocolVersion}_{:08x}", fnv1a(bus))
}

pub fn beacon_segment_name(bus: &str) -> String {
    format!("/hrsdbc_{:08x}", fnv1a(bus))
}

pub struct Map {
    base: *mut c_void,
    size: usize,
    _fd: OwnedFd,
}

impl Map {
    pub fn base(&self) -> *mut u8 {
        self.base.cast()
    }
}

impl Drop for Map {
    fn drop(&mut self) {
        // SAFETY: `base`/`size` are exactly what mmap returned/was given.
        unsafe {
            let _ = munmap(self.base, self.size);
        }
    }
}

const MODE: Mode = Mode::from_bits_truncate(0o666);
const WAIT: Duration = Duration::from_millis(200);

/// `wait`: give a creator up to 200 ms to size the segment (the bus does; the beacon fails at once like C++).
pub fn map(name: &str, size: usize, create: bool, wait: bool) -> Result<(Map, bool), Status> {
    let mut created = false;
    let mut fd = None;
    if create && let Ok(f) = shm::open(name, OFlags::RDWR | OFlags::CREATE | OFlags::EXCL, MODE) {
        created = true;
        let _ = fchmod(&f, MODE); // not subject to umask
        if ftruncate(&f, size as u64).is_err() {
            let _ = shm::unlink(name);
            return Err(Status::CreateFailed);
        }
        fd = Some(f);
    }
    let fd = match fd {
        Some(f) => f,
        None => shm::open(name, OFlags::RDWR, MODE).map_err(|_: Errno| Status::CreateFailed)?,
    };
    if !created {
        // wait until the creator has sized the segment
        let deadline = Instant::now() + WAIT;
        loop {
            let len = fstat(&fd).map(|s| s.st_size as u64).unwrap_or(0);
            if len >= size as u64 {
                break;
            }
            if len != 0 {
                return Err(Status::VersionMismatch);
            }
            if !wait {
                return Err(Status::InitTimeout);
            }
            if Instant::now() > deadline {
                return Err(Status::InitTimeout);
            }
            std::thread::sleep(Duration::from_millis(2));
        }
    }
    // SAFETY: shared read/write mapping of a segment we just sized/checked; unmapped in `Map::drop`.
    let base = unsafe {
        mmap(
            null_mut(),
            size,
            ProtFlags::READ | ProtFlags::WRITE,
            MapFlags::SHARED,
            &fd,
            0,
        )
    }
    .map_err(|_| Status::MapFailed)?;
    Ok((
        Map {
            base,
            size,
            _fd: fd,
        },
        created,
    ))
}

pub fn current_pid() -> u32 {
    rustix::process::getpid().as_raw_nonzero().get() as u32
}

pub fn process_alive(pid: u32) -> bool {
    use rustix::process::{Pid, test_kill_process};
    let Some(p) = i32::try_from(pid).ok().and_then(Pid::from_raw) else {
        return false;
    };
    match test_kill_process(p) {
        Ok(()) => true,
        Err(e) => e == Errno::PERM, // exists, owned by someone else
    }
}

/// Monotonic nanoseconds (CLOCK_MONOTONIC), same scale as `ssbus::nowNs`.
pub fn now_ns() -> u64 {
    let t = rustix::time::clock_gettime(rustix::time::ClockId::Monotonic);
    t.tv_sec as u64 * 1_000_000_000 + t.tv_nsec as u64
}
