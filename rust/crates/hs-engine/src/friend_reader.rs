//! Plays one friend's microphone from shared memory (`friendAudio[i]`, written by the Hub's share
//! server) into a block of the reader's own sample rate: a small jitter buffer that learns how much
//! headroom this connection needs, drift control against the friend's clock, and a resampler. Used by
//! the Hub engine (the friends it plays itself) and by HEARASIDE Track in "friend input" mode.
//! Port of `libs/ssengine/src/friend_reader.cpp`.
use hs_bus::protocol::{ChannelRing, kGuardFrames, kRingFrames, kRingMask};
use hs_bus::ring::{ReadResult, ring_read_at};
use hs_dsp::dsp::DriftController;
use hs_dsp::resampler::VarResampler;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering::Acquire;

const BINS: usize = 129;
const SUB: usize = 10;

pub struct FriendReader {
    sample_rate: f64,
    max_block: usize,
    rs_same: Option<VarResampler>,
    rs_48: Option<VarResampler>,
    rs_441: Option<VarResampler>,
    drift: DriftController,
    in_buf: [Vec<f32>; 2],
    cursor: u64,
    last_write: u64,
    starved_at: u64,
    base: u64,
    key: u32,
    primed: bool,
    muted: bool,
    played: bool,
    target: f64,
    block_fr: f64,
    fill_avg: f64,
    since_write: f64,
    min_margin: f64,
    window_sec: f64,
    hold_sec: f64,
    buffer_ms: f64,
    /// how long the buffer has been over its target without a silent stretch to drop
    skip_wait_sec: f64,
    // a short gap: `cursor` stays put, the output is silence (faded in / out) until a whole block is there again
    /// silent output frames so far
    muted_out: f64,
    /// input frames missing when it began
    missing: f64,
    fade_left: i32,
    /// last sample played
    last_out: [f32; 2],
    /// where the running fade-in started
    fade_from: [f32; 2],
    underruns: u32,
    reprimes: u32,
    concealed_ms: f64,
    // arrival jitter: lateness of each writePos advance against the earliest in the window, 1 ms bins, 10 x 0.5 s
    hist: [[u16; BINS]; SUB],
    lmin: [f64; SUB],
    adv_min: [f64; SUB],
    t: f64,
    sub_sec: f64,
    p95_ms: f64,
    cur: usize,
}

impl Default for FriendReader {
    fn default() -> Self {
        Self::new()
    }
}

impl FriendReader {
    pub fn new() -> Self {
        FriendReader {
            sample_rate: 48000.0,
            max_block: 256,
            rs_same: None,
            rs_48: None,
            rs_441: None,
            drift: DriftController::default(),
            in_buf: [Vec::new(), Vec::new()],
            cursor: 0,
            last_write: 0,
            starved_at: 0,
            base: 0,
            key: 0,
            primed: false,
            muted: false,
            played: false,
            target: 0.0,
            block_fr: 0.0,
            fill_avg: 0.0,
            since_write: 0.0,
            min_margin: 1.0e9,
            window_sec: 0.0,
            hold_sec: 0.0,
            buffer_ms: 0.0,
            skip_wait_sec: 0.0,
            muted_out: 0.0,
            missing: 0.0,
            fade_left: 0,
            last_out: [0.0; 2],
            fade_from: [0.0; 2],
            underruns: 0,
            reprimes: 0,
            concealed_ms: 0.0,
            hist: [[0; BINS]; SUB],
            lmin: [0.0; SUB],
            adv_min: [0.0; SUB],
            t: 0.0,
            sub_sec: 0.0,
            p95_ms: -1.0,
            cur: 0,
        }
    }

    /// Message thread: allocates (resamplers for the friend's 48 / 44.1 kHz or our own rate).
    pub fn prepare(&mut self, sample_rate: f64, max_block: usize) {
        self.sample_rate = if sample_rate > 0.0 {
            sample_rate
        } else {
            48000.0
        };
        self.max_block = max_block.max(16);
        let r = (self.sample_rate + 0.5) as u32;
        self.rs_same = VarResampler::new(r, r, 4).ok();
        self.rs_48 = VarResampler::new(48000, r, 4).ok();
        self.rs_441 = VarResampler::new(44100, r, 4).ok();
        for c in &mut self.in_buf {
            *c = vec![0.0; self.max_block * 4 + 128];
        }
        self.reset();
    }

    /// Audio thread: forget the position (a new friend, or after a gap): the next pull starts safe.
    pub fn reset(&mut self) {
        self.primed = false;
        self.key = 0;
        self.muted = false;
        self.played = false;
        self.underruns = 0;
        self.reprimes = 0;
        self.concealed_ms = 0.0;
        self.p95_ms = -1.0;
        self.skip_wait_sec = 0.0;
    }

    /// The jitter buffer in use right now (ms).
    pub fn buffer_ms(&self) -> f64 {
        self.buffer_ms
    }

    pub fn primed(&self) -> bool {
        self.primed
    }

    /// Short gaps concealed.
    pub fn underruns(&self) -> u32 {
        self.underruns
    }

    /// Long gaps and overruns while playing: restarts with a new position.
    pub fn reprimes(&self) -> u32 {
        self.reprimes
    }

    /// Silence played after the first sound, ms.
    pub fn concealed_ms(&self) -> f64 {
        self.concealed_ms
    }

    /// The p95 of how late data arrives against a steady flow over the last ~5 s (ms, -1 = not enough data yet).
    pub fn jitter_p95_ms(&self) -> f64 {
        self.p95_ms
    }

    /// Lateness of each writePos advance against the earliest one in the last ~5 s, in a fixed 1 ms
    /// histogram (no allocation).
    fn observe(&mut self, w: u64, fr: f64, dt: f64) {
        if w != self.last_write {
            // when the stream's newest frame showed up, minus when a steady flow would have had it
            let late = self.t - w.wrapping_sub(self.base) as i64 as f64 / fr;
            if w < self.last_write || self.since_write > 0.25 {
                self.lmin.fill(late); // writer restarted or a long outage: new baseline
            }
            self.lmin[self.cur] = self.lmin[self.cur].min(late);
            if w > self.last_write {
                self.adv_min[self.cur] = self.adv_min[self.cur].min((w - self.last_write) as f64);
            }
            let ref_ = self.lmin.iter().copied().fold(f64::INFINITY, f64::min);
            // (the C++ cast an unbounded double to int: saturating here)
            let idx = (((late - ref_) * 1000.0) as i64).clamp(0, BINS as i64 - 1) as usize;
            let bin = &mut self.hist[self.cur][idx];
            *bin = bin.saturating_add(1);
            self.since_write = 0.0;
        } else {
            self.since_write += dt;
        }
        self.last_write = w;
        self.t += dt;
        self.sub_sec += dt;
        if self.sub_sec < 0.5 {
            return;
        }
        self.sub_sec = 0.0;
        self.cur = (self.cur + 1) % SUB;
        self.hist[self.cur] = [0; BINS];
        self.lmin[self.cur] = 1.0e18;
        self.adv_min[self.cur] = 1.0e18;
        let mut total = 0u32;
        let mut sum = [0u32; BINS];
        for row in &self.hist {
            for (sb, &h) in sum.iter_mut().zip(row) {
                *sb += u32::from(h);
                total += u32::from(h);
            }
        }
        if total < 20 {
            self.p95_ms = -1.0;
            return;
        }
        let want = (total * 95).div_ceil(100);
        let (mut acc, mut b) = (0u32, 0usize);
        while b < BINS - 1 {
            acc += sum[b];
            if acc >= want {
                break;
            }
            b += 1;
        }
        self.p95_ms = (b + 1) as f64; // upper edge of the bin
    }

    /// Data arrives in steps: the average fill sits half a step below the fill right after a packet (frames).
    fn half_packet(&self) -> f64 {
        let pkt = self.adv_min.iter().copied().fold(1.0e18, f64::min);
        if pkt < 1.0e17 && pkt > self.block_fr {
            pkt * 0.5
        } else {
            0.0
        }
    }

    /// Frames from `from` on (at most `max_frames`) that are silent on both channels (-40 dBFS).
    fn quiet_run(ring: &ChannelRing, from: u64, max_frames: u64) -> u64 {
        const QUIET: f32 = 0.01;
        // SAFETY: plain reads of the two channels of the ring; a concurrent writer can only change the
        // value of a float, which this scan tolerates (it is a heuristic).
        let ch = unsafe { &*ring.ch.get() };
        let mut k = 0;
        while k < max_frames {
            let i = ((from + k) & u64::from(kRingMask)) as usize;
            if ch[0][i].abs() > QUIET || ch[1][i].abs() > QUIET {
                break;
            }
            k += 1;
        }
        k
    }

    /// Block + measured jitter + 2 ms + half a packet; until there is a measurement a modest guess.
    fn ideal_target(&self, fr: f64) -> f64 {
        let half = self.half_packet();
        let ms = if self.p95_ms < 0.0 {
            20.0
        } else {
            self.p95_ms + 2.0
        };
        (self.block_fr + fr * ms * 0.001 + half)
            .clamp(self.block_fr + fr * 0.002, self.block_fr + fr * 0.4)
    }

    fn lost(&mut self, frames: f64) {
        if self.played {
            self.concealed_ms += frames * 1000.0 / self.sample_rate; // silence we play after the first sound
        }
    }

    fn silence(&mut self, out: &mut [&mut [f32]; 2], n: usize) {
        for c in out.iter_mut() {
            c[..n].fill(0.0);
        }
        self.last_out = [0.0; 2];
    }

    /// Silence for this block, after a short decay from the last sample played (no step in the output).
    fn decay_then_silence(&mut self, out: &mut [&mut [f32]; 2], n: usize, fade_len: usize) {
        for (c, o) in out.iter_mut().enumerate() {
            o[..n].fill(0.0);
            for j in 0..fade_len {
                o[j] = self.last_out[c] * (fade_len - 1 - j) as f32 / fade_len as f32;
            }
        }
        self.last_out = [0.0; 2];
    }

    fn reprime(&mut self, w: u64, out: &mut [&mut [f32]; 2], n: usize, fade_len: usize) -> bool {
        self.primed = false;
        self.muted = false;
        self.starved_at = w;
        self.reprimes += 1;
        self.lost(n as f64);
        self.decay_then_silence(out, n, fade_len);
        false
    }

    /// Audio thread. Fills `out[0..1]` with `n` frames. `false` = nothing to play (the friend is not
    /// sending, or not enough audio has arrived yet): `out` is silence. `rate` is the friend's sample
    /// rate. No allocation, no locks.
    #[allow(clippy::too_many_lines)]
    pub fn pull(
        &mut self,
        ring: &ChannelRing,
        write_pos: &AtomicU64,
        rate: u32,
        mut out: [&mut [f32]; 2],
        n: usize,
    ) -> bool {
        let n = n.min(out[0].len()).min(out[1].len());
        let sr = (self.sample_rate + 0.5) as u32;
        let have_rs = if rate == sr {
            self.rs_same.is_some()
        } else if rate == 48000 {
            self.rs_48.is_some()
        } else if rate == 44100 {
            self.rs_441.is_some()
        } else {
            false
        };
        if !have_rs || n == 0 || n > self.max_block {
            self.primed = false;
            self.muted = false;
            self.silence(&mut out, n);
            return false;
        }
        let dt = n as f64 / self.sample_rate;
        let fr = f64::from(rate);
        let w = write_pos.load(Acquire);
        let fade_frames = ((self.sample_rate * 0.002) as i32).max(1);
        self.block_fr = self.block_fr.max(n as f64 * fr / self.sample_rate);
        if self.key != rate {
            // first block or the browser changed its rate: start with a modest guess, the measurements take over
            self.key = rate;
            self.block_fr = n as f64 * fr / self.sample_rate;
            self.lmin.fill(1.0e18);
            self.adv_min.fill(1.0e18);
            self.hist = [[0; BINS]; SUB];
            self.t = 0.0;
            self.sub_sec = 0.0;
            self.cur = 0;
            self.p95_ms = -1.0;
            self.base = w;
            self.starved_at = w;
            self.last_write = w;
            self.since_write = 0.0;
            self.target = self.ideal_target(fr);
            self.hold_sec = 0.0;
            self.primed = false;
            self.muted = false;
        }
        self.observe(w, fr, dt);
        let fade_len = (fade_frames as usize).min(n);

        let mut fill = w.wrapping_sub(self.cursor) as i64 as f64;
        let max_fill = f64::from(kRingFrames - kGuardFrames);
        if !self.primed || fill < 0.0 || fill > max_fill.min(self.target * 2.0 + fr * 0.05) {
            if self.primed && self.played {
                self.reprimes += 1; // too much data or a nonsense position while playing: jump to the newest
            }
            if (w as f64) < self.starved_at as f64 + self.target + n as f64 {
                self.lost(n as f64);
                self.silence(&mut out, n);
                return false;
            }
            // w is at the top of a step now, the controller wants the mean
            self.cursor = w.saturating_sub((self.target + self.half_packet()) as u64);
            self.drift.reset(self.target);
            if let Some(rs) = self.resampler(rate, sr) {
                rs.reset();
            }
            self.primed = true;
            self.played = true;
            self.muted = false;
            self.fade_left = fade_frames;
            self.fade_from = self.last_out; // a fade longer than a block must keep fading from this
            fill = self.target;
            self.fill_avg = self.target;
            self.min_margin = 1.0e9;
            self.window_sec = 0.0;
        }
        let need = f64::from(
            self.resampler(rate, sr)
                .map_or(0, |rs| rs.input_for(n as u32)),
        );
        let mut start_mute = false;
        if self.muted {
            if fill < need {
                // still waiting: a short gap keeps its place, a long one starts over
                if self.muted_out >= self.sample_rate * 0.03 {
                    // long gap: learn from it as well, or a stall every few seconds repeats forever
                    self.target =
                        (self.target + self.missing + self.muted_out * fr / self.sample_rate)
                            .min(self.block_fr + fr * 0.4);
                    self.hold_sec = 5.0;
                    return self.reprime(w, &mut out, n, fade_len);
                }
                self.muted_out += n as f64;
                self.lost(n as f64);
                self.silence(&mut out, n);
                return true;
            }
            // data is back: raise the target by what was missing, so the next one like it does not gap
            self.target = (self.target + self.missing + self.muted_out * fr / self.sample_rate)
                .min(self.block_fr + fr * 0.4);
            self.drift.set_target(self.target);
            self.hold_sec = 5.0;
            self.muted = false;
            self.fade_left = fade_frames;
            self.fade_from = self.last_out;
        } else if fill < need {
            start_mute = true;
        } else {
            self.fill_avg += (fill - self.fill_avg) * (1.0 - (-dt / 0.3).exp());
            self.min_margin = self.min_margin.min(fill - need);
            self.window_sec += dt;
            self.hold_sec -= dt;
            if self.window_sec >= 1.0 {
                // follow the measured jitter: up at once, down by half the spare margin per second
                let ideal = self.ideal_target(fr);
                if ideal > self.target {
                    self.target = ideal;
                } else if self.hold_sec <= 0.0 && (self.fill_avg - self.target).abs() < fr * 0.002 {
                    let spare = self.min_margin - fr * 0.002;
                    let cut = (0.5 * (self.target - ideal)).min(0.5 * spare);
                    if cut > fr * 0.0002 {
                        self.target -= cut;
                    }
                }
                self.target = self.target.max(self.block_fr + fr * 0.002);
                self.drift.set_target(self.target);
                self.window_sec = 0.0;
                self.min_margin = 1.0e9;
            }
            // more buffered than the target (a stall raised it, or a burst came in): drop the extra where
            // the friend is silent instead of draining it at the drift's 0.5 % for half a minute; with no
            // silence for 3 s, drop it anyway (the smoothed fill: the instant one saws up and down by a
            // packet with every arrival)
            if self.fill_avg - self.target > fr * 0.006 {
                self.skip_wait_sec += dt;
                // never into a gap
                let excess =
                    0.0f64.max((self.fill_avg - self.target).min(fill - need - fr * 0.002));
                let mut skip = Self::quiet_run(ring, self.cursor, excess as u64);
                if (skip as f64) < excess.min(fr * 0.005).floor() {
                    skip = 0; // a few silent samples are no pause
                }
                if skip == 0 && self.skip_wait_sec >= 3.0 {
                    skip = excess as u64;
                }
                if skip > 0 {
                    self.cursor += skip;
                    fill -= skip as f64;
                    self.fill_avg -= skip as f64;
                    self.drift.reset(self.target); // its integral still pushes for the old error: it would overshoot below the target
                    self.fade_left = fade_frames;
                    self.fade_from = self.last_out;
                    self.skip_wait_sec = 0.0;
                }
            } else {
                self.skip_wait_sec = 0.0;
            }
            let c = self.drift.update(fill, dt);
            let ppm = (c * 1.0e6).round() as i32;
            if let Some(rs) = self.resampler(rate, sr) {
                rs.set_correction_ppm(ppm);
            }
        }
        let mut made = 0usize;
        let mut pass = 0;
        while pass < 3 && made < n {
            pass += 1;
            let want = self
                .resampler(rate, sr)
                .map_or(0, |rs| rs.input_for((n - made) as u32))
                .min(self.in_buf[0].len() as u32);
            let mut avail = 0u32;
            let res = {
                let [b0, b1] = &mut self.in_buf;
                ring_read_at(
                    ring,
                    write_pos,
                    self.cursor,
                    &mut [&mut b0[..want as usize], &mut b1[..want as usize]],
                    want,
                    Some(&mut avail),
                )
            };
            if res == ReadResult::Overrun {
                return self.reprime(w, &mut out, n, fade_len);
            }
            if avail == 0 {
                break;
            }
            let (mut used_in, mut got) = (avail, (n - made) as u32);
            {
                let Self {
                    in_buf,
                    rs_same,
                    rs_48,
                    rs_441,
                    ..
                } = self;
                let rs = match rate {
                    r if r == sr => rs_same.as_mut(),
                    48000 => rs_48.as_mut(),
                    _ => rs_441.as_mut(),
                };
                let [o0, o1] = &mut out;
                if let Some(rs) = rs {
                    rs.process(
                        [&in_buf[0][..avail as usize], &in_buf[1][..avail as usize]],
                        &mut used_in,
                        [&mut o0[made..n], &mut o1[made..n]],
                        &mut got,
                    );
                }
            }
            self.cursor += u64::from(used_in);
            made += got as usize;
            if used_in == 0 && got == 0 {
                break;
            }
        }
        if made < n {
            for c in out.iter_mut() {
                c[made..n].fill(0.0);
            }
        }
        if self.fade_left > 0 {
            // no click when the voice (re)starts: the last sample before it (0 after silence) fades out as
            // the new data fades in
            let m = n.min(self.fade_left as usize);
            for (c, o) in out.iter_mut().enumerate() {
                for (j, x) in o[..m].iter_mut().enumerate() {
                    let g =
                        (fade_frames - self.fade_left + j as i32 + 1) as f32 / fade_frames as f32;
                    *x = *x * g + self.fade_from[c] * (1.0 - g);
                }
            }
            self.fade_left -= m as i32;
        }
        if start_mute && made < n {
            // short gap: fade out what we have, then hold the last sample's decay, then wait for data
            let start = made.saturating_sub(fade_len);
            for (c, o) in out.iter_mut().enumerate() {
                let hold = if made > 0 {
                    o[made - 1]
                } else {
                    self.last_out[c]
                };
                for j in 0..fade_len {
                    let i = start + j;
                    let v = if i < made { o[i] } else { hold };
                    o[i] = v * (fade_len - 1 - j) as f32 / fade_len as f32;
                }
            }
            self.muted = true;
            self.fade_left = 0;
            self.underruns += 1;
            self.muted_out = (n - made) as f64;
            self.missing = need - fill;
            self.lost(self.muted_out);
            self.last_out = [0.0; 2];
            return true;
        }
        self.last_out = [out[0][n - 1], out[1][n - 1]];
        self.buffer_ms = self.fill_avg * 1000.0 / fr;
        true
    }

    fn resampler(&mut self, rate: u32, sr: u32) -> Option<&mut VarResampler> {
        if rate == sr {
            self.rs_same.as_mut()
        } else if rate == 48000 {
            self.rs_48.as_mut()
        } else if rate == 44100 {
            self.rs_441.as_mut()
        } else {
            None
        }
    }
}
