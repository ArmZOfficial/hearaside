//! Real-time safety of the audio-thread calls (PLAN section 8): `VocalChain::process`, `PreflightAudio::process` and
//! `MusicOutput::pull_audio` make no heap allocation once prepared. Same counting-allocator pattern as hs-engine's and
//! hs-win's `tests/rt_safety.rs`.
mod common;

use common::{Temp, write_wav};
use hs_karaoke::player::music_player;
use hs_karaoke::preflight::preflight;
use hs_karaoke::vocal::VocalChain;
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::time::{Duration, Instant};

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

#[test]
fn vocal_chain_process_does_not_allocate() {
    let mut chain = VocalChain::new();
    chain.prepare(48000.0, 256);
    let p = chain.params().clone();
    let mic: Vec<f32> = (0..4096).map(|i| 0.3 * (i as f32 * 0.03).sin()).collect();
    let (mut l, mut r, mut net) = (vec![0.0f32; 4096], vec![0.0f32; 4096], vec![0.0f32; 4096]);
    let n = allocations(|| {
        for wet in [false, true] {
            p.set_send_wet(wet);
            for b in 0..200 {
                let k = [64, 256, 4096][b % 3]; // 4096: longer than prepared, runs in pieces
                chain.process(
                    Some(&mic[..k]),
                    Some(&mut l[..k]),
                    Some(&mut r[..k]),
                    Some(&mut net[..k]),
                    k,
                );
            }
        }
        p.set_muted(true);
        chain.process(Some(&mic), Some(&mut l), Some(&mut r), Some(&mut net), 4096);
    });
    assert_eq!(n, 0);
}

#[test]
fn preflight_audio_does_not_allocate() {
    let (mut ui, mut audio) = preflight(48000.0);
    let mic = vec![0.1f32; 256];
    let (mut l, mut r) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    ui.start_check(-1.0, -1.0, false);
    let n = allocations(|| {
        for _ in 0..60 {
            audio.process(Some(&mic), Some(&mut l), Some(&mut r), 256);
        }
    });
    assert_eq!(n, 0);
    assert!(ui.poll());
}

#[test]
fn music_pull_does_not_allocate() {
    // 0.5 s at 44.1 kHz (resampled on the decoder thread)
    let wav = Temp::new("rt.wav");
    write_wav(&wav.0, 44100, 22050, |i| 0.25 * (i as f32 * 0.06).sin());

    let (mut player, mut out) = music_player(48000.0);
    player.add_track(&wav.0);
    player.play();
    let (mut l, mut r) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let t = Instant::now();
    let mut heard = false;
    let mut n = 0;
    while t.elapsed() < Duration::from_secs(3) && (player.is_playing() || !heard) {
        n += allocations(|| out.pull_audio(Some(&mut l), Some(&mut r), 256));
        heard |= l.iter().any(|v| *v != 0.0);
        std::thread::sleep(Duration::from_millis(2));
    }
    // and the seek / stop paths
    player.seek_to(0.1);
    n += allocations(|| out.pull_audio(Some(&mut l), None, 256));
    player.stop();
    n += allocations(|| out.pull_audio(None, Some(&mut r), 256));
    assert!(heard);
    assert_eq!(n, 0);
}
