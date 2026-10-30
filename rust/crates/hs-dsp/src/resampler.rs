//! `VarResampler`: thin wrapper around the SpeexDSP resampler with ppm-resolution ratio control.
//! Port of `ssdsp::VarResampler` (`libs/ssdsp/src/consumer.cpp`); the resampler itself is the same C
//! code as the C++ build (see `build.rs`), so outputs match the golden captures.
use core::ffi::c_int;

#[repr(C)]
struct SpeexState {
    _private: [u8; 0],
}

unsafe extern "C" {
    #[link_name = "hsdsp_resampler_init_frac"]
    fn speex_init_frac(
        nb_channels: u32,
        ratio_num: u32,
        ratio_den: u32,
        in_rate: u32,
        out_rate: u32,
        quality: c_int,
        err: *mut c_int,
    ) -> *mut SpeexState;
    #[link_name = "hsdsp_resampler_destroy"]
    fn speex_destroy(st: *mut SpeexState);
    #[link_name = "hsdsp_resampler_process_float"]
    fn speex_process_float(
        st: *mut SpeexState,
        channel: u32,
        input: *const f32,
        in_len: *mut u32,
        out: *mut f32,
        out_len: *mut u32,
    ) -> c_int;
    #[link_name = "hsdsp_resampler_set_rate_frac"]
    fn speex_set_rate_frac(
        st: *mut SpeexState,
        ratio_num: u32,
        ratio_den: u32,
        in_rate: u32,
        out_rate: u32,
    ) -> c_int;
    #[link_name = "hsdsp_resampler_reset_mem"]
    fn speex_reset_mem(st: *mut SpeexState) -> c_int;
    #[link_name = "hsdsp_resampler_skip_zeros"]
    fn speex_skip_zeros(st: *mut SpeexState) -> c_int;
    #[link_name = "hsdsp_resampler_get_input_latency"]
    fn speex_input_latency(st: *mut SpeexState) -> c_int;
}

fn gcd(a: u32, b: u32) -> u32 {
    if b == 0 { a } else { gcd(b, a % b) }
}

/// Stereo resampler with a `setCorrectionPpm` knob for drift compensation.
pub struct VarResampler {
    st: *mut SpeexState,
    in_rate: u32,
    out_rate: u32,
    base_num: u32,
    base_den: u32,
    ppm: i32,
    last_num: u32,
    /// input still swallowed by the filter delay after `reset()`
    priming: u32,
}

// SAFETY: the Speex state is owned exclusively by this value and has no thread affinity.
unsafe impl Send for VarResampler {}

impl VarResampler {
    /// Speex quality 0..10 (the C++ default is 5). Allocates: not for the audio thread.
    pub fn new(in_rate: u32, out_rate: u32, quality: i32) -> Result<Self, i32> {
        let g = gcd(in_rate, out_rate).max(1);
        let (base_num, base_den) = (in_rate / g, out_rate / g);
        let mut err = 0;
        // SAFETY: plain constructor call; `err` is a valid out-pointer.
        let st =
            unsafe { speex_init_frac(2, base_num, base_den, in_rate, out_rate, quality, &mut err) };
        if st.is_null() {
            return Err(err);
        }
        let mut r = VarResampler {
            st,
            in_rate,
            out_rate,
            base_num,
            base_den,
            ppm: 0,
            last_num: 0,
            priming: 0,
        };
        r.presize();
        r.reset();
        Ok(r)
    }

    /// Grows Speex' buffers now to what the largest correction will need, then goes back to the base ratio
    /// (Speex only ever grows them, and recomputes the same filter for the same ratio). Without this the
    /// first `set_correction_ppm` - on the audio thread - reallocated the filter (the C++ did, see the
    /// R0.2 real-time audit), and so did every correction that pushed a same-rate or downsampling ratio
    /// above its previous peak.
    fn presize(&mut self) {
        let k = u64::from((65535 / self.base_den).max(1));
        let den = u64::from(self.base_den) * k;
        let base = u64::from(self.base_num) * k;
        let lo = (base as f64 * 0.99).floor().max(1.0) as u64;
        let hi = (base as f64 * 1.01).ceil() as u64;
        // The filter grows with the ratio, but its oversampling halves each time num passes 2^i x den, so
        // just below such a boundary the table can be larger than at the top end: visit both ends and both
        // sides of every boundary inside the range.
        let mut nums = vec![lo, hi];
        for i in 1..8 {
            let edge = den << i;
            if edge > lo && edge <= hi {
                nums.extend([edge - 1, edge]);
            }
        }
        for num in nums {
            let num = num.clamp(1, u64::from(u32::MAX)) as u32;
            // SAFETY: `st` is live; den <= 65535 (see `set_correction_ppm`).
            unsafe { speex_set_rate_frac(self.st, num, den as u32, self.in_rate, self.out_rate) };
        }
        // SAFETY: as above.
        unsafe {
            speex_set_rate_frac(
                self.st,
                self.base_num,
                self.base_den,
                self.in_rate,
                self.out_rate,
            )
        };
    }

    pub fn in_rate(&self) -> u32 {
        self.in_rate
    }

    pub fn out_rate(&self) -> u32 {
        self.out_rate
    }

    pub fn set_correction_ppm(&mut self, ppm: i32) {
        if ppm == self.ppm {
            return;
        }
        self.ppm = ppm;
        // num/den = in/out * (1 + ppm/1e6). Speex rescales its phase as samp_frac_num * den when the
        // ratio changes and gives up (leaving a stale filter -> out-of-bounds reads) if that overflows
        // 32 bits, so den must stay <= 65535. The resulting 5-15 ppm steps are averaged out by the
        // drift controller.
        let k = u64::from((65535 / self.base_den).max(1));
        let den = u64::from(self.base_den) * k;
        let num = f64::from(self.base_num) * k as f64 * (1.0 + f64::from(ppm) * 1.0e-6);
        let num_q = num.round().clamp(1.0, f64::from(u32::MAX)) as u32;
        if num_q == self.last_num {
            return;
        }
        self.last_num = num_q;
        // SAFETY: `st` is live; den <= 65535 fits u32.
        unsafe { speex_set_rate_frac(self.st, num_q, den as u32, self.in_rate, self.out_rate) };
    }

    /// Input frames needed to produce `out_frames`, with margin.
    pub fn input_for(&self, out_frames: u32) -> u32 {
        let ratio = f64::from(self.in_rate) / f64::from(self.out_rate)
            * (1.0 + f64::from(self.ppm) * 1.0e-6);
        (f64::from(out_frames) * ratio).ceil() as u32 + 2 + self.priming
    }

    /// Consumes up to `*in_frames`, produces up to `*out_frames`; both are updated to the real counts.
    /// The slices must hold at least that many frames (the counts are cut to the slice lengths).
    pub fn process(
        &mut self,
        input: [&[f32]; 2],
        in_frames: &mut u32,
        out: [&mut [f32]; 2],
        out_frames: &mut u32,
    ) {
        let in_cap = input[0].len().min(input[1].len()).min(u32::MAX as usize) as u32;
        let out_cap = out[0].len().min(out[1].len()).min(u32::MAX as usize) as u32;
        let (mut used, mut made) = (0, 0);
        for (c, (i, o)) in input.iter().zip(out).enumerate() {
            let mut il = (*in_frames).min(in_cap);
            let mut ol = (*out_frames).min(out_cap);
            // SAFETY: `st` is live; `i` holds `il` readable and `o` `ol` writable frames; channel < 2.
            unsafe {
                speex_process_float(
                    self.st,
                    c as u32,
                    i.as_ptr(),
                    &mut il,
                    o.as_mut_ptr(),
                    &mut ol,
                );
            }
            (used, made) = (il, ol); // identical for both channels
        }
        *in_frames = used;
        *out_frames = made;
        self.priming = self.priming.saturating_sub(used);
    }

    pub fn reset(&mut self) {
        // SAFETY: `st` is live.
        unsafe {
            speex_reset_mem(self.st);
            speex_skip_zeros(self.st);
            // after skip_zeros the first half filter length of input produces no output
            self.priming = speex_input_latency(self.st).max(0) as u32;
        }
    }
}

impl Drop for VarResampler {
    fn drop(&mut self) {
        // SAFETY: `st` came from `speex_init_frac` and is destroyed once.
        unsafe { speex_destroy(self.st) };
    }
}
