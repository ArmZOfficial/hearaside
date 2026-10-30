//! The XML key/value file `juce::PropertiesFile` (`storeAsXML`) writes: `<PROPERTIES>` holding `<VALUE name=".." val=".."/>`,
//! a value that is XML itself stored as a child element. Read and written byte-compatibly with JUCE 8 (the C++ Settings,
//! and `audioDeviceState.*` which is XML), so a C++ plug-in and a Rust plug-in can share one file during the hybrid phase.
//!
//! What differs from JUCE on purpose (see the bug list in the R3.5 handoff):
//! * a save re-reads the file and writes back only the keys this process changed, so two DAWs do not undo each other;
//! * only a value that is exactly one XML element is stored as a child (JUCE also did it for `"<b>x</b> trailing"`,
//!   which changed the text);
//! * a file that cannot be parsed is kept as `<file>.bad` before it is replaced, not silently overwritten.
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// How long a change waits for more changes before `tick` writes it (`PropertiesFile::Options::millisecondsBeforeSaving`).
pub const SAVE_DELAY: Duration = Duration::from_millis(500);

type Pairs = Vec<(String, String)>;
type Reader = fn(&Path) -> std::io::Result<Vec<u8>>;

/// What is on disk right now.
enum Disk {
    Missing,
    /// there, but it could not be read (sharing violation while another process replaces it, antivirus, permissions)
    Unreadable,
    /// read, but not a properties file
    Damaged,
    Pairs(Pairs),
}

/// A properties file in memory (insertion order kept, like `StringPairArray`).
///
/// Changes stay in memory until the owner calls `tick()` from its timer (writes once a change is `SAVE_DELAY` old), `flush()`
/// (writes now) or drops the file (writes now): a burst of changes, such as a window being dragged, is one write.
pub struct PropertiesFile {
    path: PathBuf,
    values: Pairs,
    /// what the file held when it was last read or written: the baseline for "what did this process change"
    base: Pairs,
    /// when the oldest change that is not on disk yet was made
    dirty_since: Option<Instant>,
    writes: u32,
    reader: Reader,
}

fn pairs_set(p: &mut Pairs, k: &str, v: &str) {
    match p.iter_mut().find(|(pk, _)| pk == k) {
        Some(e) => e.1 = v.to_owned(),
        None => p.push((k.to_owned(), v.to_owned())),
    }
}

fn read_disk(path: &Path, reader: Reader) -> Disk {
    match reader(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Disk::Missing,
        Err(_) => Disk::Unreadable,
        Ok(bytes) if bytes.is_empty() => Disk::Pairs(Vec::new()),
        Ok(bytes) => parse(&bytes).map_or(Disk::Damaged, Disk::Pairs),
    }
}

impl PropertiesFile {
    /// Opens `path`; a missing file is an empty set. A file that cannot be read or is not a properties file starts empty too;
    /// it is never overwritten before it can be read (see `save`).
    pub fn open(path: impl Into<PathBuf>) -> Self {
        Self::open_with(path.into(), |p| std::fs::read(p))
    }

    fn open_with(path: PathBuf, reader: Reader) -> Self {
        let values = match read_disk(&path, reader) {
            Disk::Pairs(p) => p,
            _ => Vec::new(),
        };
        Self {
            path,
            base: values.clone(),
            values,
            dirty_since: None,
            writes: 0,
            reader,
        }
    }

    /// Tests: how the file is read (a failing reader stands for a sharing violation).
    #[doc(hidden)]
    pub fn set_reader(&mut self, reader: Reader) {
        self.reader = reader;
    }

    /// Tests: how many times the file was written.
    #[doc(hidden)]
    pub fn writes(&self) -> u32 {
        self.writes
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn contains_key(&self, key: &str) -> bool {
        self.values.iter().any(|(k, _)| k == key)
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }

    /// `getValue(key, default)`
    pub fn get_or(&self, key: &str, default: &str) -> String {
        self.get(key).unwrap_or(default).to_owned()
    }

    /// `getIntValue`: the lenient parse of the JUCE String, `default` when the key is missing.
    pub fn get_int(&self, key: &str, default: i32) -> i32 {
        self.get(key).map_or(default, juce_int)
    }

    pub fn get_f64(&self, key: &str, default: f64) -> f64 {
        self.get(key).map_or(default, juce_double)
    }

    /// `getBoolValue`: any number but 0 is true, "true" is not.
    pub fn get_bool(&self, key: &str, default: bool) -> bool {
        self.get(key).map_or(default, |v| juce_int(v) != 0)
    }

    /// Sets `key` in memory (an empty key is ignored, like JUCE). Returns whether it changed. Written later by
    /// `tick` / `flush` / drop.
    pub fn set(&mut self, key: &str, value: &str) -> bool {
        if key.is_empty() || self.get(key) == Some(value) {
            return false;
        }
        pairs_set(&mut self.values, key, value);
        self.dirty_since.get_or_insert_with(Instant::now);
        true
    }

    pub fn set_bool(&mut self, key: &str, v: bool) -> bool {
        self.set(key, if v { "1" } else { "0" })
    }

    pub fn set_int(&mut self, key: &str, v: i32) -> bool {
        self.set(key, &v.to_string())
    }

    /// A double as `juce::String(double)` writes it (`%g`, 6 significant digits).
    pub fn set_f64(&mut self, key: &str, v: f64) -> bool {
        self.set(key, &juce_double_string(v))
    }

    /// `true` while a change is not on disk yet.
    pub fn needs_saving(&self) -> bool {
        self.dirty_since.is_some()
    }

    /// Call from a timer (a few times a second): writes when the oldest unsaved change is `SAVE_DELAY` old.
    pub fn tick(&mut self) -> bool {
        match self.dirty_since {
            Some(t) if t.elapsed() >= SAVE_DELAY => self.save(),
            _ => false,
        }
    }

    /// Writes now when a change is not on disk yet. Returns whether the file is up to date.
    pub fn flush(&mut self) -> bool {
        self.dirty_since.is_none() || self.save()
    }

    /// Writes the keys changed since the last read/write onto what is on disk now (another process may have written other
    /// keys meanwhile). Returns whether the file was written. When the file exists and cannot be read, nothing is written
    /// and the changes stay pending (`tick` / `flush` try again): a moment of sharing violation must not wipe the file.
    /// A file that was read but is not a properties file is kept as `<file>.bad` first.
    pub fn save(&mut self) -> bool {
        if self.path.as_os_str().is_empty() {
            return false;
        }
        let mut disk = match read_disk(&self.path, self.reader) {
            Disk::Missing => Vec::new(),
            Disk::Unreadable => return false,
            Disk::Damaged => {
                let mut bad = self.path.clone().into_os_string();
                bad.push(".bad");
                let _ = std::fs::copy(&self.path, bad);
                Vec::new()
            }
            Disk::Pairs(p) => p,
        };
        for (k, v) in &self.values {
            if self.base.iter().find(|(bk, _)| bk == k).map(|(_, bv)| bv) != Some(v) {
                pairs_set(&mut disk, k, v);
            }
        }
        for (k, _) in &self.base {
            if !self.values.iter().any(|(vk, _)| vk == k) {
                disk.retain(|(dk, _)| dk != k);
            }
        }
        let text = render(&disk);
        if write_atomic(&self.path, text.as_bytes(), false, false).is_err() {
            return false;
        }
        self.values = disk.clone();
        self.base = disk;
        self.dirty_since = None;
        self.writes += 1;
        true
    }
}

impl Drop for PropertiesFile {
    fn drop(&mut self) {
        self.flush();
    }
}

/// `loadAsXml`: the `val` attribute, or the first child element as single-line XML.
fn parse(bytes: &[u8]) -> Option<Pairs> {
    let text = std::str::from_utf8(bytes).ok()?;
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let doc = roxmltree::Document::parse(text).ok()?;
    let root = doc.root_element();
    if root.tag_name().name() != "PROPERTIES" {
        return None;
    }
    let mut out = Vec::new();
    for e in root
        .children()
        .filter(|n| n.is_element() && n.tag_name().name() == "VALUE")
    {
        let name = e.attribute("name").unwrap_or("");
        if name.is_empty() {
            continue;
        }
        let value = match e
            .children()
            .find(|c| c.is_element() || (c.is_text() && !is_blank(c.text())))
        {
            Some(c) => {
                let mut s = String::new();
                write_node(c, &mut s);
                s
            }
            None => e.attribute("val").unwrap_or("").to_owned(),
        };
        pairs_set(&mut out, name, &value);
    }
    Some(out)
}

fn is_blank(t: Option<&str>) -> bool {
    t.is_none_or(|t| t.chars().all(char::is_whitespace))
}

/// `XmlElement::toString(singleLine, withoutHeader)` of a parsed node (blank text between elements dropped).
fn write_node(n: roxmltree::Node, out: &mut String) {
    if n.is_text() {
        escape(n.text().unwrap_or(""), false, out);
        return;
    }
    if !n.is_element() {
        return;
    }
    let name = n.tag_name().name();
    out.push('<');
    out.push_str(name);
    for a in n.attributes() {
        out.push(' ');
        out.push_str(a.name());
        out.push_str("=\"");
        escape(a.value(), true, out);
        out.push('"');
    }
    let kids: Vec<_> = n
        .children()
        .filter(|c| c.is_element() || (c.is_text() && !is_blank(c.text())))
        .collect();
    if kids.is_empty() {
        out.push_str("/>");
    } else {
        out.push('>');
        for c in kids {
            write_node(c, out);
        }
        out.push_str("</");
        out.push_str(name);
        out.push('>');
    }
}

/// `XmlOutputFunctions::escapeIllegalXmlChars`: ASCII letters, digits and a fixed set of punctuation as they are,
/// `& " < >` as entities, everything else (all non-ASCII too) as `&#code;`.
fn escape(s: &str, change_newlines: bool, out: &mut String) {
    const LEGAL: &str = " .,;:-()_+=?!$#@[]/|*%~{}'\\";
    for c in s.chars() {
        match c {
            c if c.is_ascii_alphanumeric() || LEGAL.contains(c) => out.push(c),
            '&' => out.push_str("&amp;"),
            '"' => out.push_str("&quot;"),
            '>' => out.push_str("&gt;"),
            '<' => out.push_str("&lt;"),
            '\n' | '\r' if !change_newlines => out.push(c),
            c => {
                out.push_str("&#");
                out.push_str(&(c as u32).to_string());
                out.push(';');
            }
        }
    }
}

/// `Some(single-line xml)` when `value` is exactly one XML element (what JUCE's `parseXML` would take, minus the
/// trailing-garbage leniency).
fn as_xml_element(value: &str) -> Option<String> {
    let t = value.trim();
    if !t.starts_with('<') || !t.ends_with('>') {
        return None;
    }
    let doc = roxmltree::Document::parse(t).ok()?;
    let mut s = String::new();
    write_node(doc.root_element(), &mut s);
    Some(s)
}

/// The file text `PropertiesFile::saveAsXml` produces (CRLF line ends on every OS, like JUCE).
fn render(values: &[(String, String)]) -> String {
    let mut out =
        String::from("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n\r\n<PROPERTIES>\r\n");
    for (k, v) in values {
        out.push_str("  <VALUE name=\"");
        escape(k, true, &mut out);
        match as_xml_element(v) {
            Some(xml) => {
                out.push_str("\">");
                out.push_str(&xml);
                out.push_str("</VALUE>\r\n");
            }
            None => {
                out.push_str("\" val=\"");
                escape(v, true, &mut out);
                out.push_str("\"/>\r\n");
            }
        }
    }
    out.push_str("</PROPERTIES>\r\n");
    out
}

/// Writes `bytes` to a temp file beside `path` and renames it over `path` (the C++ `TemporaryFile` does the same).
/// `private`: the file is created readable by this user only (tokens).
pub(crate) fn write_atomic(
    path: &Path,
    bytes: &[u8],
    private: bool,
    durable: bool,
) -> std::io::Result<()> {
    let dir = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    std::fs::create_dir_all(dir)?;
    #[cfg(not(unix))]
    let _ = private;
    #[cfg(unix)]
    if private {
        use std::os::unix::fs::PermissionsExt;
        // the folder is ours (HEARASIDE), never a shared parent such as ~/.config
        if dir.file_name().is_some_and(|n| n == "HEARASIDE") {
            let _ = std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700));
        }
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(format!(".tmp{}", std::process::id()));
    let tmp = PathBuf::from(tmp);
    let result = (|| {
        let mut opts = std::fs::OpenOptions::new();
        opts.write(true).create(true).truncate(true);
        #[cfg(unix)]
        if private {
            use std::os::unix::fs::OpenOptionsExt;
            opts.mode(0o600);
        }
        let mut f = opts.open(&tmp)?;
        f.write_all(bytes)?;
        if durable {
            f.sync_all()?; // the tokens survive a power cut; the settings are not worth the wait
        }
        drop(f);
        // a reader that still holds the old file open (the other plug-in) can make the rename fail for a moment on Windows
        let mut last = Ok(());
        for attempt in 0..8 {
            last = std::fs::rename(&tmp, path);
            if last.is_ok() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10 * (attempt + 1)));
        }
        last
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    result
}

// ---- JUCE number text -------------------------------------------------------------------------------------------

/// `String::getIntValue()`: leading blanks, a sign, digits (wrapping on overflow), anything after is ignored.
pub(crate) fn juce_int(s: &str) -> i32 {
    let s = s.trim_start();
    let (neg, rest) = match s.as_bytes().first() {
        Some(b'-') => (true, &s[1..]),
        Some(b'+') => (false, &s[1..]),
        _ => (false, s),
    };
    let mut v: u32 = 0;
    for b in rest.bytes().take_while(u8::is_ascii_digit) {
        v = v.wrapping_mul(10).wrapping_add(u32::from(b - b'0'));
    }
    let v = v as i32;
    if neg { v.wrapping_neg() } else { v }
}

/// `String::getDoubleValue()`: the longest number at the start (after blanks), 0 when there is none.
pub(crate) fn juce_double(s: &str) -> f64 {
    let s = s.trim_start();
    let b = s.as_bytes();
    let mut i = 0;
    if matches!(b.first(), Some(b'-' | b'+')) {
        i = 1;
    }
    let digits_from = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    let mut mantissa_digits = i - digits_from;
    if i < b.len() && b[i] == b'.' {
        i += 1;
        let f = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        mantissa_digits += i - f;
    }
    if mantissa_digits == 0 {
        return 0.0;
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        let mut j = i + 1;
        if j < b.len() && (b[j] == b'-' || b[j] == b'+') {
            j += 1;
        }
        let e = j;
        while j < b.len() && b[j].is_ascii_digit() {
            j += 1;
        }
        if j > e {
            i = j;
        }
    }
    s[..i].parse::<f64>().unwrap_or(0.0)
}

/// `juce::String(double)`: C++ `ostream << double` with the default precision, i.e. `%g` with 6 significant digits.
pub(crate) fn juce_double_string(v: f64) -> String {
    if v.is_nan() {
        return "nan".into();
    }
    if v.is_infinite() {
        return if v > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if v == 0.0 {
        return if v.is_sign_negative() {
            "-0".into()
        } else {
            "0".into()
        };
    }
    // 6 significant digits in scientific form gives the decimal exponent after rounding
    let sci = format!("{v:.5e}");
    let (mant, exp) = sci.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    if !(-4..6).contains(&exp) {
        let mant = trim_zeros(mant);
        return format!("{mant}e{}{:02}", if exp < 0 { '-' } else { '+' }, exp.abs());
    }
    let decimals = (5 - exp).max(0) as usize;
    trim_zeros(&format!("{v:.decimals$}")).to_owned()
}

fn trim_zeros(s: &str) -> &str {
    if s.contains('.') {
        s.trim_end_matches('0').trim_end_matches('.')
    } else {
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn number_text_like_juce() {
        assert_eq!(juce_double_string(1.0), "1");
        assert_eq!(juce_double_string(f64::from(0.62_f32)), "0.62");
        assert_eq!(juce_double_string(f64::from(1.25_f32)), "1.25");
        assert_eq!(juce_double_string(0.95), "0.95");
        assert_eq!(juce_double_string(1234567.0), "1.23457e+06");
        assert_eq!(juce_double_string(0.0000123), "1.23e-05");
        assert_eq!(juce_double_string(100000.0), "100000");
        assert_eq!(juce_int("  -12abc"), -12);
        assert_eq!(juce_int("true"), 0);
        assert_eq!(juce_int("1"), 1);
        assert_eq!(juce_double("1.5x"), 1.5);
        assert_eq!(juce_double(".5"), 0.5);
        assert_eq!(juce_double("e5"), 0.0);
        assert_eq!(juce_double("2e2"), 200.0);
    }
}
