//! After `init`, Opus encode / decode (mono and stereo, with loss concealment and FEC) and FriendEcho make no Rust
//! heap allocation (the C++ grew its scratch buffers on the audio thread). Allocations inside libopus / SpeexDSP do
//! not go through the Rust allocator; both allocate only in their `*_create` / `*_init` calls.
use hs_net::aec::{AecMode, FriendEcho};
use hs_net::opus::{App, OpusDec, OpusEnc};
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
static GLOBAL: Counting = Counting;

fn counted(f: impl FnOnce()) -> usize {
    COUNT.with(|c| c.set(0));
    ARMED.with(|a| a.set(true));
    f();
    ARMED.with(|a| a.set(false));
    COUNT.with(Cell::get)
}

#[test]
fn codec_and_canceller_do_not_allocate_after_init() {
    let mut enc1 = OpusEnc::new();
    let mut enc2 = OpusEnc::new();
    let mut dec1 = OpusDec::new();
    let mut dec2 = OpusDec::new();
    let mut aec = FriendEcho::new();
    assert!(enc1.init(48000, 1, App::Voip) && enc2.init(48000, 2, App::Audio));
    assert!(dec1.init(48000, 1) && dec2.init(48000, 2));
    assert!(aec.init(48000, 480, 200));
    aec.set_mode(AecMode::On);

    let l: Vec<f32> = (0..5760).map(|i| (i as f32 * 0.05).sin() * 0.4).collect();
    let r: Vec<f32> = (0..5760).map(|i| (i as f32 * 0.07).sin() * 0.3).collect();
    let mut pkt = [0u8; 4000];
    let (mut ol, mut or) = (vec![0.0f32; 5760], vec![0.0f32; 5760]);
    let mut inter = vec![0.0f32; 5760 * 2];
    let mut clean = vec![0.0f32; 480];

    let n = counted(|| {
        for _ in 0..50 {
            let b = enc1.encode(&[&l[..480]], 480, &mut pkt) as usize;
            assert!(dec1.decode(Some(&pkt[..b]), &mut [&mut ol[..480]], 480, false) > 0);
            assert!(dec1.decode(Some(&pkt[..b]), &mut [&mut ol[..480]], 480, true) > 0);
            assert!(dec1.decode_plc(&mut [&mut ol[..480]], 480) > 0);
            // the longest frame (120 ms) in stereo: the C++ resized its scratch for it
            let b = enc2.encode(&[&l, &r], 5760, &mut pkt) as usize;
            assert!(dec2.decode(Some(&pkt[..b]), &mut [&mut ol, &mut or], 5760, false) > 0);
            assert!(dec2.decode_plc(&mut [&mut ol, &mut or], 5760) > 0);
            assert!(dec2.decode_interleaved(Some(&pkt[..b]), &mut inter, 5760, false) > 0);
            aec.push_reference(&l[..480]);
            aec.process(&r[..480], &mut clean, 960);
        }
    });
    assert_eq!(n, 0, "allocations on the audio path");
}
