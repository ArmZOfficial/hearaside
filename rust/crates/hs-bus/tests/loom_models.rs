//! Loom models of the bus protocols (PLAN R1.6). They re-state the exact load/store/fence sequence of
//! `ring.rs` and the seqlocks in `bus.rs` (slot identity / rename / timeline tag) on loom atomics and
//! explore every thread interleaving and permitted reordering. Shared bytes are relaxed atomic cells
//! (the real code reads them with volatile copies), so what is checked is the protocol: a reader that
//! accepts a read never sees a torn or overwritten value.
//!
//! Run: `RUSTFLAGS="--cfg loom" cargo test -p hs-bus --test loom_models --release`
#![cfg(loom)]

use loom::sync::Arc;
use loom::sync::atomic::{AtomicU32, AtomicU64, Ordering::*, fence};
use loom::thread;

fn model(bound: usize, f: impl Fn() + Sync + Send + 'static) {
    let mut b = loom::model::Builder::new();
    b.preemption_bound = Some(bound);
    b.check(f);
}

// ---------------------------------------------------------------------------------------------
// ring: one writer (blocks of `m` frames), one reader with the double `writePos` check

struct Ring {
    fenced: bool,
    size: u64,
    guard: u64,
    cells: Vec<AtomicU32>,
    wp: AtomicU64,
}

impl Ring {
    fn new(fenced: bool, size: u64, guard: u64) -> Arc<Self> {
        Arc::new(Ring {
            fenced,
            size,
            guard,
            cells: (0..size).map(|_| AtomicU32::new(0)).collect(),
            wp: AtomicU64::new(0),
        })
    }

    /// `ring_write`: frame `f` carries the value `f + 1`.
    fn write_blocks(&self, blocks: u64, m: u64) {
        let mut w = self.wp.load(Relaxed);
        for _ in 0..blocks {
            if self.fenced {
                fence(Release); // the previous publish must be visible to anyone who sees the data below
            }
            for j in 0..m {
                self.cells[((w + j) % self.size) as usize].store((w + j + 1) as u32, Relaxed);
            }
            w += m;
            self.wp.store(w, Release);
        }
    }

    /// `ring_read_at`: returns the frames copied (None = Overrun).
    fn read_at(&self, pos: u64, n: u64) -> Option<Vec<u32>> {
        let limit = self.size - self.guard;
        let w = self.wp.load(Acquire);
        if w > pos && w - pos > limit {
            return None;
        }
        let avail = if w > pos { (w - pos).min(n) } else { 0 };
        let got: Vec<u32> = (0..avail)
            .map(|i| self.cells[((pos + i) % self.size) as usize].load(Relaxed))
            .collect();
        let w2 = if self.fenced {
            fence(Acquire); // pairs with the writer's fence: new data seen => its start position seen
            self.wp.load(Relaxed)
        } else {
            self.wp.load(Acquire)
        };
        if w2 > pos && w2 - pos > limit {
            return None;
        }
        Some(got)
    }
}

fn check_ring(fenced: bool, size: u64, guard: u64, blocks: u64, m: u64, pos: u64, n: u64) {
    model(5, move || {
        let r = Ring::new(fenced, size, guard);
        let w = {
            let r = r.clone();
            thread::spawn(move || r.write_blocks(blocks, m))
        };
        let rd = {
            let r = r.clone();
            thread::spawn(move || {
                if let Some(got) = r.read_at(pos, n) {
                    for (i, v) in got.iter().enumerate() {
                        assert_eq!(
                            *v as u64,
                            pos + i as u64 + 1,
                            "accepted frame {} is wrong",
                            pos + i as u64
                        );
                    }
                }
            })
        };
        w.join().unwrap();
        rd.join().unwrap();
    });
}

#[test]
fn ring_accepted_reads_are_never_overwritten_data() {
    // blocks (2) fit in the guard (4): frame 0 is overwritten by the 5th block, the reader is at 0
    check_ring(true, 8, 4, 6, 2, 0, 2);
}

#[test]
fn ring_reader_further_along_also_safe() {
    check_ring(true, 8, 4, 6, 2, 3, 3);
}

#[test]
#[should_panic(expected = "accepted frame")]
fn ring_without_fences_can_accept_overwritten_data_on_weak_memory() {
    // The C++ ring (acquire load of writePos after the copy, no fences) is only correct on x86-64.
    // The Rust ring adds the fence pair modelled above (see `ring.rs`).
    check_ring(false, 8, 4, 6, 2, 0, 2);
}

#[test]
fn ring_long_write_split_in_guard_sized_chunks_is_safe() {
    // one 8-frame call published as 2-frame chunks (guard 2)
    check_ring(true, 8, 2, 8, 2, 0, 2);
}

#[test]
#[should_panic(expected = "accepted frame")]
fn ring_block_larger_than_guard_can_be_accepted_overwritten() {
    // Why `ring_write` publishes every kGuardFrames: if one published chunk is longer than the guard
    // (size 8, guard 2, chunk 5), frames the writer is overwriting right now still pass the lag check.
    // The safe case (chunks == guard) is `ring_long_write_split_in_guard_sized_chunks_is_safe`.
    check_ring(true, 8, 2, 3, 5, 0, 2);
}

// ---------------------------------------------------------------------------------------------
// seqlock: slot identity (fetch_add form) and rename / app request (store form)

struct SeqLock {
    seq: AtomicU32,
    a: AtomicU32,
    b: AtomicU32,
}

impl SeqLock {
    fn new() -> Arc<Self> {
        Arc::new(SeqLock {
            seq: AtomicU32::new(0),
            a: AtomicU32::new(0),
            b: AtomicU32::new(0),
        })
    }

    /// `seq_write` (set_slot_identity, set_friend_name, ...): odd while writing.
    fn write_add(&self, v: u32) {
        self.seq.fetch_add(1, Relaxed);
        fence(Release);
        self.a.store(v, Relaxed);
        self.b.store(v, Relaxed);
        self.seq.fetch_add(1, Release);
    }

    /// `request_text`: single writer, `s` even when stable, +2 per request.
    fn write_store(&self, v: u32) {
        let s = self.seq.load(Relaxed) & !1;
        self.seq.store(s + 1, Relaxed);
        fence(Release);
        self.a.store(v, Relaxed);
        self.b.store(v, Relaxed);
        self.seq.store(s + 2, Release);
    }

    /// `seq_read`: up to 8 attempts.
    fn read(&self) -> Option<(u32, u32, u32)> {
        for _ in 0..2 {
            let s1 = self.seq.load(Acquire);
            if s1 & 1 != 0 {
                continue;
            }
            let (a, b) = (self.a.load(Relaxed), self.b.load(Relaxed));
            fence(Acquire);
            if self.seq.load(Relaxed) != s1 {
                continue;
            }
            return Some((s1, a, b));
        }
        None
    }
}

fn check_seqlock(write: fn(&SeqLock, u32)) {
    model(4, move || {
        let l = SeqLock::new();
        let w = {
            let l = l.clone();
            thread::spawn(move || {
                write(&l, 1);
                write(&l, 2);
            })
        };
        if let Some((s, a, b)) = l.read() {
            assert_eq!(a, b, "torn read");
            assert_eq!(a, s / 2, "value does not belong to the sequence seen");
        }
        w.join().unwrap();
    });
}

#[test]
fn seqlock_fetch_add_form_never_returns_a_torn_value() {
    check_seqlock(SeqLock::write_add);
}

#[test]
fn seqlock_store_form_never_returns_a_torn_value() {
    check_seqlock(SeqLock::write_store);
}

// ---------------------------------------------------------------------------------------------
// timeline tag: seq = 0 while writing, n + 1 when complete (`write_tag` / `find_timeline_position`)

#[test]
fn timeline_tag_never_returns_a_half_written_entry() {
    model(4, || {
        struct Tag {
            seq: AtomicU64,
            fifo: AtomicU64,
            time: AtomicU64,
        }
        let t = Arc::new(Tag {
            seq: AtomicU64::new(0),
            fifo: AtomicU64::new(0),
            time: AtomicU64::new(0),
        });
        let w = {
            let t = t.clone();
            thread::spawn(move || {
                for n in 0..2u64 {
                    // entry slot reused: the ring of tags laps
                    t.seq.store(0, Relaxed);
                    fence(Release);
                    t.fifo.store(100 + n, Relaxed);
                    t.time.store(200 + n, Relaxed);
                    t.seq.store(n + 1, Release);
                }
            })
        };
        let s1 = t.seq.load(Acquire);
        if s1 != 0 {
            let (fifo, time) = (t.fifo.load(Relaxed), t.time.load(Relaxed));
            fence(Acquire);
            if t.seq.load(Relaxed) == s1 {
                assert_eq!(
                    (fifo, time),
                    (99 + s1, 199 + s1),
                    "half-written tag accepted"
                );
            }
        }
        w.join().unwrap();
    });
}
