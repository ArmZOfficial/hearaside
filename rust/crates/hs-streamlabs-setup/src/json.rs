//! The part of `juce::JSON` / `juce::var` the C++ tool used, so the app.asar header is parsed and written back the same
//! way: properties keep their order, a failed parse gives `Null` (JUCE: a void var), and `to_text` is
//! `JSON::toString(var, allOnOneLine = true)` (`{"a": 1, "b": [1, 2]}`, UTF-8 left as is).

#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(v) => v.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn get_mut(&mut self, key: &str) -> Option<&mut Json> {
        match self {
            Json::Obj(v) => v.iter_mut().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// `DynamicObject::setProperty`: replaces in place, or appends. Does nothing on a non-object.
    pub fn set(&mut self, key: &str, value: Json) {
        if let Json::Obj(v) = self {
            match v.iter_mut().find(|(k, _)| k == key) {
                Some((_, old)) => *old = value,
                None => v.push((key.to_string(), value)),
            }
        }
    }

    /// `int64_t(var)`
    pub fn as_i64(&self) -> i64 {
        match self {
            Json::Bool(b) => *b as i64,
            Json::Int(i) => *i,
            Json::Float(f) => *f as i64,
            Json::Str(s) => large_int_value(s),
            _ => 0,
        }
    }

    /// `var::toString()`
    pub fn as_text(&self) -> String {
        match self {
            Json::Bool(b) => (if *b { "1" } else { "0" }).into(),
            Json::Int(i) => i.to_string(),
            Json::Float(f) => float_text(*f),
            Json::Str(s) => s.clone(),
            Json::Arr(_) => "[Array]".into(),
            _ => String::new(),
        }
    }

    /// `bool(var)`
    pub fn truthy(&self) -> bool {
        match self {
            Json::Null => false,
            Json::Bool(b) => *b,
            Json::Int(i) => *i != 0,
            Json::Float(f) => *f != 0.0,
            Json::Str(s) => {
                let t = s.trim();
                int_value(s) != 0 || t.eq_ignore_ascii_case("true") || t.eq_ignore_ascii_case("yes")
            }
            Json::Arr(_) | Json::Obj(_) => true,
        }
    }

    /// `juce::JSON::toString(v, true)`
    pub fn to_text(&self) -> String {
        let mut out = String::new();
        self.write(&mut out);
        out
    }

    fn write(&self, out: &mut String) {
        match self {
            Json::Null => out.push_str("null"),
            Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
            Json::Int(i) => out.push_str(&i.to_string()),
            Json::Float(f) if f.is_finite() => out.push_str(&float_text(*f)),
            Json::Float(_) => out.push_str("null"),
            Json::Str(s) => write_string(out, s),
            Json::Arr(items) => {
                out.push('[');
                for (i, item) in items.iter().enumerate() {
                    if i > 0 {
                        out.push_str(", ");
                    }
                    item.write(out);
                }
                out.push(']');
            }
            Json::Obj(props) => {
                out.push('{');
                for (i, (k, v)) in props.iter().enumerate() {
                    if i > 0 {
                        out.push_str(", ");
                    }
                    write_string(out, k);
                    out.push_str(": ");
                    v.write(out);
                }
                out.push('}');
            }
        }
    }
}

// Doubles never occur in an asar header (sizes are integers, offsets strings); this only keeps them from being lost.
fn float_text(f: f64) -> String {
    if f.fract() == 0.0 && f.abs() < 1.0e6 {
        format!("{f:.1}")
    } else {
        f.to_string()
    }
}

fn write_string(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            '\t' => out.push_str("\\t"),
            '\r' => out.push_str("\\r"),
            '\n' => out.push_str("\\n"),
            c if (c as u32) < 32 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

/// `String::getIntValue()` (32 bit, wraps)
fn int_value(s: &str) -> i32 {
    large_int_value(s) as i32
}

/// `String::getLargeIntValue()`: leading white space, an optional '-', digits up to the first other character; wraps.
pub fn large_int_value(s: &str) -> i64 {
    let s = s.trim_start();
    let (neg, digits) = match s.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, s),
    };
    let v = digits
        .chars()
        .map_while(|c| c.to_digit(10))
        .fold(0u64, |v, d| v.wrapping_mul(10).wrapping_add(d as u64));
    if neg {
        (v as i64).wrapping_neg()
    } else {
        v as i64
    }
}

/// `JSON::parse(text)`: an object or array at the top (anything after it is ignored); otherwise `Null`, like JUCE's void var.
pub fn parse(text: &str) -> Json {
    let mut p = Parser {
        s: text.chars().collect(),
        i: 0,
    };
    p.object_or_array().unwrap_or(Json::Null)
}

struct Parser {
    s: Vec<char>,
    i: usize,
}

type R<T> = Result<T, ()>;

impl Parser {
    fn skip_ws(&mut self) {
        while self.s.get(self.i).is_some_and(|c| c.is_whitespace()) {
            self.i += 1;
        }
    }
    fn peek(&self) -> char {
        self.s.get(self.i).copied().unwrap_or('\0')
    }
    fn read(&mut self) -> char {
        let c = self.peek();
        self.i += 1;
        c
    }
    fn eat(&mut self, c: char) -> bool {
        if self.peek() == c {
            self.i += 1;
            true
        } else {
            false
        }
    }
    fn eat_str(&mut self, t: &str) -> bool {
        t.chars().all(|c| self.eat(c))
    }

    fn object_or_array(&mut self) -> R<Json> {
        self.skip_ws();
        if self.eat('{') {
            return self.object();
        }
        if self.eat('[') {
            return self.array();
        }
        if self.i >= self.s.len() {
            Ok(Json::Null)
        } else {
            Err(())
        }
    }

    fn hex_digit(&mut self) -> R<u32> {
        self.read().to_digit(16).ok_or(())
    }

    fn code_unit(&mut self) -> R<u32> {
        let mut v = 0;
        for _ in 0..4 {
            v = v << 4 | self.hex_digit()?;
        }
        Ok(v)
    }

    fn escape_sequence(&mut self) -> R<char> {
        let first = self.code_unit()?;
        if !(0xd800..0xe000).contains(&first) {
            return char::from_u32(first).ok_or(());
        }
        if first >= 0xdc00 || self.read() != '\\' || self.read() != 'u' {
            return Err(());
        }
        let low = self.code_unit()?;
        if !(0xdc00..0xe000).contains(&low) {
            return Err(());
        }
        char::from_u32(0x10000 + ((first - 0xd800) << 10) + (low - 0xdc00)).ok_or(())
    }

    fn string(&mut self, quote: char) -> R<String> {
        let mut out = String::new();
        loop {
            let mut c = self.read();
            if c == quote {
                return Ok(out);
            }
            if c == '\\' {
                c = self.read();
                c = match c {
                    'a' => '\u{7}',
                    'b' => '\u{8}',
                    'f' => '\u{c}',
                    'n' => '\n',
                    'r' => '\r',
                    't' => '\t',
                    'u' => self.escape_sequence()?,
                    other => other,
                };
            }
            if c == '\0' {
                return Err(());
            }
            out.push(c);
        }
    }

    fn any(&mut self) -> R<Json> {
        self.skip_ws();
        let start = self.i;
        match self.read() {
            '{' => self.object(),
            '[' => self.array(),
            '"' => Ok(Json::Str(self.string('"')?)),
            '\'' => Ok(Json::Str(self.string('\'')?)),
            '-' => {
                self.skip_ws();
                self.number(true)
            }
            '0'..='9' => {
                self.i = start;
                self.number(false)
            }
            't' if self.eat_str("rue") => Ok(Json::Bool(true)),
            'f' if self.eat_str("alse") => Ok(Json::Bool(false)),
            'n' if self.eat_str("ull") => Ok(Json::Null),
            _ => Err(()),
        }
    }

    fn number(&mut self, negative: bool) -> R<Json> {
        let start = self.i;
        if !self.read().is_ascii_digit() {
            return Err(());
        }
        let mut v: i64 = 0;
        self.i = start;
        loop {
            let last = self.i;
            let c = self.read();
            if let Some(d) = c.to_digit(10) {
                v = v.wrapping_mul(10).wrapping_add(d as i64);
            } else if matches!(c, 'e' | 'E' | '.') {
                self.i = start;
                let f = self.double();
                return Ok(Json::Float(if negative { -f } else { f }));
            } else if c.is_whitespace() || matches!(c, ',' | '}' | ']' | '\0') {
                self.i = last;
                return Ok(Json::Int(if negative { v.wrapping_neg() } else { v }));
            } else {
                return Err(());
            }
        }
    }

    fn double(&mut self) -> f64 {
        let start = self.i;
        let digits = |p: &mut Parser| {
            while p.peek().is_ascii_digit() {
                p.i += 1;
            }
        };
        digits(self);
        if self.eat('.') {
            digits(self);
        }
        if matches!(self.peek(), 'e' | 'E') {
            let save = self.i;
            self.i += 1;
            if matches!(self.peek(), '+' | '-') {
                self.i += 1;
            }
            if self.peek().is_ascii_digit() {
                digits(self);
            } else {
                self.i = save;
            }
        }
        self.s[start..self.i]
            .iter()
            .collect::<String>()
            .parse()
            .unwrap_or(0.0)
    }

    fn object(&mut self) -> R<Json> {
        let mut props: Vec<(String, Json)> = Vec::new();
        loop {
            self.skip_ws();
            match self.read() {
                '}' => return Ok(Json::Obj(props)),
                '"' => {}
                _ => return Err(()),
            }
            let name = self.string('"')?;
            if name.is_empty() {
                return Err(());
            }
            self.skip_ws();
            if self.read() != ':' {
                return Err(());
            }
            let value = self.any()?;
            match props.iter_mut().find(|(k, _)| *k == name) {
                Some((_, old)) => *old = value,
                None => props.push((name, value)),
            }
            self.skip_ws();
            if self.eat(',') {
                continue;
            }
            if self.eat('}') {
                return Ok(Json::Obj(props));
            }
            return Err(());
        }
    }

    fn array(&mut self) -> R<Json> {
        let mut items = Vec::new();
        loop {
            self.skip_ws();
            if self.eat(']') {
                return Ok(Json::Arr(items));
            }
            if self.i >= self.s.len() {
                return Err(());
            }
            items.push(self.any()?);
            self.skip_ws();
            if self.eat(',') {
                continue;
            }
            if self.eat(']') {
                return Ok(Json::Arr(items));
            }
            return Err(());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_like_juce_single_line() {
        let j = parse(
            r#" {"a":1,"b":[1,2,{"c":"x\ny"}],"d":{},"e":[],"f":true,"g":null,"h":"é\u0001\"\\"} trailing"#,
        );
        assert_eq!(
            j.to_text(),
            r#"{"a": 1, "b": [1, 2, {"c": "x\ny"}], "d": {}, "e": [], "f": true, "g": null, "h": "é\u0001\"\\"}"#
        );
    }

    #[test]
    fn keeps_property_order_and_replaces_in_place() {
        let mut j = parse(r#"{"z":1,"a":2,"z":3}"#);
        assert_eq!(j.to_text(), r#"{"z": 3, "a": 2}"#);
        j.set("z", Json::Str("9".into()));
        j.set("n", Json::Int(5_000_000_000));
        assert_eq!(j.to_text(), r#"{"z": "9", "a": 2, "n": 5000000000}"#);
    }

    #[test]
    fn malformed_gives_null() {
        for bad in [
            "",
            "   ",
            "[1,",
            "{\"a\":",
            "{\"a\" 1}",
            "{a:1}",
            "{\"\":1}",
            "12",
            "nope",
            "{\"a\":12ab}",
            "{\"a\":\"x}",
            "{\"a\":\"\\ud800\"}",
        ] {
            assert_eq!(parse(bad), Json::Null, "{bad:?}");
        }
    }

    #[test]
    fn var_conversions() {
        assert_eq!(Json::Str(" -12abc".into()).as_i64(), -12);
        assert_eq!(Json::Int(7).as_text(), "7");
        assert!(
            Json::Str("yes".into()).truthy()
                && Json::Str("2".into()).truthy()
                && !Json::Str("no".into()).truthy()
        );
        assert!(!Json::Null.truthy() && Json::Bool(true).truthy());
        assert_eq!(
            parse("[1.5, -2, 3e2, 'q', \"\\ud83d\\ude00\"]").to_text(),
            "[1.5, -2, 300.0, \"q\", \"\u{1f600}\"]"
        );
    }
}
