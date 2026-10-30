//! UI text in English (default) and Thai: `Strings.h` / `Strings.cpp` of the C++ plug-ins, which stay the single source
//! of truth. `build.rs` reads their key list and `s(Str::Key, "th", "en")` table, so a key or a text cannot drift.
use std::sync::atomic::{AtomicU8, Ordering};

include!(concat!(env!("OUT_DIR"), "/strings_table.rs"));

/// The UI language (`hearaside::Language`). English is the default for new users.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Language {
    Thai,
    English,
}

static CURRENT: AtomicU8 = AtomicU8::new(1); // 0 = Thai, 1 = English

/// Like `setCurrentLanguage`: one language for the whole process.
pub fn set_current_language(l: Language) {
    CURRENT.store(u8::from(l == Language::English), Ordering::Relaxed);
}

pub fn current_language() -> Language {
    if CURRENT.load(Ordering::Relaxed) == 1 {
        Language::English
    } else {
        Language::Thai
    }
}

/// The text of `key` in `lang`; `"?"` when `Strings.cpp` has none (the C++ `tr()` does the same).
pub fn tr_in(key: Str, lang: Language) -> &'static str {
    match TABLE[key as usize] {
        Some((th, en)) => match lang {
            Language::Thai => th,
            Language::English => en,
        },
        None => "?",
    }
}

/// The text of `key` in the current language (`tr`).
pub fn tr(key: Str) -> &'static str {
    tr_in(key, current_language())
}

/// `tr()` with each `%s` / `%d` filled in order from `args` (`trf`). A placeholder with no argument left stays as it is.
pub fn trf(key: Str, args: &[&str]) -> String {
    let pattern = tr(key);
    let mut out = String::with_capacity(pattern.len() + 16);
    let mut next = args.iter();
    let mut chars = pattern.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '%'
            && matches!(chars.peek(), Some('s' | 'd'))
            && let Some(a) = next.next()
        {
            out.push_str(a);
            chars.next();
        } else {
            out.push(c);
        }
    }
    out
}

/// `"−3.0 dB"` / `"−∞ dB"` / `"+2.5 dB"` with a real minus sign (`valuetext::format_db` with the −30 dB floor).
pub fn format_db(db: f32, with_unit: bool) -> String {
    crate::valuetext::format_db(db, crate::valuetext::FLOOR_DB, with_unit)
}
