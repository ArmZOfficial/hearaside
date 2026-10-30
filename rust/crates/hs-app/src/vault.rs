//! Protecting the account tokens on disk and taking turns between processes, exactly the way the C++
//! `AccountManager` does (so the C++ and Rust plug-ins can read each other's `account.dat` and share the refresh lock):
//! * Windows: DPAPI (`CryptProtectData` / `CryptUnprotectData`, description `HEARASIDE`, no UI, no extra entropy), per user;
//! * elsewhere: no OS key store yet, the file is plain JSON readable by its owner only (`properties::write_atomic(.., private)`);
//! * the refresh lock: the named mutex `Global\HEARASIDE_account_refresh` (Windows) or an `fcntl` lock on a file in the
//!   temp folder (what `juce::InterProcessLock` uses), so a C++ process and a Rust process exclude each other.
use std::time::Duration;

/// The bytes to store for `text`; `None` = the tokens could not be protected and must not be written at all.
pub(crate) fn protect(text: &str) -> Option<Vec<u8>> {
    if text.is_empty() {
        return None;
    }
    #[cfg(windows)]
    {
        win::protect(text.as_bytes())
    }
    #[cfg(not(windows))]
    {
        Some(text.as_bytes().to_vec())
    }
}

/// The text `bytes` hold; `None` = not ours or not this user's (treated as signed out, never as plain text).
pub(crate) fn unprotect(bytes: &[u8]) -> Option<String> {
    if bytes.is_empty() {
        return None;
    }
    #[cfg(windows)]
    {
        win::unprotect(bytes).and_then(|b| String::from_utf8(b).ok())
    }
    #[cfg(not(windows))]
    {
        String::from_utf8(bytes.to_vec()).ok()
    }
}

/// Whether the token protection works on this computer: protect a secret, see that the stored bytes are not the text
/// (Windows: DPAPI really encrypted it), and get the same text back (`AccountManager::testDpapiRoundtrip`).
pub fn roundtrip() -> bool {
    const SECRET: &str = "test_refresh_token_256bit_hearaside_secret";
    let Some(stored) = protect(SECRET) else {
        return false;
    };
    if cfg!(windows) && stored == SECRET.as_bytes() {
        return false;
    }
    unprotect(&stored).as_deref() == Some(SECRET)
}

/// A held inter-process lock; released on drop (on the thread that took it).
pub(crate) struct LockGuard {
    #[cfg(windows)]
    handle: windows_sys::Win32::Foundation::HANDLE,
    #[cfg(unix)]
    fd: libc::c_int,
}

/// What `lock` found.
pub(crate) enum Lock {
    Held(LockGuard),
    /// no lock can be made here (a folder nobody may write, no mutex): go on without it, like the C++ does
    Unavailable,
    /// someone else kept it for `timeout`, or `stop` was asked for: do not go on
    TimedOut,
}

/// Takes the lock called `name`, waiting up to `timeout` in slices of 100 ms that also watch `stop`.
pub(crate) fn lock(name: &str, timeout: Duration, stop: &dyn Fn() -> bool) -> Lock {
    #[cfg(windows)]
    {
        win::lock(name, timeout, stop)
    }
    #[cfg(unix)]
    {
        unix::lock(name, timeout, stop)
    }
}

#[cfg(windows)]
mod win {
    use super::{Lock, LockGuard};
    use std::time::{Duration, Instant};
    use windows_sys::Win32::Foundation::{
        CloseHandle, ERROR_ALREADY_EXISTS, GetLastError, LocalFree, WAIT_ABANDONED, WAIT_OBJECT_0,
        WAIT_TIMEOUT,
    };
    use windows_sys::Win32::Security::Cryptography::{
        CRYPT_INTEGER_BLOB, CRYPTPROTECT_UI_FORBIDDEN, CryptProtectData, CryptUnprotectData,
    };
    use windows_sys::Win32::System::Threading::{CreateMutexW, ReleaseMutex, WaitForSingleObject};

    fn blob_out(out: &CRYPT_INTEGER_BLOB) -> Vec<u8> {
        // SAFETY: on success CryptProtectData / CryptUnprotectData fill `out` with `cbData` readable bytes at `pbData`.
        let v = unsafe { std::slice::from_raw_parts(out.pbData, out.cbData as usize) }.to_vec();
        // SAFETY: `pbData` was allocated by the API with LocalAlloc and is not used after this call.
        unsafe { LocalFree(out.pbData.cast()) };
        v
    }

    pub(super) fn protect(data: &[u8]) -> Option<Vec<u8>> {
        let input = CRYPT_INTEGER_BLOB {
            cbData: u32::try_from(data.len()).ok()?,
            pbData: data.as_ptr().cast_mut(),
        };
        let mut out = CRYPT_INTEGER_BLOB {
            cbData: 0,
            pbData: std::ptr::null_mut(),
        };
        let descr: Vec<u16> = "HEARASIDE\0".encode_utf16().collect();
        // SAFETY: `input` points at `data` (alive for the call, only read), `descr` is a NUL-terminated UTF-16 string,
        // the other pointers are allowed to be null, and `out` is a valid output blob.
        let ok = unsafe {
            CryptProtectData(
                &input,
                descr.as_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut out,
            )
        };
        (ok != 0).then(|| blob_out(&out))
    }

    pub(super) fn unprotect(data: &[u8]) -> Option<Vec<u8>> {
        let input = CRYPT_INTEGER_BLOB {
            cbData: u32::try_from(data.len()).ok()?,
            pbData: data.as_ptr().cast_mut(),
        };
        let mut out = CRYPT_INTEGER_BLOB {
            cbData: 0,
            pbData: std::ptr::null_mut(),
        };
        // SAFETY: as in `protect`; the description output is not wanted (null).
        let ok = unsafe {
            CryptUnprotectData(
                &input,
                std::ptr::null_mut(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut out,
            )
        };
        (ok != 0).then(|| blob_out(&out))
    }

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    pub(super) fn lock(name: &str, timeout: Duration, stop: &dyn Fn() -> bool) -> Lock {
        let name = name.replace('\\', "/");
        // juce::InterProcessLock: a global mutex, else a local one
        let mut handle = std::ptr::null_mut();
        for prefix in ["Global\\", "Local\\"] {
            let w = wide(&format!("{prefix}{name}"));
            // SAFETY: `w` is a NUL-terminated UTF-16 name; no security attributes.
            handle = unsafe { CreateMutexW(std::ptr::null(), 1, w.as_ptr()) };
            if !handle.is_null() {
                break;
            }
        }
        if handle.is_null() {
            return Lock::Unavailable;
        }
        // SAFETY: reads the thread's last error right after the call that set it.
        if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
            let end = Instant::now() + timeout;
            loop {
                // SAFETY: `handle` is the mutex handle just created.
                let r = unsafe { WaitForSingleObject(handle, 100) };
                if r == WAIT_OBJECT_0 || r == WAIT_ABANDONED {
                    break;
                }
                if r != WAIT_TIMEOUT || stop() || Instant::now() >= end {
                    // SAFETY: closing our own handle once.
                    unsafe { CloseHandle(handle) };
                    return if r == WAIT_TIMEOUT {
                        Lock::TimedOut
                    } else {
                        Lock::Unavailable
                    };
                }
            }
        }
        Lock::Held(LockGuard { handle })
    }

    impl Drop for LockGuard {
        fn drop(&mut self) {
            // SAFETY: the handle is a mutex this thread owns (taken in `lock` on the same thread); released and closed once.
            unsafe {
                ReleaseMutex(self.handle);
                CloseHandle(self.handle);
            }
        }
    }
}

#[cfg(unix)]
mod unix {
    use super::{Lock, LockGuard};
    use std::ffi::CString;
    use std::path::PathBuf;
    use std::time::{Duration, Instant};

    /// `None`: this folder cannot be used for the lock file. `Some(None)`: held by someone else for the whole wait.
    fn lock_file(
        dir: PathBuf,
        name: &str,
        timeout: Duration,
        stop: &dyn Fn() -> bool,
    ) -> Option<Option<LockGuard>> {
        std::fs::create_dir_all(&dir).ok()?;
        let path = dir.join(name);
        let c = CString::new(path.to_str()?).ok()?;
        // SAFETY: plain open(2) of a path we made; the descriptor is closed in `LockGuard::drop` or below.
        let fd = unsafe { libc::open(c.as_ptr(), libc::O_RDWR | libc::O_CREAT, 0o600) };
        if fd < 0 {
            return None;
        }
        let end = Instant::now() + timeout;
        loop {
            // SAFETY: `flock` is plain data; all-zero is a valid value.
            let mut fl: libc::flock = unsafe { std::mem::zeroed() };
            fl.l_whence = libc::SEEK_SET as _;
            fl.l_type = libc::F_WRLCK as _;
            // SAFETY: `fd` is open and `fl` outlives the call.
            if unsafe { libc::fcntl(fd, libc::F_SETLK, &fl) } >= 0 {
                return Some(Some(LockGuard { fd }));
            }
            let err = std::io::Error::last_os_error().raw_os_error();
            if err == Some(libc::EBADF) || err == Some(libc::ENOTSUP) {
                // SAFETY: closing the descriptor opened above.
                unsafe { libc::close(fd) };
                return None; // a file system that cannot lock: like the C++, go on without
            }
            if stop() || Instant::now() >= end {
                // SAFETY: closing the descriptor opened above.
                unsafe { libc::close(fd) };
                return Some(None);
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    pub(super) fn lock(name: &str, timeout: Duration, stop: &dyn Fn() -> bool) -> Lock {
        let mut dirs = Vec::new();
        if cfg!(target_os = "macos") {
            if let Some(h) = std::env::var_os("HOME") {
                dirs.push(PathBuf::from(h).join("Library/Caches/com.juce.locks"));
            }
            dirs.push(PathBuf::from("/tmp/com.juce.locks"));
        } else {
            // the same file as juce::InterProcessLock, so a C++ process and this one exclude each other
            dirs.push(if std::path::Path::new("/var/tmp").is_dir() {
                PathBuf::from("/var/tmp")
            } else {
                PathBuf::from("/tmp")
            });
        }
        for d in dirs {
            match lock_file(d, name, timeout, stop) {
                Some(Some(g)) => return Lock::Held(g),
                Some(None) => return Lock::TimedOut,
                None => continue, // cannot open it here (another user's file, no permission): try the next, else go on unlocked
            }
        }
        Lock::Unavailable
    }

    impl Drop for LockGuard {
        fn drop(&mut self) {
            // SAFETY: `fd` is the descriptor holding our lock; unlocked and closed once.
            unsafe {
                let mut fl: libc::flock = std::mem::zeroed();
                fl.l_whence = libc::SEEK_SET as _;
                fl.l_type = libc::F_UNLCK as _;
                libc::fcntl(self.fd, libc::F_SETLKW, &fl);
                libc::close(self.fd);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn protect_roundtrip_and_not_plain() {
        let text = "test_refresh_token_256bit_hearaside_secret";
        let blob = protect(text).expect("protect");
        if cfg!(windows) {
            assert!(
                !blob.windows(text.len()).any(|w| w == text.as_bytes()),
                "DPAPI blob must not contain the plain text"
            );
        }
        assert_eq!(unprotect(&blob).as_deref(), Some(text));
        assert!(protect("").is_none());
        assert!(unprotect(b"").is_none());
        if cfg!(windows) {
            assert!(
                unprotect(b"{\"access_token\":\"plain\"}").is_none(),
                "plain JSON is never accepted on Windows"
            );
        }
    }

    #[test]
    fn lock_is_exclusive_between_threads() {
        let name = format!("HEARASIDE_test_lock_{}", std::process::id());
        let Lock::Held(first) = lock(&name, Duration::from_secs(1), &|| false) else {
            panic!("first")
        };
        let n2 = name.clone();
        // another thread of this process asks for the same named lock: a mutex is per thread on Windows, fcntl locks are
        // per process on Unix (so this half only checks the Windows semantics)
        #[cfg(windows)]
        {
            let t = std::thread::spawn(move || {
                matches!(
                    lock(&n2, Duration::from_millis(250), &|| false),
                    Lock::TimedOut
                )
            });
            assert!(
                t.join().unwrap(),
                "second taker must time out while the first holds it"
            );
            // and a waiter whose owner asks to stop gives up at once instead of waiting out the timeout
            let n3 = name.clone();
            let t0 = std::time::Instant::now();
            let t = std::thread::spawn(move || {
                matches!(lock(&n3, Duration::from_secs(20), &|| true), Lock::TimedOut)
            });
            assert!(t.join().unwrap());
            assert!(t0.elapsed() < Duration::from_secs(2));
        }
        #[cfg(not(windows))]
        let _ = n2;
        drop(first);
        assert!(matches!(
            lock(&name, Duration::from_millis(500), &|| false),
            Lock::Held(_)
        ));
    }
}
