//! Latency and lag measuring for auto sync: the history the Hub's audio thread writes (each Track's
//! viewers signal, the master bus input and the App Audio signals, decimated by 4) and the
//! measurements that read it from another thread. Port of `HubEngine::measureLatencies` /
//! `measureLag` / `takeSourceReadStats` (the friends' echo/leak/pairing/probe parts: R2.3).
//!
//! The C++ shared plain float arrays between the audio thread and the measuring thread behind a
//! release/acquire write counter. Here the history cells are relaxed atomics (a plain `mov` on
//! x86-64), so the sharing is sound and needs no lock on the audio side; a `HubMeasurer` is a cheap
//! clonable handle for the measuring thread and never blocks `prepare` or `process`.
use hs_bus::bus::{bits_float, float_bits};
use hs_bus::protocol::*;
use hs_bus::shm::{BeaconMap, SharedMemory};
use std::sync::atomic::Ordering::{Acquire, Relaxed};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicU8, AtomicU32, AtomicU64};
use std::sync::{Arc, Mutex};

pub(crate) struct SrcStats {
    pub lead_min: AtomicI64,
    pub lead_max: AtomicI64,
    pub jumps: AtomicU32,
    pub blocks: AtomicU32,
}

impl SrcStats {
    fn new() -> Self {
        SrcStats {
            lead_min: AtomicI64::new(i64::MAX),
            lead_max: AtomicI64::new(i64::MIN),
            jumps: AtomicU32::new(0),
            blocks: AtomicU32::new(0),
        }
    }
}

/// The decimated history of one `prepare` (sample rate / 4 per cell).
pub(crate) struct History {
    pub sample_rate: f64,
    pub cap_len: u32,
    /// kMaxSlots x cap_len
    pub slots: Box<[AtomicU32]>,
    pub input: Box<[AtomicU32]>,
    /// kMaxSources x cap_len
    pub sources: Box<[AtomicU32]>,
    /// what friends hear (the Stream Mix as the DAW made it), cap_len
    pub friend_mix: Box<[AtomicU32]>,
    /// each friend's voice as the Hub plays it, kMaxFriends x cap_len
    pub friends: Box<[AtomicU32]>,
    /// history frames written so far (release store by the audio thread; u64: a u32 wrapped after 4 days and the C++ caller then underflowed)
    pub write: AtomicU64,
    pub last_block: AtomicI32,
    pub sync_safety: AtomicI32,
    pub src_stats: [SrcStats; kMaxSources],
}

fn zeroed(n: usize) -> Box<[AtomicU32]> {
    (0..n).map(|_| AtomicU32::new(0)).collect()
}

impl History {
    pub fn new(sample_rate: f64, cap_len: u32) -> Self {
        let cl = cap_len as usize;
        History {
            sample_rate,
            cap_len,
            slots: zeroed(kMaxSlots * cl),
            input: zeroed(cl),
            sources: zeroed(kMaxSources * cl),
            friend_mix: zeroed(cl),
            friends: zeroed(kMaxFriends * cl),
            write: AtomicU64::new(0),
            last_block: AtomicI32::new(256),
            sync_safety: AtomicI32::new(0),
            src_stats: std::array::from_fn(|_| SrcStats::new()),
        }
    }

    pub fn slot_ring(&self, i: usize) -> &[AtomicU32] {
        let cl = self.cap_len as usize;
        &self.slots[i * cl..(i + 1) * cl]
    }

    pub fn friend_ring(&self, j: usize) -> &[AtomicU32] {
        let cl = self.cap_len as usize;
        &self.friends[j * cl..(j + 1) * cl]
    }

    pub fn source_ring(&self, i: usize) -> &[AtomicU32] {
        let cl = self.cap_len as usize;
        &self.sources[i * cl..(i + 1) * cl]
    }

    /// The last `m` cells before write position `w`, oldest first.
    pub(crate) fn copy_last(&self, ring: &[AtomicU32], w: u64, m: usize, dst: &mut Vec<f32>) {
        dst.clear();
        dst.extend((0..m).map(|i| {
            let at = (w - m as u64 + i as u64) % u64::from(self.cap_len);
            bits_float(ring[at as usize].load(Relaxed))
        }));
    }
}

/// What the Hub does with each friend (set from the message thread, read on the audio thread).
/// C layout: the hybrid C++ reads it as `HubEngine::FriendControl`.
#[repr(C)]
pub struct FriendControl {
    /// `FRIEND_MON | FRIEND_STR | FRIEND_SOLO`: you hear / viewers hear / solo for viewers
    pub flags: AtomicU32,
    /// dB, for you and the viewers (f32 bits)
    pub gain_bits: AtomicU32,
    /// -1 .. +1, viewers only (f32 bits)
    pub pan_bits: AtomicU32,
}

pub const FRIEND_MON: u32 = 1;
pub const FRIEND_STR: u32 = 2;
pub const FRIEND_SOLO: u32 = 4;

/// What the audio thread sees of each friend (read by the UI).
/// C layout: the hybrid C++ reads it as `HubEngine::FriendInfo`.
#[repr(C)]
#[derive(Default)]
pub struct FriendInfo {
    /// audio is being played now
    pub playing: AtomicU32,
    /// jitter buffer the Hub keeps for this friend (ms, f32 bits)
    pub buffer_ms_bits: AtomicU32,
    /// how late this friend is against the music, all included (ms, f32 bits)
    pub total_ms_bits: AtomicU32,
    /// extra delay the Hub gives this friend so they line up (ms, f32 bits)
    pub line_ms_bits: AtomicU32,
    /// slower than the Line-up limit: not counted in D
    pub over_limit: AtomicU32,
    /// short gaps the jitter buffer had to hide
    pub underruns: AtomicU32,
    /// silence played for them (gaps + restarts), ms
    pub concealed_ms: AtomicU32,
    /// how late packets arrive over the last ~5 s, p95 (ms, f32 bits, 0 = not measured yet)
    pub jitter_p95_ms_bits: AtomicU32,
    /// speaker leak detected (friendMix leaked into the friend's mic)
    pub leak_detected: AtomicU32,
    /// friend voice present in the master bus while the route is Direct
    pub echo_in_master: AtomicU32,
}

/// A pair chosen by hand (or found by the probe), remembered by the friend input's track name and the
/// Track's uuid so the project can keep it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairMemory {
    pub feeder: String,
    pub slot_uuid: String,
}

/// The probe's working state (the measuring thread only; behind `Shared::probe`).
#[derive(Default)]
pub(crate) struct ProbeState {
    pub best: i32,
    pub li: f64,
    pub hold_to: u64,
    pub word: u64,
    pub from: u64,
    pub wall_ns: u64,
}

/// Shared between the engine (audio side) and every non-audio handle (`HubMeasurer`).
pub(crate) struct Shared {
    /// random token of this Hub (claims the bus)
    pub token: u64,
    /// 0 disconnected, 1 owner, 2 secondary (written by `maintain`, any thread reads it)
    pub role: AtomicU8,
    /// `maintain` asks the audio side to rescan the slots at its next block
    pub rescan: AtomicBool,
    /// the Hub's version for plug-ins of another version to see
    pub beacon: Mutex<Option<BeaconMap>>,
    pub hist: Mutex<Arc<History>>,
    pub shm: Mutex<Option<Arc<SharedMemory>>>,
    pub friend_ctl: [FriendControl; kMaxFriends],
    pub friend_info: [FriendInfo; kMaxFriends],
    pub line_up_on: AtomicBool,
    pub line_up_limit_bits: AtomicU32,
    pub line_up_ms_bits: AtomicU32,
    pub line_up_slowest: AtomicI32,
    pub line_up_changes: AtomicU32,
    /// per friend: slot + that slot's claim + friend id in one word, 0 = none
    pub manual_pair: [AtomicU64; kMaxFriends],
    /// per friend: Li in 0.01 ms + 1, 0 = not measured
    pub manual_li: [AtomicU32; kMaxFriends],
    /// the feeder being probed, -1 = none; the audio thread silences the viewers and friends while >= 0
    pub probe_feeder: AtomicI32,
    /// 0 = starting, 1 = silenced and waiting, 2 = sweeping, 3 = answer known, waiting for the sweep to leave
    pub probe_stage: AtomicI32,
    pub probe: Mutex<ProbeState>,
    /// the longest a Track's sound took to reach the master at the last measuring (ms, 200 = not measured)
    pub slot_to_master_ms: AtomicU32,
    pub memory: Mutex<Vec<PairMemory>>,
}

impl Shared {
    pub fn new(hist: Arc<History>, token: u64) -> Self {
        Shared {
            token,
            role: AtomicU8::new(0),
            rescan: AtomicBool::new(false),
            beacon: Mutex::new(None),
            hist: Mutex::new(hist),
            shm: Mutex::new(None),
            friend_ctl: std::array::from_fn(|_| FriendControl {
                flags: AtomicU32::new(FRIEND_MON | FRIEND_STR),
                gain_bits: AtomicU32::new(0),
                pan_bits: AtomicU32::new(0),
            }),
            friend_info: std::array::from_fn(|_| FriendInfo::default()),
            line_up_on: AtomicBool::new(false),
            line_up_limit_bits: AtomicU32::new(600.0f32.to_bits()),
            line_up_ms_bits: AtomicU32::new(0),
            line_up_slowest: AtomicI32::new(-1),
            line_up_changes: AtomicU32::new(0),
            manual_pair: std::array::from_fn(|_| AtomicU64::new(0)),
            manual_li: std::array::from_fn(|_| AtomicU32::new(0)),
            probe_feeder: AtomicI32::new(-1),
            probe_stage: AtomicI32::new(0),
            probe: Mutex::new(ProbeState::default()),
            slot_to_master_ms: AtomicU32::new(200.0f32.to_bits()),
            memory: Mutex::new(Vec::new()),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FxLatency {
    /// -1 = not measured yet
    pub master_ms: f64,
    pub tracks_ms: f64,
}

impl Default for FxLatency {
    fn default() -> Self {
        FxLatency {
            master_ms: -1.0,
            tracks_ms: -1.0,
        }
    }
}

/// How the Hub has been reading an App Audio's signal since the last call (auto sync log).
#[derive(Debug, Clone, Copy, Default)]
pub struct SourceReadStats {
    pub lead_min: i64,
    pub lead_max: i64,
    pub jumps: u32,
    pub blocks: u32,
}

/// Handle for the measuring thread ("any non-audio thread, a few times a second").
#[derive(Clone)]
pub struct HubMeasurer {
    pub(crate) shared: Arc<Shared>,
}

/// `v >= min`, false for NaN (a NaN sample must never count as sound or as a match).
fn at_least(v: f64, min: f64) -> bool {
    v >= min
}

/// Lag (in history frames, `kmin..=kmax`) at which `x` best matches `y`, by normalised correlation
/// over `y[y_start, y_start + n)`. `cum` = prefix sums of x^2. Coarse pass on every `lag_step`-th lag
/// and every 4th sample, then a full pass around it and a parabolic refinement. Returns the lag when
/// the score reaches `min_score`, and the best score found either way.
#[allow(clippy::too_many_arguments)]
pub(crate) fn best_lag(
    y: &[f32],
    x: &[f32],
    n: usize,
    y_start: usize,
    kmin: i32,
    kmax: i32,
    cum: &[f64],
    ey: f64,
    min_score: f64,
    any_polarity: bool,
    lag_step: usize,
) -> (Option<f64>, f64) {
    let score = |k: i32, step: usize| -> f64 {
        let xs = y_start as i64 - i64::from(k);
        let mut c = 0.0f64;
        let mut i = 0;
        while i < n {
            c += f64::from(y[y_start + i]) * f64::from(x[(xs + i as i64) as usize]);
            i += step;
        }
        let ex = cum[(xs + n as i64) as usize] - cum[xs as usize];
        let v = if ex > 0.0 {
            c * step as f64 / (ex * ey).sqrt()
        } else {
            0.0
        };
        if any_polarity { v.abs() } else { v }
    };
    let (mut kc, mut bc) = (kmin, -2.0f64);
    let mut k = kmin;
    while k <= kmax {
        // whitened signals have a narrow peak: lag_step 1
        let v = score(k, 4);
        if v > bc {
            bc = v;
            kc = k;
        }
        k += lag_step as i32;
    }
    let lo = kmin.max(kc - 4);
    let hi = kmax.min(kc + 4);
    let v: Vec<f64> = (lo..=hi).map(|k| score(k, 1)).collect();
    let mut best = 0;
    for (i, &s) in v.iter().enumerate() {
        if s > v[best] {
            best = i;
        }
    }
    let top = v[best];
    // (negated so that a NaN score is no match: the C++ treated it as one at the first lag)
    if !at_least(top, min_score) {
        return (None, top);
    }
    let mut lag = f64::from(lo) + best as f64;
    if best > 0 && best + 1 < v.len() {
        let (p, q, r) = (v[best - 1], v[best], v[best + 1]);
        let den = p - 2.0 * q + r;
        if den < 0.0 {
            lag += 0.5 * (p - r) / den;
        }
    }
    (Some(lag), top)
}

pub(crate) fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

impl HubMeasurer {
    pub(crate) fn snapshot_bus(&self) -> Option<Arc<SharedMemory>> {
        lock(&self.shared.shm).clone()
    }

    pub(crate) fn snapshot(&self) -> (Arc<History>, Option<Arc<SharedMemory>>) {
        let h = self
            .shared
            .hist
            .lock()
            .map_or_else(|e| e.into_inner().clone(), |g| g.clone());
        let s = self
            .shared
            .shm
            .lock()
            .map_or_else(|e| e.into_inner().clone(), |g| g.clone());
        (h, s)
    }

    /// Plug-in latency, measured by finding how late each Track's viewers signal arrives on the master
    /// bus. The DAW delays every track to line up with the slowest chain, so with two or more tracks
    /// sounding the spread gives each Track's chain latency (kept in `SlotHeader::chainLatencyBits`,
    /// which the Track saves). Live, with one track sounding, the remembered chain latencies give the
    /// master plug-ins above the Hub. `tracks_ms` = slowest remembered chain among active tracks.
    /// -1 = not measured yet.
    pub fn measure_latencies(&self) -> FxLatency {
        let (hist, shm) = self.snapshot();
        let mut out = FxLatency::default();
        let Some(shm) = shm else { return out };
        if hist.cap_len == 0 {
            return out;
        }
        let b = shm.layout();
        let rate = hist.sample_rate / 4.0;
        let ms_per_lag = 4000.0 / hist.sample_rate;
        // the Hub reads the Tracks' signals behind the master bus by the sync-safety blocks
        let lead = (f64::from(hist.sync_safety.load(Relaxed))
            * f64::from(hist.last_block.load(Relaxed))
            / 4.0)
            .round() as i32;
        let n = (rate * 0.25) as usize; // 250 ms window
        let k = (rate * 0.5) as usize; // up to 500 ms of latency
        let kneg = (lead + 8).max(0) as usize;
        let m = n + k + kneg;
        let w = hist.write.load(Acquire);
        let have_hist = (w as usize) >= m && m + 4096 <= hist.cap_len as usize;
        let mut x = Vec::new();
        if have_hist {
            let mut y = Vec::new();
            hist.copy_last(&hist.input, w, m, &mut y);
            let y_start = m - kneg - n;
            let ey: f64 = y[y_start..y_start + n]
                .iter()
                .map(|&v| f64::from(v) * f64::from(v))
                .sum();
            let sound = ey > n as f64 * 1e-6; // about -60 dBFS

            let mut lag_ms = [-1.0f64; kMaxSlots]; // how late each track reaches the master now, -1 = unknown
            let mut cum = vec![0.0f64; m + 1];
            let mut measured = 0;
            for (i, lm) in lag_ms.iter_mut().enumerate() {
                if !sound {
                    break;
                }
                if b.slots[i].state.load(Acquire) != SlotState::kSlotActive {
                    continue;
                }
                hist.copy_last(hist.slot_ring(i), w, m, &mut x);
                for j in 0..m {
                    cum[j + 1] = cum[j] + f64::from(x[j]) * f64::from(x[j]);
                }
                if !at_least(cum[m], m as f64 * 1e-7) {
                    continue; // this track is silent for the viewers right now (or not a number)
                }
                // a single track inside a whole mix correlates less than the mix itself
                let (lag, _) = best_lag(
                    &y,
                    &x,
                    n,
                    y_start,
                    -(kneg as i32),
                    k as i32,
                    &cum,
                    ey,
                    0.3,
                    false,
                    4,
                );
                if let Some(lag) = lag {
                    *lm = ((lag + f64::from(lead)) * ms_per_lag).max(0.0);
                    measured += 1;
                }
            }

            if measured > 0 {
                // how long a sound takes from a Track to the master: the probe waits that long for its sweep
                let slowest_lag = lag_ms.iter().copied().fold(0.0, f64::max);
                self.shared
                    .slot_to_master_ms
                    .store((slowest_lag.min(500.0) as f32).to_bits(), Relaxed);
            }
            // two or more tracks: the latest one has no plug-in latency, the others are that much earlier
            if measured >= 2 {
                let latest = lag_ms.iter().copied().fold(0.0, f64::max);
                for (i, &lm) in lag_ms.iter().enumerate() {
                    if lm >= 0.0 {
                        b.slots[i]
                            .chainLatencyBits
                            .store(float_bits((latest - lm) as f32), Relaxed);
                    }
                }
            }
            // every active track's remembered chain latency (live: a lone vocal keeps its measured value)
            let chain = |i: usize| f64::from(bits_float(b.slots[i].chainLatencyBits.load(Relaxed)));
            let slowest = (0..kMaxSlots)
                .filter(|&i| b.slots[i].state.load(Acquire) == SlotState::kSlotActive)
                .map(chain)
                .fold(0.0, f64::max);
            out.tracks_ms = slowest;
            // master plug-ins = how late a track arrives minus the line-up delay the DAW gave it
            for (i, &lm) in lag_ms.iter().enumerate() {
                if lm < 0.0 {
                    continue;
                }
                let mm = (lm - (slowest - chain(i))).max(0.0);
                out.master_ms = if out.master_ms < 0.0 {
                    mm
                } else {
                    out.master_ms.min(mm)
                };
            }

            self.friend_echo_and_leak(b, &hist, w, m, n, k, kneg, &y, y_start, ey, sound);
        }
        self.pair_friend_inputs(b, &hist, w, m, n, k, rate, ms_per_lag, have_hist);
        out
    }

    /// Speaker leak (what friends hear leaked back into a friend's mic) and echo in the master (a friend's
    /// voice reaching the master while the Hub plays them directly), per live friend.
    #[allow(clippy::too_many_arguments)]
    fn friend_echo_and_leak(
        &self,
        b: &BusLayout,
        hist: &History,
        w: u64,
        m: usize,
        n: usize,
        k: usize,
        kneg: usize,
        y: &[f32],
        y_start: usize,
        ey: f64,
        sound: bool,
    ) {
        let energy = |v: &[f32], cum: &mut Vec<f64>| {
            cum.clear();
            cum.push(0.0);
            let mut acc = 0.0f64;
            for &s in v {
                acc += f64::from(s) * f64::from(s);
                cum.push(acc);
            }
        };
        let (mut f_mix, mut cum_mix) = (Vec::new(), Vec::new());
        hist.copy_last(&hist.friend_mix, w, m, &mut f_mix);
        energy(&f_mix, &mut cum_mix);
        let e_mix = cum_mix[y_start + n] - cum_mix[y_start];
        let mix_sound = e_mix > n as f64 * 1e-6;
        let (mut f_mic, mut cum_mic) = (Vec::new(), Vec::new());
        for j in 0..kMaxFriends {
            let fh = &b.friends.0[j];
            let info = &self.shared.friend_info[j];
            let fid = fh.id.load(Acquire);
            if fid == 0 || fh.state.load(Relaxed) != FriendState::kFriendLive {
                info.leak_detected.store(0, Relaxed);
                info.echo_in_master.store(0, Relaxed);
                continue;
            }
            hist.copy_last(hist.friend_ring(j), w, m, &mut f_mic);
            energy(&f_mic, &mut cum_mic);
            let e_mic = cum_mic[y_start + n] - cum_mic[y_start];
            let mic_sound = e_mic > n as f64 * 1e-6;
            // (1) speaker leak: the mix sent to the friend came back through the friend's mic (positive lag)
            if mix_sound && mic_sound {
                let (lag, _) = best_lag(
                    &f_mic, &f_mix, n, y_start, 0, k as i32, &cum_mix, e_mic, 0.4, true, 4,
                );
                info.leak_detected.store(u32::from(lag.is_some()), Relaxed);
            } else if mic_sound {
                info.leak_detected.store(0, Relaxed);
            }
            // (2) echo in the master: the friend's voice in the master bus while the route is Direct
            if fh.route.load(Relaxed) == FriendRoute::kRouteDirect {
                if sound && mic_sound {
                    let (lag, _) = best_lag(
                        y,
                        &f_mic,
                        n,
                        y_start,
                        -(kneg as i32),
                        k as i32,
                        &cum_mic,
                        ey,
                        0.4,
                        true,
                        4,
                    );
                    info.echo_in_master.store(u32::from(lag.is_some()), Relaxed);
                } else if mic_sound {
                    info.echo_in_master.store(0, Relaxed);
                }
            } else {
                info.echo_in_master.store(0, Relaxed);
            }
        }
    }

    /// How late (ms) one Track's (or App Audio's, `source = true`) signal reaches the master bus right
    /// now, `None` = not found (silent or not correlated above `min_score`). `any_polarity` also
    /// accepts an inverted copy (a microphone). The second value is the best correlation found, even
    /// when `None`. `faint`: a weak, filtered copy (music leaking from headphones into a mic, through
    /// noise suppressors): 1 s window, whitened, also up to 400 ms early (negative result).
    /// `remove_ms`: a direct copy of the signal that reaches the master this many ms late (a channel
    /// without a HEARASIDE Track, an FX return) is subtracted first, so it can't outvote the mic.
    pub fn measure_lag(
        &self,
        index: usize,
        min_score: f64,
        any_polarity: bool,
        source: bool,
        faint: bool,
        remove_ms: Option<f64>,
    ) -> (Option<f64>, f64) {
        let remove_ms = remove_ms.filter(|v| v.is_finite());
        let (hist, shm) = self.snapshot();
        if shm.is_none()
            || hist.cap_len == 0
            || index >= if source { kMaxSources } else { kMaxSlots }
        {
            return (None, 0.0);
        }
        let ring = if source {
            hist.source_ring(index)
        } else {
            hist.slot_ring(index)
        };
        let rate = hist.sample_rate / 4.0;
        let lead = (f64::from(hist.sync_safety.load(Relaxed))
            * f64::from(hist.last_block.load(Relaxed))
            / 4.0)
            .round() as i32;
        let n = (rate * if faint { 1.0 } else { 0.25 }) as usize;
        let k = (rate * 0.5) as usize;
        let kneg = (lead + 8) as usize + if faint { (rate * 0.4) as usize } else { 0 };
        let m = n + k + kneg;
        let w = hist.write.load(Acquire);
        if (w as usize) < m || m + 4096 > hist.cap_len as usize {
            return (None, 0.0);
        }
        let (mut x, mut y) = (Vec::new(), Vec::new());
        hist.copy_last(&hist.input, w, m, &mut y);
        hist.copy_last(ring, w, m, &mut x);
        if faint {
            // first difference: music is mostly bass, a mic held to headphones hears mostly the rest
            for i in (1..m).rev() {
                y[i] -= y[i - 1];
                x[i] -= x[i - 1];
            }
        }
        let y_start = m - kneg - n;
        if let Some(rm) = remove_ms {
            // y = g * x(n - kr) + the rest: take the best-fitting g over the window and subtract that copy
            let kr = rm * hist.sample_rate / 4000.0 - f64::from(lead);
            let k0 = kr.floor() as i64;
            let fr = kr - k0 as f64;
            let shifted = |nn: usize| -> f64 {
                let a = nn as i64 - k0;
                if a > 0 && a < m as i64 {
                    (1.0 - fr) * f64::from(x[a as usize]) + fr * f64::from(x[(a - 1) as usize])
                } else {
                    0.0
                }
            };
            let (mut yx, mut xx) = (0.0f64, 0.0f64);
            #[allow(clippy::needless_range_loop)]
            for nn in y_start..y_start + n {
                let s = shifted(nn);
                yx += f64::from(y[nn]) * s;
                xx += s * s;
            }
            if xx > 0.0 {
                let g = yx / xx;
                for (nn, yv) in y.iter_mut().enumerate() {
                    *yv -= (g * shifted(nn)) as f32;
                }
            }
        }
        let ey: f64 = y[y_start..y_start + n]
            .iter()
            .map(|&v| f64::from(v) * f64::from(v))
            .sum();
        let mut cum = vec![0.0f64; m + 1];
        for j in 0..m {
            cum[j + 1] = cum[j] + f64::from(x[j]) * f64::from(x[j]);
        }
        let floor = if faint { 1e-9 } else { 1e-7 }; // the difference of a quiet bleed is quieter still
        if !at_least(ey, n as f64 * floor) || !at_least(cum[m], m as f64 * floor) {
            return (None, 0.0);
        }
        let (lag, score) = best_lag(
            &y,
            &x,
            n,
            y_start,
            -(kneg as i32),
            k as i32,
            &cum,
            ey,
            min_score,
            any_polarity,
            if faint { 1 } else { 4 },
        );
        let Some(lag) = lag else { return (None, score) };
        let ms = (lag + f64::from(lead)) * 4000.0 / hist.sample_rate;
        (Some(if faint { ms } else { ms.max(0.0) }), score)
    }

    pub fn friend_control(&self, j: usize) -> &FriendControl {
        &self.shared.friend_ctl[j]
    }

    pub fn friend_info(&self, j: usize) -> &FriendInfo {
        &self.shared.friend_info[j]
    }

    /// Line up friends for viewers: the Stream Mix waits D = the slowest friend (at most `limit_ms`).
    pub fn set_line_up(&self, on: bool, limit_ms: f32) {
        self.shared
            .line_up_limit_bits
            .store(limit_ms.max(0.0).to_bits(), Relaxed);
        self.shared
            .line_up_on
            .store(on, std::sync::atomic::Ordering::Release);
    }

    /// D now (ms).
    pub fn line_up_ms(&self) -> f32 {
        bits_float(self.shared.line_up_ms_bits.load(Relaxed))
    }

    /// The friend that sets D, or -1.
    pub fn line_up_slowest(&self) -> i32 {
        self.shared.line_up_slowest.load(Relaxed)
    }

    /// Counts changes of the Line-up delay (read `line_up_ms` for the value).
    pub fn line_up_changes(&self) -> u32 {
        self.shared.line_up_changes.load(Relaxed)
    }

    /// The bus the engine is connected to (shared with it), if any.
    pub fn shared_memory(&self) -> Option<Arc<SharedMemory>> {
        self.snapshot_bus()
    }

    /// The last block the audio thread processed (frames).
    pub fn last_block_size(&self) -> i32 {
        self.snapshot().0.last_block.load(Relaxed)
    }

    pub fn probing(&self) -> bool {
        self.shared.probe_feeder.load(Acquire) >= 0
    }

    /// Frames of history analysed so far, at the sample rate (moves with the audio, not the clock).
    pub fn history_frames(&self) -> u64 {
        self.snapshot().0.write.load(Acquire) * 4
    }

    /// How the Hub has been reading an App Audio's signal since the last call.
    pub fn take_source_read_stats(&self, index: usize) -> SourceReadStats {
        let hist = self.snapshot().0;
        let Some(s) = hist.src_stats.get(index) else {
            return SourceReadStats::default();
        };
        SourceReadStats {
            lead_min: s.lead_min.swap(i64::MAX, Relaxed),
            lead_max: s.lead_max.swap(i64::MIN, Relaxed),
            jumps: s.jumps.swap(0, Relaxed),
            blocks: s.blocks.swap(0, Relaxed),
        }
    }
}
