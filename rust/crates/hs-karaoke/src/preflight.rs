//! Pre-flight check before singing: plays a 50 ms chirp into the headphones, records 250 ms of mic, and tells
//! closed headphones, open-back headphones and speakers apart by how much of the chirp comes back.
//! Port of `apps/karaoke/PreflightChecker.{h,cpp}`.
//!
//! The C++ object was shared by the audio and message threads. Here [`preflight`] makes two halves over one shared
//! state: [`PreflightAudio`] for the audio callback and [`PreflightChecker`] for the UI, whose `poll()` runs the
//! analysis once the recording is complete. C++ bugs this fixes:
//! 1. the analysis was posted with `callAsync([this] ...)`: a checker destroyed before it ran was used after free;
//! 2. `startCheck` cleared the recording and moved the positions while the audio thread could still be writing them;
//! 3. an analysis left over from a cancelled or restarted check overwrote the new check's result.
use crate::round_to_int;
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, AtomicU32, Ordering};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LeakStatus {
    Untested,
    Testing,
    /// closed headphones, no leak
    HeadphonesClean,
    /// very low leak (open-back headphones), safe
    HeadphonesOpen,
    /// loud leak (speakers, feedback risk)
    SpeakerLeakDetected,
}

#[derive(Debug, Clone, PartialEq)]
pub struct PreflightResult {
    pub mic_detected: bool,
    pub mic_rms_db: f32,
    pub mic_peak_db: f32,
    pub leak_status: LeakStatus,
    pub leak_energy_db: f32,
    pub correlation_peak: f32,
    pub rtt_ms: f32,
    pub jitter_ms: f32,
    pub is_ready: bool,
    pub summary: String,
    pub advice: String,
}

impl Default for PreflightResult {
    fn default() -> Self {
        Self {
            mic_detected: false,
            mic_rms_db: -100.0,
            mic_peak_db: -100.0,
            leak_status: LeakStatus::Untested,
            leak_energy_db: -100.0,
            correlation_peak: 0.0,
            rtt_ms: -1.0,
            jitter_ms: -1.0,
            is_ready: false,
            summary: String::new(),
            advice: String::new(),
        }
    }
}

const READY: &str = "พร้อมร้อง ✓";

/// Compares a mic recording with the reference chirp (used directly by the tests, like the C++ static).
pub fn evaluate(
    mic: &[f32],
    chirp: &[f32],
    net_rtt_ms: f32,
    net_jitter_ms: f32,
    aec_active: bool,
) -> PreflightResult {
    let mut res = PreflightResult {
        rtt_ms: net_rtt_ms,
        jitter_ms: net_jitter_ms,
        ..Default::default()
    };
    if mic.is_empty() || chirp.is_empty() {
        res.summary = "ข้อมูลเสียงไม่สมบูรณ์".into();
        return res;
    }

    // 1. mic level
    let (mut sum_sq, mut peak) = (0.0f64, 0.0f32);
    for &s in mic {
        let a = s.abs();
        sum_sq += f64::from(a) * f64::from(a);
        if a > peak {
            peak = a;
        }
    }
    let rms = (sum_sq / mic.len() as f64).sqrt();
    res.mic_rms_db = if rms > 1e-6 {
        (20.0 * rms.log10()) as f32
    } else {
        -100.0
    };
    res.mic_peak_db = if peak > 1e-6 {
        (20.0 * f64::from(peak).log10()) as f32
    } else {
        -100.0
    };
    res.mic_detected = peak > 0.0005; // > -66 dBFS

    // 2. chirp energy
    let chirp_energy = chirp
        .iter()
        .map(|&c| f64::from(c) * f64::from(c))
        .sum::<f64>()
        .max(1e-12);

    // 3. normalised cross-correlation over every lag that fits
    let (mut max_corr, mut best_energy) = (0.0f64, 0.0f64);
    for window in mic.windows(chirp.len()) {
        let (mut dot, mut energy) = (0.0f64, 0.0f64);
        for (&m, &c) in window.iter().zip(chirp) {
            let m = f64::from(m);
            dot += m * f64::from(c);
            energy += m * m;
        }
        let norm = (energy * chirp_energy).sqrt();
        let corr = if norm > 1e-10 { dot.abs() / norm } else { 0.0 };
        if corr > max_corr {
            (max_corr, best_energy) = (corr, energy);
        }
    }
    res.correlation_peak = max_corr as f32;
    let leak_ratio = best_energy / chirp_energy;
    res.leak_energy_db = if leak_ratio > 1e-8 {
        (10.0 * leak_ratio.log10()) as f32
    } else {
        -100.0
    };

    // 4. speakers: peak >= 0.20 and > -35 dB; open-back: >= 0.08 and > -45 dB; else clean
    res.leak_status = if res.correlation_peak >= 0.20 && res.leak_energy_db > -35.0 {
        LeakStatus::SpeakerLeakDetected
    } else if res.correlation_peak >= 0.08 && res.leak_energy_db > -45.0 {
        LeakStatus::HeadphonesOpen
    } else {
        LeakStatus::HeadphonesClean
    };

    // 5. advice
    let (mut issues, mut goods) = (Vec::<String>::new(), Vec::<String>::new());
    if res.mic_detected {
        goods.push("ไมโครโฟนรับเสียงได้ปกติ".into());
    } else {
        issues.push("ไมโครโฟนเงียบเกินไป กรุณาตรวจสอบว่าเปิดไมค์หรือยัง".into());
    }
    match res.leak_status {
        LeakStatus::SpeakerLeakDetected if aec_active => {
            goods.push("ตรวจพบเสียงลำโพง (ระบบตัดเสียงสะท้อน AEC กำลังทำงาน ✓)".into())
        }
        LeakStatus::SpeakerLeakDetected => {
            issues.push("ตรวจพบเสียงลำโพงรั่วเข้าไมค์ แนะนำให้ใส่หูฟัง หรือเปิดโหมดลำโพง (AEC)".into())
        }
        LeakStatus::HeadphonesOpen => goods.push("หูฟังระดับเสียงปลอดภัย ✓".into()),
        _ => goods.push("ใช้หูฟัง ไม่มีเสียงสะท้อน ✓".into()),
    }
    if net_rtt_ms > 0.0 {
        let rtt = round_to_int(f64::from(net_rtt_ms));
        if net_rtt_ms > 120.0 || net_jitter_ms > 30.0 {
            issues.push(format!(
                "เน็ตมีความหน่วงหรือกระตุก (RTT {rtt} ms) แนะนำต่อสาย LAN"
            ));
        } else {
            goods.push(format!("เน็ตเร็ว เสถียรดีมาก (RTT {rtt} ms)"));
        }
    }

    res.is_ready = issues.is_empty();
    if res.is_ready {
        res.summary = READY.into();
        res.advice = goods.join(" · "); // never empty: the leak line is always there
    } else {
        res.summary = "ต้องปรับตั้งค่าก่อนร้อง".into();
        res.advice = issues.join(" · ");
    }
    res
}

/// The 50 ms Hann-windowed 500 Hz to 2.5 kHz sweep at -18 dBFS.
pub fn chirp(sample_rate: f64) -> Vec<f32> {
    let n = (sample_rate * 0.050) as usize;
    let (f0, f1) = (500.0, 2500.0);
    let k = (f1 - f0) / (n as f64 / sample_rate);
    let two_pi = 2.0 * std::f64::consts::PI;
    (0..n)
        .map(|i| {
            let t = i as f64 / sample_rate;
            let phase = two_pi * (f0 * t + 0.5 * k * t * t);
            let win = 0.5 * (1.0 - (two_pi * i as f64 / n as f64).cos());
            (0.125 * win * phase.sin()) as f32
        })
        .collect()
}

const IDLE: u8 = 0;
/// `start_check` asked; the audio thread rewinds and moves on to `RUNNING`
const ARMED: u8 = 1;
const RUNNING: u8 = 2;
/// recording complete, waiting for `poll`
const DONE: u8 = 3;

struct Shared {
    state: AtomicU8,
    chirp: Box<[f32]>,
    /// f32 bits: written by the audio thread while `RUNNING`, read by `poll` after `DONE`
    rec: Box<[AtomicU32]>,
}

/// Makes the two halves for one sample rate (allocates; call again when the rate changes).
pub fn preflight(sample_rate: f64) -> (PreflightChecker, PreflightAudio) {
    let sr = if sample_rate > 0.0 {
        sample_rate
    } else {
        48000.0
    };
    let total = (sr * 0.250) as usize; // 250 ms
    let shared = Arc::new(Shared {
        state: AtomicU8::new(IDLE),
        chirp: chirp(sr).into(),
        rec: (0..total).map(|_| AtomicU32::new(0)).collect(),
    });
    (
        PreflightChecker {
            shared: shared.clone(),
            rtt_ms: -1.0,
            jitter_ms: -1.0,
            aec_active: false,
            result: PreflightResult::default(),
        },
        PreflightAudio {
            shared,
            play: 0,
            rec: 0,
        },
    )
}

/// The UI half.
pub struct PreflightChecker {
    shared: Arc<Shared>,
    rtt_ms: f32,
    jitter_ms: f32,
    aec_active: bool,
    result: PreflightResult,
}

impl PreflightChecker {
    pub fn start_check(&mut self, net_rtt_ms: f32, net_jitter_ms: f32, aec_active: bool) {
        (self.rtt_ms, self.jitter_ms, self.aec_active) = (net_rtt_ms, net_jitter_ms, aec_active);
        self.result = PreflightResult {
            leak_status: LeakStatus::Testing,
            summary: "กำลังตรวจสอบอุปกรณ์เสียงและระบบ...".into(),
            ..Default::default()
        };
        self.shared.state.store(ARMED, Ordering::Release);
    }

    pub fn cancel(&mut self) {
        self.shared.state.store(IDLE, Ordering::Release);
    }

    pub fn is_testing(&self) -> bool {
        matches!(self.shared.state.load(Ordering::Acquire), ARMED | RUNNING)
    }

    /// Call from the UI timer: analyses a finished recording. True when `result()` changed.
    pub fn poll(&mut self) -> bool {
        let s = &*self.shared;
        if s.state
            .compare_exchange(DONE, IDLE, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            return false;
        }
        let mic: Vec<f32> = s
            .rec
            .iter()
            .map(|a| f32::from_bits(a.load(Ordering::Relaxed)))
            .collect();
        self.result = evaluate(&mic, &s.chirp, self.rtt_ms, self.jitter_ms, self.aec_active);
        true
    }

    pub fn result(&self) -> &PreflightResult {
        &self.result
    }

    pub fn chirp(&self) -> &[f32] {
        &self.shared.chirp
    }
}

/// The audio-callback half.
pub struct PreflightAudio {
    shared: Arc<Shared>,
    play: usize,
    rec: usize,
}

impl PreflightAudio {
    /// Audio thread: no allocation, no lock. While a check runs, adds the chirp to the outputs and records the mic.
    pub fn process(
        &mut self,
        mic: Option<&[f32]>,
        out_l: Option<&mut [f32]>,
        out_r: Option<&mut [f32]>,
        n: usize,
    ) {
        let s = &*self.shared;
        let mut state = s.state.load(Ordering::Acquire);
        if state == ARMED
            && s.state
                .compare_exchange(ARMED, RUNNING, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
        {
            (self.play, self.rec) = (0, 0);
            state = RUNNING;
        }
        if state != RUNNING || n == 0 {
            return;
        }

        // 1. chirp
        let count = n.min(s.chirp.len() - self.play);
        let src = &s.chirp[self.play..self.play + count];
        for o in [out_l, out_r].into_iter().flatten() {
            for (d, c) in o.iter_mut().zip(src) {
                *d += c;
            }
        }
        self.play += count;

        // 2. mic
        let count = n.min(s.rec.len() - self.rec);
        for (i, slot) in s.rec[self.rec..self.rec + count].iter().enumerate() {
            let v = mic.and_then(|m| m.get(i)).copied().unwrap_or(0.0);
            slot.store(v.to_bits(), Ordering::Relaxed);
        }
        self.rec += count;
        if self.rec >= s.rec.len() {
            // a cancel or a new start in between wins
            let _ = s
                .state
                .compare_exchange(RUNNING, DONE, Ordering::Release, Ordering::Relaxed);
        }
    }
}
