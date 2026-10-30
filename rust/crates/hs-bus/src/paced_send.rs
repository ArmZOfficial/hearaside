//! What the share server sends to one slow connection: a queue that holds at most what the socket has
//! not taken yet, and the rule for where a ring reader continues when the line could not keep up.
//! Port of `libs/ssbus/include/ssbus/paced_send.h`. Not for the audio thread; no sockets in here.

/// Whole messages go in, bytes go out as the socket takes them. Audio is only accepted while nothing
/// waits, so a slow line never makes it grow (what would be late is dropped, the caller jumps ahead).
#[derive(Default)]
pub struct SendQueue {
    buf: Vec<u8>,
    off: usize,
    /// where the audio packet at the front ends
    audio_end: usize,
}

impl SendQueue {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn idle(&self) -> bool {
        self.off >= self.buf.len()
    }

    pub fn pending(&self) -> usize {
        self.buf.len() - self.off
    }

    /// What the queue really holds in memory (tests).
    pub fn stored(&self) -> usize {
        self.buf.len()
    }

    /// Bytes waiting that are not the audio packet at the front: what the cap on control messages counts.
    pub fn control_pending(&self) -> usize {
        self.buf.len() - self.off.max(self.audio_end)
    }

    /// A control message (ping, pong): always kept, in order, after what waits already.
    pub fn append(&mut self, p: &[u8]) {
        if self.idle() {
            self.buf.clear();
            self.off = 0;
            self.audio_end = 0;
        } else if self.off > 4096 && self.off * 2 > self.buf.len() {
            // What is sent already must not stay: a peer that never lets the queue go idle would grow it forever.
            self.buf.drain(..self.off);
            self.audio_end = self.audio_end.saturating_sub(self.off);
            self.off = 0;
        }
        self.buf.extend_from_slice(p);
    }

    /// The same, but only while `cap` bytes of control messages do not wait yet. `false` = not kept.
    pub fn append_if_room(&mut self, p: &[u8], cap: usize) -> bool {
        if self.control_pending() >= cap {
            return false;
        }
        self.append(p);
        true
    }

    /// Audio: kept only when nothing waits. `false` = dropped.
    pub fn offer(&mut self, p: &[u8]) -> bool {
        if !self.idle() {
            return false;
        }
        self.append(p);
        self.audio_end = self.buf.len();
        true
    }

    /// `send(bytes)` returns the bytes taken, 0 = the socket is full right now, < 0 = the connection is
    /// gone. Returns `false` when it is gone. Stops at the first "full".
    pub fn flush(&mut self, mut send: impl FnMut(&[u8]) -> isize) -> bool {
        while !self.idle() {
            let w = send(&self.buf[self.off..]);
            if w < 0 {
                return false;
            }
            if w == 0 {
                return true;
            }
            self.off += w as usize;
        }
        true
    }
}

/// Where to read next from a ring that `write` has run ahead of: more than `max_lag_ms` behind = skip
/// to `keep_ms` before the newest sample (the oldest audio is dropped), else stay.
pub fn catch_up(cursor: u64, write: u64, rate: u32, max_lag_ms: u32, keep_ms: u32) -> u64 {
    if write < cursor || write - cursor <= u64::from(rate) * u64::from(max_lag_ms) / 1000 {
        return cursor;
    }
    let keep = u64::from(rate) * u64::from(keep_ms) / 1000;
    write.saturating_sub(keep)
}
