//! `MusicPlayer`: the port of `karaoke: MusicPlayer file loading, playlist navigation, transport controls, and RT
//! pullAudio` from `tests/unit/test_karaoke.cpp`, plus resampling, seek, end of track and playlist edits.
//!
//! The decoder runs on its own thread, so where the C++ test pulled straight after `play()`, these tests pull until
//! the audio arrives (a real device callback comes every few ms; the first audio is one decoder wake-up away).
mod common;

use common::{Temp, write_wav};
use hs_karaoke::player::{MusicOutput, MusicPlayer, music_player};
use std::path::Path;
use std::time::{Duration, Instant};

fn sine(rate: u32) -> impl Fn(usize) -> f32 {
    move |i| 0.5 * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / rate as f32).sin()
}

fn peak(x: &[f32]) -> f32 {
    x.iter().fold(0.0f32, |m, v| m.max(v.abs()))
}

/// Pulls 256-frame blocks until `until` holds for the left channel so far (or 3 s pass); returns everything pulled.
fn pull_until(out: &mut MusicOutput, until: impl Fn(&[f32]) -> bool) -> Vec<f32> {
    let (mut l, mut r) = ([0.0f32; 256], [0.0f32; 256]);
    let mut all = Vec::new();
    let t = Instant::now();
    while !until(&all) && t.elapsed() < Duration::from_secs(3) {
        out.pull_audio(Some(&mut l), Some(&mut r), 256);
        assert_eq!(l, r, "stereo file of identical channels");
        all.extend_from_slice(&l);
        std::thread::sleep(Duration::from_millis(3)); // a bit faster than real time (5.3 ms per block)
    }
    all
}

#[test]
fn karaoke_music_player_loading_playlist_transport_and_rt_pull() {
    let wav = Temp::new("song.wav");
    write_wav(&wav.0, 48000, 9600, sine(48000)); // 0.2 s

    let (mut player, mut out): (MusicPlayer, MusicOutput) = music_player(48000.0);
    player.add_track(&wav.0);
    assert_eq!(player.track_count(), 1);
    assert_eq!(player.current_track_index(), Some(0));
    assert!(player.length_seconds() > 0.15);
    assert_eq!(
        player.track_at(0).unwrap().title,
        wav.0.file_stem().unwrap().to_string_lossy()
    );

    // stopped: silence
    assert!(!player.is_playing());
    let (mut l, mut r) = ([1.0f32; 256], [1.0f32; 256]);
    out.pull_audio(Some(&mut l), Some(&mut r), 256);
    assert_eq!(peak(&l), 0.0);

    player.play();
    assert!(player.is_playing());
    let got = pull_until(&mut out, |a| peak(a) > 0.1 && a.len() >= 2560);
    assert!(peak(&got) > 0.1);
    assert!(player.current_position_sec() > 0.0);

    player.pause();
    assert!(!player.is_playing());
    player.stop();
    assert!(!player.is_playing());
    assert_eq!(player.current_position_sec(), 0.0);

    player.set_volume(0.5);
    assert!((player.volume() - 0.5).abs() < 0.01);

    player.clear_playlist();
    assert_eq!(player.track_count(), 0);
    assert_eq!(player.current_track_index(), None);
    assert_eq!(player.length_seconds(), 0.0);
}

#[test]
fn plays_the_file_sample_for_sample_then_finishes_and_moves_on() {
    let wav = Temp::new("ramp.wav");
    let ramp = |i: usize| (i % 1000) as f32 / 2000.0;
    write_wav(&wav.0, 48000, 4800, ramp);
    let (mut player, mut out) = music_player(48000.0);
    player.add_track(&wav.0);
    player.set_volume(0.5);
    player.play();
    let got = pull_until(&mut out, |a| a.len() > 4800 && !player.is_playing());
    let start = got
        .iter()
        .position(|v| *v != 0.0)
        .unwrap_or(0)
        .saturating_sub(1);
    for i in 1..4800 {
        let want = ((ramp(i).clamp(-1.0, 1.0) * 32767.0) as i16) as f32 / 32768.0 * 0.5;
        assert!(
            (got[start + i] - want).abs() < 1e-6,
            "frame {i}: {} vs {want}",
            got[start + i]
        );
    }
    // the end: stopped, and `poll` moves on to the next track (the only one: it starts again, like the C++)
    assert!(!player.is_playing());
    assert!(player.poll());
    assert!(player.is_playing());
    assert_eq!(player.current_track_index(), Some(0));
    assert!(!player.poll());
}

#[test]
fn a_44k1_file_is_resampled_to_the_device_rate() {
    let wav = Temp::new("44k.wav");
    write_wav(&wav.0, 44100, 44100, sine(44100)); // 1 s of 440 Hz
    let (mut player, mut out) = music_player(48000.0);
    player.add_track(&wav.0);
    assert!((player.length_seconds() - 1.0).abs() < 1e-9);
    player.play();
    let got = pull_until(&mut out, |a| a.len() >= 48000 + 4096);
    let sounding = got.iter().filter(|v| v.abs() > 1e-4).count();
    assert!(
        (sounding as i64 - 48000).abs() < 600,
        "{sounding} frames of audio at 48 kHz"
    );
    // still 440 Hz: count rising zero crossings in the middle 0.5 s
    let first = got.iter().position(|v| v.abs() > 1e-4).unwrap();
    let mid = &got[first + 12000..first + 36000];
    let crossings = mid.windows(2).filter(|w| w[0] < 0.0 && w[1] >= 0.0).count();
    assert!(
        (crossings as i64 - 220).abs() <= 2,
        "{crossings} cycles in 0.5 s"
    );
}

#[test]
fn seek_drops_the_old_audio() {
    let wav = Temp::new("steps.wav");
    // 0..1 s at 0.1, 1..2 s at 0.4 (DC steps make the position easy to read)
    write_wav(&wav.0, 48000, 96000, |i| if i < 48000 { 0.1 } else { 0.4 });
    let (mut player, mut out) = music_player(48000.0);
    player.add_track(&wav.0);
    player.play();
    let got = pull_until(&mut out, |a| a.iter().any(|v| *v > 0.05));
    assert!(got.iter().all(|v| *v < 0.2));

    player.seek_to(1.5);
    assert_eq!(player.current_position_sec(), 1.5);
    let after = pull_until(&mut out, |a| a.iter().any(|v| *v > 0.05));
    let first = after.iter().position(|v| *v > 0.05).unwrap();
    assert!(
        after[first..].iter().all(|v| (*v - 0.4).abs() < 0.01),
        "nothing from before the seek"
    );
    let pos = player.current_position_sec();
    assert!((1.5..1.6).contains(&pos), "{pos}");
}

#[test]
fn playlist_edits_follow_the_current_track() {
    let a = Temp::new("a.wav");
    let b = Temp::new("b.wav");
    let c = Temp::new("c.wav");
    write_wav(&a.0, 48000, 4800, |_| 0.0);
    write_wav(&b.0, 48000, 9600, |_| 0.0);
    write_wav(&c.0, 48000, 14400, |_| 0.0);
    let (mut player, _out) = music_player(48000.0);
    player.add_track(Path::new("does-not-exist.wav"));
    assert_eq!(player.track_count(), 0);
    for t in [&a, &b, &c] {
        player.add_track(&t.0);
    }
    assert_eq!(player.current_track_index(), Some(0));
    player.prev_track(); // wraps
    assert_eq!(player.current_track_index(), Some(2));
    assert!((player.length_seconds() - 0.3).abs() < 1e-9);
    player.remove_track(0);
    assert_eq!(player.current_track_index(), Some(1));
    player.remove_track(1); // the current one
    assert_eq!(player.current_track_index(), None);
    assert_eq!(player.length_seconds(), 0.0);
    assert!(!player.is_playing());
    player.play(); // nothing selected: takes the first
    assert_eq!(player.current_track_index(), Some(0));
    assert!(player.is_playing());
    player.select_track(7);
    assert_eq!(player.current_track_index(), Some(0));
}

#[test]
fn seeking_past_the_end_ends_the_track() {
    // the reader refuses the seek and stays put: the C++ gave silence and moved on, so must this
    let wav = Temp::new("short.wav");
    write_wav(&wav.0, 48000, 48000, |_| 0.3);
    let (mut player, mut out) = music_player(48000.0);
    player.add_track(&wav.0);
    player.play();
    pull_until(&mut out, |a| a.iter().any(|v| *v > 0.1));
    player.seek_to(5.0);
    let after = pull_until(&mut out, |_| !player.is_playing());
    assert!(!player.is_playing());
    assert!(
        after.iter().all(|v| *v == 0.0),
        "no audio from before the seek"
    );
    assert!(player.poll(), "moves on like the C++");
}
