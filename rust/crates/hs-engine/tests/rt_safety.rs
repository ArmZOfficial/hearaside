//! Real-time safety (PLAN R2.4, port and extension of `tests/unit/test_rt_safety.cpp`): after a warm-up,
//! the audio-thread entry points make no heap allocation. A counting global allocator counts every
//! allocation and reallocation made on the current thread while a scope is armed.
//!
//! Wider than the C++ test: the Hub runs with live friends, Line up, a carried friend and the friend tap,
//! FriendReader plays real (jittered) audio so the drift control moves the resampler, and StreamConsumer
//! is counted after its first (connecting) pull. Not covered here: C allocations inside SpeexDSP (they do
//! not go through the Rust allocator; see the ledger entry about `VarResampler::presize`).
#![allow(clippy::needless_range_loop)]

use hs_bus::bus::float_bits;
use hs_bus::protocol::*;
use hs_bus::ring::ring_write;
use hs_bus::shm::{current_pid, now_ns};
use hs_dsp::consumer::StreamConsumer;
use hs_engine::friend_reader::FriendReader;
use hs_engine::hub_engine::{HubEngine, HubParams};
use hs_engine::track_publisher::TrackPublisher;
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::atomic::Ordering::SeqCst;

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

fn bus(tag: &str) -> String {
    // a counter, not the clock: two tests starting in the same tick shared a bus (the second Hub was not the owner)
    static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    format!("rt_{tag}_{}_{n}", current_pid())
}

fn noise_at(t: i64, seed: u32) -> f32 {
    let mut h = (t as u32).wrapping_mul(2_654_435_761) ^ seed.wrapping_mul(40503);
    h ^= h >> 15;
    h = h.wrapping_mul(2_246_822_519);
    h ^= h >> 13;
    h = h.wrapping_mul(3_266_489_917);
    h ^= h >> 16;
    ((h & 0xffff) as f32 / 65535.0 - 0.5) * 0.5
}

#[test]
fn the_counter_counts() {
    let n = allocations(|| {
        let v: Vec<u8> = Vec::with_capacity(10);
        std::hint::black_box(&v);
    });
    assert_eq!(n, 1);
}

#[test]
fn hub_engine_and_track_publisher_process_allocate_nothing_with_friends_live() {
    let name = bus("hub");
    let mut hub = HubEngine::new();
    assert!(hub.connect(&name));
    hub.prepare(48000.0, 256);
    hub.maintain();
    hub.set_line_up(true, 600.0);
    hub.set_friend_tap(Some(Box::new(|_, ch, _, _| {
        std::hint::black_box(ch);
    })));
    let mut a = TrackPublisher::new();
    let mut c = TrackPublisher::new();
    a.connect(&name, "rt-a", 48000, 2);
    c.connect(&name, "rt-carrier", 48000, 1);
    a.prepare(48000, 2);
    c.prepare(48000, 1);
    let b = hub.bus().unwrap();
    // two friends sending, one of them carried by a DAW track (late path)
    for j in 0..2 {
        let h = &b.friends.0[j];
        h.id.store(100 + j as u32, SeqCst);
        h.sampleRate.store(48000, SeqCst);
        h.delayBits
            .store(float_bits(60.0 + 100.0 * j as f32), SeqCst);
        h.route.store(
            if j == 1 {
                FriendRoute::kRouteDaw
            } else {
                FriendRoute::kRouteDirect
            },
            SeqCst,
        );
        h.state.store(FriendState::kFriendLive, SeqCst);
    }
    let cs = c.slot().unwrap();
    cs.fedBy.store(101, SeqCst);
    cs.flags.fetch_or(SlotFlags::kFlagViewersViaHub, SeqCst);

    let (mut l, mut r, mut x) = (vec![0.0f32; 256], vec![0.0f32; 256], vec![0.0f32; 256]);
    let mut pk = vec![0.0f32; 960];
    let mut sent = [0i64; 2];
    let mut t = 0i64;
    let mut cycle = |hub: &mut HubEngine, t: &mut i64| {
        let b = hub.bus().unwrap();
        for j in 0..2 {
            b.friends.0[j].heartbeatNs.store(now_ns(), SeqCst);
            while sent[j] + 960 + 2400 <= *t {
                for (i, s) in pk.iter_mut().enumerate() {
                    *s = noise_at(sent[j] + i as i64, 29 + j as u32);
                }
                ring_write(&b.friendAudio.0[j], &b.friends.0[j].writePos.0, &[&pk], 960);
                sent[j] += 960;
            }
        }
        for (i, s) in x.iter_mut().enumerate() {
            *s = noise_at(*t + i as i64, 11);
        }
        a.process(&[&x, &x], 256, *t, true, false, false);
        c.process(&[&x], 256, *t, true, false, false);
        l.copy_from_slice(&x);
        r.copy_from_slice(&x);
        hub.process(
            &mut [&mut l, &mut r],
            &HubParams::default(),
            *t,
            true,
            false,
        );
        *t += 256;
    };
    // warm-up: friends prime, Line up settles (the first D change waits 0.5 s), the history fills
    for _ in 0..800 {
        cycle(&mut hub, &mut t);
    }
    assert_eq!(
        hub.friend_info(0).playing.load(SeqCst),
        1,
        "friend 0 never started"
    );
    let n = allocations(|| {
        for _ in 0..400 {
            cycle(&mut hub, &mut t);
        }
    });
    assert_eq!(n, 0, "allocations on the audio path");
}

#[test]
fn friend_reader_pull_allocates_nothing_while_it_plays_jittery_audio() {
    let mut reader = FriendReader::new();
    reader.prepare(48000.0, 256);
    // SAFETY: floats; all-zero is valid.
    let ring: Box<ChannelRing> =
        unsafe { Box::from_raw(std::alloc::alloc_zeroed(Layout::new::<ChannelRing>()).cast()) };
    let wp = std::sync::atomic::AtomicU64::new(0);
    let (mut o0, mut o1) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let mut pk = vec![0.0f32; 441];
    let mut sent = 0i64;
    let mut seed = 7u32;
    let mut k = 0i64;
    let mut step = |reader: &mut FriendReader| {
        // a 44.1 kHz friend (resampler path), packets with +-8 ms jitter
        let now_ms = k as f64 * 256.0 / 48.0;
        seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        let jitter = f64::from(seed >> 8) / f64::from(1u32 << 24) * 16.0 - 8.0;
        while (sent / 441) as f64 * 10.0 + 40.0 + jitter <= now_ms {
            for (i, s) in pk.iter_mut().enumerate() {
                *s = noise_at(sent + i as i64, 3);
            }
            ring_write(&ring, &wp, &[&pk], 441);
            sent += 441;
        }
        reader.pull(&ring, &wp, 44100, [&mut o0, &mut o1], 256);
        k += 1;
    };
    for _ in 0..600 {
        step(&mut reader);
    }
    assert!(reader.primed());
    let n = allocations(|| {
        for _ in 0..2000 {
            step(&mut reader);
        }
    });
    assert_eq!(n, 0);
}

#[test]
fn stream_consumer_pull_allocates_nothing_after_it_connected() {
    let name = bus("consumer");
    let mut hub = HubEngine::new();
    hub.connect(&name);
    hub.prepare(44100.0, 256);
    hub.maintain();
    let mut cons = StreamConsumer::new();
    cons.set_report_to_hub(false);
    cons.configure(&name, 0, 30.0, 48000);
    let (mut l, mut r) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let (mut ol, mut or) = (vec![0.0f32; 480], vec![0.0f32; 480]);
    let mut t = 0i64;
    let (mut t_daw, mut t_obs) = (0.0f64, 0.0f64);
    // the first pull connects (opens the shared memory): the counter must see that, or it proves nothing
    let (mut fl, mut fr) = (vec![0.0f32; 480], vec![0.0f32; 480]);
    let first = allocations(|| cons.pull([&mut fl, &mut fr], 480, 0.010));
    assert!(first > 0, "the counter missed the connecting pull");
    let mut run = |hub: &mut HubEngine, cons: &mut StreamConsumer, until: f64| {
        while t_obs < until {
            if t_daw <= t_obs {
                for (i, s) in l.iter_mut().enumerate() {
                    *s = noise_at(t + i as i64, 5);
                }
                r.copy_from_slice(&l);
                hub.process(&mut [&mut l, &mut r], &HubParams::default(), t, true, false);
                t += 256;
                t_daw += 256.0 / (44100.0 * (1.0 + 150e-6));
                continue;
            }
            cons.pull([&mut ol, &mut or], 480, 0.010);
            t_obs += 0.010;
        }
    };
    run(&mut hub, &mut cons, 3.0); // resyncs, settles
    assert!(cons.status().streaming);
    let n = allocations(|| run(&mut hub, &mut cons, 13.0));
    assert_eq!(n, 0);
}
