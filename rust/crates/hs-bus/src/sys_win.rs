//! Windows side of `shm`: named pagefile-backed mapping with the same security descriptor as
//! `shm_win.cpp` (Everyone + anonymous full access, low mandatory label) so an elevated DAW and a
//! normal OBS can share one segment.
use super::Status;
use crate::protocol::kProtocolVersion;
use core::ffi::c_void;
use core::ptr::{null, null_mut};
use windows_sys::Win32::Foundation::{
    CloseHandle, ERROR_ACCESS_DENIED, ERROR_ALREADY_EXISTS, GetLastError, HANDLE,
    INVALID_HANDLE_VALUE, LocalFree, WAIT_TIMEOUT,
};
use windows_sys::Win32::Security::Authorization::{
    ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
};
use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;
use windows_sys::Win32::Storage::FileSystem::SYNCHRONIZE;
use windows_sys::Win32::System::Memory::{
    CreateFileMappingW, FILE_MAP_ALL_ACCESS, MEMORY_BASIC_INFORMATION, MEMORY_MAPPED_VIEW_ADDRESS,
    MapViewOfFile, OpenFileMappingW, PAGE_READWRITE, UnmapViewOfFile, VirtualQuery,
};
use windows_sys::Win32::System::Performance::{QueryPerformanceCounter, QueryPerformanceFrequency};
use windows_sys::Win32::System::Threading::{
    GetCurrentProcessId, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, WaitForSingleObject,
};

const SDDL: &str = "D:(A;;GA;;;WD)(A;;GA;;;AN)(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NW;;;LW)";

pub fn bus_segment_name(bus: &str) -> String {
    format!("Local\\HEARASIDE_v{kProtocolVersion}_{bus}")
}

pub fn beacon_segment_name(bus: &str) -> String {
    format!("Local\\HEARASIDE_beacon_{bus}")
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

pub struct Map {
    base: *mut c_void,
    handle: HANDLE,
}

impl Map {
    pub fn base(&self) -> *mut u8 {
        self.base.cast()
    }
}

impl Drop for Map {
    fn drop(&mut self) {
        // SAFETY: `base` came from MapViewOfFile and `handle` from CreateFileMappingW/OpenFileMappingW,
        // both owned exclusively by this value.
        unsafe {
            UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: self.base });
            CloseHandle(self.handle);
        }
    }
}

/// Opens the named mapping of `size` bytes. Returns the map and whether this call created it.
pub fn map(name: &str, size: usize, create: bool, _wait: bool) -> Result<(Map, bool), Status> {
    let wname = wide(name);
    let mut created = false;
    // SAFETY: plain Win32 calls with valid NUL-terminated names; the descriptor is freed right after
    // CreateFileMappingW has copied it.
    let handle = unsafe {
        if create {
            let sddl = wide(SDDL);
            let mut sd = null_mut();
            let ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                &mut sd,
                null_mut(),
            ) != 0;
            let sa = SECURITY_ATTRIBUTES {
                nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
                lpSecurityDescriptor: if ok { sd } else { null_mut() },
                bInheritHandle: 0,
            };
            let h = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                if ok { &sa } else { null() },
                PAGE_READWRITE,
                (size as u64 >> 32) as u32,
                size as u32,
                wname.as_ptr(),
            );
            let err = GetLastError();
            if ok {
                LocalFree(sd);
            }
            if h.is_null() {
                // May exist but refuse a create with these attributes: plain open.
                OpenFileMappingW(FILE_MAP_ALL_ACCESS, 0, wname.as_ptr())
            } else {
                created = err != ERROR_ALREADY_EXISTS;
                h
            }
        } else {
            OpenFileMappingW(FILE_MAP_ALL_ACCESS, 0, wname.as_ptr())
        }
    };
    if handle.is_null() {
        return Err(Status::CreateFailed);
    }
    // SAFETY: `handle` is a valid mapping handle; the view is released in `Map::drop`.
    let view = unsafe { MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size) };
    if view.Value.is_null() {
        // SAFETY: handle is ours and not used again.
        unsafe { CloseHandle(handle) };
        return Err(Status::MapFailed);
    }
    let map = Map {
        base: view.Value,
        handle,
    };
    // SAFETY: all-zero is a valid MEMORY_BASIC_INFORMATION out-parameter.
    let mut mbi: MEMORY_BASIC_INFORMATION = unsafe { core::mem::zeroed() };
    // SAFETY: `view` is a live mapping and `mbi` is a correctly sized out-parameter.
    let q = unsafe { VirtualQuery(view.Value, &mut mbi, size_of::<MEMORY_BASIC_INFORMATION>()) };
    if q == 0 || mbi.RegionSize < size {
        return Err(Status::VersionMismatch); // `map` drops: unmaps and closes
    }
    Ok((map, created))
}

pub fn current_pid() -> u32 {
    // SAFETY: no preconditions.
    unsafe { GetCurrentProcessId() }
}

pub fn process_alive(pid: u32) -> bool {
    if pid == 0 {
        return false;
    }
    if pid == current_pid() {
        return true;
    }
    // SAFETY: OpenProcess/WaitForSingleObject/CloseHandle on a handle we own.
    unsafe {
        let h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() {
            // Access denied: it exists, at a higher integrity level or under another user.
            return GetLastError() == ERROR_ACCESS_DENIED;
        }
        let r = WaitForSingleObject(h, 0);
        CloseHandle(h);
        r == WAIT_TIMEOUT
    }
}

/// Monotonic nanoseconds (QueryPerformanceCounter), same scale as `ssbus::nowNs`.
pub fn now_ns() -> u64 {
    use std::sync::OnceLock;
    static FREQ: OnceLock<u64> = OnceLock::new();
    let freq = *FREQ.get_or_init(|| {
        let mut f = 0i64;
        // SAFETY: valid out-pointer.
        unsafe { QueryPerformanceFrequency(&mut f) };
        f.max(1) as u64
    });
    let mut c = 0i64;
    // SAFETY: valid out-pointer.
    unsafe { QueryPerformanceCounter(&mut c) };
    let c = c as u64;
    // split to avoid overflow of c * 1e9
    (c / freq) * 1_000_000_000 + (c % freq) * 1_000_000_000 / freq
}
