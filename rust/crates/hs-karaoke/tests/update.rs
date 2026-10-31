//! `UpdateChecker`: the port of `karaoke: UpdateChecker semver comparison` from `tests/unit/test_karaoke.cpp`, and the
//! reading of `release.json` (with the site's real file).
use hs_karaoke::update::{CURRENT_VERSION, compare_versions, release_info};
use serde_json::json;

#[test]
fn karaoke_update_checker_semver_comparison() {
    assert_eq!(compare_versions("1.0.5", "1.0.5"), 0);
    assert_eq!(compare_versions("1.0.0", "1.0"), 0);
    assert_eq!(compare_versions("2.0.0.0", "2.0"), 0);

    assert_eq!(compare_versions("1.0.6", "1.0.5"), 1);
    assert_eq!(compare_versions("1.0.10", "1.0.9"), 1); // numbers, not text
    assert_eq!(compare_versions("1.1.0", "1.0.5"), 1);
    assert_eq!(compare_versions("2.0.0", "1.99.99"), 1);
    assert_eq!(compare_versions("1.0.5.1", "1.0.5"), 1);
    assert_eq!(compare_versions("1.0.5-beta", "1.0.4"), 1);

    assert_eq!(compare_versions("1.0.4", "1.0.5"), -1);
    assert_eq!(compare_versions("0.9.9", "1.0.0"), -1);
    assert_eq!(compare_versions("1.0.4.9", "1.0.5"), -1);

    assert_eq!(compare_versions("", "1.0.0"), -1);
    assert_eq!(compare_versions("1.0.0", ""), 1);
}

#[test]
fn version_comes_from_cmake() {
    let cmake = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../CMakeLists.txt"
    ))
    .unwrap();
    assert!(cmake.contains(&format!("project(Hearaside VERSION {CURRENT_VERSION} ")));
}

#[test]
fn release_json_fields() {
    let j = json!({
        "version": " 9.1.0 ",
        "date": "1 Nov 2026",
        "installer": "https://example.com/HEARASIDE-Setup-9.1.0.exe",
        "notes": [{ "version": "9.1.0", "th": "ไทย", "en": "English" }, { "th": "old" }]
    });
    let info = release_info(Some(&j), "1.0.10");
    assert_eq!(info.current_version, "1.0.10");
    assert_eq!(info.latest_version, "9.1.0");
    assert_eq!(info.release_date, "1 Nov 2026");
    assert_eq!(
        info.download_url,
        "https://example.com/HEARASIDE-Setup-9.1.0.exe"
    );
    assert_eq!(
        (info.notes_th.as_str(), info.notes_en.as_str()),
        ("ไทย", "English")
    );
    assert!(info.has_update);

    // relative installer path: the C++ keeps the download page
    let info = release_info(
        Some(&json!({ "version": "1.0.10", "installer": "/downloads/x.exe" })),
        "1.0.10",
    );
    assert_eq!(info.download_url, "https://hearaside.netlify.app/download");
    assert!(!info.has_update);

    let none = release_info(None, "1.0.10");
    assert!(!none.has_update && none.latest_version.is_empty());
    assert!(!release_info(Some(&json!([1, 2])), "1.0.10").has_update);
}

#[test]
fn the_sites_own_release_json() {
    for f in [
        "web/site/public/release.json",
        "web/site/public/content/release.json",
    ] {
        let path = format!("{}/../../../{f}", env!("CARGO_MANIFEST_DIR"));
        let j: serde_json::Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        let info = release_info(Some(&j), "0.0.1");
        assert!(info.has_update, "{f}");
        assert!(
            !info.notes_th.is_empty() && !info.notes_en.is_empty(),
            "{f}"
        );
    }
}
