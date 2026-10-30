use super::*;
use crate::abi::ObsSourceInfo;
use hs_bus::protocol::HubFlags;
use hs_bus::shm::current_pid;
use hs_engine::hub_engine::{HubEngine, HubParams};
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::mem::offset_of;

// ---- counting allocator (as in hs-engine/tests/rt_safety.rs) ----------------------------------------------------

struct Counting;

thread_local! {
    static ARMED: Cell<bool> = const { Cell::new(false) };
    static COUNT: Cell<usize> = const { Cell::new(0) };
}

fn note() {
    let _ = ARMED.try_with(|a| {
        if a.get() {
            let _ = COUNT.try_with(|c| c.set(c.get() + 1));
        }
    });
}

// SAFETY: forwards to the system allocator; the bookkeeping touches only thread-local Cells.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: same contract as the caller's.
        unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: same contract as the caller's.
        unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        note();
        // SAFETY: same contract as the caller's.
        unsafe { System.realloc(ptr, layout, new_size) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: same contract as the caller's.
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

/// Runs `f` with the counter armed on this thread; returns the allocations it made.
fn allocations(f: impl FnOnce()) -> usize {
    COUNT.with(|c| c.set(0));
    ARMED.with(|a| a.set(true));
    f();
    ARMED.with(|a| a.set(false));
    COUNT.with(Cell::get)
}

fn bus_name(tag: &str) -> String {
    static NEXT: AtomicU32 = AtomicU32::new(0);
    format!(
        "obs_{tag}_{}_{}",
        current_pid(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    )
}

// ---- settings mapping and defaults ---------------------------------------------------------------------------------

#[test]
fn defaults_are_main_output_0_and_adaptive_buffer() {
    assert_eq!(DEFAULT_BUS, "Main");
    assert_eq!(DEFAULT_OUTPUT, 0);
    assert_eq!(DEFAULT_BUFFER_MS, 0);
    assert_eq!(
        Config::default(),
        Config {
            bus: "Main".into(),
            output: 0,
            buffer_ms: 0
        }
    );
    // the defaults the panel shows map to the defaults of a fresh source
    assert_eq!(
        Config::from_settings(Some(DEFAULT_BUS), DEFAULT_OUTPUT, DEFAULT_BUFFER_MS),
        Config::default()
    );
}

#[test]
fn empty_or_missing_bus_name_means_main() {
    assert_eq!(Config::from_settings(None, 0, 0).bus, "Main");
    assert_eq!(Config::from_settings(Some(""), 0, 0).bus, "Main");
    assert_eq!(Config::from_settings(Some("Stage"), 0, 0).bus, "Stage");
}

#[test]
fn a_nonzero_buffer_under_5_ms_means_adaptive() {
    for ms in [-100, -1, 0, 1, 4] {
        assert_eq!(Config::from_settings(None, 0, ms).buffer_ms, 0, "{ms}");
    }
    for ms in [5, 15, 30, 60, 500] {
        assert_eq!(
            Config::from_settings(None, 0, ms).buffer_ms,
            ms as i32,
            "{ms}"
        );
    }
}

#[test]
fn out_of_range_numbers_saturate_instead_of_wrapping() {
    // the C++ `int(obs_data_get_int(..))` turns 2^32 + 30 into 30
    let c = Config::from_settings(None, (1 << 32) + 3, (1 << 32) + 30);
    assert_eq!(c.output, i32::MAX);
    assert_eq!(c.buffer_ms, i32::MAX);
    assert_eq!(Config::from_settings(None, i64::MIN, 0).output, i32::MIN);
}

#[test]
fn shared_starts_dirty_and_every_change_marks_it_dirty_again() {
    let shared = Shared::default();
    let mut feeder = Feeder::new(48000, 0);
    assert_eq!(feeder.applied, 0);
    feeder.apply_config(&shared);
    let first = feeder.applied;
    assert_ne!(
        first, 0,
        "a new source configures the consumer on its first tick"
    );
    feeder.apply_config(&shared);
    assert_eq!(
        feeder.applied, first,
        "unchanged settings are not re-applied"
    );
    shared.set_config(Config::from_settings(Some("Stage"), 2, 30));
    assert_eq!(shared.bus(), "Stage");
    feeder.apply_config(&shared);
    assert_ne!(feeder.applied, first);
}

#[test]
fn thai_is_chosen_by_the_obs_locale_prefix() {
    assert!(is_thai(Some("th-TH")));
    assert!(is_thai(Some("th")));
    assert!(!is_thai(Some("en-US")));
    assert!(!is_thai(Some("")));
    assert!(!is_thai(None));
}

// ---- texts -----------------------------------------------------------------------------------------------------

fn snap(connected: bool, alive: bool, flags: u32) -> StatusSnap {
    StatusSnap {
        connected,
        hub_alive: alive,
        hub_flags: flags,
        daw_rate: 44100,
        buffer_ms: 31.25,
        auto_buffer: true,
        drift_ppm: -12.4,
        underruns: 7,
    }
}

#[test]
fn status_line_in_every_state() {
    assert_eq!(
        status_text(&snap(false, false, 0), false),
        T_WAITING.str(false)
    );
    assert_eq!(
        status_text(&snap(false, true, 0), true),
        T_WAITING.str(true)
    );
    assert_eq!(
        status_text(&snap(true, false, HubFlags::kHubBypassed), false),
        T_BYPASSED.str(false)
    );
    assert_eq!(
        status_text(
            &snap(true, false, HubFlags::kHubBypassed | HubFlags::kHubPanic),
            true
        ),
        T_BYPASSED.str(true)
    );
    assert_eq!(
        status_text(&snap(true, false, 0), false),
        T_NOT_RUNNING.str(false)
    );
    assert_eq!(
        status_text(&snap(true, false, HubFlags::kHubPanic), true),
        T_NOT_RUNNING.str(true)
    );
    assert_eq!(
        status_text(&snap(true, true, 0), false),
        "Connected · DAW 44.1 kHz · buffer 31.2 ms (auto) · drift -12 ppm · underrun 7"
    );
    assert_eq!(
        status_text(&snap(true, true, HubFlags::kHubPanic), false),
        "Connected · DAW 44.1 kHz · buffer 31.2 ms (auto) · drift -12 ppm · underrun 7 · stream muted in Hub"
    );
    let mut fixed = snap(true, true, 0);
    fixed.auto_buffer = false;
    fixed.drift_ppm = 3.6;
    fixed.buffer_ms = 30.0;
    assert_eq!(
        status_text(&fixed, false),
        "Connected · DAW 44.1 kHz · buffer 30.0 ms · drift +4 ppm · underrun 7"
    );
    assert_eq!(
        status_text(&fixed, true),
        "เชื่อมแล้ว · DAW 44.1 kHz · buffer 30.0 ms · drift +4 ppm · underrun 7"
    );
    fixed.auto_buffer = true;
    fixed.hub_flags = HubFlags::kHubPanic;
    assert_eq!(
        status_text(&fixed, true),
        "เชื่อมแล้ว · DAW 44.1 kHz · buffer 30.0 ms (อัตโนมัติ) · drift +4 ppm · underrun 7 · ตัดเสียงคนดูอยู่ใน Hub"
    );
}

#[test]
fn stem_labels() {
    assert_eq!(stem_label(3, ""), "Stem 3");
    assert_eq!(stem_label(3, "Stem 3"), "Stem 3");
    assert_eq!(stem_label(3, "Drums"), "Stem 3 - Drums");
    assert_eq!(stem_label(1, "Stem 3"), "Stem 1 - Stem 3");
}

#[test]
fn buffer_choices_are_auto_15_30_60() {
    let values: Vec<i64> = BUFFER_CHOICES.iter().map(|(_, ms)| *ms).collect();
    assert_eq!(values, [0, 15, 30, 60]);
}

#[test]
fn every_text_has_both_languages() {
    for t in ALL_TEXTS {
        assert!(!t.en.is_empty() && !t.th.is_empty());
        assert_ne!(t.en, t.th);
    }
}

#[test]
fn status_cell_round_trips() {
    let cell = StatusCell::default();
    assert_eq!(cell.load(), StatusSnap::default());
    let st = hs_dsp::consumer::ConsumerStatus {
        connected: true,
        hub_alive: false,
        auto_buffer: true,
        daw_rate: 96000,
        buffer_ms: 12.5,
        drift_ppm: -3.25,
        underruns: 9,
        hub_flags: 5,
        ..Default::default()
    };
    cell.store(&st);
    assert_eq!(
        cell.load(),
        StatusSnap {
            connected: true,
            hub_alive: false,
            hub_flags: 5,
            daw_rate: 96000,
            buffer_ms: 12.5,
            auto_buffer: true,
            drift_ppm: -3.25,
            underruns: 9
        }
    );
}

// ---- timestamps --------------------------------------------------------------------------------------------------

#[test]
fn stamps_follow_a_steady_clock_without_drifting_ahead() {
    let dur = 5_000_000;
    let mut s = Stamper::new(1_000_000_000);
    let mut now = 1_000_000_000;
    let mut prev = 0;
    for _ in 0..2000 {
        now += dur;
        let t = s.stamp(now, dur);
        assert!(t >= prev, "stamps never go backwards");
        prev = t;
        // block start stays within a block of "now - duration"
        assert!(t.abs_diff(now - dur) <= 20_000_000);
    }
}

#[test]
fn stamps_re_anchor_after_a_stall() {
    let dur = 5_000_000;
    let mut s = Stamper::new(1_000_000_000);
    s.stamp(1_005_000_000, dur);
    // 500 ms late: far beyond 20 ms from where the running stamp is
    assert_eq!(s.stamp(1_505_000_000, dur), 1_505_000_000 - dur);
    // and it continues contiguously from there (a little pull towards "fresh", never a gap of more than 1/8)
    let next = s.stamp(1_510_000_000, dur);
    assert!(next >= 1_505_000_000 && next <= 1_505_000_000 + dur);
}

#[test]
fn a_clock_that_starts_before_one_block_stamps_close_to_now() {
    // now <= duration: "fresh" is now itself (no underflow), reached by the usual 1/8 pull from the start stamp
    let mut s = Stamper::new(0);
    assert_eq!(s.stamp(1_000_000, 5_000_000), 125_000);
}

// ---- Streamlabs layout ---------------------------------------------------------------------------------------------

#[test]
fn streamlabs_layout_inserts_one_slot_and_drops_the_icon_callbacks() {
    // mark every 8-byte slot of OBS's struct with its own index
    let mut bytes_in = ObsSourceInfo::zeroed().as_bytes().to_vec();
    for (i, c) in bytes_in.as_chunks_mut::<8>().0.iter_mut().enumerate() {
        c.copy_from_slice(&(i as u64 + 1).to_le_bytes());
    }
    let insert = offset_of!(ObsSourceInfo, enum_all_sources);
    let tail = offset_of!(ObsSourceInfo, get_dark_icon);
    let out = streamlabs_source_info(&bytes_in, insert, tail);
    assert_eq!(out.len(), ObsSourceInfo::SIZE + 8);
    assert_eq!(&out[..insert], &bytes_in[..insert]);
    assert_eq!(
        &out[insert..insert + 8],
        &[0u8; 8],
        "the fork's extra slot is null"
    );
    assert_eq!(
        &out[insert + 8..tail + 8],
        &bytes_in[insert..tail],
        "the rest moves one slot up"
    );
    assert_eq!(
        &out[tail + 8..],
        &[0u8; 16],
        "message / get_messages are null"
    );
}

// ---- the compat contract against the C++ source ------------------------------------------------------------------------

mod cpp {
    //! Reads `obs/hearaside-obs/src/plugin.cpp` (the file this crate ports) so a name, default or text cannot drift
    //! from it unnoticed. Not a C++ parser: only string literals and the few call shapes the file uses.
    pub fn source() -> String {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../obs/hearaside-obs/src/plugin.cpp"
        );
        std::fs::read_to_string(path).unwrap_or_else(|e| panic!("cannot read {path}: {e}"))
    }

    /// Literal (or run of adjacent literals, which C++ concatenates) starting at byte `i` after whitespace;
    /// returns its decoded text and the index after it.
    pub fn literal(src: &str, mut i: usize) -> Option<(String, usize)> {
        let b = src.as_bytes();
        let mut out = String::new();
        let mut found = false;
        loop {
            let mut j = i;
            while j < b.len() && b[j].is_ascii_whitespace() {
                j += 1;
            }
            if j >= b.len() || b[j] != b'"' {
                break;
            }
            j += 1;
            let start = j;
            while j < b.len() && b[j] != b'"' {
                j += if b[j] == b'\\' { 2 } else { 1 };
            }
            out.push_str(&src[start..j].replace("\\\"", "\"").replace("\\\\", "\\"));
            i = j + 1;
            found = true;
        }
        found.then_some((out, i))
    }

    pub fn skip_ws(src: &str, mut i: usize) -> usize {
        while i < src.len() && src.as_bytes()[i].is_ascii_whitespace() {
            i += 1;
        }
        i
    }

    pub fn expect(src: &str, i: usize, c: u8) -> Option<usize> {
        let i = skip_ws(src, i);
        (src.as_bytes().get(i) == Some(&c)).then_some(i + 1)
    }

    /// Every `T("en", "th")` call: (en, th, index after the closing parenthesis).
    pub fn t_calls(src: &str) -> Vec<(String, String, usize)> {
        let mut out = Vec::new();
        for (at, _) in src.match_indices("T(") {
            if at > 0
                && (src.as_bytes()[at - 1].is_ascii_alphanumeric()
                    || src.as_bytes()[at - 1] == b'_')
            {
                continue;
            }
            let parsed = (|| {
                let (en, i) = literal(src, at + 2)?;
                let i = expect(src, i, b',')?;
                let (th, i) = literal(src, i)?;
                let i = expect(src, i, b')')?;
                Some((en, th, i))
            })();
            out.extend(parsed);
        }
        out
    }

    /// The first literal after each `call_prefix` ("obs_properties_add_text(props," ...).
    pub fn first_literals(src: &str, call_prefix: &str) -> Vec<String> {
        src.match_indices(call_prefix)
            .filter_map(|(at, _)| literal(src, at + call_prefix.len()).map(|l| l.0))
            .collect()
    }
}

#[test]
fn compat_ids_hotkeys_and_module_strings_match_plugin_cpp() {
    let src = cpp::source();
    assert_eq!(
        cpp::first_literals(&src, "info.id =")[0],
        SOURCE_ID.to_str().unwrap()
    );
    assert_eq!(
        cpp::first_literals(&src, "obs_hotkey_register_source(source,"),
        [
            HOTKEY_MUTE_STREAM.to_str().unwrap(),
            HOTKEY_PREVIEW.to_str().unwrap()
        ]
    );
    assert_eq!(SOURCE_ID.to_str().unwrap(), "hearaside_source");
    assert_eq!(
        HOTKEY_MUTE_STREAM.to_str().unwrap(),
        "hearaside.mute_stream"
    );
    assert_eq!(HOTKEY_PREVIEW.to_str().unwrap(), "hearaside.preview");
    for (decl, value) in [
        ("OBS_MODULE_AUTHOR(", MODULE_AUTHOR),
        ("obs_module_name(void) { return", SOURCE_NAME),
        ("obs_module_description(void) { return", MODULE_DESCRIPTION),
        ("ss_get_name(void*) { return", SOURCE_NAME),
    ] {
        assert_eq!(
            cpp::first_literals(&src, decl),
            [value.to_str().unwrap()],
            "{decl}"
        );
    }
}

#[test]
fn compat_property_and_setting_keys_match_plugin_cpp() {
    let src = cpp::source();
    let mut props: Vec<String> = [
        "obs_properties_add_text(props,",
        "obs_properties_add_list(props,",
        "obs_properties_add_button(props,",
    ]
    .iter()
    .flat_map(|p| cpp::first_literals(&src, p))
    .collect();
    props.sort();
    let mut ours: Vec<&str> = [
        key::BUS,
        key::OUTPUT,
        key::BUFFER_MS,
        prop::MONITORING_WARNING,
        prop::MONITOR_OFF,
        prop::STATUS_INFO,
        prop::REFRESH,
    ]
    .iter()
    .map(|k| k.to_str().unwrap())
    .collect();
    ours.sort_unstable();
    assert_eq!(props, ours);

    // every key the C++ reads or writes in the saved settings
    let mut settings: Vec<String> = [
        "obs_data_get_string(settings,",
        "obs_data_get_int(settings,",
        "obs_data_get_bool(settings,",
        "obs_data_set_bool(settings,",
    ]
    .iter()
    .flat_map(|p| cpp::first_literals(&src, p))
    .collect();
    settings.sort();
    settings.dedup();
    let mut ours: Vec<&str> = [
        key::BUS,
        key::OUTPUT,
        key::BUFFER_MS,
        key::MONITORING_CHECKED,
    ]
    .iter()
    .map(|k| k.to_str().unwrap())
    .collect();
    ours.sort_unstable();
    assert_eq!(settings, ours);
}

#[test]
fn compat_defaults_match_plugin_cpp() {
    let src = cpp::source();
    let default_str = "obs_data_set_default_string(settings,";
    let at = src.find(default_str).expect("default string") + default_str.len();
    let (k, i) = cpp::literal(&src, at).unwrap();
    let i = cpp::expect(&src, i, b',').unwrap();
    let (v, _) = cpp::literal(&src, i).unwrap();
    assert_eq!(
        (k.as_str(), v.as_str()),
        (key::BUS.to_str().unwrap(), DEFAULT_BUS)
    );

    // `obs_data_set_default_int(settings, "name", N);`
    let ints: Vec<(String, i64)> = src
        .match_indices("obs_data_set_default_int(settings,")
        .map(|(at, p)| {
            let (k, i) = cpp::literal(&src, at + p.len()).unwrap();
            let i = cpp::expect(&src, i, b',').unwrap();
            let i = cpp::skip_ws(&src, i);
            let n: String = src[i..]
                .chars()
                .take_while(|c| c.is_ascii_digit() || *c == '-')
                .collect();
            (k, n.parse().unwrap())
        })
        .collect();
    assert_eq!(
        ints,
        [
            (key::OUTPUT.to_str().unwrap().to_string(), DEFAULT_OUTPUT),
            (
                key::BUFFER_MS.to_str().unwrap().to_string(),
                DEFAULT_BUFFER_MS
            )
        ]
    );
}

#[test]
fn compat_texts_match_plugin_cpp_both_ways() {
    let src = cpp::source();
    let mut theirs: Vec<(String, String)> = cpp::t_calls(&src)
        .into_iter()
        .map(|(en, th, _)| (en, th))
        .collect();
    theirs.sort();
    theirs.dedup();
    let mut ours: Vec<(String, String)> = ALL_TEXTS
        .iter()
        .map(|t| {
            (
                t.en.to_str().unwrap().to_string(),
                t.th.to_str().unwrap().to_string(),
            )
        })
        .collect();
    ours.sort();
    assert_eq!(ours.len(), ALL_TEXTS.len());
    assert_eq!(
        theirs, ours,
        "T(en, th) pairs of plugin.cpp and ALL_TEXTS differ"
    );
}

#[test]
fn compat_buffer_list_matches_plugin_cpp() {
    let src = cpp::source();
    let prefix = "obs_property_list_add_int(buf,";
    let theirs: Vec<(String, String, i64)> = src
        .match_indices(prefix)
        .map(|(at, p)| {
            let (en, th, i) = cpp::t_calls(&src[at + p.len()..])[0].clone();
            let rest = &src[at + p.len() + i..];
            let n: String = rest
                .trim_start_matches([',', ' '])
                .chars()
                .take_while(char::is_ascii_digit)
                .collect();
            (en, th, n.parse().unwrap())
        })
        .collect();
    let ours: Vec<(String, String, i64)> = BUFFER_CHOICES
        .iter()
        .map(|(t, ms)| {
            (
                t.en.to_str().unwrap().to_string(),
                t.th.to_str().unwrap().to_string(),
                *ms,
            )
        })
        .collect();
    assert_eq!(theirs, ours);
}

// ---- the feed loop ---------------------------------------------------------------------------------------------------

fn noise(t: i64) -> f32 {
    let mut h = (t as u32).wrapping_mul(2_654_435_761) ^ 40503u32.wrapping_mul(5);
    h ^= h >> 15;
    h = h.wrapping_mul(2_246_822_519);
    h ^= h >> 13;
    ((h & 0xffff) as f32 / 65535.0 - 0.5) * 0.5
}

#[test]
fn feeder_without_a_hub_gives_silence_and_reports_waiting() {
    let shared = Shared::default();
    shared.set_config(Config::from_settings(Some(&bus_name("none")), 0, 30));
    let mut f = Feeder::new(48000, 1_000_000_000);
    let mut now = 1_000_000_000u64;
    for _ in 0..300 {
        now += TICK_NS;
        f.apply_config(&shared);
        let b = f.pull_block(now, &shared);
        assert_eq!((b.frames, b.rate), (240, 48000));
        assert!(b.left.iter().chain(b.right).all(|s| *s == 0.0));
    }
    assert_eq!(
        status_text(&shared.status.load(), false),
        T_WAITING.str(false)
    );
}

#[test]
fn feeder_rate_zero_falls_back_to_48k() {
    let f = Feeder::new(0, 0);
    assert_eq!((f.rate, f.frames), (48000, 240));
    assert_eq!(Feeder::new(44100, 0).frames, 220);
}

#[test]
fn feeder_delivers_the_hub_audio_with_gapless_stamps_and_allocates_nothing_once_connected() {
    let name = bus_name("feed");
    let mut hub = HubEngine::new();
    hub.connect(&name);
    hub.prepare(44100.0, 256);
    hub.maintain();

    let shared = Shared::default();
    shared.set_config(Config::from_settings(Some(&name), 0, 0)); // adaptive, like a new source
    let mut feeder = Feeder::new(48000, 5_000_000_000);
    let (mut l, mut r) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let (mut t, mut t_daw, mut t_obs) = (0i64, 0.0f64, 0.0f64);
    let mut now = 5_000_000_000u64;
    let mut loud_blocks = 0;
    let mut stamps = Vec::with_capacity(4000); // the counted run must not grow it

    let mut run = |until: f64,
                   feeder: &mut Feeder,
                   hub: &mut HubEngine,
                   stamps: &mut Vec<u64>,
                   loud: &mut i32| {
        while t_obs < until {
            if t_daw <= t_obs {
                for (i, s) in l.iter_mut().enumerate() {
                    *s = noise(t + i as i64);
                }
                r.copy_from_slice(&l);
                hub.process(&mut [&mut l, &mut r], &HubParams::default(), t, true, false);
                t += 256;
                t_daw += 256.0 / (44100.0 * (1.0 + 150e-6));
                continue;
            }
            now += TICK_NS;
            feeder.apply_config(&shared);
            let b = feeder.pull_block(now, &shared);
            if b.left.iter().any(|s| s.abs() > 0.01) {
                *loud += 1;
            }
            assert_eq!(
                (b.frames, b.rate, b.left.len(), b.right.len()),
                (240, 48000, 240, 240)
            );
            stamps.push(b.timestamp);
            t_obs += 0.005;
        }
    };
    // the first ticks connect (open the shm, build the resampler): the counter must see that, or it proves nothing
    let first = allocations(|| run(0.05, &mut feeder, &mut hub, &mut stamps, &mut loud_blocks));
    assert!(first > 0, "the counter missed the connecting pull");
    run(3.0, &mut feeder, &mut hub, &mut stamps, &mut loud_blocks); // resyncs, settles
    let st = shared.status.load();
    assert!(st.connected && st.hub_alive, "{st:?}");
    assert!(st.auto_buffer);
    assert!(
        loud_blocks > 300,
        "the DAW audio reaches OBS ({loud_blocks} loud blocks of ~600)"
    );
    assert!(status_text(&st, false).starts_with("Connected · DAW 44.1 kHz"));

    stamps.clear();
    let n = allocations(|| run(13.0, &mut feeder, &mut hub, &mut stamps, &mut loud_blocks));
    assert_eq!(n, 0, "allocations on the feed path");
    // gapless: each block starts where the previous ended, give or take the gentle pull towards "now"
    for w in stamps.windows(2) {
        assert!(w[1] > w[0], "stamps strictly increase");
        assert!(
            w[1] - w[0] <= 5_000_000 + 5_000_000 / 8 + 1,
            "no gap between blocks: {}",
            w[1] - w[0]
        );
    }
}
