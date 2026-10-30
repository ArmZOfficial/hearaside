//! A worker thread that cannot be stopped in time (it is inside a network call that has no cancel) must never run code of
//! an unloaded module: the host may `FreeLibrary` / `dlclose` the plug-in the moment its destructor returns. So before a
//! thread is left behind, the module that holds this code is pinned in memory for the rest of the process.
use std::sync::Once;
use std::sync::atomic::{AtomicU32, Ordering};

static DETACHED: AtomicU32 = AtomicU32::new(0);
static PIN: Once = Once::new();

/// How many worker threads were left running when their owner was dropped (tests, diagnostics). Only grows.
#[doc(hidden)]
pub fn detached_workers() -> u32 {
    DETACHED.load(Ordering::SeqCst)
}

/// Call before leaving a running thread behind.
pub(crate) fn pin_module_for_detached_thread() {
    DETACHED.fetch_add(1, Ordering::SeqCst);
    PIN.call_once(pin_module);
}

#[cfg(windows)]
fn pin_module() {
    use windows_sys::Win32::Foundation::HMODULE;
    use windows_sys::Win32::System::LibraryLoader::{
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_PIN, GetModuleHandleExW,
    };
    let mut module: HMODULE = std::ptr::null_mut();
    // SAFETY: with FROM_ADDRESS the "name" is an address inside the module (this function), not a string; the PIN flag
    // keeps the module loaded until the process ends; `module` is a valid out pointer.
    unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            pin_module as *const () as *const u16,
            &mut module,
        );
    }
}

#[cfg(unix)]
fn pin_module() {
    // SAFETY: `Dl_info` is plain data (all-zero is valid); `dladdr` is given an address inside this module and fills it;
    // `dli_fname` then points at a NUL-terminated path owned by the loader, valid for the call to `dlopen`, which with
    // RTLD_NOLOAD | RTLD_NODELETE only marks the already loaded module as never to be unloaded.
    unsafe {
        let mut info: libc::Dl_info = std::mem::zeroed();
        if libc::dladdr(pin_module as *const () as *const libc::c_void, &mut info) != 0
            && !info.dli_fname.is_null()
        {
            libc::dlopen(
                info.dli_fname,
                libc::RTLD_NOW | libc::RTLD_NOLOAD | libc::RTLD_NODELETE,
            );
        }
    }
}

#[cfg(not(any(windows, unix)))]
fn pin_module() {}
