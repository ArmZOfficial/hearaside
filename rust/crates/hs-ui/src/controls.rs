//! What the inputs of `controls.slint` decide in Rust (plugins/common/ui/Controls.cpp): the text of an EditableValue and
//! what a typed text means, and the rules for renaming. The parsing itself is `hs_app::valuetext` (ValueText.cpp).
use hs_app::valuetext as vt;

/// `EditableValue::Kind`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ValueKind {
    Db,
    Ms,
    Pan,
    Percent,
}

/// An EditableValue's range: `lo..=hi` in the unit of the kind (dB, ms, -100..100, a 0..1 fraction), and the dB that reads as -inf.
#[derive(Clone, Copy, Debug)]
pub struct ValueField {
    pub kind: ValueKind,
    pub lo: f32,
    pub hi: f32,
    pub floor_db: f32,
}

impl ValueField {
    pub fn new(kind: ValueKind, lo: f32, hi: f32) -> Self {
        Self {
            kind,
            lo,
            hi,
            floor_db: vt::FLOOR_DB,
        }
    }

    /// What the field shows: `-3.0 dB`, `40 ms`, `L40`, `62%`.
    pub fn text(&self, value: f32) -> String {
        match self.kind {
            ValueKind::Db => vt::format_db(value, self.floor_db, true),
            ValueKind::Ms => vt::format_ms(value),
            ValueKind::Pan => vt::format_pan(value),
            ValueKind::Percent => vt::format_percent(value),
        }
    }

    /// What the editor starts with, selected: the number without its unit.
    pub fn edit_text(&self, value: f32) -> String {
        match self.kind {
            ValueKind::Db => vt::edit_db(value, self.floor_db),
            ValueKind::Ms => vt::format_ms(value).trim_end_matches(" ms").to_string(),
            ValueKind::Pan => vt::format_pan(value),
            ValueKind::Percent => vt::format_percent(value).trim_end_matches('%').to_string(),
        }
    }

    /// The value a typed text means, clamped to the range; `None` = nonsense, the old value stays.
    pub fn commit(&self, typed: &str) -> Option<f32> {
        match self.kind {
            ValueKind::Db => vt::parse_db(typed, self.lo, self.hi),
            ValueKind::Ms => vt::parse_ms(typed, self.lo, self.hi),
            ValueKind::Pan => vt::parse_pan(typed).map(|v| v.clamp(self.lo, self.hi)),
            ValueKind::Percent => vt::parse_percent(typed, self.lo, self.hi),
        }
    }
}

/// Names are 63 bytes + NUL in shared memory.
pub const NAME_MAX_BYTES: usize = 63;

/// The text of a name field after an edit: no line breaks or tabs (`Utf8Limit`), cut at a character to 63 UTF-8 bytes.
/// Call it from the field's `edited` callback and set the text back when it differs.
pub fn limit_name(typed: &str) -> String {
    let clean: String = typed
        .chars()
        .filter(|c| !matches!(c, '\r' | '\n' | '\t'))
        .collect();
    vt::truncate_utf8(&clean, NAME_MAX_BYTES).to_string()
}

/// `InlineName::finish`: what to rename the track to after `typed` was entered. `None` = nothing changes; `Some("")` = back
/// to the DAW's name (`daw`).
pub fn rename_outcome(typed: &str, current: &str, daw: &str) -> Option<String> {
    let typed = vt::truncate_utf8(typed.trim(), NAME_MAX_BYTES);
    if typed == current || (typed.is_empty() && current == daw) {
        return None;
    }
    Some(typed.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn db_field_shows_and_edits_like_the_cpp() {
        let f = ValueField::new(ValueKind::Db, -30.0, 6.0);
        assert_eq!(f.text(-3.0), "\u{2212}3.0 dB");
        assert_eq!(f.text(0.0), "0.0 dB");
        assert_eq!(f.text(-30.0), "\u{2212}\u{221e} dB");
        assert_eq!(f.edit_text(-3.0), "-3.0");
        assert_eq!(f.commit("-2,5 dB"), Some(-2.5));
        assert_eq!(f.commit("99"), Some(6.0));
        assert_eq!(f.commit("-inf"), Some(-30.0));
        assert_eq!(f.commit("loud"), None);
    }

    #[test]
    fn ms_pan_and_percent_fields() {
        let ms = ValueField::new(ValueKind::Ms, 0.0, 500.0);
        assert_eq!(
            (ms.text(40.4).as_str(), ms.edit_text(40.4).as_str()),
            ("40 ms", "40")
        );
        assert_eq!(
            (ms.commit("41 ms"), ms.commit("9000"), ms.commit("x")),
            (Some(41.0), Some(500.0), None)
        );
        let pan = ValueField::new(ValueKind::Pan, -100.0, 100.0);
        assert_eq!(
            (
                pan.text(-40.0).as_str(),
                pan.text(0.0).as_str(),
                pan.edit_text(25.0).as_str()
            ),
            ("L40", "C", "R25")
        );
        assert_eq!(
            (pan.commit("r25"), pan.commit("c"), pan.commit("L250")),
            (Some(25.0), Some(0.0), Some(-100.0))
        );
        let pct = ValueField::new(ValueKind::Percent, 0.0, 1.0);
        assert_eq!(
            (pct.text(0.62).as_str(), pct.edit_text(0.62).as_str()),
            ("62%", "62")
        );
        assert_eq!(
            (pct.commit("62%"), pct.commit("250")),
            (Some(0.62), Some(1.0))
        );
    }

    #[test]
    fn a_pan_limited_to_one_side_clamps_to_the_range() {
        let right_only = ValueField::new(ValueKind::Pan, 0.0, 100.0);
        assert_eq!(right_only.commit("L40"), Some(0.0));
    }

    #[test]
    fn names_are_cut_at_63_bytes_on_a_character_and_lose_line_breaks() {
        assert_eq!(limit_name("a\r\nb\tc"), "abc");
        let thai = "ไมค์".repeat(10);
        let cut = limit_name(&thai);
        assert!(
            cut.len() <= NAME_MAX_BYTES && thai.starts_with(&cut) && cut.len() > 54,
            "{} bytes",
            cut.len()
        );
        assert_eq!(limit_name("Vocal"), "Vocal");
    }

    #[test]
    fn rename_outcome_follows_inline_name_finish() {
        assert_eq!(
            rename_outcome("  Lead  ", "Vocal", "Audio 7"),
            Some("Lead".into())
        );
        assert_eq!(rename_outcome("Vocal", "Vocal", "Audio 7"), None);
        assert_eq!(rename_outcome("", "Vocal", "Audio 7"), Some(String::new()));
        assert_eq!(rename_outcome("  ", "Audio 7", "Audio 7"), None);
        let long = "x".repeat(80);
        assert_eq!(rename_outcome(&long, "Vocal", "Audio 7").unwrap().len(), 63);
    }
}
