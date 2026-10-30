//! The settings file is shared with the C++ plug-ins: a file the C++ wrote must load here, and a file written here must be
//! one `juce::PropertiesFile` reads. The fixtures are in the format JUCE 8 writes (CRLF, `&#N;` for non-ASCII, a value that is
//! XML stored as a child element), copied from a real `settings-*.xml` written by `ui-snapshot`.
mod common;
use common::TempDir;
use hs_app::settings::{default_settings_path, user_app_data_dir};
use hs_app::{Appearance, Language, PropertiesFile, Settings, ThemeMode};
use std::sync::{Arc, Mutex};

/// A file exactly as the C++ wrote it on the developer's machine (`%APPDATA%/HEARASIDE/settings-ui-snapshot.xml`).
const REAL_CPP_FILE: &str = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n\r\n<PROPERTIES>\r\n  <VALUE name=\"flag.restApi\" val=\"1\"/>\r\n  <VALUE name=\"restApiKey\" val=\"0123456789abcdef0123456789abcdef\"/>\r\n</PROPERTIES>\r\n";

/// What the C++ Settings leaves after a user chose Thai, dark, size 1.25, a share site, a password with Thai letters and
/// `&`, the Karaoke app's audio device state (XML), one window size and a line-up limit outside the range.
const RICH_CPP_FILE: &str = concat!(
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n\r\n<PROPERTIES>\r\n",
    "  <VALUE name=\"language\" val=\"th\"/>\r\n",
    "  <VALUE name=\"theme\" val=\"dark\"/>\r\n",
    "  <VALUE name=\"uiScale\" val=\"1.25\"/>\r\n",
    "  <VALUE name=\"uiScaleAuto\" val=\"0\"/>\r\n",
    "  <VALUE name=\"reduceMotion\" val=\"1\"/>\r\n",
    "  <VALUE name=\"glassAlpha\" val=\"0.7\"/>\r\n",
    "  <VALUE name=\"hostColours\" val=\"0\"/>\r\n",
    "  <VALUE name=\"shareBase\" val=\"https://share.example.com\"/>\r\n",
    "  <VALUE name=\"listenPassword\" val=\"&#3619;&#3627;&#3633;&#3626; &amp; &lt;x&gt;\"/>\r\n",
    "  <VALUE name=\"window.hub\" val=\"900x640\"/>\r\n",
    "  <VALUE name=\"window.track\" val=\"abc\"/>\r\n",
    "  <VALUE name=\"lineUpLimitMs\" val=\"5000\"/>\r\n",
    "  <VALUE name=\"flag.permanentLinksOff\" val=\"1\"/>\r\n",
    "  <VALUE name=\"flag.restApi\" val=\"true\"/>\r\n",
    "  <VALUE name=\"virtualMicDevice\" val=\"CABLE Input (VB-Audio Virtual Cable)\"/>\r\n",
    "  <VALUE name=\"audioDeviceState.karaoke\">\r\n",
    "    <DEVICESETUP deviceType=\"Windows Audio\" audioOutputDeviceName=\"Speakers (Realtek)\" audioInputDeviceName=\"Mic &amp; Line\"\r\n",
    "                 audioDeviceRate=\"48000.0\" audioDeviceBufferSize=\"256\"/>\r\n",
    "  </VALUE>\r\n",
    "  <VALUE name=\"someFutureKey\" val=\"kept\"/>\r\n",
    "</PROPERTIES>\r\n"
);

fn write(dir: &TempDir, text: &str) -> std::path::PathBuf {
    let p = dir.file("settings.xml");
    std::fs::write(&p, text).unwrap();
    p
}

fn text(p: &std::path::Path) -> String {
    String::from_utf8(std::fs::read(p).unwrap()).unwrap()
}

#[test]
fn real_cpp_file_loads() {
    let d = TempDir::new("real");
    let s = Settings::open(write(&d, REAL_CPP_FILE));
    assert!(s.rest_api());
    assert_eq!(s.rest_api_key(), "0123456789abcdef0123456789abcdef");
    assert_eq!(
        s.language(),
        Language::English,
        "defaults for what the file does not say"
    );
}

#[test]
fn defaults_without_a_file() {
    let d = TempDir::new("defaults");
    let p = d.file("settings.xml");
    let s = Settings::open(&p);
    assert_eq!(s.language(), Language::English);
    assert_eq!(s.theme_mode(), ThemeMode::Auto);
    assert_eq!((s.ui_scale(), s.ui_scale_chosen()), (1.0, false));
    assert!(!s.reduce_motion() && s.host_colours());
    assert_eq!(s.glass_alpha(), 0.62);
    assert_eq!(s.share_base(), "");
    assert!(s.permanent_links() && !s.friend_approval_required() && !s.rest_api());
    assert_eq!((s.line_up_limit_ms(), s.window_size("hub")), (600, (0, 0)));
    assert_eq!(
        (
            s.listen_password(),
            s.rest_api_key(),
            s.virtual_mic_device(),
            s.audio_device_state("karaoke")
        ),
        Default::default()
    );
    assert!(!p.exists(), "reading never creates the file");
}

#[test]
fn rich_cpp_file_loads_with_the_cpp_rules() {
    let d = TempDir::new("rich");
    let s = Settings::open(write(&d, RICH_CPP_FILE));
    assert_eq!(s.language(), Language::Thai);
    assert_eq!(s.theme_mode(), ThemeMode::Dark);
    assert_eq!((s.ui_scale(), s.ui_scale_chosen()), (1.25, true));
    assert!(s.reduce_motion() && !s.host_colours());
    assert_eq!(s.glass_alpha(), 0.7);
    assert_eq!(s.share_base(), "https://share.example.com");
    assert_eq!(
        s.listen_password(),
        "รหัส & <x>",
        "&#N; entities decode to the Thai letters"
    );
    assert_eq!(s.window_size("hub"), (900, 640));
    assert_eq!(s.window_size("track"), (0, 0), "not WxH: none");
    assert_eq!(s.window_size("app"), (0, 0));
    assert_eq!(s.line_up_limit_ms(), 1000, "clamped on read like jlimit");
    assert!(!s.permanent_links());
    assert!(
        !s.flag("restApi"),
        "getBoolValue is getIntValue != 0: \"true\" is false, as in the C++"
    );
    assert_eq!(
        s.virtual_mic_device(),
        "CABLE Input (VB-Audio Virtual Cable)"
    );
    // a value that is XML comes back as single-line XML (XmlElement::toString singleLine)
    assert_eq!(
        s.audio_device_state("karaoke"),
        "<DEVICESETUP deviceType=\"Windows Audio\" audioOutputDeviceName=\"Speakers (Realtek)\" audioInputDeviceName=\"Mic &amp; Line\" audioDeviceRate=\"48000.0\" audioDeviceBufferSize=\"256\"/>"
    );
}

#[test]
fn out_of_range_values_in_the_file_are_clamped_like_jlimit() {
    let d = TempDir::new("clamp");
    let p = write(
        &d,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n\r\n<PROPERTIES>\r\n  <VALUE name=\"uiScale\" val=\"0.5\"/>\r\n  <VALUE name=\"glassAlpha\" val=\"7\"/>\r\n</PROPERTIES>\r\n",
    );
    let s = Settings::open(p);
    assert_eq!(s.ui_scale(), 1.0);
    assert_eq!(s.glass_alpha(), 0.95);
    assert!(
        s.ui_scale_chosen(),
        "uiScale present and uiScaleAuto absent: chosen"
    );
}

#[test]
fn what_rust_writes_is_what_juce_writes() {
    let d = TempDir::new("write");
    let p = d.file("settings.xml");
    let mut s = Settings::open(&p);
    s.set_language(Language::Thai);
    s.set_theme_mode(ThemeMode::Dark);
    s.set_listen_password("รหัส & <x> \"q\"");
    s.set_window_size("hub", 900, 640);
    s.set_window_size("bad", 0, 5);
    s.set_line_up_limit_ms(5000);
    s.set_flag("triedPreview", true);
    s.set_audio_device_state(
        "karaoke",
        "<DEVICESETUP deviceType=\"Windows Audio\" rate=\"48000.0\"/>",
    );
    s.set_share_base("  Share.Example.com/// ");
    let expected = concat!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n\r\n<PROPERTIES>\r\n",
        "  <VALUE name=\"language\" val=\"th\"/>\r\n",
        "  <VALUE name=\"theme\" val=\"dark\"/>\r\n",
        "  <VALUE name=\"listenPassword\" val=\"&#3619;&#3627;&#3633;&#3626; &amp; &lt;x&gt; &quot;q&quot;\"/>\r\n",
        "  <VALUE name=\"window.hub\" val=\"900x640\"/>\r\n",
        "  <VALUE name=\"lineUpLimitMs\" val=\"1000\"/>\r\n",
        "  <VALUE name=\"flag.triedPreview\" val=\"1\"/>\r\n",
        "  <VALUE name=\"audioDeviceState.karaoke\"><DEVICESETUP deviceType=\"Windows Audio\" rate=\"48000.0\"/></VALUE>\r\n",
        "  <VALUE name=\"shareBase\" val=\"https://Share.Example.com\"/>\r\n",
        "</PROPERTIES>\r\n"
    );
    assert!(!p.exists(), "nothing is written before tick / flush / drop");
    assert!(s.flush());
    assert_eq!(text(&p), expected);
    // and it loads again, to the same values
    let again = Settings::open(&p);
    assert_eq!(
        (again.language(), again.theme_mode()),
        (Language::Thai, ThemeMode::Dark)
    );
    assert_eq!(again.listen_password(), "รหัส & <x> \"q\"");
    assert_eq!(
        again.audio_device_state("karaoke"),
        "<DEVICESETUP deviceType=\"Windows Audio\" rate=\"48000.0\"/>"
    );
    // the folder holds the file and nothing else (no temp files left behind)
    let names: Vec<_> = std::fs::read_dir(&d.0)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    assert_eq!(names, ["settings.xml"]);
    // the written file is well-formed XML with the structure JUCE expects
    let t = text(&p);
    let doc = roxmltree::Document::parse(&t).unwrap();
    assert_eq!(doc.root_element().tag_name().name(), "PROPERTIES");
    assert!(
        doc.root_element()
            .children()
            .filter(|n| n.is_element())
            .all(|n| n.tag_name().name() == "VALUE" && n.attribute("name").is_some())
    );
}

#[test]
fn unknown_keys_and_xml_values_survive_a_rust_save() {
    let d = TempDir::new("keep");
    let p = write(&d, RICH_CPP_FILE);
    let mut s = Settings::open(&p);
    s.set_language(Language::English);
    assert!(s.flush()); // rewrites the file
    let t = text(&p);
    assert!(t.contains("<VALUE name=\"someFutureKey\" val=\"kept\"/>"));
    assert!(t.contains(
        "<VALUE name=\"audioDeviceState.karaoke\"><DEVICESETUP deviceType=\"Windows Audio\""
    ));
    assert!(t.contains("&#3619;&#3627;&#3633;&#3626; &amp; &lt;x&gt;"));
    let f = PropertiesFile::open(&p);
    assert_eq!(f.get("someFutureKey"), Some("kept"));
    assert_eq!(f.get("language"), Some("en"));
    assert_eq!(f.get("window.track"), Some("abc"));
}

#[test]
fn bug_a_value_that_only_starts_like_xml_is_not_changed() {
    // C++ bug: PropertiesFile stored any value that parseXML could read the start of as a child element, so
    // "<b>x</b> and more" came back as "<b>x</b>" (the password lost its tail).
    let d = TempDir::new("xmlish");
    let p = d.file("settings.xml");
    let mut s = Settings::open(&p);
    for v in [
        "<b>x</b> and more",
        "<oops",
        "a <b/> b",
        "<a><b></a>",
        "  <A/>  junk",
    ] {
        s.set_listen_password(v);
        assert!(s.flush());
        assert_eq!(Settings::open(&p).listen_password(), v, "{v:?}");
    }
}

#[test]
fn bug_two_processes_do_not_undo_each_other() {
    // C++ bug: each process wrote its whole in-memory copy, so a DAW that stayed open overwrote what another DAW saved.
    let d = TempDir::new("merge");
    let p = d.file("settings.xml");
    let mut a = Settings::open(&p);
    let mut b = Settings::open(&p);
    a.set_window_size("hub", 800, 600);
    assert!(a.flush());
    b.set_share_base("https://b.example.com");
    b.set_flag("startHidden", true);
    assert!(b.flush());
    a.set_language(Language::Thai);
    assert!(a.flush());
    let c = Settings::open(&p);
    assert_eq!(c.window_size("hub"), (800, 600));
    assert_eq!(c.share_base(), "https://b.example.com");
    assert!(c.flag("startHidden"));
    assert_eq!(c.language(), Language::Thai);
    // a key one process removed stays removed in the file
    let mut f = PropertiesFile::open(&p);
    assert!(f.get("window.hub").is_some());
    let other = PropertiesFile::open(&p);
    assert_eq!(other.get("shareBase"), Some("https://b.example.com"));
    f.set("window.hub", "1x1");
    assert!(f.flush());
    assert_eq!(Settings::open(&p).window_size("hub"), (1, 1));
}

#[test]
fn bug_a_damaged_file_is_kept_not_silently_overwritten() {
    // C++ bug: a settings file that could not be parsed was replaced by the next save, without a trace.
    let d = TempDir::new("damaged");
    let p = write(&d, "this is not xml at all \u{0}\u{1}");
    let mut s = Settings::open(&p);
    assert_eq!(s.language(), Language::English, "treated as empty");
    s.set_language(Language::Thai);
    assert!(s.flush());
    assert!(text(&p).contains("name=\"language\" val=\"th\""));
    let bad = d.file("settings.xml.bad");
    assert!(bad.exists(), "the damaged file was copied aside");
    assert!(text(&bad).starts_with("this is not xml"));
}

#[test]
fn appearance_hook_and_change_callback() {
    let d = TempDir::new("hook");
    let mut s = Settings::open(d.file("settings.xml"));
    let seen: Arc<Mutex<Vec<Appearance>>> = Arc::default();
    let changes: Arc<Mutex<u32>> = Arc::default();
    let (sc, cc) = (Arc::clone(&seen), Arc::clone(&changes));
    s.set_appearance_hook(Some(Box::new(move |a| sc.lock().unwrap().push(a.clone()))));
    s.set_on_change(Some(Box::new(move || *cc.lock().unwrap() += 1)));
    s.set_language(Language::Thai);
    s.set_language(Language::Thai); // same value: nothing
    s.set_window_size("hub", 100, 100); // quiet
    s.set_share_base("x.example.com");
    assert_eq!(seen.lock().unwrap().len(), 1);
    assert_eq!(seen.lock().unwrap()[0].language, Some(Language::Thai));
    assert_eq!(*changes.lock().unwrap(), 1);
    // the account's copy comes down: applied, saved, but not reported as a local change
    s.apply_appearance(&Appearance {
        theme: Some(ThemeMode::Light),
        ui_scale: Some(1.5),
        glass_alpha: Some(0.1),
        ..Appearance::default()
    });
    assert_eq!(
        (s.theme_mode(), s.ui_scale(), s.glass_alpha(), s.language()),
        (ThemeMode::Light, 1.5, 0.35, Language::Thai)
    );
    assert!(s.ui_scale_chosen());
    assert_eq!(
        seen.lock().unwrap().len(),
        1,
        "apply_appearance stays silent"
    );
    s.set_reduce_motion(true);
    assert_eq!(seen.lock().unwrap().len(), 2);
    let ap = s.appearance();
    assert_eq!(
        (ap.ui_scale, ap.reduce_motion, ap.theme),
        (Some(1.5), Some(true), Some(ThemeMode::Light))
    );
}

#[test]
fn ui_scale_chosen_rules() {
    let d = TempDir::new("scale");
    let p = d.file("settings.xml");
    let mut s = Settings::open(&p);
    s.set_ui_scale_auto(1.4);
    assert_eq!((s.ui_scale(), s.ui_scale_chosen()), (1.4, false));
    assert!(s.flush());
    assert!(
        !Settings::open(&p).ui_scale_chosen(),
        "auto size is saved as not chosen"
    );
    s.set_ui_scale(3.0);
    assert_eq!((s.ui_scale(), s.ui_scale_chosen()), (2.0, true));
    s.set_ui_scale(2.0); // same size: still marks it chosen
    assert!(s.flush());
    assert!(Settings::open(&p).ui_scale_chosen());
}

#[test]
fn share_base_is_cleaned_up() {
    let d = TempDir::new("base");
    let mut s = Settings::open(d.file("settings.xml"));
    s.set_default_share_base("https://built-in.example");
    assert_eq!(
        s.share_base(),
        "https://built-in.example",
        "the build option while the file has none"
    );
    s.set_share_base("");
    assert_eq!(
        s.share_base(),
        "",
        "an empty value in the file means no permanent links"
    );
    s.set_share_base("HTTP://127.0.0.1:3000/");
    assert_eq!(s.share_base(), "HTTP://127.0.0.1:3000");
    s.set_share_base("my.site");
    assert_eq!(s.share_base(), "https://my.site");
}

#[test]
fn default_paths() {
    let p = default_settings_path().expect("a home folder");
    assert_eq!(p.parent().unwrap().file_name().unwrap(), "HEARASIDE");
    assert_eq!(p.extension().unwrap(), "xml");
    let app = user_app_data_dir().expect("app data");
    if cfg!(windows) {
        assert_eq!(
            p.parent().unwrap().parent().unwrap(),
            app,
            "%APPDATA%\\HEARASIDE\\settings.xml"
        );
    }
}

fn real_read(p: &std::path::Path) -> std::io::Result<Vec<u8>> {
    std::fs::read(p)
}

fn sharing_violation(_: &std::path::Path) -> std::io::Result<Vec<u8>> {
    Err(std::io::Error::from(std::io::ErrorKind::PermissionDenied))
}

#[test]
fn bug_a_second_change_does_not_write_back_a_stale_value() {
    // Review bug: the merge protected another process's change once; the cached fields stayed stale and the next change
    // wrote the old language over it.
    let d = TempDir::new("stale");
    let p = d.file("settings.xml");
    let mut a = Settings::open(&p);
    let mut b = Settings::open(&p);
    b.set_language(Language::Thai);
    assert!(b.flush());
    a.set_ui_scale(1.5);
    assert!(a.flush());
    a.set_ui_scale(1.7);
    assert!(a.flush());
    assert_eq!(Settings::open(&p).language(), Language::Thai, "still B's");
    assert_eq!(
        a.language(),
        Language::Thai,
        "A reads B's value after its write"
    );
    a.set_theme_mode(ThemeMode::Dark);
    assert!(a.flush());
    let c = Settings::open(&p);
    assert_eq!(
        (c.language(), c.theme_mode(), c.ui_scale()),
        (Language::Thai, ThemeMode::Dark, 1.7)
    );
    // a change B makes while A has a change waiting is also kept
    a.set_reduce_motion(true);
    b.set_glass_alpha(0.9);
    assert!(b.flush());
    assert!(a.flush());
    let c = Settings::open(&p);
    assert_eq!((c.reduce_motion(), c.glass_alpha()), (true, 0.9));
}

#[test]
fn bug_a_file_that_cannot_be_read_for_a_moment_is_not_wiped() {
    // Review bug: an I/O error while reading at save time counted as an empty file, so the merge wrote only the changed
    // keys and the window sizes, REST key, password and audio device state were lost.
    let d = TempDir::new("unreadable");
    let p = write(&d, RICH_CPP_FILE);
    let mut s = Settings::open(&p);
    s.raw_mut().set_reader(sharing_violation);
    s.set_window_size("hub", 1, 1);
    assert!(!s.flush(), "cannot be saved while the file cannot be read");
    assert!(s.raw().needs_saving(), "the change stays pending");
    assert_eq!(text(&p), RICH_CPP_FILE, "the file is untouched");
    s.raw_mut().set_reader(real_read);
    assert!(s.flush(), "saved once it can be read again");
    let f = PropertiesFile::open(&p);
    assert_eq!(f.get("window.hub"), Some("1x1"));
    assert_eq!(f.get("listenPassword"), Some("รหัส & <x>"));
    assert!(f.get("audioDeviceState.karaoke").is_some());
    assert_eq!(f.get("someFutureKey"), Some("kept"));
    assert!(
        !d.file("settings.xml.bad").exists(),
        "an unreadable file is not a damaged one"
    );
}

#[cfg(windows)]
#[test]
fn bug_a_real_sharing_violation_does_not_wipe_the_file() {
    use std::os::windows::fs::OpenOptionsExt;
    let d = TempDir::new("sharing");
    let p = write(&d, RICH_CPP_FILE);
    let mut s = Settings::open(&p);
    s.set_window_size("app", 3, 3);
    let held = std::fs::OpenOptions::new()
        .read(true)
        .share_mode(0)
        .open(&p)
        .unwrap();
    assert!(!s.flush());
    drop(held);
    assert!(s.flush());
    let f = PropertiesFile::open(&p);
    assert_eq!(f.get("window.app"), Some("3x3"));
    assert_eq!(f.get("someFutureKey"), Some("kept"));
}

#[test]
fn changes_are_batched_and_written_by_tick_flush_or_drop() {
    // Review: every change wrote the file; now like the C++ (500 ms), a burst is one write.
    let d = TempDir::new("batch");
    let p = d.file("settings.xml");
    let mut s = Settings::open(&p);
    for i in 0..100 {
        s.set_window_size("hub", 100 + i, 50);
    }
    assert_eq!(s.raw().writes(), 0);
    assert!(!p.exists());
    s.tick();
    assert_eq!(s.raw().writes(), 0, "too soon for tick");
    assert!(s.flush());
    assert_eq!(s.raw().writes(), 1, "a hundred changes, one write");
    assert_eq!(Settings::open(&p).window_size("hub"), (199, 50));
    assert!(
        s.flush() && s.raw().writes() == 1,
        "nothing pending: nothing written"
    );
    // tick writes once the change is old enough
    s.set_window_size("hub", 7, 7);
    assert!(common::wait_for(3000, || {
        s.tick();
        s.raw().writes() == 2
    }));
    assert_eq!(Settings::open(&p).window_size("hub"), (7, 7));
    // and drop writes what is pending
    s.set_window_size("hub", 8, 8);
    drop(s);
    assert_eq!(Settings::open(&p).window_size("hub"), (8, 8));
}

#[test]
fn bug_choosing_the_size_that_is_already_shown_is_remembered() {
    // C++ bug: setUiScale(s) with s equal to the shown size wrote only uiScaleAuto=0 and no uiScale, so on a fresh install
    // the choice was forgotten at the next start (chosen needs both keys).
    let d = TempDir::new("scale-same");
    let p = d.file("settings.xml");
    let mut s = Settings::open(&p);
    s.set_ui_scale(1.0);
    assert!(s.flush());
    let again = Settings::open(&p);
    assert!(again.ui_scale_chosen());
    assert_eq!(again.ui_scale(), 1.0);
}
