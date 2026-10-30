//! Port of `libs/ssdsp/include/ssdsp/dsp.h`: vector helpers, Smoother, pan, peak, lookahead Limiter,
//! BS.1770 LoudnessMeter and the PI DriftController. Same arithmetic as the C++ (f32 where it uses
//! float, f64 where it uses double), so outputs match the golden captures within 1e-6.

use std::f32::consts::SQRT_2;

/// At or below this a gain is treated as silence.
pub const MINUS_INF_DB: f32 = -60.0;

pub fn db_to_gain(db: f32) -> f32 {
    if db <= MINUS_INF_DB {
        0.0
    } else {
        10.0f32.powf(db * 0.05)
    }
}

pub fn gain_to_db(g: f32) -> f32 {
    if g <= 1.0e-9 {
        -200.0
    } else {
        20.0 * g.log10()
    }
}

// ---------------------------------------------------------------------------------------------
// Vector operations. Plain lane-wise loops: the compiler vectorizes them (SSE/NEON) and the result
// is identical to the C++ intrinsics because every lane is independent.

pub fn vector_add(dst: &mut [f32], src: &[f32]) {
    for (d, s) in dst.iter_mut().zip(src) {
        *d += *s;
    }
}

pub fn vector_add_scaled(dst: &mut [f32], src: &[f32], scale: f32) {
    for (d, s) in dst.iter_mut().zip(src) {
        *d += *s * scale;
    }
}

pub fn vector_multiply(dst: &mut [f32], src: &[f32]) {
    for (d, s) in dst.iter_mut().zip(src) {
        *d *= *s;
    }
}

pub fn vector_scale(dst: &mut [f32], scale: f32) {
    for d in dst {
        *d *= scale;
    }
}

pub fn vector_copy(dst: &mut [f32], src: &[f32]) {
    dst[..src.len()].copy_from_slice(src);
}

pub fn vector_zero(dst: &mut [f32]) {
    dst.fill(0.0);
}

// ---------------------------------------------------------------------------------------------
/// Linear ramp smoother (like `juce::SmoothedValue<Linear>`), no allocation.
#[derive(Debug, Clone)]
pub struct Smoother {
    current: f32,
    target: f32,
    step: f32,
    ramp_len: i32,
    steps: i32,
}

impl Default for Smoother {
    fn default() -> Self {
        Smoother {
            current: 0.0,
            target: 0.0,
            step: 0.0,
            ramp_len: 1,
            steps: 0,
        }
    }
}

impl Smoother {
    pub fn prepare(&mut self, sample_rate: f64, ramp_ms: f64) {
        self.ramp_len = ((sample_rate * ramp_ms * 0.001) as i32).max(1);
        self.steps = 0;
        self.current = self.target;
    }

    pub fn set_target(&mut self, t: f32) {
        if t == self.target {
            return;
        }
        self.target = t;
        self.steps = self.ramp_len;
        self.step = (self.target - self.current) / self.steps as f32;
    }

    pub fn snap(&mut self, v: f32) {
        self.current = v;
        self.target = v;
        self.steps = 0;
    }

    pub fn next_value(&mut self) -> f32 {
        if self.steps > 0 {
            self.current += self.step;
            self.steps -= 1;
            if self.steps == 0 {
                self.current = self.target;
            }
        }
        self.current
    }

    pub fn is_smoothing(&self) -> bool {
        self.steps > 0
    }

    pub fn current(&self) -> f32 {
        self.current
    }

    pub fn target(&self) -> f32 {
        self.target
    }

    /// Multiplies the first `n` frames of every channel by the ramp. Returns `false` when the whole
    /// block was exactly zero gain.
    pub fn apply(&mut self, ch: &mut [&mut [f32]], n: usize) -> bool {
        let n = ch.iter().map(|c| c.len()).fold(n, usize::min); // never past a buffer
        if !self.is_smoothing() {
            let g = self.current;
            if g == 1.0 {
                return true;
            }
            for c in ch.iter_mut() {
                vector_scale(&mut c[..n], g);
            }
            return g != 0.0;
        }
        for i in 0..n {
            let g = self.next_value();
            for c in ch.iter_mut() {
                c[i] *= g;
            }
        }
        true
    }
}

// ---------------------------------------------------------------------------------------------
/// Equal-power pan / balance, `pan` in [-1, 1]. Centre = unity on both sides. Returns `(left, right)`.
pub fn pan_gains(pan: f32) -> (f32, f32) {
    let pan = pan.clamp(-1.0, 1.0);
    let theta = (pan + 1.0) * std::f32::consts::FRAC_PI_4;
    if pan == 0.0 {
        return (1.0, 1.0); // keep the centre exactly at unity (cos/sin rounding)
    }
    (SQRT_2 * theta.cos(), SQRT_2 * theta.sin())
}

pub fn block_peak(x: &[f32]) -> f32 {
    x.iter().fold(0.0f32, |p, v| p.max(v.abs()))
}

// ---------------------------------------------------------------------------------------------
/// Brick-wall lookahead limiter (stereo). Guarantees |out| <= ceiling.
/// Gain = box-filter(release-smoothed(sliding-min over lookahead window of required gain)).
#[derive(Debug, Clone)]
pub struct Limiter {
    la: i32,
    rel_coef: f32,
    ceiling: f32,
    release: f32,
    gain_reduction: f32,
    delay: [Vec<f32>; 2],
    boxv: Vec<f32>,
    dq_val: Vec<f32>,
    dq_idx: Vec<i64>,
    box_sum: f64,
    pos: i32,
    head: i32,
    tail: i32,
    recompute: i32,
    t: i64,
}

impl Default for Limiter {
    fn default() -> Self {
        Limiter {
            la: 48,
            rel_coef: 0.001,
            ceiling: 0.891,
            release: 1.0,
            gain_reduction: 1.0,
            delay: [Vec::new(), Vec::new()],
            boxv: Vec::new(),
            dq_val: Vec::new(),
            dq_idx: Vec::new(),
            box_sum: 0.0,
            pos: 0,
            head: 0,
            tail: 0,
            recompute: 0,
            t: 0,
        }
    }
}

impl Limiter {
    /// Allocates (not for the audio thread). Defaults in C++: lookahead 1 ms, release 120 ms.
    pub fn prepare(&mut self, sample_rate: f64, lookahead_ms: f64, release_ms: f64) {
        self.la = ((sample_rate * lookahead_ms * 0.001).round() as i32).max(1);
        self.rel_coef = (1.0 - (-1.0 / (sample_rate * release_ms * 0.001)).exp()) as f32;
        let la = self.la as usize;
        self.delay[0] = vec![0.0; la];
        self.delay[1] = vec![0.0; la];
        self.boxv = vec![1.0; la];
        self.dq_val = vec![1.0; la + 2];
        self.dq_idx = vec![0; la + 2];
        self.reset();
    }

    pub fn reset(&mut self) {
        self.delay[0].fill(0.0);
        self.delay[1].fill(0.0);
        self.boxv.fill(1.0);
        self.box_sum = f64::from(self.la);
        self.pos = 0;
        self.t = 0;
        self.head = 0;
        self.tail = 0;
        self.release = 1.0;
        self.recompute = 0;
        self.gain_reduction = 1.0;
    }

    pub fn latency(&self) -> i32 {
        self.la
    }

    pub fn set_ceiling(&mut self, linear: f32) {
        self.ceiling = linear.max(1.0e-4);
    }

    pub fn last_gain(&self) -> f32 {
        self.gain_reduction
    }

    pub fn process(&mut self, l: &mut [f32], r: &mut [f32], n: usize, enabled: bool) {
        let n = n.min(l.len()).min(r.len()); // never past a buffer
        if self.boxv.is_empty() {
            return; // not prepared yet
        }
        let cap = self.la + 2;
        let mut min_gain = 1.0f32;
        for i in 0..n {
            let (xl, xr) = (l[i], r[i]);
            let peak = xl.abs().max(xr.abs());
            let req = if enabled && peak > self.ceiling {
                self.ceiling / peak
            } else {
                1.0
            };

            // sliding minimum over the last la+1 samples (monotonic deque in fixed arrays)
            while self.head != self.tail {
                let last = (self.tail + cap - 1) % cap;
                if self.dq_val[last as usize] >= req {
                    self.tail = last;
                } else {
                    break;
                }
            }
            self.dq_val[self.tail as usize] = req;
            self.dq_idx[self.tail as usize] = self.t;
            self.tail = (self.tail + 1) % cap;
            while self.t - self.dq_idx[self.head as usize] > i64::from(self.la) {
                self.head = (self.head + 1) % cap;
            }
            let held = self.dq_val[self.head as usize];

            self.release = if held < self.release {
                held
            } else {
                self.release + (held - self.release) * self.rel_coef
            };

            let p = self.pos as usize;
            self.box_sum += f64::from(self.release) - f64::from(self.boxv[p]);
            self.boxv[p] = self.release;
            let g = ((self.box_sum / f64::from(self.la)) as f32).min(1.0);

            let (dl, dr) = (self.delay[0][p], self.delay[1][p]);
            self.delay[0][p] = xl;
            self.delay[1][p] = xr;
            self.pos = (self.pos + 1) % self.la;
            self.t += 1;

            let (mut ol, mut or) = (dl * g, dr * g);
            if enabled {
                // guard against rounding only
                ol = ol.clamp(-self.ceiling, self.ceiling);
                or = or.clamp(-self.ceiling, self.ceiling);
            }
            l[i] = ol;
            r[i] = or;
            min_gain = min_gain.min(g);
        }
        self.recompute += 1;
        if self.recompute >= 256 {
            // avoid floating drift of the running sum
            self.recompute = 0;
            self.box_sum = self.boxv.iter().map(|&v| f64::from(v)).sum();
        }
        self.gain_reduction = min_gain;
    }
}

// ---------------------------------------------------------------------------------------------
/// ITU-R BS.1770 loudness (momentary 400 ms, short-term 3 s), stereo.
#[derive(Debug, Clone)]
pub struct LoudnessMeter {
    s1: Coeffs,
    s2: Coeffs,
    z1: [State; 2],
    z2: [State; 2],
    acc: f64,
    acc_n: i32,
    sub_len: i32,
    blocks: i64,
    hist: [f64; K_HIST],
    momentary: f32,
    short_term: f32,
}

#[derive(Debug, Clone, Copy, Default)]
struct Coeffs {
    b0: f64,
    b1: f64,
    b2: f64,
    a1: f64,
    a2: f64,
}

#[derive(Debug, Clone, Copy, Default)]
struct State {
    z1: f64,
    z2: f64,
}

const K_HIST: usize = 30;

fn biquad(k: &Coeffs, s: &mut State, x: f64) -> f64 {
    // transposed DF2
    let y = k.b0 * x + s.z1;
    s.z1 = k.b1 * x - k.a1 * y + s.z2;
    s.z2 = k.b2 * x - k.a2 * y;
    y
}

fn to_lufs(ms: f64) -> f32 {
    if ms <= 1.0e-20 {
        -200.0
    } else {
        (-0.691 + 10.0 * ms.log10()) as f32
    }
}

impl Default for LoudnessMeter {
    fn default() -> Self {
        LoudnessMeter {
            s1: Coeffs::default(),
            s2: Coeffs::default(),
            z1: [State::default(); 2],
            z2: [State::default(); 2],
            acc: 0.0,
            acc_n: 0,
            sub_len: 4800,
            blocks: 0,
            hist: [0.0; K_HIST],
            momentary: -200.0,
            short_term: -200.0,
        }
    }
}

impl LoudnessMeter {
    pub fn prepare(&mut self, fs: f64) {
        use std::f64::consts::PI;
        // Stage 1: high shelf
        {
            let (f0, g, q) = (1681.974450955533, 3.999843853973347, 0.7071752369554196);
            let k = (PI * f0 / fs).tan();
            let vh = 10.0f64.powf(g / 20.0);
            let vb = vh.powf(0.4996667741545416);
            let a0 = 1.0 + k / q + k * k;
            self.s1 = Coeffs {
                b0: (vh + vb * k / q + k * k) / a0,
                b1: 2.0 * (k * k - vh) / a0,
                b2: (vh - vb * k / q + k * k) / a0,
                a1: 2.0 * (k * k - 1.0) / a0,
                a2: (1.0 - k / q + k * k) / a0,
            };
        }
        // Stage 2: RLB high pass
        {
            let (f0, q) = (38.13547087602444, 0.5003270373238773);
            let k = (PI * f0 / fs).tan();
            let a0 = 1.0 + k / q + k * k;
            self.s2 = Coeffs {
                b0: 1.0,
                b1: -2.0,
                b2: 1.0,
                a1: 2.0 * (k * k - 1.0) / a0,
                a2: (1.0 - k / q + k * k) / a0,
            };
        }
        self.sub_len = ((fs * 0.1).round() as i32).max(1);
        self.reset();
    }

    pub fn reset(&mut self) {
        self.z1 = [State::default(); 2];
        self.z2 = [State::default(); 2];
        self.acc = 0.0;
        self.acc_n = 0;
        self.blocks = 0;
        self.hist = [0.0; K_HIST];
        self.momentary = -200.0;
        self.short_term = -200.0;
    }

    pub fn process(&mut self, l: &[f32], r: &[f32], n: usize) {
        let n = n.min(l.len()).min(r.len()); // never past a buffer
        for i in 0..n {
            let mut sum = 0.0;
            for (c, x) in [l[i], r[i]].into_iter().enumerate() {
                let y1 = biquad(&self.s1, &mut self.z1[c], f64::from(x));
                let y2 = biquad(&self.s2, &mut self.z2[c], y1);
                sum += y2 * y2;
            }
            self.acc += sum;
            self.acc_n += 1;
            if self.acc_n >= self.sub_len {
                self.hist[(self.blocks % K_HIST as i64) as usize] =
                    self.acc / f64::from(self.acc_n);
                self.blocks += 1;
                self.acc = 0.0;
                self.acc_n = 0;
                self.momentary = to_lufs(self.average(4));
                self.short_term = to_lufs(self.average(30));
            }
        }
    }

    pub fn momentary(&self) -> f32 {
        self.momentary
    }

    pub fn short_term(&self) -> f32 {
        self.short_term
    }

    fn average(&self, count: i64) -> f64 {
        let have = self.blocks.min(count);
        if have <= 0 {
            return 0.0;
        }
        let s: f64 = (0..have)
            .map(|k| self.hist[((self.blocks - 1 - k) % K_HIST as i64) as usize])
            .sum();
        s / count as f64 // partial windows count as silence (a real meter warming up)
    }
}

// ---------------------------------------------------------------------------------------------
/// PI controller that keeps a consumer's buffer fill at a target by nudging the resampling ratio.
/// Output: relative correction `c`, use `ratio = in_rate * (1 + c) / out_rate`.
#[derive(Debug, Clone)]
pub struct DriftController {
    target: f64,
    filtered: f64,
    integral: f64,
    correction: f64,
    primed: bool,
}

impl Default for DriftController {
    fn default() -> Self {
        DriftController {
            target: 1.0,
            filtered: 0.0,
            integral: 0.0,
            correction: 0.0,
            primed: false,
        }
    }
}

impl DriftController {
    /// ±0.5 %
    pub const MAX_CORRECTION: f64 = 0.005;
    const SMOOTH_SEC: f64 = 0.5;
    const KP: f64 = 0.002;
    const KI: f64 = 0.0004;
    const INTEGRAL_LIMIT: f64 = Self::MAX_CORRECTION / Self::KI;

    pub fn reset(&mut self, target_frames: f64) {
        self.target = target_frames.max(1.0);
        self.filtered = 0.0;
        self.integral = 0.0;
        self.correction = 0.0;
        self.primed = false;
    }

    /// `fill` = frames available to read, `dt_sec` = time since the previous call.
    pub fn update(&mut self, fill: f64, dt_sec: f64) -> f64 {
        let e = (fill - self.target) / self.target;
        if !self.primed {
            self.filtered = e;
            self.primed = true;
        }
        let a = 1.0 - (-dt_sec / Self::SMOOTH_SEC).exp();
        self.filtered += (e - self.filtered) * a;
        self.integral = (self.integral + self.filtered * dt_sec)
            .clamp(-Self::INTEGRAL_LIMIT, Self::INTEGRAL_LIMIT);
        self.correction = (Self::KP * self.filtered + Self::KI * self.integral)
            .clamp(-Self::MAX_CORRECTION, Self::MAX_CORRECTION);
        self.correction
    }

    /// Moves the target without resetting the controller (used by the adaptive buffer).
    pub fn set_target(&mut self, target_frames: f64) {
        let t = target_frames.max(1.0);
        self.filtered *= self.target / t; // keep the absolute error continuous
        self.target = t;
    }

    pub fn correction(&self) -> f64 {
        self.correction
    }

    pub fn filtered_error(&self) -> f64 {
        self.filtered
    }

    pub fn target(&self) -> f64 {
        self.target
    }
}
