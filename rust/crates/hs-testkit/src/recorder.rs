//! `TakeRecorder`: prints the audio a plug-in adds to its track into a 32-bit float WAV file (`plugins/app/TakeRecorder`).
//! The audio thread only copies into a lock-free single-producer ring that was allocated with the recorder (a full ring drops
//! frames and says so; nothing allocates or locks); the message thread empties it into the file. `close` waits until the
//! audio thread is out of `write` before the last flush, so the file holds every frame the audio thread counted. A take
//! carries a BWF time reference (where it starts on the DAW timeline) so "move to origin" puts it back in place.
//!
//! Candidate for the Rust App Audio and Hub plug-ins (R7 / R8): it lives here until one of them needs it.
use hs_app::paths::{DateTime, documents_dir, local_time, utc_from_unix};
use std::fs::{self, File};
use std::io::{Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering::{AcqRel, Acquire, Relaxed, Release, SeqCst};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU8, AtomicU32, AtomicU64};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::{SystemTime, UNIX_EPOCH};

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

/// 4 s of headroom at 48 kHz for a slow disk before frames are dropped (the C++ gives 4 s too).
const DEFAULT_HEADROOM_FRAMES: usize = 4 * 48_000;
const BEXT_BYTES: u32 = 602;
/// where the 64-bit time reference sits in the file: RIFF header (12) + chunk header (8) + 338
const TIME_REFERENCE_AT: u64 = 12 + 8 + 338;

// states of a take
const CLOSED: u8 = 0;
/// open, waiting for the first block that wants to be recorded
const ARMED: u8 = 1;
const WRITING: u8 = 2;
/// the audio side stopped it (`want` went false)
const ENDED: u8 = 3;

struct Task {
    file: File,
    path: PathBuf,
    label: String,
    bytes: Vec<u8>,
    data_bytes: u64,
}

struct Inner {
    sample_rate: AtomicU32,
    state: AtomicU8,
    /// the audio thread is inside `write`
    in_use: AtomicBool,
    shift: AtomicI64,
    start: AtomicI64,
    frames: AtomicI64,
    dropped: AtomicBool,
    /// interleaved stereo floats (as bits); the audio thread writes, `service` reads
    ring: Box<[AtomicU32]>,
    write_pos: AtomicU64,
    read_pos: AtomicU64,
    task: Mutex<Option<Task>>,
    last: Mutex<Option<(PathBuf, f64)>>,
    folder: Mutex<Option<PathBuf>>,
}

/// One take at a time. Clones share the take.
#[derive(Clone)]
pub struct TakeRecorder {
    i: Arc<Inner>,
}

impl Default for TakeRecorder {
    fn default() -> Self {
        Self::new()
    }
}

/// `Documents\HEARASIDE\Recordings` (the real Documents folder, which OneDrive may have moved).
pub fn default_folder() -> PathBuf {
    documents_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join("HEARASIDE")
        .join("Recordings")
}

fn now_secs() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs() as i64)
}

/// This computer's local time at `secs` (UTC when it cannot say).
fn local(secs: i64) -> DateTime {
    local_time(secs).unwrap_or_else(|| utc_from_unix(secs))
}

/// `2026-10-31 12-00-00`, the form of `Time::formatted("%Y-%m-%d %H-%M-%S")`.
fn stamp(t: &DateTime) -> String {
    format!(
        "{} {:02}-{:02}-{:02}",
        t.date_text(),
        t.hour,
        t.minute,
        t.second
    )
}

/// `juce::File::createLegalFileName`: no path or reserved characters.
fn legal(name: &str) -> String {
    let s: String = name
        .chars()
        .map(|c| {
            if c.is_control() || "<>:\"/\\|?*".contains(c) {
                '_'
            } else {
                c
            }
        })
        .collect();
    let s = s.trim().trim_end_matches('.').to_owned();
    if s.is_empty() { "take".into() } else { s }
}

fn new_file(dir: &Path, base: &str) -> Option<(File, PathBuf)> {
    for n in 0..1000 {
        let name = if n == 0 {
            format!("{base}.wav")
        } else {
            format!("{base}_{n}.wav")
        };
        let path = dir.join(name);
        if let Ok(f) = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
        {
            return Some((f, path));
        }
    }
    None
}

fn put(bext: &mut [u8], at: usize, text: &str, max: usize) {
    let b = text.as_bytes();
    let n = b.len().min(max);
    bext[at..at + n].copy_from_slice(&b[..n]);
}

fn header(rate: u32, label: &str, now: &DateTime) -> Vec<u8> {
    let mut h = Vec::with_capacity(12 + 8 + BEXT_BYTES as usize + 8 + 16 + 8);
    h.extend_from_slice(b"RIFF\0\0\0\0WAVE");
    h.extend_from_slice(b"bext");
    h.extend_from_slice(&BEXT_BYTES.to_le_bytes());
    let mut bext = vec![0u8; BEXT_BYTES as usize];
    put(&mut bext, 0, &format!("HEARASIDE App Audio: {label}"), 255);
    put(&mut bext, 256, "HEARASIDE", 32); // originator
    put(&mut bext, 320, &now.date_text(), 10); // origination date
    put(&mut bext, 330, &now.time_text(), 8); // origination time
    bext[346..348].copy_from_slice(&1u16.to_le_bytes()); // BWF version 1
    h.extend_from_slice(&bext);
    h.extend_from_slice(b"fmt ");
    h.extend_from_slice(&16u32.to_le_bytes());
    h.extend_from_slice(&3u16.to_le_bytes()); // IEEE float
    h.extend_from_slice(&2u16.to_le_bytes());
    h.extend_from_slice(&rate.to_le_bytes());
    h.extend_from_slice(&(rate * 8).to_le_bytes());
    h.extend_from_slice(&8u16.to_le_bytes());
    h.extend_from_slice(&32u16.to_le_bytes());
    h.extend_from_slice(b"data\0\0\0\0");
    h
}

impl TakeRecorder {
    pub fn new() -> Self {
        Self::with_headroom_frames(DEFAULT_HEADROOM_FRAMES)
    }

    /// A recorder whose ring holds `frames` stereo frames (what the audio thread can get ahead of the disk).
    pub fn with_headroom_frames(frames: usize) -> Self {
        TakeRecorder {
            i: Arc::new(Inner {
                sample_rate: AtomicU32::new(48_000),
                state: AtomicU8::new(CLOSED),
                in_use: AtomicBool::new(false),
                shift: AtomicI64::new(0),
                start: AtomicI64::new(-1),
                frames: AtomicI64::new(0),
                dropped: AtomicBool::new(false),
                ring: (0..frames.max(1) * 2).map(|_| AtomicU32::new(0)).collect(),
                write_pos: AtomicU64::new(0),
                read_pos: AtomicU64::new(0),
                task: Mutex::new(None),
                last: Mutex::new(None),
                folder: Mutex::new(None),
            }),
        }
    }

    /// Takes go to `dir` instead of `Documents\HEARASIDE\Recordings` (tests).
    pub fn set_folder(&self, dir: impl Into<PathBuf>) {
        *lock(&self.i.folder) = Some(dir.into());
    }

    /// Closes any open take.
    pub fn prepare(&self, sample_rate: f64) {
        self.close();
        let r = if sample_rate > 0.0 {
            sample_rate
        } else {
            48_000.0
        };
        self.i.sample_rate.store(r.round() as u32, Relaxed);
    }

    /// A new file; writing starts with the next block that wants it. `false` = no file could be made.
    pub fn open(&self, label: &str) -> bool {
        self.close();
        let dir = lock(&self.i.folder).clone().unwrap_or_else(default_folder);
        if fs::create_dir_all(&dir).is_err() {
            return false;
        }
        let now = local(now_secs());
        let base = legal(&format!("{label} {}", stamp(&now)));
        let Some((mut file, path)) = new_file(&dir, &base) else {
            return false;
        };
        let rate = self.i.sample_rate.load(Relaxed);
        if file.write_all(&header(rate, label, &now)).is_err() {
            drop(file);
            let _ = fs::remove_file(&path);
            return false;
        }
        // the state is CLOSED here, so the audio thread does not touch the ring
        self.i
            .read_pos
            .store(self.i.write_pos.load(Acquire), Release);
        self.i.frames.store(0, Relaxed);
        self.i.start.store(-1, Relaxed);
        self.i.dropped.store(false, Relaxed);
        *lock(&self.i.task) = Some(Task {
            file,
            path,
            label: label.to_owned(),
            bytes: Vec::with_capacity(64 * 1024),
            data_bytes: 0,
        });
        self.i.state.store(ARMED, SeqCst);
        true
    }

    /// Writes what the audio side collected. Call it a few times a second (and `close` does).
    pub fn service(&self) {
        let i = &*self.i;
        let mut guard = lock(&i.task);
        let Some(t) = guard.as_mut() else { return };
        let w = i.write_pos.load(Acquire);
        let r = i.read_pos.load(Relaxed);
        if w == r {
            return;
        }
        let cap = i.ring.len() as u64;
        t.bytes.clear();
        for k in r..w {
            t.bytes
                .extend_from_slice(&i.ring[(k % cap) as usize].load(Relaxed).to_le_bytes());
        }
        i.read_pos.store(w, Release);
        if t.file.write_all(&t.bytes).is_ok() {
            t.data_bytes += t.bytes.len() as u64;
        } else {
            i.dropped.store(true, Relaxed);
        }
    }

    /// Finishes the take: an empty one is deleted, else the sizes and the time reference are written. Every frame the audio
    /// thread counted is in the file.
    pub fn close(&self) {
        let i = &*self.i;
        // the audio side stops adding (it checks the state after it says it is in use) and the block in flight finishes
        i.state.store(CLOSED, SeqCst);
        while i.in_use.load(SeqCst) {
            std::thread::yield_now();
        }
        self.service();
        let Some(mut t) = lock(&i.task).take() else {
            return;
        };
        let frames = i.frames.load(Relaxed);
        if frames == 0 {
            drop(t.file);
            let _ = fs::remove_file(&t.path);
            return;
        }
        let rate = f64::from(i.sample_rate.load(Relaxed));
        let data = u32::try_from(t.data_bytes).unwrap_or(u32::MAX);
        let riff = u32::try_from(t.data_bytes + 4 + 8 + u64::from(BEXT_BYTES) + 8 + 16 + 8)
            .unwrap_or(u32::MAX);
        let start = i.start.load(Relaxed);
        let patch = |f: &mut File| -> std::io::Result<()> {
            f.seek(SeekFrom::Start(4))?;
            f.write_all(&riff.to_le_bytes())?;
            f.seek(SeekFrom::Start(12 + 8 + u64::from(BEXT_BYTES) + 8 + 16 + 4))?;
            f.write_all(&data.to_le_bytes())?;
            if start >= 0 {
                f.seek(SeekFrom::Start(TIME_REFERENCE_AT))?;
                f.write_all(&start.to_le_bytes())?;
            }
            f.flush()
        };
        let _ = patch(&mut t.file);
        drop(t.file);
        let seconds = frames as f64 / rate;
        // a take armed ahead is named after when it really started
        let started = local(now_secs() - seconds as i64);
        let dir = t.path.parent().map(Path::to_path_buf).unwrap_or_default();
        let want = legal(&format!("{} {}", t.label, stamp(&started)));
        let mut path = t.path;
        let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned());
        if !stem.is_some_and(|s| s.starts_with(&want))
            && let Some((f, named)) = new_file(&dir, &want)
        {
            drop(f);
            if fs::rename(&path, &named).is_ok() {
                path = named;
            } else {
                let _ = fs::remove_file(&named);
            }
        }
        *lock(&i.last) = Some((path, seconds));
    }

    pub fn is_open(&self) -> bool {
        self.i.state.load(Relaxed) != CLOSED
    }

    /// Seconds written to the open take.
    pub fn seconds(&self) -> f64 {
        self.i.frames.load(Relaxed) as f64 / f64::from(self.i.sample_rate.load(Relaxed))
    }

    /// The last finished take.
    pub fn last_take(&self) -> Option<PathBuf> {
        lock(&self.i.last).as_ref().map(|l| l.0.clone())
    }

    pub fn last_seconds(&self) -> f64 {
        lock(&self.i.last).as_ref().map_or(0.0, |l| l.1)
    }

    /// A friend's voice reaches us this many frames after it belongs on the timeline (network + browser + our own buffer):
    /// the take's position is moved back by that much. Any thread; read when a take starts.
    pub fn set_shift_frames(&self, frames: i64) {
        self.i.shift.store(frames.max(0), Relaxed);
    }

    /// frames lost because the ring was full or the disk failed
    pub fn dropped(&self) -> bool {
        self.i.dropped.load(Relaxed)
    }

    /// Audio thread. `want`: this block belongs to the take (a take that started and then gets `want = false` ends).
    /// `has_time` false = no timeline position (transport stopped). Never allocates, never locks.
    pub fn write(&self, ch: [&[f32]; 2], want: bool, time_samples: i64, has_time: bool) {
        let i = &*self.i;
        i.in_use.store(true, SeqCst);
        // `close` stores CLOSED and then waits for `in_use`: in this order the block either stops here or is finished first
        let state = i.state.load(SeqCst);
        if state == ARMED || state == WRITING {
            self.write_block(state, ch, want, time_samples, has_time);
        }
        i.in_use.store(false, SeqCst);
    }

    fn write_block(
        &self,
        state: u8,
        ch: [&[f32]; 2],
        want: bool,
        time_samples: i64,
        has_time: bool,
    ) {
        let i = &*self.i;
        if !want {
            if state == WRITING {
                let _ = i.state.compare_exchange(WRITING, ENDED, AcqRel, Relaxed);
            }
            return;
        }
        let n = ch[0].len().min(ch[1].len());
        if state == ARMED {
            let at = if has_time && time_samples >= 0 {
                (time_samples - i.shift.load(Relaxed)).max(0)
            } else {
                -1
            };
            i.start.store(at, Relaxed);
            let _ = i.state.compare_exchange(ARMED, WRITING, AcqRel, Relaxed);
        }
        let cap = i.ring.len() as u64;
        let (w, r) = (i.write_pos.load(Relaxed), i.read_pos.load(Acquire));
        if cap - (w - r) >= n as u64 * 2 {
            let mut k = w;
            for (l, rr) in ch[0].iter().zip(ch[1]).take(n) {
                i.ring[(k % cap) as usize].store(l.to_bits(), Relaxed);
                i.ring[((k + 1) % cap) as usize].store(rr.to_bits(), Relaxed);
                k += 2;
            }
            i.write_pos.store(k, Release);
        } else {
            i.dropped.store(true, Relaxed);
        }
        i.frames.fetch_add(n as i64, Relaxed);
    }
}

impl Drop for TakeRecorder {
    fn drop(&mut self) {
        // the last handle finishes the file
        if Arc::strong_count(&self.i) == 1 {
            self.close();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicBool;

    fn temp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("hs-testkit-rec-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        d
    }

    fn text_at(b: &[u8], at: usize, len: usize) -> String {
        String::from_utf8_lossy(&b[at..at + len])
            .trim_end_matches('\0')
            .to_owned()
    }

    #[test]
    fn a_take_is_a_float_wav_with_its_position() {
        let dir = temp("take");
        let r = TakeRecorder::new();
        r.set_folder(&dir);
        r.prepare(48_000.0);
        r.set_shift_frames(4800);
        assert!(r.open("Mint"));
        let block = vec![0.5f32; 480];
        for k in 0..100 {
            r.write([&block, &block], true, 100_000 + k * 480, true);
            if k % 10 == 0 {
                r.service();
            }
        }
        assert!((r.seconds() - 1.0).abs() < 1e-9);
        r.close();
        let take = r.last_take().expect("a take");
        assert!(
            take.file_name()
                .unwrap()
                .to_string_lossy()
                .starts_with("Mint "),
            "{take:?}"
        );
        let b = fs::read(&take).unwrap();
        assert_eq!(&b[..4], b"RIFF");
        assert_eq!(&b[8..12], b"WAVE");
        let riff = u32::from_le_bytes(b[4..8].try_into().unwrap()) as usize;
        assert_eq!(riff + 8, b.len(), "RIFF size");
        let data_at = 12 + 8 + 602 + 8 + 16 + 4;
        assert_eq!(&b[data_at - 4..data_at], b"data");
        let data = u32::from_le_bytes(b[data_at..data_at + 4].try_into().unwrap()) as usize;
        assert_eq!(data, 48_000 * 8, "one second of stereo floats");
        assert_eq!(b.len(), data_at + 4 + data);
        let time_ref = i64::from_le_bytes(b[358..366].try_into().unwrap());
        assert_eq!(time_ref, 100_000 - 4800, "moved back by the friend's delay");
        assert_eq!(
            f32::from_le_bytes(b[data_at + 4..data_at + 8].try_into().unwrap()),
            0.5
        );
        assert!((r.last_seconds() - 1.0).abs() < 1e-9);
        // the BWF fields like the C++ wrote them (bext data starts at 20)
        assert_eq!(text_at(&b, 20, 255), "HEARASIDE App Audio: Mint");
        assert_eq!(text_at(&b, 20 + 256, 32), "HEARASIDE");
        let (date, time) = (text_at(&b, 20 + 320, 10), text_at(&b, 20 + 330, 8));
        let now = local(now_secs());
        assert!(
            date == now.date_text() || date == local(now_secs() - 5).date_text(),
            "{date}"
        );
        assert!(
            time.len() == 8 && time.as_bytes()[2] == b':' && time.as_bytes()[5] == b':',
            "{time}"
        );
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn an_empty_take_leaves_no_file_and_a_full_ring_drops_instead_of_growing() {
        let dir = temp("empty");
        let r = TakeRecorder::new();
        r.set_folder(&dir);
        assert!(r.open("Nobody"));
        r.close();
        assert!(r.last_take().is_none());
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);

        let r = TakeRecorder::with_headroom_frames(8000 * 4);
        r.set_folder(&dir);
        r.prepare(8000.0);
        assert!(r.open("Fast"));
        let block = vec![0.1f32; 8000];
        for _ in 0..6 {
            r.write([&block, &block], true, -1, false); // 6 s without a service() in between: 4 s fit
        }
        assert!(r.dropped());
        assert_eq!(
            r.i.write_pos.load(Relaxed),
            8000 * 4 * 2,
            "the ring holds 4 s"
        );
        r.close();
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn close_while_the_audio_thread_is_writing_loses_no_frame() {
        // the last block in flight must reach the file: the frames counted are the frames in the file
        let dir = temp("close-live");
        for round in 0..30 {
            let r = TakeRecorder::with_headroom_frames(48_000 * 4);
            r.set_folder(&dir);
            assert!(r.open("Live"));
            let stop = Arc::new(AtomicBool::new(false));
            let writer = {
                let (r, stop) = (r.clone(), Arc::clone(&stop));
                std::thread::spawn(move || {
                    let block: Vec<f32> = (0..96).map(|k| k as f32 / 100.0).collect();
                    while !stop.load(Relaxed) {
                        r.write([&block, &block], true, -1, false);
                        std::thread::sleep(std::time::Duration::from_micros(50));
                    }
                })
            };
            let until =
                std::time::Instant::now() + std::time::Duration::from_millis(5 + (round % 7) * 3);
            while std::time::Instant::now() < until {
                r.service();
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
            r.close(); // the writer is still going
            let counted = r.i.frames.load(Relaxed);
            stop.store(true, Relaxed);
            writer.join().unwrap();
            assert_eq!(
                r.i.frames.load(Relaxed),
                counted,
                "nothing is counted after close"
            );
            assert!(!r.dropped(), "round {round}");
            let take = r.last_take().expect("a take");
            let b = fs::read(&take).unwrap();
            let data_at = 12 + 8 + 602 + 8 + 16 + 4;
            let data = u32::from_le_bytes(b[data_at..data_at + 4].try_into().unwrap()) as i64;
            assert_eq!(data, counted * 8, "round {round}: frames in the file");
            assert_eq!(b.len() as i64, data_at as i64 + 4 + data);
            let _ = fs::remove_file(take);
        }
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn names_and_dates() {
        let t = DateTime {
            year: 2026,
            month: 10,
            day: 31,
            hour: 12,
            minute: 0,
            second: 5,
        };
        assert_eq!(stamp(&t), "2026-10-31 12-00-05");
        assert_eq!(legal("a/b:c"), "a_b_c");
        assert_eq!(legal(" . "), "take");
    }

    #[test]
    fn documents_folder_ends_with_hearaside_recordings() {
        let f = default_folder();
        assert!(
            f.ends_with(Path::new("HEARASIDE").join("Recordings")),
            "{f:?}"
        );
    }
}
