//! The backing-track player: a playlist of audio files, transport, volume, and the audio-thread pull.
//! Port of the `AudioFile` mode of `apps/karaoke/MusicPlayer.{h,cpp}`.
//!
//! The C++ ran `juce::AudioTransportSource` without a read-ahead thread, so every audio callback read and decoded the
//! file and took the transport's `CriticalSection` (disk I/O and a lock on the audio thread, against Hard rule 1).
//! Here a decoder thread (symphonia, then `hs_dsp`'s Speex resampler to the device rate) fills a lock-free ring and the
//! audio thread only copies out of it. Transport changes (load, seek, stop) carry a generation number: the audio thread
//! drops what the ring holds once the decoder has confirmed it stopped writing the old position, so no old audio plays
//! after a seek. End of a track is noticed by `poll()` on the UI timer (the C++ change listener), which moves on to the
//! next track like the C++ (the playlist wraps).
//!
//! The device rate is fixed per [`music_player`] pair (the C++ could `prepare` again and keep its playlist): on a rate
//! change the owner makes a new pair and adds the tracks again (R5.4 decides whether a rate command is worth it).
//!
//! Not ported here: the `AppCapture` source mode (WASAPI process loopback; its Rust port belongs with the App Audio
//! capture code in `hs-win`, R7) and the `onTrackChanged` / `onPlaybackStateChanged` callbacks (the UI reads the state
//! on its timer, R5.5).
use crate::AtomicF32;
use hs_dsp::resampler::VarResampler;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering::*};
use std::sync::mpsc::{Receiver, Sender, channel};
use std::thread::JoinHandle;
use std::time::Duration;
use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::{CODEC_TYPE_NULL, Decoder, DecoderOptions};
use symphonia::core::errors::Error;
use symphonia::core::formats::{FormatOptions, FormatReader, SeekMode, SeekTo};
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;
use symphonia::core::units::Time;

#[derive(Debug, Clone, PartialEq)]
pub struct TrackInfo {
    pub path: PathBuf,
    pub title: String,
    pub duration_sec: f64,
}

/// ~0.68 s at 48 kHz: the decoder thread is never more than that ahead
const RING_FRAMES: u64 = 1 << 15;
const NO_GEN: u32 = u32::MAX;

struct Shared {
    /// interleaved L R as f32 bits
    ring: Box<[AtomicU32]>,
    /// frames; written by the decoder only
    write: AtomicU64,
    /// frames; written by the audio thread only
    read: AtomicU64,
    playing: AtomicBool,
    volume: AtomicF32,
    /// UI: bumped by every load / seek / stop
    generation: AtomicU32,
    /// f64 bits: where the newest generation starts, in seconds
    target: AtomicU64,
    /// decoder: the generation whose command it has applied (all audio of older ones is in the ring)
    ack: AtomicU32,
    /// audio: the generation whose flush it has done
    seen: AtomicU32,
    /// audio: frames played since that flush, and the position it started at (f64 bits)
    played: AtomicU64,
    start: AtomicU64,
    /// decoder: the generation whose stream ended
    ended: AtomicU32,
    /// audio: played the end of the stream; `poll` moves on
    finished: AtomicBool,
    quit: AtomicBool,
}

enum Cmd {
    Open(u32, Option<PathBuf>),
    Seek(u32, f64),
}

/// Makes the UI half and the audio-thread half for one device rate (starts the decoder thread).
pub fn music_player(sample_rate: f64) -> (MusicPlayer, MusicOutput) {
    let sr = if sample_rate > 0.0 {
        sample_rate
    } else {
        48000.0
    };
    let shared = Arc::new(Shared {
        ring: (0..RING_FRAMES * 2).map(|_| AtomicU32::new(0)).collect(),
        write: AtomicU64::new(0),
        read: AtomicU64::new(0),
        playing: AtomicBool::new(false),
        volume: AtomicF32::new(1.0),
        generation: AtomicU32::new(0),
        target: AtomicU64::new(0.0f64.to_bits()),
        ack: AtomicU32::new(0),
        seen: AtomicU32::new(0),
        played: AtomicU64::new(0),
        start: AtomicU64::new(0.0f64.to_bits()),
        ended: AtomicU32::new(NO_GEN),
        finished: AtomicBool::new(false),
        quit: AtomicBool::new(false),
    });
    let (tx, rx) = channel();
    let worker = {
        let shared = shared.clone();
        std::thread::Builder::new()
            .name("HAS Karaoke music decoder".into())
            .spawn(move || decode_loop(&shared, &rx, sr as u32))
            .expect("spawn the music decoder thread")
    };
    (
        MusicPlayer {
            shared: shared.clone(),
            tx,
            worker: Some(worker),
            sample_rate: sr,
            playlist: Vec::new(),
            current: None,
            loaded: false,
            length_sec: 0.0,
        },
        MusicOutput { shared, seen: 0 },
    )
}

/// The UI half: playlist and transport.
pub struct MusicPlayer {
    shared: Arc<Shared>,
    tx: Sender<Cmd>,
    worker: Option<JoinHandle<()>>,
    sample_rate: f64,
    playlist: Vec<TrackInfo>,
    current: Option<usize>,
    loaded: bool,
    length_sec: f64,
}

impl MusicPlayer {
    /// Opens and probes `path` on this thread (like the C++); unreadable files are added with length 0.
    pub fn add_track(&mut self, path: &Path) {
        if !path.is_file() {
            return;
        }
        self.playlist.push(TrackInfo {
            path: path.to_path_buf(),
            title: path
                .file_stem()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default(),
            duration_sec: probe_length(path).unwrap_or(0.0),
        });
        if self.playlist.len() == 1 {
            self.select_track(0);
        }
    }

    pub fn remove_track(&mut self, index: usize) {
        if index >= self.playlist.len() {
            return;
        }
        self.playlist.remove(index);
        match self.current {
            Some(c) if c == index => {
                // the C++ kept the removed track's length on show
                self.unload();
                self.current = None;
            }
            Some(c) if c > index => self.current = Some(c - 1),
            _ => {}
        }
    }

    pub fn clear_playlist(&mut self) {
        self.stop();
        self.unload();
        self.playlist.clear();
        self.current = None;
    }

    pub fn track_count(&self) -> usize {
        self.playlist.len()
    }

    pub fn track_at(&self, index: usize) -> Option<&TrackInfo> {
        self.playlist.get(index)
    }

    pub fn current_track_index(&self) -> Option<usize> {
        self.current
    }

    /// Stops and loads track `index` at 0 s; an index out of range changes nothing.
    pub fn select_track(&mut self, index: usize) {
        let Some(t) = self.playlist.get(index) else {
            return;
        };
        let path = t.path.clone();
        self.current = Some(index);
        self.shared.playing.store(false, Release);
        let length = probe_length(&path);
        self.loaded = length.is_some();
        self.length_sec = length.unwrap_or(0.0);
        let g = self.bump(0.0);
        self.send(Cmd::Open(g, Some(path)));
    }

    pub fn next_track(&mut self) {
        let n = self.playlist.len();
        if n == 0 {
            return;
        }
        self.select_track(self.current.map_or(0, |c| (c + 1) % n));
        self.play();
    }

    pub fn prev_track(&mut self) {
        let n = self.playlist.len();
        if n == 0 {
            return;
        }
        self.select_track(self.current.map_or(n - 1, |c| (c + n - 1) % n));
        self.play();
    }

    pub fn play(&mut self) {
        if self.current.is_none() && !self.playlist.is_empty() {
            self.select_track(0);
        }
        // like `AudioTransportSource::start`, nothing to play = stays stopped
        self.shared.playing.store(self.loaded, Release);
    }

    pub fn pause(&mut self) {
        self.shared.playing.store(false, Release);
    }

    pub fn stop(&mut self) {
        self.shared.playing.store(false, Release);
        self.seek_to(0.0);
    }

    pub fn is_playing(&self) -> bool {
        self.shared.playing.load(Acquire)
    }

    /// Where the audio thread is; right after a seek, where it is going.
    pub fn current_position_sec(&self) -> f64 {
        let s = &*self.shared;
        let g = s.generation.load(Acquire);
        if s.seen.load(Acquire) != g {
            return f64::from_bits(s.target.load(Acquire));
        }
        f64::from_bits(s.start.load(Acquire)) + s.played.load(Acquire) as f64 / self.sample_rate
    }

    pub fn length_seconds(&self) -> f64 {
        self.length_sec
    }

    pub fn seek_to(&mut self, seconds: f64) {
        let at = seconds.max(0.0);
        let g = self.bump(at);
        self.send(Cmd::Seek(g, at));
    }

    pub fn set_volume(&self, linear: f32) {
        self.shared.volume.store(linear);
    }

    pub fn volume(&self) -> f32 {
        self.shared.volume.load()
    }

    /// Call from the UI timer: after the end of a track, moves on to the next one. True when it did.
    pub fn poll(&mut self) -> bool {
        if !self.shared.finished.swap(false, AcqRel) {
            return false;
        }
        self.next_track();
        true
    }

    fn unload(&mut self) {
        self.shared.playing.store(false, Release);
        (self.loaded, self.length_sec) = (false, 0.0);
        let g = self.bump(0.0);
        self.send(Cmd::Open(g, None));
    }

    fn bump(&self, at: f64) -> u32 {
        let s = &*self.shared;
        s.target.store(at.to_bits(), Release);
        s.finished.store(false, Release);
        s.generation.fetch_add(1, AcqRel).wrapping_add(1)
    }

    fn send(&self, cmd: Cmd) {
        let _ = self.tx.send(cmd);
        if let Some(w) = &self.worker {
            w.thread().unpark();
        }
    }
}

impl Drop for MusicPlayer {
    fn drop(&mut self) {
        self.shared.quit.store(true, Release);
        if let Some(w) = self.worker.take() {
            w.thread().unpark();
            let _ = w.join();
        }
    }
}

/// The audio-thread half.
pub struct MusicOutput {
    shared: Arc<Shared>,
    seen: u32,
}

impl MusicOutput {
    /// Audio thread: no allocation, no lock, no syscall. Fills `n` frames (silence when stopped, between tracks or
    /// when the decoder is behind). A slice shorter than `n` cuts `n` (caller bug, debug assert).
    pub fn pull_audio(
        &mut self,
        mut out_l: Option<&mut [f32]>,
        mut out_r: Option<&mut [f32]>,
        n: usize,
    ) {
        let shortest = [out_l.as_deref(), out_r.as_deref()]
            .into_iter()
            .flatten()
            .map(<[f32]>::len)
            .min()
            .unwrap_or(n);
        debug_assert!(
            shortest >= n,
            "MusicOutput::pull_audio: slice shorter than n"
        );
        let n = n.min(shortest);
        let s = &*self.shared;

        let g = s.generation.load(Acquire);
        if g != self.seen && s.ack.load(Acquire) == g {
            // the decoder wrote nothing after this generation's command: everything in the ring is old
            s.read.store(s.write.load(Acquire), Release);
            s.start.store(s.target.load(Acquire), Release);
            s.played.store(0, Release);
            self.seen = g;
            s.seen.store(g, Release);
        }

        let mut got = 0;
        if self.seen == g && s.playing.load(Acquire) {
            let w = s.write.load(Acquire);
            let r = s.read.load(Relaxed);
            got = (w - r).min(n as u64) as usize;
            let vol = s.volume.load();
            for i in 0..got {
                let at = (((r + i as u64) % RING_FRAMES) * 2) as usize;
                if let Some(l) = out_l.as_deref_mut() {
                    l[i] = f32::from_bits(s.ring[at].load(Relaxed)) * vol;
                }
                if let Some(rr) = out_r.as_deref_mut() {
                    rr[i] = f32::from_bits(s.ring[at + 1].load(Relaxed)) * vol;
                }
            }
            s.read.store(r + got as u64, Release);
            s.played.fetch_add(got as u64, AcqRel);
            // `write` again: the last packet may have landed after `w` was read; `generation` again: a seek or
            // select made during this block must not be cancelled by the old track's end
            if got < n
                && s.ended.load(Acquire) == g
                && s.write.load(Acquire) == r + got as u64
                && s.generation.load(Acquire) == g
            {
                s.playing.store(false, Release);
                s.finished.store(true, Release);
            }
        }
        for o in [out_l, out_r].into_iter().flatten() {
            o[got..n].fill(0.0);
        }
    }
}

// ---- decoder thread ----

fn decode_loop(s: &Shared, rx: &Receiver<Cmd>, out_rate: u32) {
    let mut src: Option<Source> = None;
    let mut generation = 0u32;
    let mut pending: Vec<f32> = Vec::new(); // interleaved, not yet in the ring
    let mut done = 0; // frames of `pending` already written
    let mut at_end = false;
    loop {
        if s.quit.load(Acquire) {
            return;
        }
        let mut got_cmd = false;
        while let Ok(cmd) = rx.try_recv() {
            // a file that probed but will not decode, or a seek past the end: the track ends at once, so `poll`
            // moves on instead of showing "playing" over silence (the C++ never started such a track)
            let failed = match cmd {
                Cmd::Open(g, path) => {
                    generation = g;
                    let wanted = path.is_some();
                    src = path.and_then(|p| Source::open(&p, out_rate));
                    wanted && src.is_none()
                }
                Cmd::Seek(g, at) => {
                    generation = g;
                    src.as_mut().is_some_and(|src| !src.seek(at))
                }
            };
            (pending, done, at_end) = (Vec::new(), 0, failed);
            if failed {
                s.ended.store(generation, Release);
            }
            got_cmd = true;
        }
        if got_cmd {
            s.ack.store(generation, Release);
        }

        // write only once the audio thread has dropped the old audio, and while no newer command is on its way
        let current =
            s.seen.load(Acquire) == generation && s.generation.load(Acquire) == generation;
        let mut wrote = false;
        if current && let Some(source) = &mut src {
            if done * 2 == pending.len() && !at_end {
                pending.clear();
                done = 0;
                if !source.next(&mut pending) {
                    at_end = true;
                    s.ended.store(generation, Release);
                }
            }
            let w = s.write.load(Relaxed);
            let free = RING_FRAMES - (w - s.read.load(Acquire));
            let k = (free as usize).min(pending.len() / 2 - done);
            for i in 0..k {
                let at = (((w + i as u64) % RING_FRAMES) * 2) as usize;
                s.ring[at].store(pending[(done + i) * 2].to_bits(), Relaxed);
                s.ring[at + 1].store(pending[(done + i) * 2 + 1].to_bits(), Relaxed);
            }
            s.write.store(w + k as u64, Release);
            done += k;
            wrote = k > 0 || (done * 2 == pending.len() && !at_end);
        }
        if src.is_none() {
            std::thread::park(); // nothing to decode until a command, which unparks
        } else if !wrote {
            std::thread::park_timeout(Duration::from_millis(5));
        }
    }
}

struct Source {
    format: Box<dyn FormatReader>,
    decoder: Box<dyn Decoder>,
    track: u32,
    in_rate: u32,
    resampler: Option<VarResampler>,
    /// frames to drop after a seek that landed before the requested time
    skip: u64,
}

fn open_format(path: &Path) -> Option<(Box<dyn FormatReader>, u32)> {
    let file = File::open(path).ok()?;
    let mss = MediaSourceStream::new(Box::new(file), Default::default());
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }
    let probed = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .ok()?;
    let track = probed
        .format
        .tracks()
        .iter()
        .find(|t| t.codec_params.codec != CODEC_TYPE_NULL)?
        .id;
    Some((probed.format, track))
}

/// Length in seconds, `None` when the file cannot be read.
/// shortcut: 0 when the container does not state its frame count (MP3 without a Xing/LAME header), scan it if that shows.
fn probe_length(path: &Path) -> Option<f64> {
    let (format, track) = open_format(path)?;
    let p = &format.tracks().iter().find(|t| t.id == track)?.codec_params;
    let rate = f64::from(p.sample_rate?).max(1.0);
    Some(p.n_frames.unwrap_or(0) as f64 / rate)
}

impl Source {
    fn open(path: &Path, out_rate: u32) -> Option<Self> {
        let (format, track) = open_format(path)?;
        let params = &format.tracks().iter().find(|t| t.id == track)?.codec_params;
        let in_rate = params.sample_rate?;
        let decoder = symphonia::default::get_codecs()
            .make(params, &DecoderOptions::default())
            .ok()?;
        let resampler = if in_rate == out_rate {
            None
        } else {
            Some(VarResampler::new(in_rate, out_rate, 5).ok()?)
        };
        Some(Self {
            format,
            decoder,
            track,
            in_rate,
            resampler,
            skip: 0,
        })
    }

    /// False when the format refuses the seek (past the end): the reader is still at the old position.
    fn seek(&mut self, seconds: f64) -> bool {
        let to = SeekTo::Time {
            time: Time::from(seconds),
            track_id: Some(self.track),
        };
        let Ok(s) = self.format.seek(SeekMode::Accurate, to) else {
            return false;
        };
        self.skip = s.required_ts.saturating_sub(s.actual_ts);
        self.decoder.reset();
        if let Some(r) = &mut self.resampler {
            r.reset();
        }
        true
    }

    /// Appends the next packet as interleaved stereo at the device rate (mono is doubled, channels past 2 dropped,
    /// like JUCE's reader). False at the end of the stream or on an error that ends it.
    fn next(&mut self, out: &mut Vec<f32>) -> bool {
        let (mut l, mut r) = (Vec::new(), Vec::new());
        while l.is_empty() {
            let packet = match self.format.next_packet() {
                Ok(p) => p,
                Err(_) => return false, // end of stream, or a reset / IO error that ends it
            };
            if packet.track_id() != self.track {
                continue;
            }
            let decoded = match self.decoder.decode(&packet) {
                Ok(d) => d,
                Err(Error::DecodeError(_)) => continue, // a damaged packet: skip it
                Err(_) => return false,
            };
            let spec = *decoded.spec();
            let ch = spec.channels.count().max(1);
            let mut buf = SampleBuffer::<f32>::new(decoded.capacity() as u64, spec);
            buf.copy_interleaved_ref(decoded);
            let frames = buf.samples().chunks_exact(ch);
            let skip = self.skip.min(frames.len() as u64) as usize;
            self.skip -= skip as u64;
            for f in frames.skip(skip) {
                l.push(f[0]);
                r.push(f[ch.min(2) - 1]);
            }
        }
        match &mut self.resampler {
            None => out.extend(l.iter().zip(&r).flat_map(|(a, b)| [*a, *b])),
            Some(rs) => {
                let cap = (l.len() as u64 * u64::from(rs.out_rate()) / u64::from(self.in_rate))
                    as usize
                    + 64;
                let (mut ol, mut or) = (vec![0.0f32; cap], vec![0.0f32; cap]);
                let mut used = 0;
                while used < l.len() {
                    let mut in_frames = (l.len() - used) as u32;
                    let mut out_frames = cap as u32;
                    rs.process(
                        [&l[used..], &r[used..]],
                        &mut in_frames,
                        [&mut ol, &mut or],
                        &mut out_frames,
                    );
                    let made = out_frames as usize;
                    out.extend(
                        ol[..made]
                            .iter()
                            .zip(&or[..made])
                            .flat_map(|(a, b)| [*a, *b]),
                    );
                    if in_frames == 0 && made == 0 {
                        break; // cannot happen with room for the whole packet; never spin
                    }
                    used += in_frames as usize;
                }
            }
        }
        true
    }
}
