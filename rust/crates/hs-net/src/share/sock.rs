//! Socket helpers of the share server: poll based waits, one non-blocking send, `SO_SNDBUF`, MMCSS.
//! Port of the helpers at the top of `plugins/common/ShareServer.cpp`. All sockets are non-blocking; every wait is a
//! `poll` so a thread never sits in a call that `stop()` cannot interrupt.
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::time::Duration;

#[cfg(unix)]
use std::os::fd::AsRawFd;
#[cfg(windows)]
use std::os::windows::io::AsRawSocket;

/// The kernel's queue per connection (Windows would grow it to MBs; Linux doubles this). Less caps a 100 ms line
/// below PCM stereo.
pub(crate) const SEND_BUFFER_BYTES: i32 = 64 * 1024;

#[cfg(windows)]
mod sys {
    use super::*;
    use windows_sys::Win32::Foundation::{HANDLE_FLAG_INHERIT, SetHandleInformation};
    use windows_sys::Win32::Networking::WinSock::{
        POLLERR, POLLHUP, POLLNVAL, POLLRDNORM, POLLWRNORM, SO_SNDBUF, SOCKET, SOL_SOCKET,
        WSAPOLLFD, WSAPoll, setsockopt,
    };

    pub(crate) type Raw = SOCKET;
    pub(crate) fn raw_stream(s: &TcpStream) -> Raw {
        s.as_raw_socket() as Raw
    }
    pub(crate) fn raw_listener(s: &TcpListener) -> Raw {
        s.as_raw_socket() as Raw
    }
    /// `(poll result, revents)` for read (`true`) or write readiness.
    pub(crate) fn poll(raw: Raw, read: bool, ms: i32) -> (i32, i16, bool) {
        let mut p = WSAPOLLFD {
            fd: raw,
            events: if read { POLLRDNORM } else { POLLWRNORM },
            revents: 0,
        };
        // SAFETY: one valid WSAPOLLFD on the stack; WSAPoll only reads and writes it for the call.
        let r = unsafe { WSAPoll(&mut p, 1, ms) };
        (
            r,
            p.revents,
            p.revents & (POLLERR | POLLHUP | POLLNVAL) != 0,
        )
    }
    pub(crate) fn ready_bit(revents: i16, read: bool) -> bool {
        revents & if read { POLLRDNORM } else { POLLWRNORM } != 0
    }
    pub(crate) fn set_sndbuf(raw: Raw, bytes: i32) {
        // SAFETY: `raw` is a live socket owned by the caller; the option value is a valid `int` on the stack.
        unsafe {
            setsockopt(
                raw,
                SOL_SOCKET,
                SO_SNDBUF,
                (&raw const bytes).cast(),
                size_of::<i32>() as i32,
            )
        };
    }
    /// A process the DAW starts must not keep our sockets open after we close them.
    pub(crate) fn no_inherit(raw: Raw) {
        // SAFETY: a socket is a HANDLE; clearing the inherit flag of a live handle has no other effect.
        unsafe { SetHandleInformation(raw as _, HANDLE_FLAG_INHERIT, 0) };
    }
}

#[cfg(unix)]
mod sys {
    use super::*;

    pub(crate) type Raw = libc::c_int;
    pub(crate) fn raw_stream(s: &TcpStream) -> Raw {
        s.as_raw_fd()
    }
    pub(crate) fn raw_listener(s: &TcpListener) -> Raw {
        s.as_raw_fd()
    }
    pub(crate) fn poll(raw: Raw, read: bool, ms: i32) -> (i32, i16, bool) {
        let mut p = libc::pollfd {
            fd: raw,
            events: if read { libc::POLLIN } else { libc::POLLOUT },
            revents: 0,
        };
        // SAFETY: one valid pollfd on the stack; poll only reads and writes it for the call.
        let r = unsafe { libc::poll(&mut p, 1, ms) };
        (
            r,
            p.revents,
            p.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0,
        )
    }
    pub(crate) fn ready_bit(revents: i16, read: bool) -> bool {
        revents & if read { libc::POLLIN } else { libc::POLLOUT } != 0
    }
    pub(crate) fn set_sndbuf(raw: Raw, bytes: i32) {
        // SAFETY: `raw` is a live socket owned by the caller; the option value is a valid `int` on the stack.
        unsafe {
            libc::setsockopt(
                raw,
                libc::SOL_SOCKET,
                libc::SO_SNDBUF,
                (&raw const bytes).cast(),
                size_of::<i32>() as libc::socklen_t,
            )
        };
    }
    pub(crate) fn no_inherit(_raw: Raw) {} // std opens every socket close-on-exec
}

/// Waits until the socket is readable (or the time is up). 1 = readable, 0 = timed out, < 0 = error / closed.
pub(crate) fn wait_readable(s: &TcpStream, ms: i32) -> i32 {
    let (r, rev, bad) = sys::poll(sys::raw_stream(s), true, ms);
    if r < 0 {
        return -1;
    }
    if r == 0 {
        return 0;
    }
    // A half-closed connection reports HUP together with the bytes that came before the FIN (WSAPoll does): those
    // must still be read, `read` returns 0 for the end. C++ bug: HUP alone made this "closed" and lost them.
    if sys::ready_bit(rev, true) {
        1
    } else if bad {
        -1
    } else {
        0
    }
}

/// Waits until the socket may take more (or the time is up).
pub(crate) fn wait_writable(s: &TcpStream, ms: i32) {
    let _ = sys::poll(sys::raw_stream(s), false, ms);
}

/// The listener has a connection to accept.
pub(crate) fn listener_readable(l: &TcpListener, ms: i32) -> bool {
    let (r, rev, _) = sys::poll(sys::raw_listener(l), true, ms);
    r > 0 && sys::ready_bit(rev, true)
}

/// A small kernel queue per connection; a failure is not an error.
pub(crate) fn limit_send_buffer(s: &TcpStream) {
    sys::set_sndbuf(sys::raw_stream(s), SEND_BUFFER_BYTES);
}

pub(crate) fn no_inherit_stream(s: &TcpStream) {
    sys::no_inherit(sys::raw_stream(s));
}

pub(crate) fn no_inherit_listener(l: &TcpListener) {
    sys::no_inherit(sys::raw_listener(l));
}

/// One send on the raw socket: bytes taken, 0 = full right now, < 0 = the connection is gone.
pub(crate) fn try_send(mut s: &TcpStream, data: &[u8]) -> isize {
    if data.is_empty() {
        return 0;
    }
    match s.write(&data[..data.len().min(1 << 20)]) {
        Ok(n) => n as isize,
        Err(e)
            if matches!(
                e.kind(),
                io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
            ) =>
        {
            0
        }
        Err(_) => -1,
    }
}

/// Everything, or `false` after 5 s without the socket taking a byte (a page, a short frame: not for audio).
pub(crate) fn write_all(s: &TcpStream, data: &[u8]) -> bool {
    let mut rest = data;
    let mut stalled = 0;
    while !rest.is_empty() {
        let w = try_send(s, rest);
        if w < 0 {
            return false;
        }
        if w == 0 {
            stalled += 1;
            if stalled > 50 {
                return false;
            }
            wait_writable(s, 100);
            continue;
        }
        stalled = 0;
        rest = &rest[w as usize..];
    }
    true
}

/// One read from a non-blocking socket: bytes, 0 = nothing yet, < 0 = closed / error.
pub(crate) fn try_read(mut s: &TcpStream, buf: &mut [u8]) -> isize {
    match s.read(buf) {
        Ok(0) => -1,
        Ok(n) => n as isize,
        Err(e)
            if matches!(
                e.kind(),
                io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
            ) =>
        {
            0
        }
        Err(_) => -1,
    }
}

/// Reads exactly `buf.len()` bytes; gives up after `timeout_ms` without progress or when `alive` turns false.
pub(crate) fn read_exact(
    s: &TcpStream,
    buf: &mut [u8],
    timeout_ms: i32,
    alive: &dyn Fn() -> bool,
) -> bool {
    let mut at = 0;
    let mut waited = 0;
    while at < buf.len() {
        if !alive() {
            return false;
        }
        match wait_readable(s, 100) {
            r if r < 0 => return false,
            0 => {
                waited += 100;
                if waited >= timeout_ms {
                    return false;
                }
                continue;
            }
            _ => {}
        }
        let got = try_read(s, &mut buf[at..]);
        if got < 0 {
            return false;
        }
        if got > 0 {
            at += got as usize;
            waited = 0;
        }
    }
    true
}

/// Closes both directions so a thread blocked on this socket wakes up; the descriptor itself is only released when the
/// owning `TcpStream` drops (after the thread was joined), so it can never be reused under a running thread.
pub(crate) fn shutdown(s: &TcpStream) {
    let _ = s.shutdown(std::net::Shutdown::Both);
}

/// Unix: a write to a socket the browser already closed raises SIGPIPE, which would end the DAW. Ignored once, as the
/// C++ does (a Rust runtime inside a cdylib does not do it for us); a failed write then returns an error.
pub(crate) fn ignore_sigpipe() {
    #[cfg(unix)]
    {
        static ONCE: std::sync::Once = std::sync::Once::new();
        ONCE.call_once(|| {
            // SAFETY: installing the SIG_IGN disposition has no handler code and no pointer arguments.
            unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };
        });
    }
}

/// Apple has no MSG_NOSIGNAL: the socket itself must not raise SIGPIPE.
#[cfg(target_vendor = "apple")]
pub(crate) fn no_sigpipe(s: &TcpStream) {
    let one: libc::c_int = 1;
    // SAFETY: a live socket and a valid `int` option value on the stack.
    unsafe {
        libc::setsockopt(
            s.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_NOSIGPIPE,
            (&raw const one).cast(),
            size_of::<libc::c_int>() as libc::socklen_t,
        )
    };
}

#[cfg(not(target_vendor = "apple"))]
pub(crate) fn no_sigpipe(_s: &TcpStream) {}

/// MMCSS "Audio" priority for real-time network audio threads on Windows.
pub(crate) struct Mmcss {
    #[cfg(windows)]
    handle: windows_sys::Win32::Foundation::HANDLE,
}

#[cfg(windows)]
impl Mmcss {
    pub(crate) fn new(task: &str) -> Self {
        use windows_sys::Win32::System::Threading::AvSetMmThreadCharacteristicsW;
        let wide: Vec<u16> = task.encode_utf16().chain(Some(0)).collect();
        let mut index = 0u32;
        // SAFETY: `wide` is NUL terminated and outlives the call; `index` is a valid out pointer.
        let handle = unsafe { AvSetMmThreadCharacteristicsW(wide.as_ptr(), &mut index) };
        Mmcss { handle }
    }
}

#[cfg(windows)]
impl Drop for Mmcss {
    fn drop(&mut self) {
        use windows_sys::Win32::System::Threading::AvRevertMmThreadCharacteristics;
        if !self.handle.is_null() {
            // SAFETY: the handle came from `AvSetMmThreadCharacteristicsW` on this thread and is reverted once.
            unsafe { AvRevertMmThreadCharacteristics(self.handle) };
        }
    }
}

#[cfg(not(windows))]
impl Mmcss {
    pub(crate) fn new(_task: &str) -> Self {
        Mmcss {}
    }
}

pub(crate) fn sleep_ms(ms: u64) {
    std::thread::sleep(Duration::from_millis(ms));
}
