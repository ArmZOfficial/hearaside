//! The libobs 32.2.2 ABI this module touches, written by hand (no bindgen / libclang build dependency: the
//! module uses about thirty functions and two structs). Every number below is checked at compile time and
//! comes from `tools/layout_probe.c`, which prints the same facts from the real headers in `external/obs-studio`.
//! A libobs bump means running that probe and diffing it against this file.
use std::ffi::{c_char, c_void};
use std::mem::{offset_of, size_of};

/// `LIBOBS_API_VER` (32.2.2): what `obs_module_ver` reports.
pub const LIBOBS_API_VER: u32 = (32 << 24) | (2 << 16) | 2;

pub const OBS_SOURCE_TYPE_INPUT: u32 = 0;
pub const OBS_SOURCE_AUDIO: u32 = 1 << 1;
pub const OBS_ICON_TYPE_AUDIO_INPUT: u32 = 4;
pub const SPEAKERS_STEREO: u32 = 2;
pub const AUDIO_FORMAT_FLOAT_PLANAR: u32 = 8;
pub const OBS_MONITORING_TYPE_NONE: u32 = 0;
pub const OBS_TEXT_DEFAULT: u32 = 0;
pub const OBS_TEXT_INFO: u32 = 3;
pub const OBS_TEXT_INFO_WARNING: u32 = 1;
pub const OBS_COMBO_TYPE_LIST: u32 = 2;
pub const OBS_COMBO_FORMAT_INT: u32 = 1;
pub const LOG_WARNING: i32 = 200;
pub const LOG_INFO: i32 = 300;

pub const MAX_AV_PLANES: usize = 8;

/// `obs_hotkey_id` (`size_t`)
pub type ObsHotkeyId = usize;

/// `struct obs_source_info`: only the fields the module sets are named, the rest is zeroed padding of the same
/// size (pointer slots: `Option<fn>` is one pointer, null = `None`).
#[repr(C)]
pub struct ObsSourceInfo {
    pub id: *const c_char,
    pub type_: u32,
    pub output_flags: u32,
    pub get_name: Option<unsafe extern "C" fn(*mut c_void) -> *const c_char>,
    pub create: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void>,
    pub destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    _get_width_height: [usize; 2],
    pub get_defaults: Option<unsafe extern "C" fn(*mut c_void)>,
    pub get_properties: Option<unsafe extern "C" fn(*mut c_void) -> *mut c_void>,
    pub update: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    _activate_to_type_data: [usize; 19],
    pub audio_render: usize,
    pub enum_all_sources: usize,
    _transition_start_to_audio_mix: [usize; 5],
    pub icon_type: u32,
    _media_play_pause_to_media_get_state: [usize; 9],
    pub version: u32,
    _unversioned_id_to_filter_add: [usize; 4],
    pub get_dark_icon: usize,
    pub get_light_icon: usize,
}

impl ObsSourceInfo {
    pub const SIZE: usize = size_of::<Self>();

    /// All fields null / 0, like the C++ `obs_source_info info {}`.
    pub const fn zeroed() -> Self {
        // SAFETY: every field is an integer, a raw pointer or an `Option<fn>`: all-zero bytes are valid for each.
        unsafe { std::mem::zeroed() }
    }

    /// The struct as bytes, for [`crate::logic::streamlabs_source_info`].
    pub fn as_bytes(&self) -> &[u8] {
        // SAFETY: a `repr(C)` struct of pointers and integers, `SIZE` bytes long; the padding bytes are zero
        // because the value came from `zeroed()` and fields are only ever assigned whole.
        unsafe { std::slice::from_raw_parts((self as *const Self).cast::<u8>(), Self::SIZE) }
    }
}

/// `struct obs_source_audio`
#[repr(C)]
pub struct ObsSourceAudio {
    pub data: [*const u8; MAX_AV_PLANES],
    pub frames: u32,
    pub speakers: u32,
    pub format: u32,
    pub samples_per_sec: u32,
    pub timestamp: u64,
}

/// OBS's `obs_source_audio` plus the `int64_t dec_frame_pts` the Streamlabs fork appends.
#[repr(C)]
pub struct StreamlabsAudio {
    pub obs: ObsSourceAudio,
    pub dec_frame_pts: i64,
}

const _: () = {
    assert!(size_of::<ObsSourceInfo>() == 424);
    assert!(offset_of!(ObsSourceInfo, id) == 0);
    assert!(offset_of!(ObsSourceInfo, type_) == 8);
    assert!(offset_of!(ObsSourceInfo, output_flags) == 12);
    assert!(offset_of!(ObsSourceInfo, get_name) == 16);
    assert!(offset_of!(ObsSourceInfo, create) == 24);
    assert!(offset_of!(ObsSourceInfo, destroy) == 32);
    assert!(offset_of!(ObsSourceInfo, get_defaults) == 56);
    assert!(offset_of!(ObsSourceInfo, get_properties) == 64);
    assert!(offset_of!(ObsSourceInfo, update) == 72);
    assert!(offset_of!(ObsSourceInfo, audio_render) == 232);
    assert!(offset_of!(ObsSourceInfo, enum_all_sources) == 240);
    assert!(offset_of!(ObsSourceInfo, icon_type) == 288);
    assert!(offset_of!(ObsSourceInfo, version) == 368);
    assert!(offset_of!(ObsSourceInfo, get_dark_icon) == 408);
    assert!(offset_of!(ObsSourceInfo, get_light_icon) == 416);

    assert!(size_of::<ObsSourceAudio>() == 88);
    assert!(offset_of!(ObsSourceAudio, data) == 0);
    assert!(offset_of!(ObsSourceAudio, frames) == 64);
    assert!(offset_of!(ObsSourceAudio, speakers) == 68);
    assert!(offset_of!(ObsSourceAudio, format) == 72);
    assert!(offset_of!(ObsSourceAudio, samples_per_sec) == 76);
    assert!(offset_of!(ObsSourceAudio, timestamp) == 80);

    // plugin.cpp's static_asserts: the fork's dec_frame_pts follows the OBS fields, and OBS's struct ends with
    // the two icon callbacks (the fork has message / get_messages there)
    assert!(offset_of!(StreamlabsAudio, dec_frame_pts) == size_of::<ObsSourceAudio>());
    assert!(
        offset_of!(ObsSourceInfo, enum_all_sources)
            == offset_of!(ObsSourceInfo, audio_render) + size_of::<usize>()
    );
    assert!(
        offset_of!(ObsSourceInfo, get_light_icon) + size_of::<usize>()
            == size_of::<ObsSourceInfo>()
    );
    assert!(
        offset_of!(ObsSourceInfo, get_dark_icon) + size_of::<usize>()
            == offset_of!(ObsSourceInfo, get_light_icon)
    );
};
