//! The libobs-facing half of the module: exported `obs_module_*` entry points, the `obs_source_info` callbacks and
//! the feed thread that calls `obs_source_output_audio`. Logic lives in `logic.rs`.
//!
//! Every callback is called by OBS across the C ABI, so each body runs under `catch_unwind`: a panic must not
//! unwind into OBS (it returns the failure value instead).
use crate::abi::*;
use crate::logic::*;
use crate::remote;
use hs_bus::bus::read_stream_name;
use hs_bus::protocol::{kNameBytes, kNumStreamOuts};
use hs_bus::shm::SharedMemory;
use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::mem::offset_of;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::thread::JoinHandle;

type HotkeyFn = unsafe extern "C" fn(*mut c_void, ObsHotkeyId, *mut c_void, bool);
type ButtonFn = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> bool;

// The libobs functions the module uses (signatures from the 32.2.2 headers). On Windows they are imported from
// obs.dll by name without an import library (`raw-dylib`), the same imports the C++ module gets from the
// obs.lib that CMake builds out of obs.dll. Elsewhere the host process has libobs loaded and resolves them.
#[cfg_attr(windows, link(name = "obs", kind = "raw-dylib"))]
unsafe extern "C" {
    fn blog(log_level: c_int, format: *const c_char, ...);
    fn obs_get_locale() -> *const c_char;
    #[cfg_attr(not(windows), allow(dead_code))] // only the Windows fork detection asks for it
    fn obs_get_version() -> u32;
    fn obs_get_version_string() -> *const c_char;
    fn obs_get_audio() -> *mut c_void;
    fn audio_output_get_sample_rate(audio: *const c_void) -> u32;
    fn os_gettime_ns() -> u64;
    fn os_sleepto_ns(time_target: u64) -> bool;

    fn obs_register_source_s(info: *const ObsSourceInfo, size: usize);
    fn obs_source_output_audio(source: *mut c_void, audio: *const ObsSourceAudio);
    fn obs_source_set_monitoring_type(source: *mut c_void, kind: u32);
    fn obs_source_get_monitoring_type(source: *const c_void) -> u32;

    fn obs_hotkey_register_source(
        source: *mut c_void,
        name: *const c_char,
        description: *const c_char,
        func: HotkeyFn,
        data: *mut c_void,
    ) -> ObsHotkeyId;
    fn obs_hotkey_unregister(id: ObsHotkeyId);

    fn obs_data_get_string(data: *mut c_void, name: *const c_char) -> *const c_char;
    fn obs_data_get_int(data: *mut c_void, name: *const c_char) -> i64;
    fn obs_data_get_bool(data: *mut c_void, name: *const c_char) -> bool;
    fn obs_data_set_bool(data: *mut c_void, name: *const c_char, val: bool);
    fn obs_data_set_default_string(data: *mut c_void, name: *const c_char, val: *const c_char);
    fn obs_data_set_default_int(data: *mut c_void, name: *const c_char, val: i64);

    fn obs_properties_create() -> *mut c_void;
    fn obs_properties_add_text(
        props: *mut c_void,
        name: *const c_char,
        description: *const c_char,
        kind: u32,
    ) -> *mut c_void;
    fn obs_properties_add_list(
        props: *mut c_void,
        name: *const c_char,
        description: *const c_char,
        kind: u32,
        format: u32,
    ) -> *mut c_void;
    fn obs_properties_add_button(
        props: *mut c_void,
        name: *const c_char,
        text: *const c_char,
        callback: ButtonFn,
    ) -> *mut c_void;
    fn obs_property_list_add_int(p: *mut c_void, name: *const c_char, val: i64) -> usize;
    fn obs_property_text_set_info_type(p: *mut c_void, kind: u32);
    fn obs_property_set_long_description(p: *mut c_void, long_description: *const c_char);
}

fn guard<R>(default: R, f: impl FnOnce() -> R) -> R {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(default)
}

/// `thai()`: OBS runs in a Thai locale.
fn thai() -> bool {
    // SAFETY: `obs_get_locale` takes no arguments and returns a NUL-terminated string owned by libobs, or null.
    let loc = unsafe { obs_get_locale() };
    // SAFETY: non-null, so it points at a NUL-terminated string that stays valid for the call.
    let loc = (!loc.is_null()).then(|| unsafe { CStr::from_ptr(loc) });
    is_thai(loc.and_then(|l| l.to_str().ok()))
}

fn cstring(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}

// ---- host detection (OBS or Streamlabs Desktop) --------------------------------------------------------

/// Streamlabs Desktop runs a libobs fork (stream-labs/obs-studio, 31.x). Same functions and signatures as OBS,
/// but two structs are laid out differently: `obs_source_info` has `audio_render_do` right after `audio_render`
/// and `message` / `get_messages` where OBS has the icon callbacks, and `obs_source_audio` ends with an
/// `int64_t dec_frame_pts`. Built for OBS, the module fills the fork's layout itself when it runs there (a
/// mismatch crashes the host).
static STREAMLABS: AtomicBool = AtomicBool::new(false);

enum Host {
    Obs,
    Streamlabs,
    UnknownFork,
}

#[cfg(windows)]
fn host() -> Host {
    use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
    let name: Vec<u16> = "obs.dll\0".encode_utf16().collect();
    // SAFETY: `name` is NUL-terminated UTF-16 and lives across the call; GetModuleHandleW only looks up a loaded module.
    let obs = unsafe { GetModuleHandleW(name.as_ptr()) };
    // SAFETY: `obs` is a valid module handle when non-null, and the name is NUL-terminated ASCII.
    let streamlabs_fork = !obs.is_null()
        && unsafe { GetProcAddress(obs, c"streamlabs_set_audio_flag".as_ptr().cast()) }.is_some();
    if !streamlabs_fork {
        return Host::Obs;
    }
    // SAFETY: no arguments, no preconditions.
    if unsafe { obs_get_version() } >> 24 == 31 {
        Host::Streamlabs // the layout above is the 31.x fork's
    } else {
        Host::UnknownFork
    }
}

#[cfg(not(windows))]
fn host() -> Host {
    Host::Obs
}

// ---- one source ------------------------------------------------------------------------------------------------

struct Source {
    source: *mut c_void,
    shared: Arc<Shared>,
    thread: Option<JoinHandle<()>>,
    hotkeys: Vec<ObsHotkeyId>,
}

struct SendPtr(*mut c_void);
// SAFETY: the `obs_source_t*` is only handed to `obs_source_output_audio`, which libobs allows from any thread,
// and the source (`ss_destroy`) joins the feed thread before OBS can free it.
unsafe impl Send for SendPtr {}

/// `Source::run`: pulls from the bus every 5 ms and hands the block to OBS. After the consumer connected, nothing
/// here allocates, locks or logs (the settings lock is taken only when the settings changed).
fn feed(source: SendPtr, shared: Arc<Shared>) {
    let source = source.0;
    // SAFETY: libobs calls without preconditions beyond a running core; `obs_get_audio` may be null, in which case
    // `audio_output_get_sample_rate` reports 0 and `Feeder::new` falls back to 48 kHz.
    let (rate, mut next) = unsafe {
        (
            audio_output_get_sample_rate(obs_get_audio()),
            os_gettime_ns(),
        )
    };
    let mut feeder = Feeder::new(rate, next);
    let streamlabs = STREAMLABS.load(Ordering::Relaxed);
    while shared.running.load(Ordering::Relaxed) {
        next += TICK_NS;
        // SAFETY: plain libobs time functions.
        if !unsafe { os_sleepto_ns(next) } {
            // we are late (system stall): don't try to catch up a long backlog
            // SAFETY: as above.
            let now = unsafe { os_gettime_ns() };
            if now > next + STALL_NS {
                next = now;
            }
        }
        feeder.apply_config(&shared);
        // SAFETY: as above.
        let now = unsafe { os_gettime_ns() };
        let b = feeder.pull_block(now, &shared);
        let mut data = [std::ptr::null(); MAX_AV_PLANES];
        data[0] = b.left.as_ptr().cast::<u8>();
        data[1] = b.right.as_ptr().cast::<u8>();
        let audio = ObsSourceAudio {
            data,
            frames: b.frames,
            speakers: SPEAKERS_STEREO,
            format: AUDIO_FORMAT_FLOAT_PLANAR,
            samples_per_sec: b.rate,
            timestamp: b.timestamp,
        };
        if streamlabs {
            let sa = StreamlabsAudio {
                obs: audio,
                dec_frame_pts: 0,
            };
            // SAFETY: `source` is alive until `ss_destroy` has joined this thread; the planes point at `feeder`'s
            // buffers, which outlive the call (libobs copies the audio inside it).
            unsafe { obs_source_output_audio(source, &sa.obs) };
        } else {
            // SAFETY: as above.
            unsafe { obs_source_output_audio(source, &audio) };
        }
    }
}

/// `ss_update`'s settings mapping.
///
/// # Safety
/// `settings` must be a live `obs_data_t*`.
unsafe fn read_settings(shared: &Shared, settings: *mut c_void) {
    // SAFETY: caller's contract; the returned string, if any, is NUL-terminated and valid until the next call.
    let bus = unsafe { obs_data_get_string(settings, key::BUS.as_ptr()) };
    // SAFETY: non-null result of the call above.
    let bus = (!bus.is_null()).then(|| unsafe { CStr::from_ptr(bus) });
    // SAFETY: caller's contract.
    let (output, buffer_ms) = unsafe {
        (
            obs_data_get_int(settings, key::OUTPUT.as_ptr()),
            obs_data_get_int(settings, key::BUFFER_MS.as_ptr()),
        )
    };
    let bus = bus.map(CStr::to_string_lossy);
    shared.set_config(Config::from_settings(bus.as_deref(), output, buffer_ms));
}

// ---- obs_source_info callbacks -----------------------------------------------------------------------------------

unsafe extern "C" fn ss_get_name(_type_data: *mut c_void) -> *const c_char {
    SOURCE_NAME.as_ptr()
}

unsafe extern "C" fn ss_update(data: *mut c_void, settings: *mut c_void) {
    guard((), || {
        // SAFETY: OBS passes back the pointer `ss_create` returned (still alive: update never overlaps destroy)
        // and a live settings object.
        unsafe { read_settings(&(*data.cast::<Source>()).shared, settings) };
    });
}

unsafe extern "C" fn hk_panic(
    data: *mut c_void,
    _id: ObsHotkeyId,
    _hotkey: *mut c_void,
    pressed: bool,
) {
    if pressed {
        // SAFETY: `data` is the `Shared` that `ss_create` registered; the hotkeys are unregistered before it is dropped.
        let shared = unsafe { &*data.cast::<Shared>() };
        guard((), || remote::toggle_panic(&shared.bus()));
    }
}

unsafe extern "C" fn hk_preview(
    data: *mut c_void,
    _id: ObsHotkeyId,
    _hotkey: *mut c_void,
    pressed: bool,
) {
    if pressed {
        // SAFETY: as in `hk_panic`.
        let shared = unsafe { &*data.cast::<Shared>() };
        guard((), || remote::toggle_preview(&shared.bus()));
    }
}

unsafe extern "C" fn ss_create(settings: *mut c_void, source: *mut c_void) -> *mut c_void {
    guard(std::ptr::null_mut(), || {
        // a new HEARASIDE source starts with Audio Monitoring off: you already hear the DAW, and OBS
        // monitoring would play everything a second time, later. Only once - a choice made later in
        // Advanced Audio Properties is kept.
        // SAFETY: OBS passes a live settings object and source.
        unsafe {
            if !obs_data_get_bool(settings, key::MONITORING_CHECKED.as_ptr()) {
                obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_NONE);
                obs_data_set_bool(settings, key::MONITORING_CHECKED.as_ptr(), true);
                blog(
                    LOG_INFO,
                    c"[HEARASIDE] new source: audio monitoring set to off".as_ptr(),
                );
            }
        }
        let shared = Arc::new(Shared::default());
        // SAFETY: live settings object, see above.
        unsafe { read_settings(&shared, settings) };
        let feed_shared = shared.clone();
        let feed_source = SendPtr(source);
        let thread = std::thread::Builder::new()
            .name("hearaside-obs".into())
            .spawn(move || {
                if catch_unwind(AssertUnwindSafe(|| feed(feed_source, feed_shared))).is_err() {
                    // SAFETY: a constant format string without conversions.
                    unsafe {
                        blog(
                            LOG_WARNING,
                            c"[HEARASIDE] the audio thread stopped after an internal error"
                                .as_ptr(),
                        )
                    };
                }
            });
        let Ok(thread) = thread else {
            // SAFETY: a constant format string without conversions.
            unsafe {
                blog(
                    LOG_WARNING,
                    c"[HEARASIDE] could not start the audio thread, source not created".as_ptr(),
                )
            };
            return std::ptr::null_mut();
        };

        // hotkeys (Settings > Hotkeys, also usable from Stream Deck); OBS saves them with the source
        let hk_data = Arc::as_ptr(&shared).cast_mut().cast::<c_void>();
        // SAFETY: `source` is live; `hk_data` points at the `Shared` that the returned `Source` keeps alive until
        // `ss_destroy` has unregistered both hotkeys; the callbacks only read it.
        let hotkeys = unsafe {
            vec![
                obs_hotkey_register_source(
                    source,
                    HOTKEY_MUTE_STREAM.as_ptr(),
                    T_HOTKEY_MUTE.pick(thai()).as_ptr(),
                    hk_panic,
                    hk_data,
                ),
                obs_hotkey_register_source(
                    source,
                    HOTKEY_PREVIEW.as_ptr(),
                    T_HOTKEY_PREVIEW.pick(thai()).as_ptr(),
                    hk_preview,
                    hk_data,
                ),
            ]
        };
        Box::into_raw(Box::new(Source {
            source,
            shared,
            thread: Some(thread),
            hotkeys,
        }))
        .cast::<c_void>()
    })
}

unsafe extern "C" fn ss_destroy(data: *mut c_void) {
    guard((), || {
        // SAFETY: the pointer `ss_create` returned, handed back exactly once.
        let mut s = unsafe { Box::from_raw(data.cast::<Source>()) };
        for &id in &s.hotkeys {
            // SAFETY: ids returned by `obs_hotkey_register_source` for this source.
            unsafe { obs_hotkey_unregister(id) };
        }
        s.shared.running.store(false, Ordering::SeqCst);
        if let Some(t) = s.thread.take() {
            let _ = t.join();
        }
    });
}

unsafe extern "C" fn ss_defaults(settings: *mut c_void) {
    guard((), || {
        // SAFETY: OBS passes a live settings object.
        unsafe {
            obs_data_set_default_string(settings, key::BUS.as_ptr(), cstring(DEFAULT_BUS).as_ptr());
            obs_data_set_default_int(settings, key::OUTPUT.as_ptr(), DEFAULT_OUTPUT);
            obs_data_set_default_int(settings, key::BUFFER_MS.as_ptr(), DEFAULT_BUFFER_MS); // adaptive
        }
    });
}

/// re-creates the properties (status)
unsafe extern "C" fn ss_refresh(
    _props: *mut c_void,
    _property: *mut c_void,
    _data: *mut c_void,
) -> bool {
    true
}

/// the user's click: undo it in Advanced Audio Properties
unsafe extern "C" fn ss_monitor_off(
    _props: *mut c_void,
    _property: *mut c_void,
    data: *mut c_void,
) -> bool {
    guard(true, || {
        if !data.is_null() {
            // SAFETY: the source pointer from `ss_create`, alive while its properties are shown.
            unsafe {
                obs_source_set_monitoring_type(
                    (*data.cast::<Source>()).source,
                    OBS_MONITORING_TYPE_NONE,
                )
            };
        }
        true
    })
}

/// The stem names the Hub published on `bus` (best effort: only while the DAW runs); entry 0 is unused.
fn hub_stem_names(bus: &str) -> Vec<String> {
    let mut names = vec![String::new(); kNumStreamOuts];
    if let Ok(shm) = SharedMemory::open_existing(bus) {
        let header = &shm.layout().streamHeader;
        for (i, name) in names.iter_mut().enumerate() {
            if let Some(raw) = read_stream_name(header, i) {
                let n = raw.iter().position(|&c| c == 0).unwrap_or(kNameBytes);
                *name = String::from_utf8_lossy(&raw[..n]).into_owned();
            }
        }
    }
    names
}

unsafe extern "C" fn ss_properties(data: *mut c_void) -> *mut c_void {
    guard(std::ptr::null_mut(), || {
        let s = (!data.is_null()).then(|| {
            // SAFETY: the pointer `ss_create` returned; OBS only asks for properties of a live source.
            unsafe { &*data.cast::<Source>() }
        });
        let thai = thai();
        let (bus, status) = s.map_or_else(
            || (DEFAULT_BUS.to_string(), StatusSnap::default()),
            |s| (s.shared.bus(), s.shared.status.load()),
        );

        // SAFETY: all calls below build a property list owned by OBS; every pointer passed is a live NUL-terminated
        // string (`CString`s are kept in locals across their call) or a handle libobs returned a moment before.
        unsafe {
            let props = obs_properties_create();
            obs_properties_add_text(
                props,
                key::BUS.as_ptr(),
                T_BUS_NAME.pick(thai).as_ptr(),
                OBS_TEXT_DEFAULT,
            );

            let out = obs_properties_add_list(
                props,
                key::OUTPUT.as_ptr(),
                T_OUTPUT.pick(thai).as_ptr(),
                OBS_COMBO_TYPE_LIST,
                OBS_COMBO_FORMAT_INT,
            );
            let names = hub_stem_names(&bus);
            obs_property_list_add_int(out, T_STREAM_MIX.pick(thai).as_ptr(), 0);
            for (i, name) in names.iter().enumerate().skip(1) {
                obs_property_list_add_int(out, cstring(&stem_label(i, name)).as_ptr(), i as i64);
            }

            let buf = obs_properties_add_list(
                props,
                key::BUFFER_MS.as_ptr(),
                T_BUFFER.pick(thai).as_ptr(),
                OBS_COMBO_TYPE_LIST,
                OBS_COMBO_FORMAT_INT,
            );
            for (text, ms) in BUFFER_CHOICES {
                obs_property_list_add_int(buf, text.pick(thai).as_ptr(), ms);
            }

            // OBS plays this source in your headphones as well: you would hear the DAW twice
            if let Some(s) = s
                && obs_source_get_monitoring_type(s.source) != OBS_MONITORING_TYPE_NONE
            {
                let warn = obs_properties_add_text(
                    props,
                    prop::MONITORING_WARNING.as_ptr(),
                    T_MONITORING_WARNING.pick(thai).as_ptr(),
                    OBS_TEXT_INFO,
                );
                obs_property_text_set_info_type(warn, OBS_TEXT_INFO_WARNING);
                obs_properties_add_button(
                    props,
                    prop::MONITOR_OFF.as_ptr(),
                    T_MONITOR_OFF.pick(thai).as_ptr(),
                    ss_monitor_off,
                );
            }

            let status = cstring(&status_text(&status, thai));
            let info = obs_properties_add_text(
                props,
                prop::STATUS_INFO.as_ptr(),
                status.as_ptr(),
                OBS_TEXT_INFO,
            );
            obs_property_set_long_description(info, T_STATUS_HELP.pick(thai).as_ptr());
            obs_properties_add_button(
                props,
                prop::REFRESH.as_ptr(),
                T_REFRESH.pick(thai).as_ptr(),
                ss_refresh,
            );
            props
        }
    })
}

// ---- module entry points (OBS_DECLARE_MODULE, OBS_MODULE_AUTHOR and the obs_module_* functions) -------------------

static MODULE_POINTER: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());

#[unsafe(no_mangle)]
pub extern "C" fn obs_module_set_pointer(module: *mut c_void) {
    MODULE_POINTER.store(module, Ordering::Relaxed);
}

#[unsafe(no_mangle)]
pub extern "C" fn obs_module_ver() -> u32 {
    LIBOBS_API_VER
}

#[unsafe(no_mangle)]
pub extern "C" fn obs_module_author() -> *const c_char {
    MODULE_AUTHOR.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn obs_module_name() -> *const c_char {
    SOURCE_NAME.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn obs_module_description() -> *const c_char {
    MODULE_DESCRIPTION.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn obs_module_load() -> bool {
    guard(false, || {
        let mut info = ObsSourceInfo::zeroed();
        info.id = SOURCE_ID.as_ptr();
        info.type_ = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_AUDIO;
        info.get_name = Some(ss_get_name);
        info.create = Some(ss_create);
        info.destroy = Some(ss_destroy);
        info.update = Some(ss_update);
        info.get_defaults = Some(ss_defaults);
        info.get_properties = Some(ss_properties);
        info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;

        let version = cstring(env!("HEARASIDE_VERSION"));
        match host() {
            // SAFETY: `info` is a complete `obs_source_info` of `SIZE` bytes (`obs_register_source` is this call
            // with `sizeof`); libobs copies it.
            Host::Obs => unsafe { obs_register_source_s(&info, ObsSourceInfo::SIZE) },
            Host::Streamlabs => {
                STREAMLABS.store(true, Ordering::Relaxed);
                let fork = streamlabs_source_info(
                    info.as_bytes(),
                    offset_of!(ObsSourceInfo, enum_all_sources),
                    offset_of!(ObsSourceInfo, get_dark_icon),
                );
                // SAFETY: `fork` holds the fork's layout and its own length; libobs copies it.
                unsafe { obs_register_source_s(fork.as_ptr().cast(), fork.len()) };
            }
            Host::UnknownFork => {
                // a layout this build does not know: no source rather than a crash
                // SAFETY: both `%s` arguments are NUL-terminated strings that outlive the call.
                unsafe {
                    blog(
                        LOG_WARNING,
                        c"[HEARASIDE] Streamlabs libobs %s is not supported by HEARASIDE %s, the source is not registered"
                            .as_ptr(),
                        obs_get_version_string(),
                        version.as_ptr(),
                    );
                }
                return true;
            }
        }

        let suffix = if STREAMLABS.load(Ordering::Relaxed) {
            c", Streamlabs Desktop"
        } else {
            c""
        };
        // SAFETY: both `%s` arguments are NUL-terminated strings that outlive the call.
        unsafe {
            blog(
                LOG_INFO,
                c"[HEARASIDE] OBS source loaded (version %s%s)".as_ptr(),
                version.as_ptr(),
                suffix.as_ptr(),
            );
        }
        true
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn obs_module_unload() {
    guard((), remote::release_all);
}
