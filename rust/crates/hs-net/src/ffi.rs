//! The C functions of libopus and SpeexDSP used here (declared by hand, as in `opus.h`, `speex_echo.h`,
//! `speex_preprocess.h`).
use std::ffi::{c_int, c_void};

pub(crate) type OpusEncoder = c_void;
pub(crate) type OpusDecoder = c_void;
pub(crate) type SpeexEchoState = c_void;
pub(crate) type SpeexPreprocessState = c_void;

pub(crate) const OPUS_OK: c_int = 0;
pub(crate) const OPUS_APPLICATION_VOIP: c_int = 2048;
pub(crate) const OPUS_APPLICATION_AUDIO: c_int = 2049;
pub(crate) const OPUS_APPLICATION_RESTRICTED_LOWDELAY: c_int = 2051;
pub(crate) const OPUS_SET_BITRATE_REQUEST: c_int = 4002;
pub(crate) const OPUS_SET_VBR_REQUEST: c_int = 4006;
pub(crate) const OPUS_SET_COMPLEXITY_REQUEST: c_int = 4010;
pub(crate) const OPUS_SET_INBAND_FEC_REQUEST: c_int = 4012;
pub(crate) const OPUS_SET_PACKET_LOSS_PERC_REQUEST: c_int = 4014;
pub(crate) const OPUS_RESET_STATE: c_int = 4028;

pub(crate) const SPEEX_ECHO_SET_SAMPLING_RATE: c_int = 24;
pub(crate) const SPEEX_PREPROCESS_SET_DENOISE: c_int = 0;
pub(crate) const SPEEX_PREPROCESS_SET_ECHO_SUPPRESS: c_int = 20;
pub(crate) const SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE: c_int = 22;
pub(crate) const SPEEX_PREPROCESS_SET_ECHO_STATE: c_int = 24;

// SAFETY: the signatures match the C headers of the bundled libopus / SpeexDSP.
unsafe extern "C" {
    pub(crate) fn opus_encoder_create(
        fs: i32,
        channels: c_int,
        application: c_int,
        error: *mut c_int,
    ) -> *mut OpusEncoder;
    pub(crate) fn opus_encoder_destroy(st: *mut OpusEncoder);
    pub(crate) fn opus_encoder_ctl(st: *mut OpusEncoder, request: c_int, ...) -> c_int;
    pub(crate) fn opus_encode_float(
        st: *mut OpusEncoder,
        pcm: *const f32,
        frame_size: c_int,
        data: *mut u8,
        max_data_bytes: i32,
    ) -> i32;
    pub(crate) fn opus_decoder_create(
        fs: i32,
        channels: c_int,
        error: *mut c_int,
    ) -> *mut OpusDecoder;
    pub(crate) fn opus_decoder_destroy(st: *mut OpusDecoder);
    pub(crate) fn opus_decoder_ctl(st: *mut OpusDecoder, request: c_int, ...) -> c_int;
    pub(crate) fn opus_decode_float(
        st: *mut OpusDecoder,
        data: *const u8,
        len: i32,
        pcm: *mut f32,
        frame_size: c_int,
        decode_fec: c_int,
    ) -> c_int;

    pub(crate) fn speex_echo_state_init(
        frame_size: c_int,
        filter_length: c_int,
    ) -> *mut SpeexEchoState;
    pub(crate) fn speex_echo_state_destroy(st: *mut SpeexEchoState);
    pub(crate) fn speex_echo_state_reset(st: *mut SpeexEchoState);
    pub(crate) fn speex_echo_ctl(
        st: *mut SpeexEchoState,
        request: c_int,
        ptr: *mut c_void,
    ) -> c_int;
    pub(crate) fn speex_echo_cancellation(
        st: *mut SpeexEchoState,
        rec: *const i16,
        play: *const i16,
        out: *mut i16,
    );
    pub(crate) fn speex_preprocess_state_init(
        frame_size: c_int,
        sampling_rate: c_int,
    ) -> *mut SpeexPreprocessState;
    pub(crate) fn speex_preprocess_state_destroy(st: *mut SpeexPreprocessState);
    pub(crate) fn speex_preprocess_ctl(
        st: *mut SpeexPreprocessState,
        request: c_int,
        ptr: *mut c_void,
    ) -> c_int;
    pub(crate) fn speex_preprocess_run(st: *mut SpeexPreprocessState, x: *mut i16) -> c_int;
}
