//! `--test-ui` and `--test-account`: the pure parts of the UI (values typed by people, names, share links) and the account
//! basics (state at start, device-code format, token protection, audio independent of the account). The pictures the C++
//! `ui-snapshot` draws are not here: there is no Rust UI before R5.
use crate::hub::{HubOptions, TestHub};
use crate::report::Report;
use crate::track::TrackParams;
use hs_app::links::{self, Kind};
use hs_app::valuetext::{
    FLOOR_DB, edit_db, format_db, format_ms, format_pan, initial_of, parse_db, parse_ms, parse_pan,
    truncate_utf8,
};
use hs_app::{AccountConfig, AccountManager, Language, State, Str, tr_in};
use std::path::Path;

const MINUS: &str = "\u{2212}";
const INFINITY: &str = "\u{221e}";

#[allow(clippy::too_many_lines)]
pub fn run_ui(r: &Report, account_file: &Path) -> i32 {
    let near = |v: Option<f32>, x: f32| v.is_some_and(|v| (v - x).abs() < 1.0e-4);
    // EditableValue: dB (prompt 3.2)
    r.expect("dB: -3", near(parse_db("-3", -30.0, 6.0), -3.0));
    r.expect(
        "dB: U+2212 3",
        near(parse_db(&format!("{MINUS}3"), -30.0, 6.0), -3.0),
    );
    r.expect("dB: -3 dB", near(parse_db("-3 dB", -30.0, 6.0), -3.0));
    r.expect("dB: +2", near(parse_db("+2", -30.0, 6.0), 2.0));
    r.expect("dB: 2,5", near(parse_db("2,5", -30.0, 6.0), 2.5));
    r.expect(
        "dB: -inf = bottom",
        near(parse_db("-inf", -30.0, 6.0), -30.0),
    );
    r.expect(
        "dB: infinity sign = bottom",
        near(parse_db(INFINITY, -30.0, 6.0), -30.0),
    );
    r.expect(
        "dB: out of range clamps",
        near(parse_db("+20", -30.0, 6.0), 6.0) && near(parse_db("-99", -30.0, 6.0), -30.0),
    );
    r.expect(
        "dB: nonsense keeps the old value",
        parse_db("loud", -30.0, 6.0).is_none()
            && parse_db("", -30.0, 6.0).is_none()
            && parse_db("1.2.3", -30.0, 6.0).is_none(),
    );
    r.expect(
        "dB: shown 0.0 dB",
        format_db(0.0, FLOOR_DB, true) == "0.0 dB",
    );
    r.expect(
        "dB: shown with U+2212",
        format_db(-3.0, FLOOR_DB, true) == format!("{MINUS}3.0 dB"),
    );
    r.expect(
        "dB: shown +2.0 dB",
        format_db(2.0, FLOOR_DB, true) == "+2.0 dB",
    );
    r.expect(
        "dB: shown -inf",
        format_db(-30.0, FLOOR_DB, true) == format!("{MINUS}{INFINITY} dB"),
    );
    r.expect(
        "dB: field text",
        edit_db(-30.0, FLOOR_DB) == "-inf"
            && edit_db(2.0, FLOOR_DB) == "+2.0"
            && edit_db(-3.0, FLOOR_DB) == "-3.0",
    );
    // ms
    r.expect(
        "ms: 40 ms",
        near(parse_ms("40 ms", 0.0, 500.0), 40.0) && format_ms(40.0) == "40 ms",
    );
    r.expect("ms: clamps", near(parse_ms("900", 0.0, 500.0), 500.0));
    // pan
    r.expect("pan: L40", near(parse_pan("L40"), -40.0));
    r.expect("pan: C", near(parse_pan("C"), 0.0));
    r.expect("pan: R25", near(parse_pan("R25"), 25.0));
    r.expect("pan: l 40", near(parse_pan("l 40"), -40.0));
    r.expect("pan: -40", near(parse_pan("-40"), -40.0));
    r.expect("pan: R250 clamps", near(parse_pan("R250"), 100.0));
    r.expect("pan: nonsense", parse_pan("left").is_none());
    r.expect(
        "pan: shown",
        format_pan(-40.0) == "L40" && format_pan(0.0) == "C" && format_pan(25.0) == "R25",
    );
    // names: UTF-8 cut at a code point (63 bytes + NUL in shared memory)
    let thai = "เสียงร้องประสานชุดที่สองแบบยาวมากจริงๆ";
    let cut = truncate_utf8(thai, 63);
    r.expect(
        "name: Thai cut fits 63 bytes",
        cut.len() <= 63 && thai.starts_with(cut) && cut.chars().count() == 21,
    );
    let emoji = "Mint \u{1f3a4}\u{1f3a4}\u{1f3a4}";
    r.expect(
        "name: emoji never split",
        truncate_utf8(emoji, 8) == "Mint " && truncate_utf8(emoji, 10) == "Mint \u{1f3a4}",
    );
    r.expect(
        "name: short names untouched",
        truncate_utf8("Vocal", 63) == "Vocal",
    );
    r.expect(
        "initial of a Thai name skips the leading vowel",
        initial_of("เสียง") == "ส",
    );
    // share links (S7 parser)
    r.expect(
        "link: permanent /l/",
        links::parse("https://hearaside.vercel.app/l/7Kq2mW9fAbCd").kind == Kind::Listen,
    );
    r.expect(
        "link: tunnel /s/ with a slash",
        links::parse("https://abc-def.trycloudflare.com/s/7Kq2mW9fAbCd/").kind == Kind::Send,
    );
    r.expect(
        "link: Wi-Fi address",
        links::parse("http://192.168.1.5:47810/s/7Kq2mW9fAbCd").kind == Kind::Send,
    );
    r.expect(
        "link: token kept",
        links::parse("https://x.example/l/7Kq2mW9fAbCd?x=1").token == "7Kq2mW9fAbCd",
    );
    r.expect(
        "link: not a link",
        links::parse("hello").kind == Kind::None
            && links::parse("https://example.com/").kind == Kind::None
            && links::parse("https://example.com/x/7Kq2mW9fAbCd").kind == Kind::None
            && links::parse("ftp://h/l/7Kq2mW9fAbCd").kind == Kind::None,
    );
    r.expect(
        "link: short token refused",
        links::parse("https://h/l/abc").kind == Kind::None,
    );
    // buffer warning (PLAN 1.11): both languages have the text (the English one names the 128 limit)
    let (en, th) = (
        tr_in(Str::FriendBufferWarning, Language::English),
        tr_in(Str::FriendBufferWarning, Language::Thai),
    );
    r.expect(
        "buffer warning text exists in Thai and English",
        en.contains("buffer") && en.contains("128") && th != "?" && th != en,
    );
    let account = run_account(r, account_file, false);
    // C++ bug: the summary line was printed before the last check and without the buffer warning's result
    let ui = r.finish("ui");
    i32::from(ui != 0 || account != 0)
}

/// `--test-account`: the account manager's rules, token protection and "audio never waits for the account".
pub fn run_account_only(r: &Report, account_file: &Path) -> i32 {
    run_account(r, account_file, true)
}

fn run_account(r: &Report, account_file: &Path, summary: bool) -> i32 {
    let before = r.failures();
    // the tools keep their own account file, never the user's; no sign-in is attempted
    let mgr = AccountManager::new(AccountConfig {
        storage_file: Some(account_file.to_path_buf()),
        lock_name: Some("HEARASIDE_account_refresh_testkit".into()),
        ..AccountConfig::default()
    });
    // 1. initial state
    r.expect(
        "account starts signed-out or offline",
        matches!(mgr.state(), State::SignedOut | State::Offline),
    );
    // 2. server URL
    r.expect(
        "serverBaseUrl defaults to valid URL",
        mgr.server_base_url().starts_with("http"),
    );
    // 3. the stored tokens are protected (Windows: DPAPI) and come back
    r.expect(
        "DPAPI CryptProtect/Unprotect roundtrip",
        hs_app::vault_roundtrip(),
    );
    // 4. audio safety: the Hub and a Track keep processing while the account manager is alive
    let settings =
        std::env::temp_dir().join(format!("hs-testkit-account-{}.xml", std::process::id()));
    {
        let mut hub = TestHub::new(&HubOptions {
            bus: format!("TestAccountAudio-{}", std::process::id()),
            settings_path: Some(settings.clone()),
            demo_tracks: false,
            ..HubOptions::default()
        });
        let track = hub.add_track("Test", TrackParams::default());
        hub.pump(400);
        let ok = hub.bus().is_some_and(|b| {
            let hb = b
                .header
                .hubHeartbeatNs
                .load(std::sync::atomic::Ordering::Relaxed);
            hb != 0
                && hs_bus::shm::now_ns().saturating_sub(hb) < 100_000_000
                && b.header
                    .hubBlockSize
                    .load(std::sync::atomic::Ordering::Relaxed)
                    == 256
        }) && track.is_some_and(|t| t.peak_in() > 0.01); // the Track's sine reached the bus
        r.expect(
            "audio processing completely unaffected by account manager",
            ok,
        );
    }
    // 5. HEARASIDE_TEST_LIVE: the real server's sign-in code and the panel pictures need the UI (R5)
    if std::env::var_os("HEARASIDE_TEST_LIVE").is_some_and(|v| !v.is_empty()) {
        r.line("SKIPPED: HEARASIDE_TEST_LIVE (sign-in code from the live server, panel screenshots) needs the Rust UI (R5)");
    }
    drop(mgr);
    let _ = std::fs::remove_file(&settings);
    if summary {
        return r.finish("account");
    }
    i32::from(r.failures() != before)
}
