//! Real-time safety of the buffer switch (PLAN §8): after `open` and `start`, a switch on the driver's thread makes no
//! heap allocation, whatever the sample types. Same counting-allocator pattern as hs-engine's `tests/rt_safety.rs`:
//! a global allocator counts every allocation and reallocation made on the current thread while a scope is armed.

mod common;

use common::*;
use hs_win::asio::{AudioCallback, Block};
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

struct Counting;

thread_local! {
    static ARMED: Cell<bool> = const { Cell::new(false) };
    static COUNT: Cell<usize> = const { Cell::new(0) };
}

fn note() {
    // `try_with`: the allocator also runs while thread locals are being torn down
    let _ = ARMED.try_with(|a| {
        if a.get() {
            let _ = COUNT.try_with(|c| c.set(c.get() + 1));
        }
    });
}

// SAFETY: forwards to the system allocator; the bookkeeping touches only thread-local Cells.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: same contract as the caller's.
        unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: same contract as the caller's.
        unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        note();
        // SAFETY: same contract as the caller's.
        unsafe { System.realloc(ptr, layout, new_size) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: same contract as the caller's.
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

/// Runs `f` with the counter armed on this thread; returns the allocations it made.
fn allocations(f: impl FnOnce()) -> usize {
    COUNT.with(|c| c.set(0));
    ARMED.with(|a| a.set(true));
    f();
    ARMED.with(|a| a.set(false));
    COUNT.with(Cell::get)
}

/// A callback that touches every channel, like a real one.
struct Mix;

impl AudioCallback for Mix {
    fn process(&mut self, io: &mut Block<'_>) {
        for c in 0..io.num_outputs() {
            for i in 0..io.frames() {
                let v = io
                    .input(c % io.num_inputs().max(1))
                    .get(i)
                    .copied()
                    .unwrap_or(0.0);
                io.output(c)[i] = v * 0.5 + 0.1;
            }
        }
    }
}

#[test]
fn the_counter_counts() {
    assert!(allocations(|| drop(std::hint::black_box(vec![1u8, 2, 3]))) >= 1);
}

#[test]
fn the_buffer_switch_does_not_allocate() {
    let _s = serial();
    // every type the host takes, in and out
    let cfg = Cfg {
        inputs: vec![
            (INT16, "a"),
            (INT24, "b"),
            (FLOAT32, "c"),
            (FLOAT64, "d"),
            (INT32_24, "e"),
        ],
        outputs: vec![
            (INT16, "a"),
            (INT24, "b"),
            (FLOAT32, "c"),
            (FLOAT64, "d"),
            (INT32_24, "e"),
        ],
        buffer: (32, 1024, 256, -1),
        ..Cfg::default()
    };
    let (mut d, probe, ctl) = device(cfg);
    d.open(&[true; 5], &[true; 5], 0.0, 0).unwrap();
    assert!(d.start(Box::new(Mix)));
    let sw = ctl.switch_fn(); // fetched outside the counted scope: the fake's bookkeeping allocates
    sw(0, 1);
    sw(1, 1); // warm-up
    let before = probe
        .output_ready_calls
        .load(std::sync::atomic::Ordering::SeqCst);
    let n = allocations(|| {
        for i in 0..2000 {
            sw(i & 1, 1);
        }
    });
    assert_eq!(n, 0, "the buffer switch allocated");
    assert_eq!(
        probe
            .output_ready_calls
            .load(std::sync::atomic::Ordering::SeqCst)
            - before,
        2000,
        "the loop ran"
    );

    // no callback at all (stopped): still nothing
    d.stop();
    let n = allocations(|| {
        for i in 0..100 {
            sw(i & 1, 1);
        }
    });
    assert_eq!(n, 0);
}

#[test]
fn the_message_and_rate_callbacks_do_not_allocate_either() {
    let _s = serial();
    let (mut d, _, ctl) = device(Cfg::default());
    d.open(&[true], &[true], 0.0, 0).unwrap();
    assert_eq!(ctl.message(3, 0), 1);
    let n = allocations(|| {
        for _ in 0..100 {
            ctl.message(3, 0);
            ctl.message(1, 3);
            ctl.rate_changed();
        }
    });
    assert_eq!(n, 0);
}
