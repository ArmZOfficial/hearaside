//! `HubEngine`: lives in the HEARASIDE Hub on the master bus. Port of the audio core of
//! `libs/ssengine/src/hub_engine.cpp` (connect / maintain / prepare / process: re-anchor, sync safety,
//! viewer delay, crossfade, solo, panic, stems, preview, limiter, loudness, latency history).
//!
//! Still to come in later tasks (hooks are in place and documented where they plug in):
//! - the friends' direct path (`friends_*`, FriendReader, Line up): R2.3 (`friends` module stubs here)
//! - auto-sync measuring, pairing and the probe (`measureLatencies`, `measureLag`, `pairWith`, ...): R2.2b
//!
//! Ownership: `process` takes `&mut self`, like every other method that changes state, so a plug-in
//! wrapper has to serialise message-thread calls with the audio thread (try-lock on the audio side).
//! This replaces the C++'s mix of atomics and retired mappings; the wrapper decides how to share.
use hs_bus::bus::{bits_float, find_timeline_position, float_bits, random_token, release_hub};
use hs_bus::protocol::*;
use hs_bus::ring::{ReadResult, ring_read_at, ring_write};
use hs_bus::shm::{BeaconMap, SharedMemory, now_ns};
use hs_dsp::dsp::{
    Limiter, LoudnessMeter, Smoother, block_peak, db_to_gain, pan_gains, vector_add_scaled,
    vector_multiply,
};
use std::sync::Arc;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering::{Acquire, Relaxed, Release};

use crate::measure::{FriendControl, FriendInfo, History, HubMeasurer, Shared};

#[path = "friends.rs"]
mod friends;
use crate::track_publisher::NO_TIME;
pub use friends::FriendTap;
use friends::{DelayLine, FriendPlayer};
use std::sync::Mutex;

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// another Hub may take over after 2 s silence
pub(crate) const HUB_STALE_NS: u64 = 2_000_000_000;
pub(crate) const SLOT_STALE_NS: u64 = 5_000_000_000;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HubParams {
    pub master_db: f32,
    pub limiter_on: bool,
    pub ceiling_db: f32,
    pub preview: bool,
    pub panic: bool,
    /// extra blocks of latency (0..2)
    pub sync_safety: i32,
    pub bypassed: bool,
    /// nothing to the viewers (auto sync is measuring); the master bus is still analysed
    pub silence: bool,
}

impl Default for HubParams {
    fn default() -> Self {
        HubParams {
            master_db: 0.0,
            limiter_on: true,
            ceiling_db: -1.0,
            preview: false,
            panic: false,
            sync_safety: 0,
            bypassed: false,
            silence: false,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Disconnected,
    Owner,
    Secondary,
}

/// The sweep a friend input plays when the Hub probes it (300 ms, logarithmic, -24 dBFS peak).
pub fn probe_sweep(sample_rate: f64) -> Vec<f32> {
    use std::f64::consts::PI;
    let n = (sample_rate * 0.3) as i32;
    let (f0, f1, t_len) = (200.0f64, 2000.0f64, 0.3f64);
    let k = (f1 / f0).ln(); // low enough to survive the tap's decimation by 4
    let fade = (sample_rate * 0.005) as i32;
    (0..n.max(0))
        .map(|i| {
            let t = f64::from(i) / sample_rate;
            let mut a = 0.0631 * (2.0 * PI * f0 * t_len / k * ((t * k / t_len).exp() - 1.0)).sin(); // -24 dBFS
            if i < fade {
                a *= 0.5 - 0.5 * (PI * f64::from(i) / f64::from(fade)).cos();
            }
            if n - 1 - i < fade {
                a *= 0.5 - 0.5 * (PI * f64::from(n - 1 - i) / f64::from(fade)).cos();
            }
            a as f32
        })
        .collect()
}

#[derive(Clone, Default)]
struct SlotRt {
    known: bool,
    epoch: u32,
    claim_ns: u64,
    need_anchor: bool,
    /// FIFO position aligned with the Hub "now"
    cursor: u64,
    /// current delay in frames
    delay: u32,
    // crossfade from an old read position (seek / delay change / re-anchor)
    xf_from: u64,
    xf_remain: u32,
    xf_active: bool,
    mismatch: i32,
    ahead_blocks: i32,
    timeline_locked: bool,
    // stems
    gain: Smoother,
    pan_l: Smoother,
    pan_r: Smoother,
    /// headphones
    mon: Smoother,
    peak: [f32; 2],
}

impl SlotRt {
    fn fresh() -> Self {
        SlotRt {
            need_anchor: true,
            ..Default::default()
        }
    }
}

type Stereo = [Vec<f32>; 2];

/// The per-chunk buffers `process_chunk` takes out of `self` and lends to `friends_mix`.
pub(crate) struct ChunkBuffers<'a> {
    pub outs: &'a mut [Stereo; kNumStreamOuts],
    pub late: &'a mut [Stereo; kNumStreamOuts],
    pub voice: &'a mut [Vec<f32>; kMaxFriends],
    pub phones: &'a mut Stereo,
}

fn stereo(n: usize) -> Stereo {
    [vec![0.0; n], vec![0.0; n]]
}

pub struct HubEngine {
    shm: Option<Arc<SharedMemory>>,
    bus_name: String,
    /// bumped on (re)connect so the audio side resets slot state
    gen_: u32,
    seen_gen: u32,

    sample_rate: f64,
    max_block: usize,
    sync_safety: i32,
    xf_len: u32,

    slots: Vec<SlotRt>,
    tmp_a: Stereo,
    tmp_b: Stereo,
    phones: Stereo,
    /// per-sample viewer signal of the slot being mixed (left, right)
    scratch: Stereo,
    // history for the latency measuring (decimated by 4): written here, read by the measuring task (R2.2b)
    slot_block: Vec<f32>,
    /// the decimated history, read by the measuring thread (see `measure.rs`)
    hist: Arc<History>,
    shared: Arc<Shared>,
    slot_live: [bool; kMaxSlots],
    dec_slot: [f32; kMaxSlots],
    quiet: [u32; kMaxSlots],
    src_cursor: [u64; kMaxSources],
    src_known: [bool; kMaxSources],
    dec_src: [f32; kMaxSources],
    src_quiet: [u32; kMaxSources],
    dec_in: f32,
    dec_count: i32,

    outs: [Stereo; kNumStreamOuts],
    out_used: [bool; kNumStreamOuts],
    limiters: Vec<Limiter>,
    loudness: LoudnessMeter,
    master: Smoother,
    preview: Smoother,
    /// what friends hear fades out and in around a probe
    friend_mix_gain: Smoother,
    out_peak: [[f32; 2]; kNumStreamOuts],

    // friends' side of the mix (filled by R2.3; zero until then)
    late: [Stereo; kNumStreamOuts],
    late_any: bool,
    voice: [Vec<f32>; kMaxFriends],
    used_tot: [u32; kMaxFriends],
    line_d: u32,
    line_old_d: u32,
    line_xf_remain: u32,
    line_hold: u32,
    /// each friend's own delay (frames) D was set against, before the crossfade
    old_tot: [u32; kMaxFriends],
    friends: Vec<FriendPlayer>,
    /// the Stream Mix and the stems, delayed by D
    out_delay: [DelayLine; kNumStreamOuts],
    delay_scratch: Stereo,
    delay_cap: u32,
    dec_friend_mix: f32,
    dec_friends: [f32; kMaxFriends],
    friend_quiet: [u32; kMaxFriends],
    friend_tap: Option<FriendTap>,

    was_playing: bool,
    expected_time: i64,
    block_counter: u64,
    active_slots_mask: u64,
    scan_countdown: u32,
    was_owner: bool,
    was_offline: bool,
}

impl Default for HubEngine {
    fn default() -> Self {
        Self::new()
    }
}

impl HubEngine {
    pub fn new() -> Self {
        let hist0 = Arc::new(History::new(48000.0, 0));
        HubEngine {
            shm: None,
            bus_name: String::new(),
            gen_: 0,
            seen_gen: u32::MAX,
            sample_rate: 48000.0,
            max_block: 0,
            sync_safety: 0,
            xf_len: 240,
            slots: vec![SlotRt::fresh(); kMaxSlots],
            tmp_a: Stereo::default(),
            tmp_b: Stereo::default(),
            phones: Stereo::default(),
            scratch: Stereo::default(),
            slot_block: Vec::new(),
            hist: Arc::clone(&hist0),
            shared: Arc::new(Shared::new(hist0, random_token())),
            slot_live: [false; kMaxSlots],
            dec_slot: [0.0; kMaxSlots],
            quiet: [0; kMaxSlots],
            src_cursor: [0; kMaxSources],
            src_known: [false; kMaxSources],
            dec_src: [0.0; kMaxSources],
            src_quiet: [0; kMaxSources],
            dec_in: 0.0,
            dec_count: 0,
            outs: std::array::from_fn(|_| Stereo::default()),
            out_used: [false; kNumStreamOuts],
            limiters: (0..kNumStreamOuts).map(|_| Limiter::default()).collect(),
            loudness: LoudnessMeter::default(),
            master: Smoother::default(),
            preview: Smoother::default(),
            friend_mix_gain: Smoother::default(),
            out_peak: [[0.0; 2]; kNumStreamOuts],
            late: std::array::from_fn(|_| Stereo::default()),
            late_any: false,
            voice: std::array::from_fn(|_| Vec::new()),
            used_tot: [0; kMaxFriends],
            line_d: 0,
            line_old_d: 0,
            line_xf_remain: 0,
            line_hold: 0,
            old_tot: [0; kMaxFriends],
            friends: Vec::new(),
            out_delay: std::array::from_fn(|_| DelayLine::default()),
            delay_scratch: Stereo::default(),
            delay_cap: 0,
            dec_friend_mix: 0.0,
            dec_friends: [0.0; kMaxFriends],
            friend_quiet: [0; kMaxFriends],
            friend_tap: None,
            was_playing: false,
            expected_time: NO_TIME,
            block_counter: 0,
            active_slots_mask: 0,
            scan_countdown: 0,
            was_owner: false,
            was_offline: false,
        }
    }

    // ---- message thread ------------------------------------------------------------------

    pub fn connect(&mut self, bus_name: &str) -> bool {
        self.disconnect();
        self.bus_name = if bus_name.is_empty() {
            "Main".into()
        } else {
            bus_name.into()
        };
        let Ok(shm) = SharedMemory::open(&self.bus_name) else {
            return false;
        };
        // the history belongs to the previous bus: measuring it would write latencies into another bus' Tracks
        if self.hist.cap_len > 0 {
            self.reset_history(self.hist.sample_rate, self.hist.cap_len);
        }
        let shm = Arc::new(shm);
        *lock(&self.shared.shm) = Some(Arc::clone(&shm));
        self.shm = Some(shm);
        // best effort: without it only the version hint is missing
        *lock(&self.shared.beacon) = BeaconMap::open(&self.bus_name);
        self.gen_ = self.gen_.wrapping_add(1);
        self.maintain();
        true
    }

    pub fn disconnect(&mut self) {
        {
            // a late probe step must not write the stage after this
            let _probe = lock(&self.shared.probe);
            self.shared.probe_feeder.store(-1, Release);
            self.shared.probe_stage.store(0, Relaxed);
            self.shared
                .slot_to_master_ms
                .store(200.0f32.to_bits(), Relaxed);
        }
        *lock(&self.shared.shm) = None;
        if let Some(shm) = self.shm.take() {
            release_hub(shm.layout(), self.shared.token);
        }
        *lock(&self.shared.beacon) = None;
        self.shared.role.store(0, Release);
    }

    /// Call periodically (e.g. 2 Hz): claims ownership if free/stale, reclaims dead slots. The work is done by
    /// `HubMeasurer::maintain`, which any non-audio thread can call without stopping the audio side.
    pub fn maintain(&mut self) {
        self.measurer().maintain();
    }

    pub fn prepare(&mut self, sample_rate: f64, max_block: usize) {
        self.sample_rate = if sample_rate > 0.0 {
            sample_rate
        } else {
            48000.0
        };
        self.max_block = max_block.clamp(16, kRingFrames as usize / 8);
        let mb = self.max_block;
        self.tmp_a = stereo(mb);
        self.tmp_b = stereo(mb);
        self.phones = stereo(mb);
        self.scratch = stereo(mb);
        for o in &mut self.outs {
            *o = stereo(mb);
        }
        for o in &mut self.late {
            *o = stereo(mb);
        }
        for v in &mut self.voice {
            *v = vec![0.0; mb];
        }
        let sr = self.sample_rate;
        let cap_len = (sr * 0.625) as u32 + 1; // 2.5 s at sample rate / 4
        self.slot_block = vec![0.0; kMaxSlots * mb];
        self.reset_history(sr, cap_len);
        for l in &mut self.limiters {
            l.prepare(sr, 1.0, 120.0);
        }
        self.loudness.prepare(sr);
        self.master.prepare(sr, 20.0);
        self.friend_mix_gain.prepare(sr, 20.0);
        self.friend_mix_gain.snap(1.0);
        self.preview.prepare(sr, 20.0);
        self.xf_len = (sr * 0.010).max(16.0) as u32;
        for s in &mut self.slots {
            *s = SlotRt::fresh();
            s.gain.prepare(sr, 20.0);
            s.pan_l.prepare(sr, 20.0);
            s.pan_l.snap(1.0);
            s.pan_r.prepare(sr, 20.0);
            s.pan_r.snap(1.0);
            s.mon.prepare(sr, 10.0);
        }
        self.friends_prepare();
        self.active_slots_mask = 0;
        self.scan_countdown = 0;
        self.out_used = [false; kNumStreamOuts];
        self.was_playing = false;
        self.expected_time = NO_TIME;
    }

    /// A fresh, all-zero history (a measuring thread keeps reading the old one until it takes the new one).
    fn reset_history(&mut self, sample_rate: f64, cap_len: u32) {
        // a probe in progress waits for history positions that a fresh history never reaches (the C++ kept the
        // viewers muted until its 10 s timeout)
        self.measurer().abort_probe();
        let hist = Arc::new(History::new(sample_rate, cap_len));
        *lock(&self.shared.hist) = Arc::clone(&hist);
        self.hist = hist;
        self.src_known = [false; kMaxSources];
        self.dec_src = [0.0; kMaxSources];
        self.src_quiet = [cap_len; kMaxSources];
        self.dec_slot = [0.0; kMaxSlots];
        self.slot_live = [false; kMaxSlots];
        self.quiet = [cap_len; kMaxSlots]; // the history starts out all zero
        self.dec_in = 0.0;
        self.dec_count = 0;
    }

    /// 0 = Stream Mix, 1..8 stems.
    pub fn set_output_name(&self, output: usize, name: &str) {
        let Some(shm) = &self.shm else { return };
        if output >= kNumStreamOuts {
            return;
        }
        let sh = &shm.layout().streamHeader;
        hs_bus::bus::set_stream_name(sh, output, name);
    }

    pub fn role(&self) -> Role {
        self.measurer().role()
    }

    pub fn bus(&self) -> Option<&BusLayout> {
        self.shm.as_deref().map(SharedMemory::layout)
    }

    pub fn bus_name(&self) -> &str {
        &self.bus_name
    }

    pub fn latency_frames(&self) -> i32 {
        self.limiters[0].latency()
    }

    pub fn last_block_size(&self) -> i32 {
        self.hist.last_block.load(Relaxed)
    }

    pub fn active_slots_mask(&self) -> u64 {
        self.active_slots_mask
    }

    /// Analysed history so far, in frames at the sample rate (moves with the audio, not the clock).
    pub fn history_frames(&self) -> u64 {
        self.hist.write.load(Acquire) * 4
    }

    pub fn friend_control(&self, j: usize) -> &FriendControl {
        &self.shared.friend_ctl[j]
    }

    pub fn friend_info(&self, j: usize) -> &FriendInfo {
        &self.shared.friend_info[j]
    }

    /// Line up friends for viewers: the Stream Mix waits D = the slowest friend (at most `limit_ms`).
    pub fn set_line_up(&self, on: bool, limit_ms: f32) {
        self.measurer().set_line_up(on, limit_ms);
    }

    /// Audio tap for recording / metering friends (called on the audio thread during the friends' mix).
    pub fn set_friend_tap(&mut self, tap: Option<FriendTap>) {
        self.friend_tap = tap;
    }

    /// Handle for the thread that measures latencies and lags (auto sync); cheap to clone.
    pub fn measurer(&self) -> HubMeasurer {
        HubMeasurer {
            shared: Arc::clone(&self.shared),
        }
    }

    // ---- audio thread --------------------------------------------------------------------

    fn start_crossfade(xf_len: u32, s: &mut SlotRt, old_read_pos: u64) {
        s.xf_from = old_read_pos;
        s.xf_remain = xf_len;
        s.xf_active = true;
    }

    fn anchor_for(
        &self,
        b: &BusLayout,
        i: usize,
        n: usize,
        time_samples: i64,
        use_timeline: bool,
    ) -> (u64, bool) {
        let sh = &b.slots[i];
        if use_timeline
            && let Some(pos) =
                find_timeline_position(sh, &b.slotTags[i], time_samples, kTagRingSize)
        {
            return (pos, true);
        }
        let w = sh.writePos.0.load(Acquire);
        let back = n as u64 * (1 + self.sync_safety.clamp(0, 2) as u64);
        (w.saturating_sub(back), false)
    }

    /// `io` in: the master bus = Stream Mix (one or two channels). `io` out: the headphone mix. Writes
    /// the Stream Mix and the stems to the bus.
    pub fn process(
        &mut self,
        io: &mut [&mut [f32]],
        p: &HubParams,
        time_samples: i64,
        playing: bool,
        offline: bool,
    ) {
        let Some(shm) = self.shm.clone() else { return };
        let b = shm.layout();
        let nch = io.len().min(kMaxChannels);
        let n = io.iter().map(|c| c.len()).min().unwrap_or(0);
        if self.max_block == 0 || n == 0 || nch == 0 {
            return;
        }
        let h = &b.header;

        let owner = h.hubOwnerToken.load(Acquire) == self.shared.token;
        if !owner {
            self.was_owner = false; // secondary Hub: pass through only
            return;
        }

        if !self.was_owner || self.gen_ != self.seen_gen {
            self.seen_gen = self.gen_;
            self.was_owner = true;
            for s in &mut self.slots {
                s.known = false;
                s.need_anchor = true;
                s.xf_active = false;
            }
            for l in &mut self.limiters {
                l.reset();
            }
            self.loudness.reset();
            // the App Audio cursors belong to the previous bus (the C++ kept them)
            self.src_known = [false; kMaxSources];
            self.friends_reset();
        }

        let sr = (self.sample_rate + 0.5) as u32;
        h.hubHeartbeatNs.store(now_ns(), Relaxed);
        h.hubSampleRate.store(sr, Relaxed);
        h.hubBlockSize.store(n as u32, Relaxed);
        self.sync_safety = p.sync_safety.clamp(0, 2);
        self.hist.sync_safety.store(self.sync_safety, Relaxed);
        // the stream itself is live
        h.hubLatencyFrames
            .store(self.limiters[0].latency() as u32, Relaxed);
        let flags = if p.preview { HubFlags::kHubPreview } else { 0 }
            | if p.panic { HubFlags::kHubPanic } else { 0 }
            | if offline { HubFlags::kHubOffline } else { 0 }
            | if p.bypassed {
                HubFlags::kHubBypassed
            } else {
                0
            }
            | if p.limiter_on {
                HubFlags::kHubLimiter
            } else {
                0
            };
        h.hubFlags.store(flags, Relaxed);

        if offline || p.bypassed {
            b.streamHeader.active.store(0, Release);
            self.was_offline = true;
            return;
        }
        if self.was_offline {
            self.was_offline = false;
            for s in &mut self.slots {
                s.need_anchor = true;
            }
        }
        b.streamHeader.sampleRate.store(sr, Relaxed);

        let mut off = 0;
        while off < n {
            let m = self.max_block.min(n - off);
            let t = if time_samples == NO_TIME {
                NO_TIME
            } else {
                time_samples + off as i64
            };
            match io {
                [a] => self.process_chunk(b, &mut [&mut a[off..off + m]], m, p, t, playing),
                [a, c, ..] => self.process_chunk(
                    b,
                    &mut [&mut a[off..off + m], &mut c[off..off + m]],
                    m,
                    p,
                    t,
                    playing,
                ),
                [] => {}
            }
            off += m;
        }
        b.streamHeader.active.store(1, Release);
        self.block_counter += 1;
    }

    #[allow(clippy::too_many_lines)]
    fn process_chunk(
        &mut self,
        b: &BusLayout,
        io: &mut [&mut [f32]],
        n: usize,
        p: &HubParams,
        time_samples: i64,
        playing: bool,
    ) {
        let num_ch = io.len();
        let sr = (self.sample_rate + 0.5) as u32;
        let has_time = time_samples != NO_TIME;
        let use_timeline = playing && has_time;
        // With sync safety the Hub mixes "L samples in the past", so tracks that the host processes
        // after the master bus (or on another thread) have already published that part of the timeline.
        let mix_time = if has_time {
            time_samples - i64::from(self.sync_safety) * n as i64
        } else {
            NO_TIME
        };

        // ---- transport events -> re-anchor every slot ----------------------------------------
        let reanchor_all = playing != self.was_playing
            || (use_timeline
                && self.expected_time != NO_TIME
                && time_samples != self.expected_time);
        self.was_playing = playing;
        self.expected_time = if use_timeline {
            time_samples + n as i64
        } else {
            NO_TIME
        };

        // The buffers are taken out of `self` for the chunk so the slot loop can borrow `self` freely.
        let mut tmp_a = std::mem::take(&mut self.tmp_a);
        let mut tmp_b = std::mem::take(&mut self.tmp_b);
        let mut phones = std::mem::take(&mut self.phones);
        let mut scratch = std::mem::take(&mut self.scratch);
        let mut outs = std::mem::take(&mut self.outs);
        let mut late = std::mem::take(&mut self.late);
        let mut voice = std::mem::take(&mut self.voice);
        let mut slot_block = std::mem::take(&mut self.slot_block);

        for c in &mut phones {
            c[..n].fill(0.0);
        }
        if self.late_any {
            for o in &mut late {
                for c in o {
                    c[..n].fill(0.0);
                }
            }
        }
        self.late_any = false;
        for v in &mut voice {
            v[..n].fill(0.0);
        }
        let mut carried = [-1i32; kMaxSlots]; // the friend slot a Track at the end of a channel carries
        let mut live = [0usize; kMaxSlots]; // slots that feed the latency history this chunk
        let mut num_live = 0;
        self.hist.last_block.store(n as i32, Relaxed);
        let mut used = [false; kNumStreamOuts]; // a stem is zeroed the first time a slot writes to it
        used[0] = true;
        // The Stream Mix is the master bus itself: every Track sends what viewers hear to the DAW, and
        // the plug-ins above the Hub have already processed it.
        for c in 0..2 {
            outs[0][c][..n].copy_from_slice(&io[c.min(num_ch - 1)][..n]);
        }

        // ---- update active slot bitmask -------------------------------------------------------
        // Refresh periodically (every 16 blocks) or on reanchor to discover newly claimed slots.
        // Inactive slots found during the loop are cleared from the bitmask immediately.
        if self.shared.rescan.swap(false, Relaxed) {
            self.scan_countdown = 0; // maintain() asked for a scan (newly claimed slots)
        }
        let mut active_mask = self.active_slots_mask;
        if reanchor_all || self.scan_countdown == 0 {
            self.scan_countdown = 16;
            active_mask = 0;
            for i in 0..kMaxSlots {
                if b.slots[i].state.load(Acquire) == SlotState::kSlotActive {
                    active_mask |= 1 << i;
                } else if self.slots[i].known {
                    let s = &mut self.slots[i];
                    s.known = false;
                    s.need_anchor = true;
                    s.xf_active = false;
                    s.gain.snap(0.0);
                    s.mon.snap(0.0);
                }
            }
            self.active_slots_mask = active_mask;
        } else {
            self.scan_countdown -= 1;
        }

        // ---- solo (stems follow the viewers' solo like the Tracks do) --------------------------
        let mut solo_active = false;
        let mut solo_mask = active_mask;
        while solo_mask != 0 {
            let i = solo_mask.trailing_zeros() as usize;
            solo_mask &= solo_mask - 1;
            let sh = &b.slots[i];
            if sh.state.load(Acquire) != SlotState::kSlotActive {
                active_mask &= !(1 << i);
                self.active_slots_mask = active_mask;
                continue;
            }
            let f = sh.flags.load(Relaxed);
            if f & SlotFlags::kFlagSolo != 0
                && f & SlotFlags::kFlagStr != 0
                && sh.sampleRate.load(Relaxed) == sr
            {
                solo_active = true;
                break;
            }
        }
        solo_active = solo_active || self.friends_solo_active(b); // a friend soloed for the viewers counts too

        let max_delay = ((0.5 * self.sample_rate + 0.5) as u32)
            .min(kRingFrames - kGuardFrames - 4 * self.max_block as u32);
        let ahead_threshold = (self.sync_safety as u64) * n as u64
            + (2.0 * n as f64).max(0.020 * self.sample_rate) as u64;
        let pk_decay = (-(n as f32) / (0.3 * sr as f32)).exp();

        let mut slot_iter = active_mask;
        while slot_iter != 0 {
            let i = slot_iter.trailing_zeros() as usize;
            slot_iter &= slot_iter - 1;
            let sh = &b.slots[i];
            if sh.state.load(Acquire) != SlotState::kSlotActive {
                active_mask &= !(1 << i);
                self.active_slots_mask = active_mask;
                let s = &mut self.slots[i];
                if s.known {
                    s.known = false;
                    s.need_anchor = true;
                    s.xf_active = false;
                    s.gain.snap(0.0);
                    s.mon.snap(0.0);
                }
                continue;
            }
            let epoch = sh.epoch.load(Acquire);
            let claim_ns = sh.claimNs.load(Relaxed);
            let mut fresh = false;
            {
                let s = &mut self.slots[i];
                if !s.known || epoch != s.epoch || claim_ns != s.claim_ns {
                    s.known = true;
                    s.epoch = epoch;
                    s.claim_ns = claim_ns;
                    s.need_anchor = true;
                    s.xf_active = false;
                    s.mismatch = 0;
                    s.ahead_blocks = 0;
                    s.gain.snap(0.0); // fade in from silence
                    s.mon.snap(0.0);
                    s.delay = 0;
                    s.peak = [0.0; 2]; // a new Track must not show the old Track's meter
                    fresh = true;
                }
            }
            if sh.sampleRate.load(Relaxed) != sr {
                sh.hubStatus
                    .store(SlotHubStatus::kHubStatusRateMismatch, Relaxed);
                sh.peakStreamBits[0].store(0, Relaxed);
                sh.peakStreamBits[1].store(0, Relaxed);
                self.slots[i].need_anchor = true;
                continue;
            }
            let f = sh.flags.load(Acquire);
            let mut status = SlotHubStatus::kHubStatusOk;

            // ---- anchoring / timeline lock -----------------------------------------------------
            if self.slots[i].need_anchor || reanchor_all {
                let (c, via_tl) = self.anchor_for(b, i, n, mix_time, use_timeline);
                let xf_len = self.xf_len;
                let s = &mut self.slots[i];
                if !fresh && !s.need_anchor && s.mon.current() > 0.0 {
                    Self::start_crossfade(xf_len, s, s.cursor);
                }
                s.cursor = c;
                s.timeline_locked = via_tl;
                s.need_anchor = false;
                s.mismatch = 0;
            } else if use_timeline && (self.block_counter & 3) == 0 {
                let hit = find_timeline_position(sh, &b.slotTags[i], mix_time, 256);
                let xf_len = self.xf_len;
                let s = &mut self.slots[i];
                s.timeline_locked = hit.is_some();
                match hit {
                    Some(pos) if pos != s.cursor => {
                        s.mismatch += 1;
                        if s.mismatch >= 2 {
                            Self::start_crossfade(xf_len, s, s.cursor);
                            s.cursor = pos;
                            s.mismatch = 0;
                        }
                    }
                    _ => s.mismatch = 0,
                }
            } else if !use_timeline {
                self.slots[i].timeline_locked = false;
            }

            carried[i] = -1;
            let delay_ms = bits_float(sh.strDelayBits.load(Relaxed));
            self.slots[i].delay =
                max_delay.min((delay_ms.max(0.0) * self.sample_rate as f32 * 0.001 + 0.5) as u32);
            if f & SlotFlags::kFlagViewersViaHub != 0 {
                let fid = sh.fedBy.load(Relaxed);
                // fid 0 = nobody: an empty friend entry has id 0 too. The flag and fedBy are two words, so a
                // Hub can always read the flag still set with fedBy already 0 (unpairing, or a crashed Hub).
                let friend_slot =
                    (0..kMaxFriends).find(|&j| fid != 0 && b.friends.0[j].id.load(Relaxed) == fid);
                carried[i] = friend_slot.map_or(-1, |j| j as i32);
                if let Some(j) = friend_slot {
                    let li_ms = bits_float(sh.fxLatencyBits.load(Relaxed));
                    let li_frames = (li_ms.max(0.0) * self.sample_rate as f32 * 0.001 + 0.5) as u32;
                    let friend_tot = self.used_tot[j].saturating_add(li_frames);
                    let line_delay = self.line_d.saturating_sub(friend_tot);
                    self.slots[i].delay = max_delay.min(line_delay);
                }
            }

            let s = &mut self.slots[i];
            let res = {
                let [a0, a1] = &mut tmp_a;
                ring_read_at(
                    &b.slotAudio.0[i],
                    &sh.writePos.0,
                    s.cursor,
                    &mut [&mut a0[..n], &mut a1[..n]],
                    n as u32,
                    None,
                )
            };
            if s.xf_active {
                {
                    let [b0, b1] = &mut tmp_b;
                    ring_read_at(
                        &b.slotAudio.0[i],
                        &sh.writePos.0,
                        s.xf_from,
                        &mut [&mut b0[..n], &mut b1[..n]],
                        n as u32,
                        None,
                    );
                }
                let done = self.xf_len - s.xf_remain;
                for j in 0..n {
                    let a = ((done + j as u32) as f32 / self.xf_len as f32).min(1.0);
                    for c in 0..2 {
                        tmp_a[c][j] = tmp_b[c][j] + (tmp_a[c][j] - tmp_b[c][j]) * a;
                    }
                }
                s.xf_from += n as u64;
                s.xf_remain = s.xf_remain.saturating_sub(n as u32);
                if s.xf_remain == 0 {
                    s.xf_active = false;
                }
            }
            s.cursor += n as u64;

            // a friend mixed in a DAW track: the other friends hear that track (your plug-ins on it),
            // not delayed, at its viewers level
            if carried[i] >= 0 {
                let gv = 0.5 * db_to_gain(bits_float(sh.strGainBits.load(Relaxed)));
                let vo = &mut voice[carried[i] as usize];
                for j in 0..n {
                    vo[j] += (tmp_a[0][j] + tmp_a[1][j]) * gv;
                }
            }

            let w = sh.writePos.0.load(Acquire);
            let lead = w.wrapping_sub(s.cursor) as i64;
            if res == ReadResult::Overrun {
                s.need_anchor = true;
                status |= SlotHubStatus::kHubStatusUnderrun;
            } else if lead < 0 {
                // The track was not processed this cycle (suspended / smart-disabled / frozen).
                s.need_anchor = true;
                status |= SlotHubStatus::kHubStatusUnderrun;
            }
            if !s.timeline_locked && lead > ahead_threshold as i64 {
                s.ahead_blocks = s.ahead_blocks.saturating_add(n as i32).min(sr as i32 + 1);
                if s.ahead_blocks > sr as i32 {
                    status |= SlotHubStatus::kHubStatusAhead;
                }
            } else {
                s.ahead_blocks = 0;
            }
            sh.hubLeadFrames
                .store(lead.clamp(0, i64::from(i32::MAX)) as u32, Relaxed);

            // ---- headphones: You Hear + Headphone Level (a bypassed Track passes its audio) ------
            s.mon.set_target(if f & SlotFlags::kFlagBypassed != 0 {
                1.0
            } else if f & SlotFlags::kFlagMon != 0 {
                db_to_gain(bits_float(sh.monTrimBits.load(Relaxed)))
            } else {
                0.0
            });
            if s.mon.is_smoothing() {
                for j in 0..n {
                    let gm = s.mon.next_value();
                    phones[0][j] += tmp_a[0][j] * gm;
                    phones[1][j] += tmp_a[1][j] * gm;
                }
            } else if s.mon.current() > 0.0 {
                let gm = s.mon.current();
                vector_add_scaled(&mut phones[0][..n], &tmp_a[0][..n], gm);
                vector_add_scaled(&mut phones[1][..n], &tmp_a[1][..n], gm);
            }

            // ---- stems + per-track viewers meter: gain / pan / solo / panic ----------------------
            let mut g = if f & SlotFlags::kFlagStr != 0 {
                db_to_gain(bits_float(sh.strGainBits.load(Relaxed)))
            } else {
                0.0
            };
            if solo_active && f & SlotFlags::kFlagSolo == 0 {
                g = 0.0;
            }
            if p.panic {
                g = 0.0;
            }
            s.gain.set_target(g);
            let (gl, gr) = pan_gains(bits_float(sh.strPanBits.load(Relaxed)));
            s.pan_l.set_target(gl);
            s.pan_r.set_target(gr);

            let stem = sh.stemIndex.load(Relaxed);
            let out_idx = if (0..kMaxStems as i32).contains(&stem) {
                1 + stem as usize
            } else {
                0
            };
            if out_idx > 0 && !used[out_idx] {
                used[out_idx] = true;
                for c in &mut outs[out_idx] {
                    c[..n].fill(0.0);
                }
            }
            // stems jump (no crossfade) when the viewers delay changes
            let use_b = s.delay > 0;
            if use_b {
                // (the C++ wraps around when the cursor is still inside the first block and reads silence)
                let from = s
                    .cursor
                    .saturating_sub(n as u64)
                    .saturating_sub(u64::from(s.delay));
                let [b0, b1] = &mut tmp_b;
                ring_read_at(
                    &b.slotAudio.0[i],
                    &sh.writePos.0,
                    from,
                    &mut [&mut b0[..n], &mut b1[..n]],
                    n as u32,
                    None,
                );
            }
            let v = if use_b { &tmp_b } else { &tmp_a };

            let (mut pk0, mut pk1) = (0.0f32, 0.0f32);
            let silent = !s.gain.is_smoothing() && s.gain.current() == 0.0;
            let mono = &mut slot_block[i * self.max_block..i * self.max_block + n]; // for the latency measuring
            live[num_live] = i;
            num_live += 1;
            if silent {
                mono.fill(0.0);
                // keep pan smoothers in sync even while silent
                for _ in 0..n {
                    s.pan_l.next_value();
                    s.pan_r.next_value();
                }
            } else {
                for j in 0..n {
                    let gg = s.gain.next_value();
                    let l = v[0][j] * gg * s.pan_l.next_value();
                    let r = v[1][j] * gg * s.pan_r.next_value();
                    scratch[0][j] = l;
                    scratch[1][j] = r;
                    mono[j] = l + r;
                    pk0 = pk0.max(l.abs());
                    pk1 = pk1.max(r.abs());
                }
                // a Track carrying a friend sends the viewers' side here, not to the DAW: it joins after
                // the Line-up delay (late), already placed at D - the friend's delay
                let via_hub = f & SlotFlags::kFlagViewersViaHub != 0;
                self.late_any = self.late_any || via_hub;
                let dst = if via_hub { &mut late } else { &mut outs };
                if out_idx > 0 {
                    for c in 0..2 {
                        for j in 0..n {
                            dst[out_idx][c][j] += scratch[c][j];
                        }
                    }
                }
                if via_hub && !p.panic {
                    for c in 0..2 {
                        for j in 0..n {
                            late[0][c][j] += scratch[c][j];
                        }
                    }
                }
            }
            s.peak[0] = pk0.max(s.peak[0] * pk_decay);
            s.peak[1] = pk1.max(s.peak[1] * pk_decay);
            sh.peakStreamBits[0].store(float_bits(s.peak[0]), Relaxed);
            sh.peakStreamBits[1].store(float_bits(s.peak[1]), Relaxed);
            sh.hubStatus.store(status, Relaxed);
        }

        // ---- history for measuring the master plug-ins' latency ------------------------------------
        let hist = Arc::clone(&self.hist);
        let cap_len = hist.cap_len;
        let w0 = if cap_len > 0 {
            hist.write.load(Relaxed)
        } else {
            0
        };
        let phase = self.dec_count;
        let mut w = w0;
        let put = |ring: &[AtomicU32], at: u64, v: f32| {
            ring[(at % u64::from(cap_len)) as usize].store(v.to_bits(), Relaxed);
        };
        if cap_len > 0 {
            // Each history sample sums 4 input samples. The work is per live slot rather than every
            // slot x every sample.
            {
                let (l, r) = (&outs[0][0], &outs[0][1]);
                let mut acc = self.dec_in;
                let mut k = phase;
                for j in 0..n {
                    acc += l[j] + r[j];
                    k += 1;
                    if k == 4 {
                        put(&hist.input, w, acc);
                        w += 1;
                        acc = 0.0;
                        k = 0;
                    }
                }
                self.dec_in = acc;
                self.dec_count = k;
            }
            let was_live = self.slot_live;
            self.slot_live = [false; kMaxSlots];
            for &i in &live[..num_live] {
                self.slot_live[i] = true;
                self.quiet[i] = 0;
                let x = &slot_block[i * self.max_block..i * self.max_block + n];
                let ring = hist.slot_ring(i);
                let mut acc = if was_live[i] { self.dec_slot[i] } else { 0.0 };
                let mut at = w0;
                let mut k = phase;
                for &xj in x {
                    acc += xj;
                    k += 1;
                    if k == 4 {
                        put(ring, at, acc);
                        at += 1;
                        acc = 0.0;
                        k = 0;
                    }
                }
                self.dec_slot[i] = acc;
            }
            // App Audio signals: read each one's newest block (it runs in the same DAW cycle as the Hub)
            for i in 0..kMaxSources {
                let src = &b.sources[i];
                let ring = hist.source_ring(i);
                if src.state.load(Acquire) != SlotState::kSlotActive {
                    self.src_known[i] = false;
                    if self.src_quiet[i] < cap_len {
                        let mut at = w0;
                        while at != w {
                            put(ring, at, 0.0);
                            at += 1;
                        }
                        self.src_quiet[i] += (w - w0) as u32;
                    }
                    continue;
                }
                let sw = src.writePos.0.load(Acquire);
                let cur = &mut self.src_cursor[i];
                let lead = sw.wrapping_sub(*cur) as i64;
                let stats = &hist.src_stats[i];
                if self.src_known[i] {
                    if lead < stats.lead_min.load(Relaxed) {
                        stats.lead_min.store(lead, Relaxed);
                    }
                    if lead > stats.lead_max.load(Relaxed) {
                        stats.lead_max.store(lead, Relaxed);
                    }
                    stats.blocks.fetch_add(1, Relaxed);
                }
                if !self.src_known[i]
                    || lead < n as i64
                    || lead > (8 * n as i64).max((self.sample_rate * 0.2) as i64)
                {
                    if self.src_known[i] {
                        stats.jumps.fetch_add(1, Relaxed);
                    }
                    *cur = sw.saturating_sub(n as u64);
                    self.src_known[i] = true;
                }
                {
                    let [t0, t1] = &mut tmp_b;
                    ring_read_at(
                        &b.sourceAudio.0[i],
                        &src.writePos.0,
                        *cur,
                        &mut [&mut t0[..n], &mut t1[..n]],
                        n as u32,
                        None,
                    );
                }
                *cur += n as u64;
                self.src_quiet[i] = 0;
                let mut acc = self.dec_src[i];
                let mut at = w0;
                let mut k = phase;
                for (x0, x1) in tmp_b[0][..n].iter().zip(&tmp_b[1][..n]) {
                    acc += x0 + x1;
                    k += 1;
                    if k == 4 {
                        put(ring, at, acc);
                        at += 1;
                        acc = 0.0;
                        k = 0;
                    }
                }
                self.dec_src[i] = acc;
            }
            // a slot that went quiet gets zero history until its whole ring is clear, then costs nothing
            for i in 0..kMaxSlots {
                if self.slot_live[i] || self.quiet[i] >= cap_len {
                    continue;
                }
                let ring = hist.slot_ring(i);
                let mut at = w0;
                while at != w {
                    put(ring, at, 0.0);
                    at += 1;
                }
                self.quiet[i] += (w - w0) as u32;
                self.dec_slot[i] = 0.0;
            }
        }

        let probe_mute = self.shared.probe_feeder.load(Relaxed) >= 0; // the Hub is listening for the sweep
        // ---- friends: headphones + Stream Mix (after the Line-up delay, before the limiter) -------------
        {
            let mut bufs = ChunkBuffers {
                outs: &mut outs,
                late: &mut late,
                voice: &mut voice,
                phones: &mut phones,
            };
            self.friends_mix(
                b,
                n,
                p,
                &used,
                solo_active,
                time_samples,
                playing,
                &mut bufs,
                w0,
                w,
                phase,
            );
        }
        if cap_len > 0 {
            hist.write.store(w, Release);
        }

        // ---- master gain, limiter, meters, write ---------------------------------------------------
        self.master
            .set_target(if p.panic || p.silence || probe_mute {
                0.0
            } else {
                db_to_gain(p.master_db)
            });
        for g in &mut tmp_b[0][..n] {
            *g = self.master.next_value();
        }

        let ceiling = db_to_gain(p.ceiling_db.clamp(-24.0, 0.0));
        let sh = &b.streamHeader;
        let decay = (-(n as f32) / (0.3 * sr as f32)).exp();
        for o in 0..kNumStreamOuts {
            let [l, r] = &mut outs[o];
            if used[o] {
                if !self.out_used[o] {
                    self.limiters[o].reset();
                }
                vector_multiply(&mut l[..n], &tmp_b[0][..n]);
                vector_multiply(&mut r[..n], &tmp_b[0][..n]);
                self.limiters[o].set_ceiling(ceiling);
                self.limiters[o].process(l, r, n, p.limiter_on);
            }
            self.out_used[o] = used[o];
            if used[o] {
                ring_write(
                    &b.streamAudio.0[o],
                    &sh.writePos.0[o],
                    &[&l[..n], &r[..n]],
                    n as u32,
                );
            } else {
                ring_write(&b.streamAudio.0[o], &sh.writePos.0[o], &[], n as u32);
            }
            self.out_peak[o][0] =
                (if used[o] { block_peak(&l[..n]) } else { 0.0 }).max(self.out_peak[o][0] * decay);
            self.out_peak[o][1] =
                (if used[o] { block_peak(&r[..n]) } else { 0.0 }).max(self.out_peak[o][1] * decay);
            sh.peakBits[o][0].store(float_bits(self.out_peak[o][0]), Relaxed);
            sh.peakBits[o][1].store(float_bits(self.out_peak[o][1]), Relaxed);
        }
        self.loudness.process(&outs[0][0], &outs[0][1], n);
        sh.loudnessMBits
            .store(float_bits(self.loudness.momentary()), Relaxed);
        sh.loudnessSBits
            .store(float_bits(self.loudness.short_term()), Relaxed);

        // ---- output: the headphone mix, or the Stream Mix while previewing (20 ms crossfade) --------
        for (c, out) in io.iter_mut().enumerate() {
            out[..n].copy_from_slice(&phones[c.min(1)][..n]);
        }
        self.preview
            .set_target(if p.preview && !probe_mute { 1.0 } else { 0.0 });
        if self.preview.is_smoothing() || self.preview.current() > 0.0 {
            for j in 0..n {
                let a = self.preview.next_value();
                for (c, out) in io.iter_mut().enumerate() {
                    let src = outs[0][c.min(1)][j];
                    out[j] += (src - out[j]) * a;
                }
            }
        }

        self.tmp_a = tmp_a;
        self.tmp_b = tmp_b;
        self.phones = phones;
        self.scratch = scratch;
        self.outs = outs;
        self.late = late;
        self.voice = voice;
        self.slot_block = slot_block;
    }
}

impl Drop for HubEngine {
    fn drop(&mut self) {
        self.disconnect();
    }
}
