//! The Rust string table is generated from `plugins/common/Strings.{h,cpp}` by `build.rs`. These tests read the C++ files
//! again, independently of the generator, and check that nothing is missing or different.
use hs_app::{
    Language, STR_COUNT as COUNT, Str, current_language, format_db, set_current_language, tr,
    tr_in, trf,
};
use std::collections::HashSet;

fn common(file: &str) -> String {
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../plugins/common")
        .join(file);
    String::from_utf8(std::fs::read(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))).unwrap()
}

/// Every `Str::Name` right after `s(` in Strings.cpp.
fn cpp_keys() -> Vec<String> {
    let cpp = common("Strings.cpp");
    let mut out = Vec::new();
    let mut rest = cpp.as_str();
    while let Some(i) = rest.find("s(Str::") {
        rest = &rest[i + 7..];
        let end = rest
            .find(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
            .unwrap();
        out.push(rest[..end].to_owned());
    }
    out
}

#[test]
fn every_key_of_the_cpp_enum_and_table_is_here() {
    let keys = cpp_keys();
    let table: HashSet<&str> = keys.iter().map(String::as_str).collect();
    assert_eq!(table.len(), keys.len(), "a key set twice in Strings.cpp");
    let ours: Vec<&str> = Str::ALL.iter().map(|k| k.name()).collect();
    let ours_set: HashSet<&str> = ours.iter().copied().collect();
    assert_eq!(ours.len(), COUNT);
    assert_eq!(
        ours_set, table,
        "the Rust keys and the keys Strings.cpp sets differ"
    );
    // the order of Strings.h
    assert_eq!(ours[0], "MonLabel");
    assert_eq!(ours[ours.len() - 1], "VirtualMicFailed");
    assert_eq!(Str::ALL[Str::SignOut as usize], Str::SignOut);
    // and every one has both texts (the C++ answers "?" otherwise)
    for k in Str::ALL {
        for l in [Language::Thai, Language::English] {
            let t = tr_in(k, l);
            assert!(
                !t.is_empty() && t != "?",
                "{} has no {:?} text",
                k.name(),
                l
            );
        }
    }
}

#[test]
fn texts_come_through_byte_identical() {
    use Language::{English, Thai};
    assert_eq!(tr_in(Str::MonLabel, Thai).as_bytes(), "คุณได้ยิน".as_bytes());
    assert_eq!(tr_in(Str::MonLabel, English), "You hear");
    // escaped quotes in the C++ literal
    assert_eq!(
        tr_in(Str::BypassWarning, Thai),
        "ปลั๊กอินถูก bypass อยู่ คุณจะได้ยินแทร็กนี้แม้ปิด \"คุณได้ยิน\""
    );
    assert_eq!(
        tr_in(Str::BypassWarning, English),
        "The plug-in is bypassed, so you hear this track even with \u{201c}You hear\u{201d} off."
    );
    // a literal split over two lines in the source
    assert_eq!(
        tr_in(Str::SumViewersOnly, English),
        "You won\u{2019}t hear this track in your headphones, but your viewers will."
    );
    // escaped backslashes
    assert_eq!(
        tr_in(Str::PrintHint, English),
        "WAV takes go to Documents\\HEARASIDE\\Recordings. Drag one onto any track; in Cubase / Reaper \u{201c}Move to origin\u{201d} puts it where it was recorded."
    );
    assert!(
        tr_in(Str::PrintHint, Thai).starts_with("ไฟล์ WAV เก็บที่ Documents\\HEARASIDE\\Recordings ")
    );
    // symbols
    assert_eq!(
        tr_in(Str::WaitingAllow, English),
        "Waiting for you to allow it\u{2026} \u{b7} expires in %s"
    );
    assert_eq!(tr_in(Str::SignedOutToast, Thai), "ออกจากระบบแล้ว");
    assert_eq!(
        tr_in(Str::UsernameRules, English),
        "a\u{2013}z, 0\u{2013}9 and _, 3 to 20 characters"
    );
}

/// The only test that touches the process-wide language.
#[test]
fn language_switch_and_placeholders() {
    set_current_language(Language::Thai);
    assert_eq!(current_language(), Language::Thai);
    assert_eq!(tr(Str::SignedOutToast), "ออกจากระบบแล้ว");
    set_current_language(Language::English);
    assert_eq!(current_language(), Language::English);
    assert_eq!(tr(Str::SignedOutToast), "Signed out");
    assert_eq!(trf(Str::SignedInToast, &["mint"]), "Signed in as mint");
    assert_eq!(
        trf(Str::PairedWith, &["Mint", "12"]),
        "Paired with \u{201c}Mint\u{201d} \u{b7} your plug-ins add 12 ms"
    );
    assert_eq!(
        trf(Str::SignedInToast, &[]),
        "Signed in as %s",
        "no argument left: the placeholder stays"
    );
    assert_eq!(trf(Str::AboutCounter, &["40"]), "40 / 160");
}

#[test]
fn format_db_like_valuetext() {
    assert_eq!(format_db(0.0, true), "0.0 dB");
    assert_eq!(format_db(-3.0, true), "\u{2212}3.0 dB");
    assert_eq!(format_db(2.0, true), "+2.0 dB");
    assert_eq!(format_db(-30.0, true), "\u{2212}\u{221e} dB");
    assert_eq!(format_db(-3.04, false), "\u{2212}3.0");
    assert_eq!(
        format_db(0.03, true),
        "0.0 dB",
        "under half a step is zero, no sign"
    );
    assert_eq!(format_db(-0.04, true), "0.0 dB");
}
