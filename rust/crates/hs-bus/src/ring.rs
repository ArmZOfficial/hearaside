//! Broadcast ring buffer: one writer, any number of readers, the writer never waits.
//! Port of `libs/ssbus/include/ssbus/ring.h`. Real-time safe: only `copy_nonoverlapping` and atomics.
//!
//! The samples are plain `f32` in shared memory; a reader that copies while the writer laps it is
//! detected by re-reading `write_pos` afterwards (the block is then zeroed and `Overrun` returned),
//! exactly like the C++ original.
use crate::protocol::{ChannelRing, kGuardFrames, kMaxChannels, kRingFrames, kRingMask};
use core::ptr;
use core::sync::atomic::{AtomicU64, Ordering, fence};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReadResult {
    Ok,
    Underrun,
    Overrun,
}

fn channel(ring: &ChannelRing, c: usize) -> *mut f32 {
    // SAFETY: `ch` is `[[f32; kRingFrames]; kMaxChannels]`, so channel `c` starts `c * kRingFrames` floats in.
    unsafe { ring.ch.get().cast::<f32>().add(c * kRingFrames as usize) }
}

/// `(start index, frames before the wrap)` for `n` frames at absolute position `pos`.
fn span(pos: u64, n: u32) -> (usize, u32) {
    let start = (pos & u64::from(kRingMask)) as u32;
    (start as usize, n.min(kRingFrames - start))
}

/// Writes `n` frames of `src` (one slice = dual mono, two = stereo; empty = silence).
/// Every slice must hold at least `n` frames; a shorter one is a caller bug (debug assert) and, rather
/// than panicking on the audio thread, `n` is cut to the shortest source.
pub fn ring_write(ring: &ChannelRing, write_pos: &AtomicU64, src: &[&[f32]], n: u32) {
    let shortest = src.iter().map(|s| s.len()).min().unwrap_or(n as usize);
    debug_assert!(shortest >= n as usize, "ring_write: source shorter than n");
    let n = n.min(shortest.min(u32::MAX as usize) as u32);
    let mut w = write_pos.load(Ordering::Relaxed); // single writer
    let mut done = 0u32;
    // Publish at least every `kGuardFrames`: a reader that accepts a frame was lapped by at most one
    // chunk, and the lag check only covers that when the chunk is no longer than the guard. (The C++
    // ring uses `kRingFrames - kGuardFrames` here, which lets a long host block tear; same layout.)
    const CHUNK: u32 = kGuardFrames;
    while done < n {
        let m = (n - done).min(CHUNK);
        // Anyone who sees the samples written below must also see the previous publish of `write_pos`
        // (pairs with the fence in `ring_read_at`; the C++ ring relies on x86 ordering instead).
        fence(Ordering::Release);
        let (start, first) = span(w, m);
        for c in 0..kMaxChannels {
            let base = channel(ring, c);
            // SAFETY: start + first <= kRingFrames and (m - first) <= start, so both pieces stay in
            // channel `c`; `s` holds at least `done + m` frames (caller contract).
            unsafe {
                if src.is_empty() {
                    ptr::write_bytes(base.add(start), 0, first as usize);
                    ptr::write_bytes(base, 0, (m - first) as usize);
                } else {
                    let s = &src[c.min(src.len() - 1)][done as usize..(done + m) as usize];
                    ptr::copy_nonoverlapping(s.as_ptr(), base.add(start), first as usize);
                    ptr::copy_nonoverlapping(
                        s[first as usize..].as_ptr(),
                        base,
                        (m - first) as usize,
                    );
                }
            }
        }
        w += u64::from(m);
        done += m;
        write_pos.store(w, Ordering::Release);
    }
}

fn zero_all(dst: &mut [&mut [f32]], n: u32) {
    for d in dst.iter_mut().take(kMaxChannels) {
        d[..n as usize].fill(0.0);
    }
}

/// Reads `n` frames starting at absolute frame `pos` into `dst[0..2]` (both channels). Frames not
/// written yet are zero-filled (`Underrun`); if any requested frame was already overwritten the whole
/// block is zeroed and `Overrun` is returned. `frames_available` receives the frames really read.
pub fn ring_read_at(
    ring: &ChannelRing,
    write_pos: &AtomicU64,
    pos: u64,
    dst: &mut [&mut [f32]],
    n: u32,
    frames_available: Option<&mut u32>,
) -> ReadResult {
    // Same rule as `ring_write`: short destination slices are a caller bug; never write past them.
    let shortest = dst
        .iter()
        .take(kMaxChannels)
        .map(|d| d.len())
        .min()
        .unwrap_or(0);
    debug_assert!(
        shortest >= n as usize,
        "ring_read_at: destination shorter than n"
    );
    let n = n.min(shortest.min(u32::MAX as usize) as u32);
    let limit = u64::from(kRingFrames - kGuardFrames);
    let set_avail = |v: u32, out: Option<&mut u32>| {
        if let Some(o) = out {
            *o = v;
        }
    };
    let w = write_pos.load(Ordering::Acquire);
    if w > pos && w - pos > limit {
        zero_all(dst, n);
        set_avail(0, frames_available);
        return ReadResult::Overrun;
    }
    let avail = if w > pos {
        (w - pos).min(u64::from(n)) as u32
    } else {
        0
    };
    for (c, d) in dst.iter_mut().enumerate().take(kMaxChannels) {
        if avail > 0 {
            let (start, first) = span(pos, avail);
            let base = channel(ring, c);
            // SAFETY: same bounds argument as in `ring_write`; `d` holds at least `n >= avail` frames.
            unsafe {
                ptr::copy_nonoverlapping(base.add(start), d.as_mut_ptr(), first as usize);
                ptr::copy_nonoverlapping(
                    base,
                    d.as_mut_ptr().add(first as usize),
                    (avail - first) as usize,
                );
            }
        }
        d[avail as usize..n as usize].fill(0.0);
    }
    fence(Ordering::Acquire);
    let w2 = write_pos.load(Ordering::Relaxed);
    if w2 > pos && w2 - pos > limit {
        // overwritten while copying
        zero_all(dst, n);
        set_avail(0, frames_available);
        return ReadResult::Overrun;
    }
    set_avail(avail, frames_available);
    if avail == n {
        ReadResult::Ok
    } else {
        ReadResult::Underrun
    }
}

/// Cursor-style read: advances `r` by the number of frames actually available.
pub fn ring_read(
    ring: &ChannelRing,
    write_pos: &AtomicU64,
    r: &mut u64,
    dst: &mut [&mut [f32]],
    n: u32,
) -> ReadResult {
    let mut got = 0;
    let res = ring_read_at(ring, write_pos, *r, dst, n, Some(&mut got));
    *r += u64::from(got);
    res
}
