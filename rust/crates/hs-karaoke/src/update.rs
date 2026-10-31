//! The "new version available" check: reads the site's `release.json` and compares versions.
//! Port of `apps/karaoke/UpdateChecker.{h,cpp}`. The C++ thread posted its result to the message thread; here the
//! callback runs on the worker thread and the caller hands it to the UI (the `hs-app` convention).
use serde_json::Value;
use std::cmp::Ordering;
use std::thread::JoinHandle;
use std::time::Duration;

/// This build's version (`project(Hearaside VERSION ...)`, as the C++ `HEARASIDE_VERSION`).
pub const CURRENT_VERSION: &str = env!("HEARASIDE_VERSION");

const URLS: [&str; 2] = [
    "https://hearaside.netlify.app/content/release.json",
    "https://hearaside.netlify.app/release.json",
];
const DOWNLOAD_PAGE: &str = "https://hearaside.netlify.app/download";

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ReleaseInfo {
    pub current_version: String,
    pub latest_version: String,
    pub download_url: String,
    pub release_date: String,
    pub notes_th: String,
    pub notes_en: String,
    pub has_update: bool,
}

/// 1 if `v1` is newer than `v2`, -1 if older, 0 if equal. Dot-separated numbers compared as numbers, missing parts are
/// 0, each part counts only its leading digits ("5-beta" = 5).
pub fn compare_versions(v1: &str, v2: &str) -> i32 {
    fn parts(v: &str) -> Vec<u64> {
        v.trim()
            .split('.')
            .map(|t| {
                let digits = t.bytes().take_while(u8::is_ascii_digit).count();
                t[..digits].parse().unwrap_or(0) // no digits (or over 20 of them) = 0
            })
            .collect()
    }
    let (p1, p2) = (parts(v1), parts(v2));
    for i in 0..p1.len().max(p2.len()) {
        let (a, b) = (
            p1.get(i).copied().unwrap_or(0),
            p2.get(i).copied().unwrap_or(0),
        );
        match a.cmp(&b) {
            Ordering::Greater => return 1,
            Ordering::Less => return -1,
            Ordering::Equal => {}
        }
    }
    0
}

/// What a fetched `release.json` (or `None`: nothing fetched) means for version `current`.
pub fn release_info(json: Option<&Value>, current: &str) -> ReleaseInfo {
    let mut info = ReleaseInfo {
        current_version: current.into(),
        download_url: DOWNLOAD_PAGE.into(),
        ..Default::default()
    };
    let Some(j) = json.filter(|j| j.is_object()) else {
        return info;
    };
    let text = |v: &Value, key: &str| v.get(key).map(json_text).unwrap_or_default();
    info.latest_version = text(j, "version").trim().to_owned();
    info.release_date = text(j, "date").trim().to_owned();
    let installer = text(j, "installer");
    if installer.starts_with("http") {
        info.download_url = installer;
    }
    if let Some(note) = j
        .get("notes")
        .and_then(|n| n.get(0))
        .filter(|n| n.is_object())
    {
        info.notes_th = text(note, "th");
        info.notes_en = text(note, "en");
    }
    info.has_update =
        !info.latest_version.is_empty() && compare_versions(&info.latest_version, current) > 0;
    info
}

/// `var::toString` of a JSON value: strings as they are, numbers and the rest as JSON text.
fn json_text(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Null => String::new(),
        other => other.to_string(),
    }
}

/// Fetches in the background (each URL tried in turn, 4 s to connect) and calls `on_result` on that thread.
pub fn check_for_updates(on_result: impl FnOnce(ReleaseInfo) + Send + 'static) -> JoinHandle<()> {
    std::thread::Builder::new()
        .name("HearasideUpdateChecker".into())
        .spawn(move || {
            let json = URLS
                .iter()
                .find_map(|u| hs_app::get_json(u, Duration::from_secs(4), Duration::from_secs(15)));
            on_result(release_info(json.as_ref(), CURRENT_VERSION));
        })
        .expect("spawn the update check thread")
}
