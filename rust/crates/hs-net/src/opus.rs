//! Opus encoder / decoder (port of `libs/ssnet/src/opus_codec.cpp`), over libopus from `external/opus`.
//!
//! Differences from the C++ (bugs fixed, see the handoff ledger): the interleaving scratch is sized in `init` for the
//! longest Opus frame (120 ms), so `encode` / `decode` never allocate on the audio thread; slices shorter than the
//! frame count are refused instead of being read or written past their end.
use crate::ffi::*;
use std::ffi::c_int;
use std::ptr;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum App {
    /// best for speech and singing clarity
    Voip,
    /// general audio / music
    Audio,
    /// restricted low delay (no speech-mode lookahead)
    LowDelay,
}

/// The longest frame Opus takes or gives in one packet: 120 ms.
fn max_frames(sample_rate: i32) -> usize {
    sample_rate.max(0) as usize * 120 / 1000
}

/// `len` holds `frames` × `channels` samples and `frames` fits a C int.
fn fits(len: usize, frames: usize, channels: i32) -> bool {
    frames <= i32::MAX as usize
        && frames
            .checked_mul(channels.max(1) as usize)
            .is_some_and(|need| len >= need)
}

fn bytes_cap(n: usize) -> i32 {
    n.min(i32::MAX as usize) as i32
}

pub struct OpusEnc {
    enc: *mut OpusEncoder,
    sample_rate: i32,
    channels: i32,
    /// interleaving buffer for planar stereo input, sized in `init`
    scratch: Vec<f32>,
}

// SAFETY: the encoder state is owned by this value alone and libopus keeps no thread-bound state.
unsafe impl Send for OpusEnc {}

impl Default for OpusEnc {
    fn default() -> Self {
        OpusEnc {
            enc: ptr::null_mut(),
            sample_rate: 48000,
            channels: 1,
            scratch: Vec::new(),
        }
    }
}

impl Drop for OpusEnc {
    fn drop(&mut self) {
        self.destroy();
    }
}

impl OpusEnc {
    pub fn new() -> Self {
        Self::default()
    }

    fn destroy(&mut self) {
        if !self.enc.is_null() {
            // SAFETY: `enc` came from `opus_encoder_create` and is destroyed once.
            unsafe { opus_encoder_destroy(self.enc) };
            self.enc = ptr::null_mut();
        }
    }

    fn ctl(&mut self, request: c_int, value: c_int) {
        if !self.enc.is_null() {
            // SAFETY: a live encoder; every request used here takes one `opus_int32` argument.
            unsafe { opus_encoder_ctl(self.enc, request, value) };
        }
    }

    /// (Re)creates the encoder: complexity 5, in-band FEC on, 5 % expected loss, VBR (as the C++).
    pub fn init(&mut self, sample_rate: i32, channels: i32, app: App) -> bool {
        self.destroy();
        let opus_app = match app {
            App::Voip => OPUS_APPLICATION_VOIP,
            App::Audio => OPUS_APPLICATION_AUDIO,
            App::LowDelay => OPUS_APPLICATION_RESTRICTED_LOWDELAY,
        };
        let mut err = OPUS_OK;
        // SAFETY: plain constructor; `err` is a valid out pointer.
        let enc = unsafe { opus_encoder_create(sample_rate, channels, opus_app, &mut err) };
        if err != OPUS_OK || enc.is_null() {
            if !enc.is_null() {
                // SAFETY: created just above, not stored.
                unsafe { opus_encoder_destroy(enc) };
            }
            return false;
        }
        self.enc = enc;
        self.sample_rate = sample_rate;
        self.channels = channels;
        let need = max_frames(sample_rate) * 2;
        if self.scratch.len() < need {
            self.scratch.resize(need, 0.0);
        }
        self.set_complexity(5);
        self.set_inband_fec(true);
        self.set_packet_loss_perc(5);
        self.set_vbr(true);
        true
    }

    /// Clears past history / predictors.
    pub fn reset(&mut self) {
        if !self.enc.is_null() {
            // SAFETY: a live encoder; OPUS_RESET_STATE takes no argument.
            unsafe { opus_encoder_ctl(self.enc, OPUS_RESET_STATE) };
        }
    }

    pub fn set_bitrate(&mut self, bps: i32) {
        if bps > 0 {
            self.ctl(OPUS_SET_BITRATE_REQUEST, bps);
        }
    }

    /// 0..10 (default 5 for fast real time)
    pub fn set_complexity(&mut self, complexity: i32) {
        self.ctl(OPUS_SET_COMPLEXITY_REQUEST, complexity.clamp(0, 10));
    }

    pub fn set_inband_fec(&mut self, enabled: bool) {
        self.ctl(OPUS_SET_INBAND_FEC_REQUEST, c_int::from(enabled));
    }

    /// 0..100 % expected packet loss
    pub fn set_packet_loss_perc(&mut self, perc: i32) {
        self.ctl(OPUS_SET_PACKET_LOSS_PERC_REQUEST, perc.clamp(0, 100));
    }

    pub fn set_vbr(&mut self, vbr: bool) {
        self.ctl(OPUS_SET_VBR_REQUEST, c_int::from(vbr));
    }

    /// Encodes planar channels (`ch[0]`, and `ch[1]` for stereo; a stereo encoder given one channel sends it on both).
    /// Returns the bytes written to `out`, or < 0 on error (also for a channel shorter than `frames`).
    pub fn encode(&mut self, ch: &[&[f32]], frames: usize, out: &mut [u8]) -> i32 {
        if self.enc.is_null() || frames == 0 || out.is_empty() {
            return -1;
        }
        let Some(&l) = ch.first() else { return -1 };
        if l.len() < frames || frames > i32::MAX as usize {
            return -1;
        }
        if self.channels == 1 {
            // SAFETY: `l` holds `frames` samples, `out` its length in bytes; the encoder is live.
            return unsafe {
                opus_encode_float(
                    self.enc,
                    l.as_ptr(),
                    frames as c_int,
                    out.as_mut_ptr(),
                    bytes_cap(out.len()),
                )
            };
        }
        let r = ch.get(1).copied().unwrap_or(l);
        if r.len() < frames {
            return -1;
        }
        let Some(s) = self.scratch.get_mut(..frames * 2) else {
            return -1; // longer than any Opus frame
        };
        for (i, p) in s.as_chunks_mut::<2>().0.iter_mut().enumerate() {
            *p = [l[i], r[i]];
        }
        // SAFETY: `s` holds `frames` interleaved stereo frames, `out` its length in bytes; the encoder is live.
        unsafe {
            opus_encode_float(
                self.enc,
                s.as_ptr(),
                frames as c_int,
                out.as_mut_ptr(),
                bytes_cap(out.len()),
            )
        }
    }

    /// Encodes interleaved audio (`frames` × channels samples). Returns bytes written, or < 0 on error.
    pub fn encode_interleaved(&mut self, pcm: &[f32], frames: usize, out: &mut [u8]) -> i32 {
        if self.enc.is_null()
            || frames == 0
            || out.is_empty()
            || !fits(pcm.len(), frames, self.channels)
        {
            return -1;
        }
        // SAFETY: `pcm` holds `frames` × channels samples, `out` its length in bytes; the encoder is live.
        unsafe {
            opus_encode_float(
                self.enc,
                pcm.as_ptr(),
                frames as c_int,
                out.as_mut_ptr(),
                bytes_cap(out.len()),
            )
        }
    }

    pub fn sample_rate(&self) -> i32 {
        self.sample_rate
    }

    pub fn channels(&self) -> i32 {
        self.channels
    }

    pub fn is_valid(&self) -> bool {
        !self.enc.is_null()
    }
}

pub struct OpusDec {
    dec: *mut OpusDecoder,
    sample_rate: i32,
    channels: i32,
    /// de-interleaving buffer for planar stereo output, sized in `init`
    scratch: Vec<f32>,
}

// SAFETY: the decoder state is owned by this value alone and libopus keeps no thread-bound state.
unsafe impl Send for OpusDec {}

impl Default for OpusDec {
    fn default() -> Self {
        OpusDec {
            dec: ptr::null_mut(),
            sample_rate: 48000,
            channels: 1,
            scratch: Vec::new(),
        }
    }
}

impl Drop for OpusDec {
    fn drop(&mut self) {
        self.destroy();
    }
}

/// (pointer, length) for libopus: no packet or an empty one asks for loss concealment.
fn packet(data: Option<&[u8]>) -> (*const u8, i32) {
    match data {
        Some(d) if !d.is_empty() => (d.as_ptr(), bytes_cap(d.len())),
        _ => (ptr::null(), 0),
    }
}

impl OpusDec {
    pub fn new() -> Self {
        Self::default()
    }

    fn destroy(&mut self) {
        if !self.dec.is_null() {
            // SAFETY: `dec` came from `opus_decoder_create` and is destroyed once.
            unsafe { opus_decoder_destroy(self.dec) };
            self.dec = ptr::null_mut();
        }
    }

    pub fn init(&mut self, sample_rate: i32, channels: i32) -> bool {
        self.destroy();
        let mut err = OPUS_OK;
        // SAFETY: plain constructor; `err` is a valid out pointer.
        let dec = unsafe { opus_decoder_create(sample_rate, channels, &mut err) };
        if err != OPUS_OK || dec.is_null() {
            if !dec.is_null() {
                // SAFETY: created just above, not stored.
                unsafe { opus_decoder_destroy(dec) };
            }
            return false;
        }
        self.dec = dec;
        self.sample_rate = sample_rate;
        self.channels = channels;
        let need = max_frames(sample_rate) * 2;
        if self.scratch.len() < need {
            self.scratch.resize(need, 0.0);
        }
        true
    }

    pub fn reset(&mut self) {
        if !self.dec.is_null() {
            // SAFETY: a live decoder; OPUS_RESET_STATE takes no argument.
            unsafe { opus_decoder_ctl(self.dec, OPUS_RESET_STATE) };
        }
    }

    /// Decodes a packet to planar channels (`ch[1]` optional for stereo). `None` or an empty packet runs loss
    /// concealment for `max_frames`; `fec` decodes the forward error correction data the packet carries for the one
    /// before it. The channels used (one for mono, one or two for stereo) must hold `max_frames`.
    /// Returns frames per channel, or < 0 on error.
    pub fn decode(
        &mut self,
        data: Option<&[u8]>,
        ch: &mut [&mut [f32]],
        max_frames: usize,
        fec: bool,
    ) -> i32 {
        let used = if self.channels == 1 { 1 } else { 2 };
        if self.dec.is_null()
            || max_frames == 0
            || max_frames > i32::MAX as usize
            || ch.is_empty()
            || ch.iter().take(used).any(|c| c.len() < max_frames)
        {
            return -1;
        }
        let (p, n) = packet(data);
        if self.channels == 1 {
            // SAFETY: `ch[0]` holds `max_frames` samples; `p`/`n` describe the packet or are null/0; live decoder.
            return unsafe {
                opus_decode_float(
                    self.dec,
                    p,
                    n,
                    ch[0].as_mut_ptr(),
                    max_frames as c_int,
                    c_int::from(fec),
                )
            };
        }
        // The scratch holds 120 ms, the longest packet; concealment longer than that runs in 120 ms steps (the C++
        // grew its buffer instead, on the audio thread).
        let cap = self.scratch.len() / 2;
        let mut done = 0usize;
        loop {
            let want = (max_frames - done).min(cap);
            // SAFETY: the scratch holds `want` interleaved stereo frames; `p`/`n` as above; live decoder.
            let got = unsafe {
                opus_decode_float(
                    self.dec,
                    p,
                    n,
                    self.scratch.as_mut_ptr(),
                    want as c_int,
                    c_int::from(fec),
                )
            };
            if got <= 0 {
                return if done > 0 { done as i32 } else { got };
            }
            let s = self.scratch[..got as usize * 2].as_chunks::<2>().0;
            let (l, rest) = ch.split_first_mut().expect("checked non-empty");
            for (i, f) in s.iter().enumerate() {
                l[done + i] = f[0];
            }
            if let Some(r) = rest.first_mut() {
                for (i, f) in s.iter().enumerate() {
                    r[done + i] = f[1];
                }
            }
            done += got as usize;
            if !p.is_null() || done >= max_frames {
                return done as i32; // a packet decodes once
            }
        }
    }

    /// Decodes to interleaved audio (`pcm` must hold `max_frames` × channels). `None` / empty runs concealment.
    pub fn decode_interleaved(
        &mut self,
        data: Option<&[u8]>,
        pcm: &mut [f32],
        max_frames: usize,
        fec: bool,
    ) -> i32 {
        if self.dec.is_null() || max_frames == 0 || !fits(pcm.len(), max_frames, self.channels) {
            return -1;
        }
        let (p, n) = packet(data);
        // SAFETY: `pcm` holds `max_frames` × channels samples; `p`/`n` describe the packet or are null/0.
        unsafe {
            opus_decode_float(
                self.dec,
                p,
                n,
                pcm.as_mut_ptr(),
                max_frames as c_int,
                c_int::from(fec),
            )
        }
    }

    /// Packet loss concealment for `frames`.
    pub fn decode_plc(&mut self, ch: &mut [&mut [f32]], frames: usize) -> i32 {
        self.decode(None, ch, frames, false)
    }

    pub fn decode_plc_interleaved(&mut self, pcm: &mut [f32], frames: usize) -> i32 {
        self.decode_interleaved(None, pcm, frames, false)
    }

    pub fn sample_rate(&self) -> i32 {
        self.sample_rate
    }

    pub fn channels(&self) -> i32 {
        self.channels
    }

    pub fn is_valid(&self) -> bool {
        !self.dec.is_null()
    }
}
