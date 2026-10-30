//! Text <-> value for every number the user can see or type (volume, delay, pan, names): `plugins/common/ValueText.{h,cpp}`.
//! Pure functions, no UI. The C++ is checked by `ui-snapshot --test-ui`; `hs-testkit --test-ui` runs the same checks here.
//!
//! Whole numbers are rounded half up (`40.5` is 41, `-40.5` is -40) in `f64`, so `0.49999997` stays 0. The C++ `juce::roundToInt`
//! rounds half to even (`40.5` is 40): the Rust way is deliberate, because a user who types `40,5` expects 41.

/// A level slider's bottom, shown as -inf.
pub const FLOOR_DB: f32 = -30.0;

const MINUS: char = '\u{2212}';
const INFINITY_SIGN: char = '\u{221e}';

fn round_to_int(x: f32) -> i32 {
    (f64::from(x) + 0.5).floor() as i32
}

/// Minus signs and decimal commas as plain ASCII, no spaces.
fn normalise(text: &str) -> String {
    text.trim()
        .chars()
        .filter_map(|c| match c {
            MINUS | '\u{2013}' => Some('-'),
            ',' => Some('.'),
            ' ' | '\t' => None,
            c => Some(c),
        })
        .collect()
}

/// Optional sign, digits, optional fraction, and nothing else (the unit is taken off before). `None` = not a number.
/// C++ bug: whatever followed the digits was ignored, so `4o` (a typo) was 4 and `3 apples` was 3.
fn number(t: &str) -> Option<f32> {
    let b = t.as_bytes();
    let mut i = 0;
    if matches!(b.first(), Some(b'-' | b'+')) {
        i = 1;
    }
    let digits_from = i;
    while i < b.len() && (b[i].is_ascii_digit() || b[i] == b'.') {
        i += 1;
    }
    let body = &t[digits_from..i];
    if i != b.len()
        || !body.bytes().any(|c| c.is_ascii_digit())
        || body.bytes().filter(|&c| c == b'.').count() > 1
    {
        return None;
    }
    t[..i].trim_start_matches('+').parse().ok()
}

/// `0.0 dB`, `-3.0 dB` with U+2212, `+2.0 dB`, `-inf dB` at or below `floor_db`.
pub fn format_db(db: f32, floor_db: f32, unit: bool) -> String {
    let mut s = String::new();
    if db <= floor_db + 1.0e-4 {
        s.push(MINUS);
        s.push(INFINITY_SIGN);
    } else {
        let r = (db * 10.0).round() / 10.0;
        if r > 0.04 {
            s.push('+');
        } else if r < -0.04 {
            s.push(MINUS);
        }
        s.push_str(&format!("{:.1}", r.abs()));
    }
    if unit {
        s.push_str(" dB");
    }
    s
}

/// What the field holds when it opens for typing: `-inf`, `+2.0`, `-3.0`, `0.0`.
pub fn edit_db(db: f32, floor_db: f32) -> String {
    if db <= floor_db + 1.0e-4 {
        return "-inf".into();
    }
    let r = (db * 10.0).round() / 10.0;
    if r > 0.04 {
        format!("+{r:.1}")
    } else if r < -0.04 {
        format!("{r:.1}")
    } else {
        "0.0".into()
    }
}

/// `-3`, `-3` (U+2212), `-3 dB`, `+2`, `2,5`, `-inf`, the infinity sign: a value inside `lo..=hi`, `-inf` = `lo`.
/// `None` when nothing is usable (the field keeps the old value).
pub fn parse_db(text: &str, lo: f32, hi: f32) -> Option<f32> {
    let mut t = normalise(text);
    if t.len() >= 2
        && t.is_char_boundary(t.len() - 2)
        && t[t.len() - 2..].eq_ignore_ascii_case("db")
    {
        t.truncate(t.len() - 2);
    }
    let inf = INFINITY_SIGN.to_string();
    if ["-inf", "inf", "-infinity"]
        .iter()
        .any(|w| t.eq_ignore_ascii_case(w))
        || t == inf
        || t == format!("-{inf}")
    {
        return Some(lo);
    }
    let v = number(&t)?;
    Some(((v * 10.0).round() / 10.0).clamp(lo, hi))
}

pub fn format_ms(ms: f32) -> String {
    format!("{} ms", round_to_int(ms))
}

/// `40`, `40 ms`, `40,5`.
pub fn parse_ms(text: &str, lo: f32, hi: f32) -> Option<f32> {
    let mut t = normalise(text);
    if t.len() >= 2
        && t.is_char_boundary(t.len() - 2)
        && t[t.len() - 2..].eq_ignore_ascii_case("ms")
    {
        t.truncate(t.len() - 2);
    }
    let v = number(&t)?;
    Some((round_to_int(v) as f32).clamp(lo, hi))
}

/// Pan -100 (left) .. 100 (right): `C`, `L40`, `R25`.
pub fn format_pan(pan: f32) -> String {
    let p = round_to_int(pan.clamp(-100.0, 100.0));
    match p {
        0 => "C".into(),
        p if p < 0 => format!("L{}", -p),
        p => format!("R{p}"),
    }
}

/// `L40`, `l 40`, `C`, `c`, `R25`, `-40`, `40`, `center` -> -100..100; `None` when unreadable.
pub fn parse_pan(text: &str) -> Option<f32> {
    let t = normalise(text).to_uppercase();
    if matches!(t.as_str(), "C" | "CENTER" | "CENTRE" | "0" | "L0" | "R0") {
        return Some(0.0);
    }
    if let Some(rest) = t.strip_prefix('L').or_else(|| t.strip_prefix('R')) {
        let v = number(rest)?;
        if v < 0.0 {
            return None;
        }
        let sign = if t.starts_with('L') { -1.0 } else { 1.0 };
        return Some((round_to_int(v) as f32 * sign).clamp(-100.0, 100.0));
    }
    Some((round_to_int(number(&t)?) as f32).clamp(-100.0, 100.0))
}

pub fn format_percent(fraction: f32) -> String {
    format!("{}%", round_to_int(fraction * 100.0))
}

/// `62`, `62%` -> 0.62.
pub fn parse_percent(text: &str, lo: f32, hi: f32) -> Option<f32> {
    let t = normalise(text);
    let v = number(t.strip_suffix('%').unwrap_or(&t))?;
    Some((v / 100.0).clamp(lo, hi))
}

/// The longest start of `s` whose UTF-8 form fits in `max_bytes`, cut at a character boundary (names are 63 bytes + NUL
/// in shared memory).
pub fn truncate_utf8(s: &str, max_bytes: usize) -> &str {
    let mut end = 0;
    for (i, c) in s.char_indices() {
        if i + c.len_utf8() > max_bytes {
            break;
        }
        end = i + c.len_utf8();
    }
    &s[..end]
}

/// The letter shown in an avatar: `Mint` -> `M`. A Thai name's leading vowel (U+0E40..U+0E44) is not its initial.
pub fn initial_of(name: &str) -> String {
    let t = name.trim();
    match t.chars().find(|c| !('\u{0e40}'..='\u{0e44}').contains(c)) {
        Some(c) => {
            let mut up = c.to_uppercase();
            // juce::toUpperCase maps one character to one: "ß" stays "ß"
            match (up.next(), up.next()) {
                (Some(u), None) => u.to_string(),
                _ => c.to_string(),
            }
        }
        None => t.chars().next().map_or_else(|| "?".into(), String::from),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn near(v: Option<f32>, x: f32) -> bool {
        v.is_some_and(|v| (v - x).abs() < 1.0e-4)
    }

    #[test]
    fn whole_numbers_round_half_up_without_the_f32_error() {
        assert_eq!(
            round_to_int(0.499_999_97),
            0,
            "adding 0.5 in f32 would give 1.0"
        );
        assert_eq!(
            (round_to_int(40.5), round_to_int(-40.5), round_to_int(-0.4)),
            (41, -40, 0)
        );
        assert_eq!(round_to_int(f32::NAN), 0);
        assert_eq!(format_ms(0.499_999_97), "0 ms");
        assert!(near(parse_ms("40,5", 0.0, 500.0), 41.0));
    }

    #[test]
    fn db_typed_values() {
        assert!(near(parse_db("-3", -30.0, 6.0), -3.0));
        assert!(near(parse_db("\u{2212}3", -30.0, 6.0), -3.0));
        assert!(near(parse_db("-3 dB", -30.0, 6.0), -3.0));
        assert!(near(parse_db("+2", -30.0, 6.0), 2.0));
        assert!(near(parse_db("2,5", -30.0, 6.0), 2.5));
        assert!(near(parse_db("-inf", -30.0, 6.0), -30.0));
        assert!(near(parse_db("\u{221e}", -30.0, 6.0), -30.0));
        assert!(near(parse_db("+20", -30.0, 6.0), 6.0) && near(parse_db("-99", -30.0, 6.0), -30.0));
        assert!(
            parse_db("loud", -30.0, 6.0).is_none()
                && parse_db("", -30.0, 6.0).is_none()
                && parse_db("1.2.3", -30.0, 6.0).is_none()
        );
    }

    #[test]
    fn db_shown() {
        assert_eq!(format_db(0.0, FLOOR_DB, true), "0.0 dB");
        assert_eq!(format_db(-3.0, FLOOR_DB, true), "\u{2212}3.0 dB");
        assert_eq!(format_db(2.0, FLOOR_DB, true), "+2.0 dB");
        assert_eq!(format_db(-30.0, FLOOR_DB, true), "\u{2212}\u{221e} dB");
        assert_eq!(format_db(-3.0, FLOOR_DB, false), "\u{2212}3.0");
        assert!(edit_db(-30.0, FLOOR_DB) == "-inf" && edit_db(2.0, FLOOR_DB) == "+2.0");
        assert_eq!(edit_db(-3.0, FLOOR_DB), "-3.0");
        assert_eq!(edit_db(0.02, FLOOR_DB), "0.0");
    }

    #[test]
    fn junk_after_the_digits_is_not_a_number() {
        // C++ bug: only the start was read, so a typo became a value
        assert!(parse_db("4o", -30.0, 6.0).is_none() && parse_db("3 apples", -30.0, 6.0).is_none());
        assert!(parse_db("-3abc dB", -30.0, 6.0).is_none());
        assert!(
            parse_ms("12abc", 0.0, 500.0).is_none() && parse_ms("40 msec", 0.0, 500.0).is_none()
        );
        assert!(
            parse_pan("L4o").is_none()
                && parse_pan("R25x").is_none()
                && parse_pan("40 left").is_none()
        );
        assert!(parse_percent("62 percent", 0.0, 1.0).is_none());
        // what stays valid
        assert!(
            near(parse_db("6.0dB", -30.0, 6.0), 6.0) && near(parse_db(" 2.5 DB ", -30.0, 6.0), 2.5)
        );
        assert!(near(parse_ms("40MS", 0.0, 500.0), 40.0) && near(parse_pan("  r 25 "), 25.0));
    }

    #[test]
    fn non_ascii_input_never_panics() {
        // the two-character unit check must not cut a character in half
        assert!(parse_db("\u{0e1a}\u{0e32}", -30.0, 6.0).is_none());
        assert!(parse_ms("\u{0e1a}", 0.0, 500.0).is_none());
        assert!(parse_pan("\u{0e1a}").is_none());
        assert!(parse_percent("\u{0e1a}%", 0.0, 1.0).is_none());
    }

    #[test]
    fn ms_pan_percent() {
        assert!(near(parse_ms("40 ms", 0.0, 500.0), 40.0) && format_ms(40.0) == "40 ms");
        assert!(near(parse_ms("900", 0.0, 500.0), 500.0));
        assert!(near(parse_ms("40,5", 0.0, 500.0), 41.0));
        assert!(
            near(parse_pan("L40"), -40.0)
                && near(parse_pan("C"), 0.0)
                && near(parse_pan("R25"), 25.0)
        );
        assert!(near(parse_pan("l 40"), -40.0) && near(parse_pan("-40"), -40.0));
        assert!(near(parse_pan("R250"), 100.0) && parse_pan("left").is_none());
        assert!(near(parse_pan("center"), 0.0) && near(parse_pan("40"), 40.0));
        assert!(format_pan(-40.0) == "L40" && format_pan(0.0) == "C" && format_pan(25.0) == "R25");
        assert_eq!(format_percent(0.62), "62%");
        assert!(
            near(parse_percent("62%", 0.0, 1.0), 0.62) && near(parse_percent("62", 0.0, 1.0), 0.62)
        );
    }

    #[test]
    fn names_are_cut_at_a_character() {
        let thai = "เสียงร้องประสานชุดที่สองแบบยาวมากจริงๆ";
        let cut = truncate_utf8(thai, 63);
        assert!(cut.len() <= 63 && thai.starts_with(cut) && cut.chars().count() == 21);
        let emoji = "Mint \u{1f3a4}\u{1f3a4}\u{1f3a4}";
        assert_eq!(truncate_utf8(emoji, 8), "Mint ");
        assert_eq!(truncate_utf8(emoji, 10), "Mint \u{1f3a4}");
        assert_eq!(truncate_utf8("Vocal", 63), "Vocal");
        assert_eq!(truncate_utf8("abc", 0), "");
    }

    #[test]
    fn initials() {
        assert_eq!(initial_of("เสียง"), "ส");
        assert_eq!(initial_of("Mint"), "M");
        assert_eq!(initial_of("  fah"), "F");
        assert_eq!(initial_of(""), "?");
        assert_eq!(initial_of("\u{00df}x"), "\u{00df}");
        // only leading vowels: the first character stays
        assert_eq!(initial_of("เ"), "เ");
    }
}
