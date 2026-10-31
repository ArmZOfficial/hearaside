//! `RoomHost`: the port of `karaoke: RoomHost bus initialization, friend slot management, streamAudio for OBS, and
//! master WAV recording` from `tests/unit/test_karaoke.cpp`, plus what the C++ test did not look at: each friend's
//! voice on its own line (C++ bug 1), muting, a long block, the WAV's content, and closing the room.
//!
//! One test function: it sets the bus name and turns the cloudflared tunnel off through the environment (as the C++
//! test did), which must happen before any other thread runs.
mod common;

use common::allocations;
use hs_bus::bus::read_friend_name;
use hs_bus::protocol::kMaxFriends;
use hs_bus::ring::{ring_read_at, ring_write};
use hs_bus::shm::{SharedMemory, now_ns};
use hs_karaoke::room::{RoomHost, mix_local};
use std::sync::atomic::Ordering::SeqCst;

fn energy(x: &[f32]) -> f32 {
    x.iter().map(|v| v * v).sum()
}

#[test]
fn karaoke_room_host_bus_slots_obs_stream_and_recording() {
    let bus_name = format!("rs-karaoke-room-{}", std::process::id());
    // SAFETY: the first statement of the only test in this binary: no other thread reads the environment yet.
    unsafe {
        std::env::set_var("HEARASIDE_DEFAULT_BUS", &bus_name);
        std::env::set_var("HEARASIDE_NO_TUNNEL", "1");
    }

    let (mut host, mut audio) =
        RoomHost::start("Test Karaoke Room", 48000.0, 256).expect("room starts");
    assert_eq!(host.bus_name(), bus_name);
    assert!(!host.has_daw_collision());
    assert_eq!(host.friend_count(), 8);
    assert!(host.server_port() > 0);
    assert_eq!(host.room_name(), "Test Karaoke Room");

    let shm = SharedMemory::open_existing(&bus_name).unwrap();
    let l = shm.layout();
    assert_eq!(l.streamHeader.active.load(SeqCst), 1);
    assert_eq!(l.streamHeader.sampleRate.load(SeqCst), 48000);
    assert_ne!(l.header.hubOwnerToken.load(SeqCst), 0);

    for i in 0..kMaxFriends {
        let s = host.slot_info(i).unwrap();
        assert_eq!((s.slot, s.id), (i, i as u32 + 1));
        assert_eq!(s.name, format!("Friend {}", i + 1));
        assert_eq!(read_friend_name(&l.friends.0[i]), s.name);
        assert!(!s.is_muted && !s.is_live);
        assert!(host.friend_url(i).contains(&s.token));
        let deep = host.friend_deep_link(i);
        assert!(
            deep.starts_with("hearaside://join?url=http%3A%2F%2F"),
            "{deep}"
        );
    }
    assert!(host.slot_info(8).is_none());

    host.set_slot_muted(3, true);
    assert!(host.slot_info(3).unwrap().is_muted);
    host.set_slot_muted(3, false);
    assert!(!host.slot_info(3).unwrap().is_muted);

    // headphones and the OBS stream
    let (voc, mus) = (vec![0.2f32; 256], vec![0.1f32; 256]);
    let (mut hl, mut hr) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let w0 = l.streamHeader.writePos.0[0].load(SeqCst);
    audio.process(
        [&voc, &voc],
        [&mus, &mus],
        [&mut hl, &mut hr],
        256,
        true,
        true,
    );
    assert!(hl[0] > 0.25 && hr[0] > 0.25);
    assert_eq!(l.streamHeader.writePos.0[0].load(SeqCst), w0 + 256);
    let read = |ring, wp: &std::sync::atomic::AtomicU64, at: u64, n: usize| {
        let (mut a, mut b) = (vec![0.0f32; n], vec![0.0f32; n]);
        ring_read_at(ring, wp, at, &mut [&mut a[..], &mut b[..]], n as u32, None);
        (a, b)
    };
    let (s0, _) = read(&l.streamAudio.0[0], &l.streamHeader.writePos.0[0], w0, 256);
    assert!((s0[0] - 0.3).abs() < 1e-6, "OBS hears vocal + music");
    // "Hear yourself" off: music only in the headphones; "Others hear you" off: OBS gets the music only
    audio.process(
        [&voc, &voc],
        [&mus, &mus],
        [&mut hl, &mut hr],
        256,
        false,
        true,
    );
    assert!((hl[0] - 0.1).abs() < 1e-6);
    let w1 = l.streamHeader.writePos.0[0].load(SeqCst);
    audio.process(
        [&voc, &voc],
        [&mus, &mus],
        [&mut hl, &mut hr],
        256,
        true,
        false,
    );
    assert!((hl[0] - 0.3).abs() < 1e-6);
    let (s1, _) = read(&l.streamAudio.0[0], &l.streamHeader.writePos.0[0], w1, 256);
    assert!((s1[0] - 0.1).abs() < 1e-6);

    // a friend (slot 2) sings: heard in the headphones, and on their own voice line (pair 1, channel 0) only
    let f = &l.friends.0[2];
    let tone: Vec<f32> = (0..256).map(|i| 0.4 * (i as f32 * 0.09).sin()).collect();
    let silent = vec![0.0f32; 256];
    let mut heard = 0.0;
    for _ in 0..60 {
        ring_write(&l.friendAudio.0[2], &f.writePos.0, &[&tone, &tone], 256);
        f.heartbeatNs.store(now_ns(), SeqCst);
        audio.process(
            [&silent, &silent],
            [&silent, &silent],
            [&mut hl, &mut hr],
            256,
            true,
            true,
        );
        heard = energy(&hl);
    }
    assert!(heard > 1.0, "the friend is in the headphones ({heard})");
    assert!(host.slot_info(2).unwrap().is_live);
    assert!(host.slot_info(2).unwrap().peak > 0.1);
    let vw = l.friendVoiceWrite.0[1].load(SeqCst);
    let (v2, v3) = read(
        &l.friendVoiceAudio.0[1],
        &l.friendVoiceWrite.0[1],
        vw - 256,
        256,
    );
    assert!(energy(&v2) > 1.0, "friend 2 on pair 1, channel 0");
    assert_eq!(energy(&v3), 0.0, "friend 3 is silent");
    for k in [0, 2, 3] {
        let w = l.friendVoiceWrite.0[k].load(SeqCst);
        let (a, b) = read(
            &l.friendVoiceAudio.0[k],
            &l.friendVoiceWrite.0[k],
            w - 256,
            256,
        );
        assert_eq!(
            energy(&a) + energy(&b),
            0.0,
            "pair {k} carries nobody (the C++ copied friend 2 into it)"
        );
    }
    host.set_slot_muted(2, true);
    ring_write(&l.friendAudio.0[2], &f.writePos.0, &[&tone, &tone], 256);
    audio.process(
        [&silent, &silent],
        [&silent, &silent],
        [&mut hl, &mut hr],
        256,
        true,
        true,
    );
    assert_eq!(energy(&hl), 0.0, "muted: not in the headphones");
    host.set_slot_muted(2, false);
    host.kick_slot(2);
    assert!(!host.slot_info(2).unwrap().is_live);

    // the audio thread allocates nothing (PLAN section 8), friends and recording included
    let wav0 = std::env::temp_dir().join(format!("hs-karaoke-room-rt-{}.wav", std::process::id()));
    assert!(host.start_recording(&wav0));
    let n = allocations(|| {
        for _ in 0..100 {
            ring_write(
                &l.friendAudio.0[5],
                &l.friends.0[5].writePos.0,
                &[&tone, &tone],
                256,
            );
            audio.process(
                [&voc, &voc],
                [&mus, &mus],
                [&mut hl, &mut hr],
                256,
                true,
                true,
            );
        }
    });
    host.stop_recording();
    let _ = std::fs::remove_file(&wav0);
    assert_eq!(n, 0, "RoomAudio::process allocated");
    host.set_slot_muted(5, true); // its buffered audio would land in the next check

    // a block longer than prepared runs in pieces (the C++ wrote past its scratch buffers)
    let (bv, bm) = (vec![0.2f32; 3000], vec![0.1f32; 3000]);
    let (mut bl, mut br) = (vec![0.0f32; 3000], vec![0.0f32; 3000]);
    let w2 = l.streamHeader.writePos.0[0].load(SeqCst);
    audio.process([&bv, &bv], [&bm, &bm], [&mut bl, &mut br], 3000, true, true);
    assert!(bl.iter().all(|v| (v - 0.3).abs() < 1e-6));
    assert_eq!(l.streamHeader.writePos.0[0].load(SeqCst), w2 + 3000);

    // recording: what the headphones played, as float WAV
    let wav = std::env::temp_dir().join(format!("hs-karaoke-room-{}.wav", std::process::id()));
    assert!(host.start_recording(&wav));
    assert!(host.is_recording());
    for _ in 0..20 {
        audio.process(
            [&voc, &voc],
            [&mus, &mus],
            [&mut hl, &mut hr],
            256,
            true,
            true,
        );
    }
    host.stop_recording();
    assert!(!host.is_recording());
    assert!((host.recorded_seconds() - 20.0 * 256.0 / 48000.0).abs() < 1e-9);
    let bytes = std::fs::read(&wav).unwrap();
    let _ = std::fs::remove_file(&wav);
    assert_eq!(&bytes[..4], b"RIFF");
    assert_eq!(u16::from_le_bytes([bytes[20], bytes[21]]), 3, "IEEE float");
    let data_len = u32::from_le_bytes(bytes[40..44].try_into().unwrap()) as usize;
    assert_eq!(data_len, 20 * 256 * 8);
    assert_eq!(bytes.len(), 44 + data_len);
    let first = f32::from_le_bytes(bytes[44..48].try_into().unwrap());
    assert!((first - 0.3).abs() < 1e-6);

    // closing the room: OBS sees it stop, the audio half falls back to the local mix
    drop(host);
    assert_eq!(l.streamHeader.active.load(SeqCst), 0);
    assert_eq!(l.header.hubOwnerToken.load(SeqCst), 0);
    let w3 = l.streamHeader.writePos.0[0].load(SeqCst);
    audio.process(
        [&voc, &voc],
        [&mus, &mus],
        [&mut hl, &mut hr],
        256,
        false,
        true,
    );
    assert!((hl[0] - 0.1).abs() < 1e-6);
    assert_eq!(
        l.streamHeader.writePos.0[0].load(SeqCst),
        w3,
        "nothing written to the bus any more"
    );
    let (mut ml, mut mr) = (vec![0.0f32; 4], vec![0.0f32; 4]);
    mix_local(
        [&voc[..4], &voc[..4]],
        [&mus[..4], &mus[..4]],
        [&mut ml, &mut mr],
        4,
        true,
    );
    assert!((ml[0] - 0.3).abs() < 1e-6 && (mr[3] - 0.3).abs() < 1e-6);
}
