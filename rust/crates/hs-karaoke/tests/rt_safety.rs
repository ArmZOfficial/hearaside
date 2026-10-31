//! Real-time safety of the audio-thread calls (PLAN section 8): `VocalChain::process`, `PreflightAudio::process` and
//! `MusicOutput::pull_audio` make no heap allocation once prepared. Same counting-allocator pattern as hs-engine's and
//! hs-win's `tests/rt_safety.rs` (in `common`). `RoomAudio::process` is checked in `room.rs`, which owns the environment.
mod common;

use common::{Temp, allocations, write_wav};
use hs_karaoke::player::music_player;
use hs_karaoke::preflight::preflight;
use hs_karaoke::vocal::VocalChain;
use std::time::{Duration, Instant};

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

#[test]
fn join_audio_does_not_allocate() {
    let (_client, mut audio) = hs_karaoke::join::join_client(48000.0, 256);
    let mic = vec![0.1f32; 256];
    let (mut l, mut r) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let n = allocations(|| {
        for i in 0..200 {
            audio.push_upstream(&mic, 256, i * 256);
            audio.pull_downstream(&mut l, &mut r, 256);
        }
    });
    assert_eq!(n, 0);
}
