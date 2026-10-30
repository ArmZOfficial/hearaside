#![allow(dead_code)]
use hs_bus::shm::{current_pid, now_ns};
use std::alloc::{Layout, alloc_zeroed};
use std::sync::atomic::{AtomicU32, Ordering};

static N: AtomicU32 = AtomicU32::new(0);

pub fn unique_bus(tag: &str) -> String {
    format!(
        "test_{tag}_{}_{}_{}",
        current_pid(),
        now_ns() % 100_000,
        N.fetch_add(1, Ordering::Relaxed)
    )
}

/// A zero-filled value on the heap (the bus structs are too big for the stack and all-zero is valid).
///
/// # Safety
/// Only for types where all-zero bits are valid, such as the shared-memory structs.
pub unsafe fn zeroed<T>() -> Box<T> {
    // SAFETY: allocated with T's layout, zero is valid per the caller.
    unsafe { Box::from_raw(alloc_zeroed(Layout::new::<T>()).cast::<T>()) }
}
