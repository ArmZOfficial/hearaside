//! The singer's vocal chain: gate, EQ (80 Hz high-pass, 2.5 kHz peak, 10 kHz shelf), 3:1 compressor, then a Freeverb
//! style reverb and a ping-pong echo for the headphones, with a dry or wet copy for the network.
//! Port of `apps/karaoke/VocalChain.{h,cpp}`; matches `compat/golden/vocal` within 1e-6 (-120 dBFS).
//!
//! The C++ used `juce::dsp::IIR::Filter` and `juce::dsp::Reverb`. Both are re-stated here from their maths (PLAN 4.3):
//! the RBJ biquads in transposed direct form II, and Jezar's Freeverb (public domain tunings: 8 combs, 4 all-passes,
//! stereo spread 23) with the 10 ms linear parameter smoothing and the `+0.1 -0.1` denormal flush of the x86 JUCE
//! build, so the output stays sample-close to the 1.0.10 app.
use crate::{AtomicF32, round_to_int};
use std::f32::consts::{FRAC_1_SQRT_2, PI};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering::Relaxed};

/// The controls the UI thread changes while the audio thread runs (the C++ atomics, relaxed like there).
pub struct VocalParams {
    muted: AtomicBool,
    send_wet: AtomicBool,
    reverb_level: AtomicF32,
    echo_level: AtomicF32,
    gate_threshold_db: AtomicF32,
    current_gate_gain: AtomicF32,
}

impl VocalParams {
    pub fn set_muted(&self, muted: bool) {
        self.muted.store(muted, Relaxed);
    }
    pub fn is_muted(&self) -> bool {
        self.muted.load(Relaxed)
    }
    pub fn set_send_wet(&self, wet: bool) {
        self.send_wet.store(wet, Relaxed);
    }
    pub fn is_send_wet(&self) -> bool {
        self.send_wet.load(Relaxed)
    }
    pub fn set_reverb_level(&self, lvl: f32) {
        self.reverb_level.store(lvl.clamp(0.0, 1.0));
    }
    pub fn reverb_level(&self) -> f32 {
        self.reverb_level.load()
    }
    pub fn set_echo_level(&self, lvl: f32) {
        self.echo_level.store(lvl.clamp(0.0, 1.0));
    }
    pub fn echo_level(&self) -> f32 {
        self.echo_level.load()
    }
    pub fn set_gate_threshold_db(&self, db: f32) {
        self.gate_threshold_db.store(db);
    }
    pub fn current_gate_gain(&self) -> f32 {
        self.current_gate_gain.load()
    }
}

pub struct VocalChain {
    params: Arc<VocalParams>,
    sample_rate: f64,
    gate_gain: f32,
    gate_hold: i32,
    low_cut: Biquad,
    mid_peak: Biquad,
    high_shelf: Biquad,
    comp_env: f32,
    reverb: Reverb,
    echo_l: Vec<f32>,
    echo_r: Vec<f32>,
    echo_write: usize,
    echo_lp_l: f32,
    echo_lp_r: f32,
    dry: Vec<f32>,
    wet_l: Vec<f32>,
    wet_r: Vec<f32>,
}

impl Default for VocalChain {
    fn default() -> Self {
        Self::new()
    }
}

impl VocalChain {
    pub fn new() -> Self {
        Self {
            params: Arc::new(VocalParams {
                muted: AtomicBool::new(false),
                send_wet: AtomicBool::new(false),
                reverb_level: AtomicF32::new(0.25),
                echo_level: AtomicF32::new(0.20),
                gate_threshold_db: AtomicF32::new(-45.0),
                current_gate_gain: AtomicF32::new(1.0),
            }),
            sample_rate: 48000.0,
            gate_gain: 1.0,
            gate_hold: 0,
            low_cut: Biquad::default(),
            mid_peak: Biquad::default(),
            high_shelf: Biquad::default(),
            comp_env: 0.0,
            reverb: Reverb::new(),
            echo_l: Vec::new(),
            echo_r: Vec::new(),
            echo_write: 0,
            echo_lp_l: 0.0,
            echo_lp_r: 0.0,
            dry: Vec::new(),
            wet_l: Vec::new(),
            wet_r: Vec::new(),
        }
    }

    /// The controls, for the UI thread.
    pub fn params(&self) -> &Arc<VocalParams> {
        &self.params
    }

    /// Allocates; not for the audio thread.
    pub fn prepare(&mut self, sample_rate: f64, max_block: usize) {
        self.sample_rate = if sample_rate > 0.0 {
            sample_rate
        } else {
            48000.0
        };
        let sr = self.sample_rate;
        let alloc = max_block.max(2048);
        self.dry = vec![0.0; alloc];
        self.wet_l = vec![0.0; alloc];
        self.wet_r = vec![0.0; alloc];

        // 2 seconds of echo ring
        let echo_len = round_to_int(sr * 2.0).max(1) as usize;
        self.echo_l = vec![0.0; echo_len];
        self.echo_r = vec![0.0; echo_len];

        self.low_cut.set(high_pass(sr, 80.0, FRAC_1_SQRT_2));
        self.mid_peak.set(peak(sr, 2500.0, 1.0, 1.25));
        self.high_shelf.set(high_shelf(sr, 10000.0, 0.707, 1.15));

        self.reverb.set_sample_rate(sr);
        self.reverb.set_parameters(ReverbParams {
            room_size: 0.55,
            damping: 0.35,
            wet_level: 1.0,
            dry_level: 0.0,
            width: 1.0,
        });
        self.reset();
    }

    pub fn reset(&mut self) {
        self.low_cut.reset();
        self.mid_peak.reset();
        self.high_shelf.reset();
        self.reverb.reset();
        self.echo_l.fill(0.0);
        self.echo_r.fill(0.0);
        self.echo_write = 0;
        self.echo_lp_l = 0.0;
        self.echo_lp_r = 0.0;
        self.gate_gain = 1.0;
        self.gate_hold = 0;
        self.comp_env = 0.0;
    }

    /// Audio thread: no allocation, no lock. Processes `n` frames of `mic` into the headphone pair and the network
    /// send (each output is optional, like the C++ null pointers). A slice shorter than `n` cuts `n` (caller bug,
    /// debug assert) instead of panicking. No mic, muted or not prepared: the outputs are silent.
    pub fn process(
        &mut self,
        mic: Option<&[f32]>,
        mut out_l: Option<&mut [f32]>,
        mut out_r: Option<&mut [f32]>,
        mut out_net: Option<&mut [f32]>,
        n: usize,
    ) {
        let shortest = [
            mic.map(<[f32]>::len),
            out_l.as_deref().map(<[f32]>::len),
            out_r.as_deref().map(<[f32]>::len),
            out_net.as_deref().map(<[f32]>::len),
        ]
        .into_iter()
        .flatten()
        .min()
        .unwrap_or(n);
        debug_assert!(shortest >= n, "VocalChain::process: slice shorter than n");
        let n = n.min(shortest);

        let mic = match mic {
            Some(m) if !self.params.is_muted() && !self.dry.is_empty() => m,
            _ => {
                for o in [
                    out_l.as_deref_mut(),
                    out_r.as_deref_mut(),
                    out_net.as_deref_mut(),
                ]
                .into_iter()
                .flatten()
                {
                    o[..n].fill(0.0);
                }
                return;
            }
        };

        // The C++ processed min(n, scratch size) frames and left the rest of the outputs as they were (stale audio
        // when a host block outgrew `prepare`): here a long block runs in scratch-sized pieces. The C++ also wrote
        // the headphone pair only when both were given; each side is written on its own here.
        let mut done = 0;
        while done < n {
            let k = (n - done).min(self.dry.len());
            self.run(&mic[done..done + k]);
            let (dry, wl, wr) = (&self.dry[..k], &self.wet_l[..k], &self.wet_r[..k]);
            if let Some(l) = out_l.as_deref_mut() {
                for (o, (d, w)) in l[done..done + k].iter_mut().zip(dry.iter().zip(wl)) {
                    *o = d + w;
                }
            }
            if let Some(r) = out_r.as_deref_mut() {
                for (o, (d, w)) in r[done..done + k].iter_mut().zip(dry.iter().zip(wr)) {
                    *o = d + w;
                }
            }
            if let Some(net) = out_net.as_deref_mut() {
                let net = &mut net[done..done + k];
                if self.params.is_send_wet() {
                    for (i, o) in net.iter_mut().enumerate() {
                        *o = dry[i] + 0.5 * (wl[i] + wr[i]);
                    }
                } else {
                    net.copy_from_slice(dry);
                }
            }
            done += k;
        }
    }

    /// One scratch-sized piece: fills `dry`, `wet_l`, `wet_r` for `mic.len()` frames.
    fn run(&mut self, mic: &[f32]) {
        let n = mic.len();
        let p = &*self.params;
        let gate_thresh = 10.0f32.powf(p.gate_threshold_db.load() * 0.05);
        let hold = round_to_int(self.sample_rate * 0.040); // 40 ms
        const GATE_ATTACK: f32 = 0.08;
        const GATE_RELEASE: f32 = 0.003;

        // Gate, EQ, compressor on the dry mic
        for (d, &x) in self.dry[..n].iter_mut().zip(mic) {
            let a = x.abs();
            if a >= gate_thresh {
                self.gate_hold = hold;
            } else if self.gate_hold > 0 {
                self.gate_hold -= 1;
            }
            let target = if self.gate_hold > 0 || a >= gate_thresh {
                1.0
            } else {
                0.0
            };
            let coeff = if target > self.gate_gain {
                GATE_ATTACK
            } else {
                GATE_RELEASE
            };
            self.gate_gain += (target - self.gate_gain) * coeff;
            let mut s = x * self.gate_gain;

            s = self.low_cut.process(s);
            s = self.mid_peak.process(s);
            s = self.high_shelf.process(s);

            // 3:1 above -16 dBFS, +2 dB makeup
            let rect = s.abs();
            self.comp_env +=
                (rect - self.comp_env) * if rect > self.comp_env { 0.02 } else { 0.002 };
            const COMP_THRESH: f32 = 0.158;
            let comp_gain = if self.comp_env > COMP_THRESH {
                (self.comp_env / COMP_THRESH).powf(-0.66)
            } else {
                1.0
            };
            *d = s * comp_gain * 1.25;
        }
        p.current_gate_gain.store(self.gate_gain);

        let rev = p.reverb_level.load();
        let echo = p.echo_level.load();
        let dry = &self.dry[..n];
        let (wl, wr) = (&mut self.wet_l[..n], &mut self.wet_r[..n]);

        if rev > 0.001 {
            wl.copy_from_slice(dry);
            wr.copy_from_slice(dry);
            self.reverb.process_stereo(wl, wr);
            wl.iter_mut().for_each(|s| *s *= rev);
            wr.iter_mut().for_each(|s| *s *= rev);
        } else {
            wl.fill(0.0);
            wr.fill(0.0);
        }

        // Ping-pong echo: L 170 ms, R 200 ms, one-pole low-pass in the feedback
        if echo > 0.001 && !self.echo_l.is_empty() {
            let cap = self.echo_l.len();
            let dl = round_to_int(self.sample_rate * 0.170) as usize % cap;
            let dr = round_to_int(self.sample_rate * 0.200) as usize % cap;
            const FEEDBACK: f32 = 0.32;
            for i in 0..n {
                let tap_l = self.echo_l[(self.echo_write + cap - dl) % cap];
                let tap_r = self.echo_r[(self.echo_write + cap - dr) % cap];
                self.echo_lp_l += (tap_l - self.echo_lp_l) * 0.35;
                self.echo_lp_r += (tap_r - self.echo_lp_r) * 0.35;
                self.echo_l[self.echo_write] = dry[i] + self.echo_lp_r * FEEDBACK;
                self.echo_r[self.echo_write] = dry[i] + self.echo_lp_l * FEEDBACK;
                self.echo_write = (self.echo_write + 1) % cap;
                wl[i] += tap_l * echo;
                wr[i] += tap_r * echo;
            }
        }
    }
}

// ---- biquad (juce::dsp::IIR::Filter<float>, order 2) ----

#[derive(Default, Clone, Copy)]
struct Biquad {
    /// b0, b1, b2, a1, a2 (divided by a0)
    c: [f32; 5],
    s: [f32; 2],
}

impl Biquad {
    /// `raw`: b0, b1, b2, a0, a1, a2.
    fn set(&mut self, raw: [f32; 6]) {
        let inv = if raw[3] != 0.0 { 1.0 / raw[3] } else { 0.0 };
        self.c = [
            raw[0] * inv,
            raw[1] * inv,
            raw[2] * inv,
            raw[4] * inv,
            raw[5] * inv,
        ];
    }

    fn reset(&mut self) {
        self.s = [0.0; 2];
    }

    /// Transposed direct form II.
    fn process(&mut self, x: f32) -> f32 {
        let c = &self.c;
        let y = c[0] * x + self.s[0];
        self.s[0] = c[1] * x - c[3] * y + self.s[1];
        self.s[1] = c[2] * x - c[4] * y;
        y
    }
}

/// The `A` of the RBJ shelf / peak: sqrt of the gain factor, floored at -300 dB like JUCE.
fn amp(gain: f32) -> f32 {
    gain.max(10.0f32.powf(-300.0 * 0.05)).sqrt()
}

fn omega(sr: f64, f: f32) -> f32 {
    2.0 * PI * f.max(2.0) / sr as f32
}

fn high_pass(sr: f64, f: f32, q: f32) -> [f32; 6] {
    let n = (PI * f / sr as f32).tan();
    let n2 = n * n;
    let inv_q = 1.0 / q;
    let c1 = 1.0 / (1.0 + inv_q * n + n2);
    [
        c1,
        c1 * -2.0,
        c1,
        1.0,
        c1 * 2.0 * (n2 - 1.0),
        c1 * (1.0 - inv_q * n + n2),
    ]
}

fn peak(sr: f64, f: f32, q: f32, gain: f32) -> [f32; 6] {
    let a = amp(gain);
    let w = omega(sr, f);
    let alpha = w.sin() / (q * 2.0);
    let c2 = -2.0 * w.cos();
    let (aa, a_over) = (alpha * a, alpha / a);
    [1.0 + aa, c2, 1.0 - aa, 1.0 + a_over, c2, 1.0 - a_over]
}

fn high_shelf(sr: f64, f: f32, q: f32, gain: f32) -> [f32; 6] {
    let a = amp(gain);
    let (am1, ap1) = (a - 1.0, a + 1.0);
    let w = omega(sr, f);
    let coso = w.cos();
    let beta = w.sin() * a.sqrt() / q;
    let am1c = am1 * coso;
    [
        a * (ap1 + am1c + beta),
        a * -2.0 * (am1 + ap1 * coso),
        a * (ap1 + am1c - beta),
        ap1 - am1c + beta,
        2.0 * (am1 - ap1 * coso),
        ap1 - am1c - beta,
    ]
}

// ---- Freeverb ----

#[derive(Clone, Copy)]
struct ReverbParams {
    room_size: f32,
    damping: f32,
    wet_level: f32,
    dry_level: f32,
    width: f32,
}

/// Linear ramp of `juce::SmoothedValue<float>`.
#[derive(Default)]
struct Smoothed {
    current: f32,
    target: f32,
    countdown: i32,
    step: f32,
    steps: i32,
}

impl Smoothed {
    fn reset(&mut self, sample_rate: f64, seconds: f64) {
        self.steps = (seconds * sample_rate).floor() as i32;
        self.current = self.target;
        self.countdown = 0;
    }

    fn set_target(&mut self, v: f32) {
        if approximately_equal(v, self.target) {
            return;
        }
        if self.steps <= 0 {
            (self.current, self.target, self.countdown) = (v, v, 0);
            return;
        }
        self.target = v;
        self.countdown = self.steps;
        self.step = (self.target - self.current) / self.countdown as f32;
    }

    fn next(&mut self) -> f32 {
        if self.countdown <= 0 {
            return self.target;
        }
        self.countdown -= 1;
        self.current = if self.countdown > 0 {
            self.current + self.step
        } else {
            self.target
        };
        self.current
    }
}

fn approximately_equal(a: f32, b: f32) -> bool {
    if !(a.is_finite() && b.is_finite()) {
        return a == b;
    }
    let d = (a - b).abs();
    d <= f32::MIN_POSITIVE || d <= f32::EPSILON * a.abs().max(b.abs())
}

/// JUCE's x86 denormal flush (`JUCE_UNDENORMALISE`): it also rounds tiny values, so it is kept on every target.
fn undenormalise(x: f32) -> f32 {
    let t = x + 0.1;
    t - 0.1
}

#[derive(Default)]
struct Comb {
    buf: Vec<f32>,
    idx: usize,
    last: f32,
}

impl Comb {
    fn set_size(&mut self, size: usize) {
        if size != self.buf.len() {
            self.idx = 0;
            self.buf = vec![0.0; size];
        }
        self.clear();
    }
    fn clear(&mut self) {
        self.last = 0.0;
        self.buf.fill(0.0);
    }
    fn process(&mut self, input: f32, damp: f32, feedback: f32) -> f32 {
        let out = self.buf[self.idx];
        self.last = undenormalise(out * (1.0 - damp) + self.last * damp);
        self.buf[self.idx] = undenormalise(input + self.last * feedback);
        self.idx = (self.idx + 1) % self.buf.len();
        out
    }
}

#[derive(Default)]
struct AllPass {
    buf: Vec<f32>,
    idx: usize,
}

impl AllPass {
    fn set_size(&mut self, size: usize) {
        if size != self.buf.len() {
            self.idx = 0;
            self.buf = vec![0.0; size];
        }
        self.buf.fill(0.0);
    }
    fn process(&mut self, input: f32) -> f32 {
        let b = self.buf[self.idx];
        self.buf[self.idx] = undenormalise(input + b * 0.5);
        self.idx = (self.idx + 1) % self.buf.len();
        b - input
    }
}

const COMB_TUNINGS: [i32; 8] = [1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617]; // at 44.1 kHz
const ALLPASS_TUNINGS: [i32; 4] = [556, 441, 341, 225];
const STEREO_SPREAD: i32 = 23;

struct Reverb {
    gain: f32,
    comb: [[Comb; 8]; 2],
    allpass: [[AllPass; 4]; 2],
    damping: Smoothed,
    feedback: Smoothed,
    dry: Smoothed,
    wet1: Smoothed,
    wet2: Smoothed,
}

impl Reverb {
    /// Like `juce::Reverb()`: default parameters applied at once, then 44.1 kHz.
    fn new() -> Self {
        let mut r = Self {
            gain: 0.0,
            comb: Default::default(),
            allpass: Default::default(),
            damping: Smoothed::default(),
            feedback: Smoothed::default(),
            dry: Smoothed::default(),
            wet1: Smoothed::default(),
            wet2: Smoothed::default(),
        };
        r.set_parameters(ReverbParams {
            room_size: 0.5,
            damping: 0.5,
            wet_level: 0.33,
            dry_level: 0.4,
            width: 1.0,
        });
        r.set_sample_rate(44100.0);
        r
    }

    fn set_parameters(&mut self, p: ReverbParams) {
        let wet = p.wet_level * 3.0;
        self.dry.set_target(p.dry_level * 2.0);
        self.wet1.set_target(0.5 * wet * (1.0 + p.width));
        self.wet2.set_target(0.5 * wet * (1.0 - p.width));
        self.gain = 0.015;
        self.damping.set_target(p.damping * 0.4);
        self.feedback.set_target(p.room_size * 0.28 + 0.7);
    }

    fn set_sample_rate(&mut self, sample_rate: f64) {
        let sr = sample_rate as i32;
        // max(1): JUCE divides by a zero size at sample rates under ~40 Hz
        let size = |tuning: i32| ((sr * tuning) / 44100).max(1) as usize;
        for (ch, spread) in [0, STEREO_SPREAD].into_iter().enumerate() {
            for (c, t) in self.comb[ch].iter_mut().zip(COMB_TUNINGS) {
                c.set_size(size(t + spread));
            }
            for (a, t) in self.allpass[ch].iter_mut().zip(ALLPASS_TUNINGS) {
                a.set_size(size(t + spread));
            }
        }
        for s in [
            &mut self.damping,
            &mut self.feedback,
            &mut self.dry,
            &mut self.wet1,
            &mut self.wet2,
        ] {
            s.reset(sample_rate, 0.01);
        }
    }

    fn reset(&mut self) {
        for ch in &mut self.comb {
            ch.iter_mut().for_each(Comb::clear);
        }
        for ch in &mut self.allpass {
            ch.iter_mut().for_each(|a| a.buf.fill(0.0));
        }
    }

    fn process_stereo(&mut self, left: &mut [f32], right: &mut [f32]) {
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let input = (*l + *r) * self.gain;
            let (mut out_l, mut out_r) = (0.0f32, 0.0f32);
            let damp = self.damping.next();
            let fb = self.feedback.next();
            for j in 0..8 {
                out_l += self.comb[0][j].process(input, damp, fb);
                out_r += self.comb[1][j].process(input, damp, fb);
            }
            for j in 0..4 {
                out_l = self.allpass[0][j].process(out_l);
                out_r = self.allpass[1][j].process(out_r);
            }
            let dry = self.dry.next();
            let wet1 = self.wet1.next();
            let wet2 = self.wet2.next();
            *l = out_l * wet1 + out_r * wet2 + *l * dry;
            *r = out_r * wet1 + out_l * wet2 + *r * dry;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn smoothing_ramps_like_juce() {
        let mut s = Smoothed::default();
        s.set_target(0.8); // no ramp length yet: jumps
        s.reset(48000.0, 0.01);
        s.set_target(0.0);
        assert_eq!(s.countdown, 480);
        let first = s.next();
        assert!((first - (0.8 - 0.8 / 480.0)).abs() < 1e-7);
        for _ in 0..478 {
            s.next();
        }
        assert_eq!(s.next(), 0.0);
        assert_eq!(s.next(), 0.0);
    }
}
