//! The Windows half: the driver list in the registry and the COM call that makes a driver. Off Windows both are empty.

use super::abi::{Drv, Guid};

#[cfg(windows)]
mod imp {
    use super::*;
    use std::ffi::c_void;
    use windows_sys::Win32::Foundation::ERROR_SUCCESS;
    use windows_sys::Win32::System::Com::{
        CLSCTX_INPROC_SERVER, CLSIDFromString, COINIT_APARTMENTTHREADED, CoCreateInstance,
        CoInitializeEx,
    };
    use windows_sys::Win32::System::Registry::{
        HKEY, HKEY_LOCAL_MACHINE, KEY_READ, RRF_RT_REG_SZ, RegCloseKey, RegEnumKeyExW,
        RegGetValueW, RegOpenKeyExW,
    };
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::GetActiveWindow;
    use windows_sys::core::GUID;

    const _: () =
        assert!(size_of::<Guid>() == size_of::<GUID>() && align_of::<Guid>() == align_of::<GUID>());

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(Some(0)).collect()
    }

    /// Drivers registered under `HKLM\SOFTWARE\ASIO`: (key name, CLSID).
    pub fn registered_drivers() -> Vec<(String, Guid)> {
        let mut found = Vec::new();
        let mut root: HKEY = std::ptr::null_mut();
        // SAFETY: a NUL-terminated key path and a live out-pointer.
        if unsafe {
            RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                wide("SOFTWARE\\ASIO").as_ptr(),
                0,
                KEY_READ,
                &mut root,
            )
        } != ERROR_SUCCESS
        {
            return found;
        }
        let value = wide("CLSID");
        for i in 0u32.. {
            let mut sub = [0u16; 256];
            let mut len = sub.len() as u32;
            // SAFETY: `sub` holds `len` UTF-16 units; the optional out-parameters are null.
            let r = unsafe {
                RegEnumKeyExW(
                    root,
                    i,
                    sub.as_mut_ptr(),
                    &mut len,
                    std::ptr::null(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            };
            if r != ERROR_SUCCESS {
                break;
            }
            let mut text = [0u16; 64];
            let mut bytes = std::mem::size_of_val(&text) as u32;
            // SAFETY: `sub` was NUL-terminated by RegEnumKeyExW, `text` has `bytes` bytes.
            let r = unsafe {
                RegGetValueW(
                    root,
                    sub.as_ptr(),
                    value.as_ptr(),
                    RRF_RT_REG_SZ,
                    std::ptr::null_mut(),
                    text.as_mut_ptr().cast(),
                    &mut bytes,
                )
            };
            if r != ERROR_SUCCESS {
                continue;
            }
            let mut clsid = GUID {
                data1: 0,
                data2: 0,
                data3: 0,
                data4: [0; 8],
            };
            // SAFETY: RegGetValueW NUL-terminates REG_SZ data it returns, even into a buffer it fills completely.
            if unsafe { CLSIDFromString(text.as_ptr(), &mut clsid) } >= 0 {
                let name = String::from_utf16_lossy(&sub[..len as usize]);
                found.push((
                    name,
                    Guid {
                        data1: clsid.data1,
                        data2: clsid.data2,
                        data3: clsid.data3,
                        data4: clsid.data4,
                    },
                ));
            }
        }
        // SAFETY: `root` was opened above and is closed once.
        unsafe { RegCloseKey(root) };
        found
    }

    /// Makes the driver object. An ASIO driver takes its own CLSID as the interface id.
    pub fn create(clsid: &Guid) -> Option<Drv> {
        // the message thread; S_FALSE / RPC_E_CHANGED_MODE (already initialised) are fine
        // SAFETY: plain COM initialisation of the calling thread.
        unsafe { CoInitializeEx(std::ptr::null(), COINIT_APARTMENTTHREADED as u32) };
        let mut p: *mut c_void = std::ptr::null_mut();
        let id = (clsid as *const Guid).cast::<GUID>();
        // SAFETY: `id` points at a GUID for the whole call, `p` is a live out-pointer.
        let hr =
            unsafe { CoCreateInstance(id, std::ptr::null_mut(), CLSCTX_INPROC_SERVER, id, &mut p) };
        if hr < 0 || p.is_null() {
            return None;
        }
        // SAFETY: CoCreateInstance returned a live COM object; an ASIO driver's interface is the vtable in `abi`.
        Some(unsafe { Drv::new(p.cast()) })
    }

    /// The app's window on the message thread: parent for the driver's dialogs.
    pub fn active_window() -> usize {
        // SAFETY: no arguments, no preconditions.
        unsafe { GetActiveWindow() as usize }
    }
}

#[cfg(not(windows))]
mod imp {
    use super::*;

    pub fn registered_drivers() -> Vec<(String, Guid)> {
        Vec::new()
    }

    pub fn create(_clsid: &Guid) -> Option<Drv> {
        None
    }

    pub fn active_window() -> usize {
        0
    }
}

pub use imp::{active_window, create, registered_drivers};
