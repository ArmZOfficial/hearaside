//! Hosting a room: the singer's own HAS Karaoke acts as a small Hub. It owns a bus (so OBS hears the Stream Mix),
//! runs the share server (listen link, eight friend links) and mixes the friends into the singer's headphones.
//! Port of `apps/karaoke/RoomHost.{h,cpp}`.
//!
//! [`RoomHost::start`] makes two halves over one room: [`RoomHost`] for the UI (links, slots, recording; dropping it
//! closes the room) and [`RoomAudio`] for the audio callback. When no room runs, the callback uses [`mix_local`].
//! `RoomAudio` keeps the bus mapped, so drop it off the audio thread (its last `Arc` unmaps the segment).
//!
//! C++ bugs this fixes:
//! 1. what friends hear of each other (`friendVoiceAudio`) got the same buffer for every pair: the last friend pulled
//!    (or stale audio), so friends heard the wrong friend and could hear themselves. Here friend `j` is written mono
//!    to pair `j / 2`, channel `j % 2`, like the Hub (`hs-engine` friends.rs);
//! 2. a block longer than `prepare`'s size wrote past the scratch buffers; here it runs in pieces;
//! 3. `stopRoom` unmapped the bus while the audio thread could still be writing to it; here the audio half keeps it
//!    mapped and only stops writing;
//! 4. `startRoom` zeroed the whole shared bus, wiping the slots of Track plug-ins already on it; here only the
//!    header fields and friend slots the room uses are set;
//! 5. `kickSlot` reset the friend's jitter buffer from the UI thread while the audio thread used it; here the audio
//!    thread resets it when asked;
//! 6. stopping a recording synchronised with the audio thread through relaxed atomics (the writer could be freed
//!    mid-write); here both sides use sequentially consistent stores and loads.
use hs_bus::bus::{
    beacon_publish, bits_float, float_bits, hub_alive, random_token, release_hub, set_friend_name,
    try_claim_hub,
};
use hs_bus::protocol::{kMaxFriends, kProtocolVersion};
use hs_bus::ring::ring_write;
use hs_bus::shm::{BeaconMap, SharedMemory, now_ns};
use hs_engine::friend_reader::FriendReader;
use hs_net::share::{FriendLink, ShareServer};
use std::fs::File;
use std::io::{Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering::*};
use std::thread::JoinHandle;
use std::time::Duration;

/// A Hub that has not beaten for this long is gone (the C++ 3 s).
const STALE_NS: u64 = 3_000_000_000;

/// One friend slot as the UI shows it.
#[derive(Debug, Clone, PartialEq)]
pub struct HostFriendSlot {
    pub slot: usize,
    pub id: u32,
    pub name: String,
    pub token: String,
    pub is_live: bool,
    pub is_muted: bool,
    pub peak: f32,
    pub delay_ms: f32,
}

/// No room: vocal (if heard) plus music into the headphones.
pub fn mix_local(
    vocal: [&[f32]; 2],
    music: [&[f32]; 2],
    headphones: [&mut [f32]; 2],
    n: usize,
    vocal_to_headphones: bool,
) {
    let v = if vocal_to_headphones { 1.0 } else { 0.0 };
    for (c, hp) in headphones.into_iter().enumerate() {
        let n = n.min(hp.len()).min(vocal[c].len()).min(music[c].len());
        for i in 0..n {
            hp[i] = vocal[c][i] * v + music[c][i];
        }
    }
}

/// What the UI and the audio thread share.
struct Room {
    bus: Arc<SharedMemory>,
    running: AtomicBool,
    muted: [AtomicBool; kMaxFriends],
    /// the UI asks the audio thread to reset a friend's jitter buffer (kick)
    reset: [AtomicBool; kMaxFriends],
    rec: Recording,
}

/// Recording the headphone mix: the audio thread fills a ring, a writer thread empties it into a WAV file.
struct Recording {
    /// interleaved L R, f32 bits; 4 s like the C++ ThreadedWriter
    ring: Box<[AtomicU32]>,
    write: AtomicU64,
    read: AtomicU64,
    on: AtomicBool,
    in_use: AtomicBool,
    recorded: AtomicU64,
    /// frames the writer could not keep up with
    dropped: AtomicU64,
}

pub struct RoomHost {
    room: Arc<Room>,
    server: ShareServer,
    sample_rate: f64,
    hub_token: u64,
    _beacon: Option<BeaconMap>,
    name: String,
    friend_tokens: [String; kMaxFriends],
    friend_names: [String; kMaxFriends],
    daw_collision: bool,
    recorder: Option<JoinHandle<()>>,
}

impl RoomHost {
    /// Opens the bus (`Main`, or `Karaoke` when a DAW's Hub already owns `Main`), claims it and starts the share
    /// server. `None` when the bus cannot be opened or claimed, or the server does not start.
    pub fn start(room_name: &str, sample_rate: f64, max_block: usize) -> Option<(Self, RoomAudio)> {
        let sr = if sample_rate > 0.0 {
            sample_rate
        } else {
            48000.0
        };
        let sr_u = (sr + 0.5) as u32;
        let name = if room_name.is_empty() {
            "Karaoke Room".to_owned()
        } else {
            room_name.to_owned()
        };

        let mut bus_name = hs_app::settings::default_bus_name();
        let daw_collision =
            SharedMemory::open_existing(&bus_name).is_ok_and(|s| hub_alive(s.layout(), STALE_NS));
        if daw_collision {
            bus_name = "Karaoke".into();
        }
        let bus = Arc::new(SharedMemory::open(&bus_name).ok()?);
        let hub_token = random_token();
        let l = bus.layout();
        if !try_claim_hub(l, hub_token, STALE_NS) {
            return None; // another HAS Karaoke already hosts on this bus
        }
        debug_assert_eq!(l.header.version, kProtocolVersion);
        l.header.hubSampleRate.store(sr_u, Relaxed);
        l.header.hubHeartbeatNs.store(now_ns(), Relaxed);
        l.header.hubFlags.store(0, Relaxed); // a crashed Hub's Panic / Bypassed / Offline flags (C++: the memset)
        l.streamHeader.sampleRate.store(sr_u, Relaxed);
        l.streamHeader.active.store(1, Release);
        // the share server reads each friend's voice at the mix's positions: start them together
        let mix_at = l.friendMixWrite.0.load(Acquire);
        for w in &l.friendVoiceWrite.0 {
            w.store(mix_at, Release);
        }

        let beacon = BeaconMap::open(&bus_name);
        if let Some(b) = &beacon {
            beacon_publish(b.header());
        }

        let friend_tokens: [String; kMaxFriends] =
            std::array::from_fn(|_| ShareServer::new_token());
        let friend_names: [String; kMaxFriends] =
            std::array::from_fn(|i| format!("Friend {}", i + 1));
        for (i, f) in l.friends.0.iter().enumerate() {
            f.id.store(i as u32 + 1, Relaxed);
            set_friend_name(f, &friend_names[i]);
            f.sampleRate.store(sr_u, Relaxed);
            f.writePos.0.store(0, Relaxed);
            f.heartbeatNs.store(0, Relaxed);
            f.peakBits.store(0, Relaxed);
            f.delayBits.store(float_bits(-1.0), Relaxed); // not measured yet
        }

        let server = ShareServer::new();
        server.set_friend_links(
            (0..kMaxFriends)
                .map(|i| FriendLink {
                    slot: i as i32,
                    id: i as u32 + 1,
                    token: friend_tokens[i].clone(),
                    name: friend_names[i].clone(),
                    host: name.clone(),
                })
                .collect(),
        );
        if !server.start(
            bus.clone(),
            &ShareServer::new_token(),
            &ShareServer::new_token(),
        ) {
            release_hub(l, hub_token);
            return None;
        }

        let room = Arc::new(Room {
            bus,
            running: AtomicBool::new(true),
            muted: Default::default(),
            reset: Default::default(),
            rec: Recording {
                ring: (0..(sr as usize * 4).max(1) * 2)
                    .map(|_| AtomicU32::new(0))
                    .collect(),
                write: AtomicU64::new(0),
                read: AtomicU64::new(0),
                on: AtomicBool::new(false),
                in_use: AtomicBool::new(false),
                recorded: AtomicU64::new(0),
                dropped: AtomicU64::new(0),
            },
        });
        let block = max_block.max(256);
        let audio = RoomAudio {
            room: room.clone(),
            readers: std::array::from_fn(|_| {
                let mut r = FriendReader::new();
                r.prepare(sr, block);
                r
            }),
            block,
            stream: [vec![0.0; block], vec![0.0; block]],
            friend: [vec![0.0; block], vec![0.0; block]],
            voice: std::array::from_fn(|_| vec![0.0; block]),
        };
        let host = Self {
            room,
            server,
            sample_rate: sr,
            hub_token,
            _beacon: beacon,
            name,
            friend_tokens,
            friend_names,
            daw_collision,
            recorder: None,
        };
        Some((host, audio))
    }

    pub fn room_name(&self) -> &str {
        &self.name
    }

    pub fn listen_url(&self) -> String {
        self.server.listen_url()
    }

    pub fn friend_url(&self, slot: usize) -> String {
        self.friend_tokens
            .get(slot)
            .map(|t| self.server.friend_url(t))
            .unwrap_or_default()
    }

    /// `hearaside://join?url=<friend link>` (opens HAS Karaoke on the friend's PC; the installer registers it).
    pub fn friend_deep_link(&self, slot: usize) -> String {
        let url = self.friend_url(slot);
        if url.is_empty() {
            return String::new();
        }
        format!("hearaside://join?url={}", escape_param(&url))
    }

    pub fn server_port(&self) -> u16 {
        self.server.port()
    }

    pub fn bus_name(&self) -> &str {
        self.room.bus.bus_name()
    }

    /// A DAW's Hub already owned `Main`, so the room went to the `Karaoke` bus (point OBS at it).
    pub fn has_daw_collision(&self) -> bool {
        self.daw_collision
    }

    pub fn friend_count(&self) -> usize {
        kMaxFriends
    }

    pub fn slot_info(&self, slot: usize) -> Option<HostFriendSlot> {
        let f = self.room.bus.layout().friends.0.get(slot)?;
        let hb = f.heartbeatNs.load(Relaxed);
        let now = now_ns();
        Some(HostFriendSlot {
            slot,
            id: slot as u32 + 1,
            name: self.friend_names[slot].clone(),
            token: self.friend_tokens[slot].clone(),
            is_live: hb > 0 && now >= hb && now - hb < STALE_NS,
            is_muted: self.room.muted[slot].load(Relaxed),
            peak: bits_float(f.peakBits.load(Relaxed)),
            delay_ms: bits_float(f.delayBits.load(Relaxed)),
        })
    }

    pub fn set_slot_muted(&self, slot: usize, muted: bool) {
        if let Some(m) = self.room.muted.get(slot) {
            m.store(muted, Relaxed);
        }
    }

    pub fn kick_slot(&self, slot: usize) {
        let Some(f) = self.room.bus.layout().friends.0.get(slot) else {
            return;
        };
        self.server.kick_friend(slot as i32);
        self.room.reset[slot].store(true, Release);
        f.heartbeatNs.store(0, Relaxed);
        f.peakBits.store(0, Relaxed);
    }

    /// Records the headphone mix as 32-bit float stereo WAV. Allocates and spawns a thread: UI thread.
    pub fn start_recording(&mut self, path: &Path) -> bool {
        self.stop_recording();
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let Ok(mut file) = File::create(path) else {
            return false;
        };
        if file
            .write_all(&wav_header(self.sample_rate as u32, 0))
            .is_err()
        {
            return false;
        }
        let rec = &self.room.rec;
        rec.read.store(rec.write.load(SeqCst), SeqCst);
        rec.recorded.store(0, Relaxed);
        rec.dropped.store(0, Relaxed);
        rec.on.store(true, SeqCst);
        let room = self.room.clone();
        let rate = self.sample_rate as u32;
        self.recorder = std::thread::Builder::new()
            .name("HAS Karaoke recorder".into())
            .spawn(move || write_loop(&room.rec, file, rate))
            .ok();
        if self.recorder.is_none() {
            rec.on.store(false, SeqCst);
        }
        self.recorder.is_some()
    }

    /// Stops after everything the audio thread wrote is in the file.
    pub fn stop_recording(&mut self) {
        let rec = &self.room.rec;
        if !rec.on.swap(false, SeqCst) {
            return;
        }
        while rec.in_use.load(SeqCst) {
            std::thread::yield_now();
        }
        if let Some(t) = self.recorder.take() {
            t.thread().unpark();
            let _ = t.join();
        }
    }

    pub fn is_recording(&self) -> bool {
        self.room.rec.on.load(Relaxed)
    }

    pub fn recorded_seconds(&self) -> f64 {
        self.room.rec.recorded.load(Relaxed) as f64 / self.sample_rate
    }
}

impl Drop for RoomHost {
    fn drop(&mut self) {
        self.stop_recording();
        self.room.running.store(false, SeqCst);
        self.server.stop();
        release_hub(self.room.bus.layout(), self.hub_token);
    }
}

/// The audio-callback half.
pub struct RoomAudio {
    room: Arc<Room>,
    readers: [FriendReader; kMaxFriends],
    block: usize,
    stream: [Vec<f32>; 2],
    friend: [Vec<f32>; 2],
    voice: [Vec<f32>; kMaxFriends],
}

impl RoomAudio {
    /// Audio thread: no allocation, no lock. Headphones = vocal (if heard) + music + the friends who are not muted;
    /// the Stream Mix for OBS and for the friends = vocal (if others hear you) + music. After the room closed it is
    /// [`mix_local`]. A slice shorter than `n` cuts `n` (caller bug, debug assert).
    pub fn process(
        &mut self,
        vocal: [&[f32]; 2],
        music: [&[f32]; 2],
        headphones: [&mut [f32]; 2],
        n: usize,
        vocal_to_headphones: bool,
        vocal_to_stream: bool,
    ) {
        let shortest = vocal
            .iter()
            .chain(&music)
            .map(|s| s.len())
            .chain(headphones.iter().map(|s| s.len()))
            .min()
            .unwrap_or(0);
        debug_assert!(shortest >= n, "RoomAudio::process: slice shorter than n");
        let n = n.min(shortest);
        if !self.room.running.load(Acquire) {
            mix_local(vocal, music, headphones, n, vocal_to_headphones);
            return;
        }
        let [hl, hr] = headphones;
        let mut done = 0;
        while done < n {
            let k = (n - done).min(self.block);
            let r = done..done + k;
            self.run(
                [&vocal[0][r.clone()], &vocal[1][r.clone()]],
                [&music[0][r.clone()], &music[1][r.clone()]],
                [&mut hl[r.clone()], &mut hr[r]],
                vocal_to_headphones,
                vocal_to_stream,
            );
            done += k;
        }
    }

    fn run(
        &mut self,
        vocal: [&[f32]; 2],
        music: [&[f32]; 2],
        hp: [&mut [f32]; 2],
        to_hp: bool,
        to_stream: bool,
    ) {
        let n = vocal[0].len();
        let l = self.room.bus.layout();

        // 1. Stream Mix (OBS + what friends hear of the host) and the start of the headphone mix
        let [hl, hr] = hp;
        let mut pk = [0.0f32; 2];
        for c in 0..2 {
            let h = if c == 0 { &mut *hl } else { &mut *hr };
            for i in 0..n {
                let (v, m) = (vocal[c][i], music[c][i]);
                let s = if to_stream { v } else { 0.0 } + m;
                self.stream[c][i] = s;
                h[i] = if to_hp { v } else { 0.0 } + m;
                pk[c] = pk[c].max(s.abs());
            }
        }
        let stream = [&self.stream[0][..n], &self.stream[1][..n]];
        ring_write(
            &l.streamAudio.0[0],
            &l.streamHeader.writePos.0[0],
            &stream,
            n as u32,
        );
        l.streamHeader.peakBits[0][0].store(float_bits(pk[0]), Relaxed);
        l.streamHeader.peakBits[0][1].store(float_bits(pk[1]), Relaxed);
        ring_write(&l.friendMixAudio.0, &l.friendMixWrite.0, &stream, n as u32);

        // 2. each friend's microphone: into the headphones (unless muted) and into their voice line
        for j in 0..kMaxFriends {
            if self.room.reset[j].swap(false, Acquire) {
                self.readers[j].reset();
            }
            let f = &l.friends.0[j];
            let [fl, fr] = &mut self.friend;
            let pulled = self.readers[j].pull(
                &l.friendAudio.0[j],
                &f.writePos.0,
                f.sampleRate.load(Relaxed),
                [&mut fl[..n], &mut fr[..n]],
                n,
            );
            let v = &mut self.voice[j][..n];
            if !pulled {
                v.fill(0.0);
                continue;
            }
            let mut fpk = 0.0f32;
            for i in 0..n {
                fpk = fpk.max(fl[i].abs()).max(fr[i].abs());
                v[i] = 0.5 * (fl[i] + fr[i]);
            }
            f.peakBits.store(float_bits(fpk), Relaxed);
            if !self.room.muted[j].load(Relaxed) {
                for i in 0..n {
                    hl[i] += fl[i];
                    hr[i] += fr[i];
                }
            }
        }

        // 3. what friends hear of each other, same positions as friendMixAudio (the server leaves out their own)
        for k in 0..kMaxFriends / 2 {
            ring_write(
                &l.friendVoiceAudio.0[k],
                &l.friendVoiceWrite.0[k],
                &[&self.voice[2 * k][..n], &self.voice[2 * k + 1][..n]],
                n as u32,
            );
        }

        // 4. recording
        let rec = &self.room.rec;
        rec.in_use.store(true, SeqCst);
        if rec.on.load(SeqCst) {
            let cap = (rec.ring.len() / 2) as u64;
            let w = rec.write.load(Relaxed);
            let free = cap - (w - rec.read.load(Acquire));
            if (n as u64) <= free {
                for i in 0..n {
                    let at = (((w + i as u64) % cap) * 2) as usize;
                    rec.ring[at].store(hl[i].to_bits(), Relaxed);
                    rec.ring[at + 1].store(hr[i].to_bits(), Relaxed);
                }
                rec.write.store(w + n as u64, Release);
                rec.recorded.fetch_add(n as u64, Relaxed);
            } else {
                rec.dropped.fetch_add(n as u64, Relaxed);
            }
        }
        rec.in_use.store(false, SeqCst);

        l.header.hubHeartbeatNs.store(now_ns(), Relaxed);
    }
}

/// `juce::URL::addEscapeChars(s, true)`: letters, digits and `_-.~` stay, every other byte becomes `%XX`.
fn escape_param(s: &str) -> String {
    let mut out = String::with_capacity(s.len() * 3);
    for b in s.bytes() {
        if b.is_ascii_alphanumeric() || b"_-.~".contains(&b) {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

/// A 32-bit float stereo WAV header for `frames` frames (written again with the real count at the end).
fn wav_header(rate: u32, frames: u64) -> [u8; 44] {
    let data = (frames * 8).min(u64::from(u32::MAX - 36)) as u32;
    let mut h = [0u8; 44];
    let mut put = |at: usize, b: &[u8]| h[at..at + b.len()].copy_from_slice(b);
    put(0, b"RIFF");
    put(4, &(36 + data).to_le_bytes());
    put(8, b"WAVEfmt ");
    put(16, &16u32.to_le_bytes());
    put(20, &3u16.to_le_bytes()); // IEEE float
    put(22, &2u16.to_le_bytes());
    put(24, &rate.to_le_bytes());
    put(28, &(rate * 8).to_le_bytes());
    put(32, &8u16.to_le_bytes());
    put(34, &32u16.to_le_bytes());
    put(36, b"data");
    put(40, &data.to_le_bytes());
    h
}

fn write_loop(rec: &Recording, mut file: File, rate: u32) {
    let cap = (rec.ring.len() / 2) as u64;
    let mut buf = Vec::new();
    let mut frames = 0u64;
    loop {
        // the audio thread may still be finishing a block it started while `on` was set
        let stopping = !rec.on.load(SeqCst) && !rec.in_use.load(SeqCst);
        let w = rec.write.load(Acquire);
        let r = rec.read.load(Relaxed);
        buf.clear();
        for f in r..w {
            let at = ((f % cap) * 2) as usize;
            buf.extend(rec.ring[at].load(Relaxed).to_le_bytes());
            buf.extend(rec.ring[at + 1].load(Relaxed).to_le_bytes());
        }
        rec.read.store(w, Release);
        if file.write_all(&buf).is_ok() {
            frames += w - r;
        }
        if stopping {
            break;
        }
        std::thread::park_timeout(Duration::from_millis(20));
    }
    let _ = file.seek(SeekFrom::Start(0));
    let _ = file.write_all(&wav_header(rate, frames));
}
