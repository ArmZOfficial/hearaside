//! Generates the UI string table from `plugins/common/Strings.{h,cpp}`: the C++ files stay the single source of truth
//! while both C++ and Rust plug-ins ship (CLAUDE.md rule 5). `Strings.h` gives the key list (`enum class Str`), the
//! `s(Str::Key, "th", "en");` calls of `Strings.cpp` give the texts. Anything the reader does not understand stops the
//! build: a table that silently drifts is worse than a broken build.
use std::collections::HashMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

fn main() {
    let dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let common = dir.join("../../../plugins/common");
    let header = common.join("Strings.h");
    let source = common.join("Strings.cpp");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-changed={}", source.display());

    let keys = parse_keys(&read(&header));
    let calls = parse_calls(&read(&source));

    let mut by_key: HashMap<&str, (&str, &str)> = HashMap::new();
    for (k, th, en) in &calls {
        if !keys.iter().any(|x| x == k) {
            panic!("Strings.cpp sets Str::{k}, which is not in the Strings.h enum");
        }
        if by_key.insert(k, (th, en)).is_some() {
            println!(
                "cargo:warning=Strings.cpp sets Str::{k} twice (the last one wins, like the C++ table)"
            );
        }
    }

    let mut out = String::new();
    out.push_str("/// Every key of `enum class Str` in `Strings.h`, in the same order.\n");
    out.push_str(
        "#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\n#[repr(usize)]\npub enum Str {\n",
    );
    for k in &keys {
        writeln!(out, "    {k},").unwrap();
    }
    out.push_str("}\n\n");
    writeln!(
        out,
        "/// Number of keys.\npub const STR_COUNT: usize = {};\n",
        keys.len()
    )
    .unwrap();
    out.push_str(
        "impl Str {\n    /// All keys in enum order.\n    pub const ALL: [Str; STR_COUNT] = [\n",
    );
    for k in &keys {
        writeln!(out, "        Str::{k},").unwrap();
    }
    out.push_str("    ];\n\n    /// The C++ name of the key.\n    pub fn name(self) -> &'static str {\n        NAMES[self as usize]\n    }\n}\n\n");
    out.push_str("static NAMES: [&str; STR_COUNT] = [\n");
    for k in &keys {
        writeln!(out, "    {k:?},").unwrap();
    }
    out.push_str("];\n\n/// (Thai, English); `None` = the key has no text in `Strings.cpp` (the C++ `tr()` answers `\"?\"`).\n");
    out.push_str("pub(crate) static TABLE: [Option<(&str, &str)>; STR_COUNT] = [\n");
    for k in &keys {
        match by_key.get(k.as_str()) {
            Some((th, en)) => writeln!(out, "    Some(({th:?}, {en:?})),").unwrap(),
            None => {
                println!("cargo:warning=Strings.cpp has no text for Str::{k}");
                out.push_str("    None,\n");
            }
        }
    }
    out.push_str("];\n");
    let target = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("strings_table.rs");
    std::fs::write(target, out).unwrap();
}

fn read(p: &Path) -> String {
    let bytes = std::fs::read(p).unwrap_or_else(|e| panic!("cannot read {}: {e}", p.display()));
    String::from_utf8(bytes).unwrap_or_else(|_| panic!("{} is not UTF-8", p.display()))
}

/// The identifiers inside `enum class Str { ... Count };`, without `Count`.
fn parse_keys(h: &str) -> Vec<String> {
    let start = h
        .find("enum class Str {")
        .expect("enum class Str not found in Strings.h")
        + "enum class Str {".len();
    let end = start + h[start..].find("};").expect("end of enum Str not found");
    let mut keys = Vec::new();
    for line in h[start..end].lines() {
        let line = line.split("//").next().unwrap();
        for part in line.split(',') {
            let p = part.trim();
            if p.is_empty() {
                continue;
            }
            assert!(
                p.chars().all(|c| c.is_ascii_alphanumeric() || c == '_'),
                "unexpected enum entry {p:?}"
            );
            keys.push(p.to_owned());
        }
    }
    assert_eq!(
        keys.pop().as_deref(),
        Some("Count"),
        "Str must end with Count"
    );
    keys
}

struct Lex<'a> {
    s: &'a [u8],
    src: &'a str,
    i: usize,
}

impl Lex<'_> {
    fn skip(&mut self) {
        loop {
            while self.i < self.s.len() && self.s[self.i].is_ascii_whitespace() {
                self.i += 1;
            }
            if self.s[self.i..].starts_with(b"//") {
                while self.i < self.s.len() && self.s[self.i] != b'\n' {
                    self.i += 1;
                }
            } else if self.s[self.i..].starts_with(b"/*") {
                self.i += 2
                    + self.src[self.i + 2..]
                        .find("*/")
                        .expect("unterminated comment")
                    + 2;
            } else {
                return;
            }
        }
    }

    fn eat(&mut self, lit: &str) -> bool {
        if self.s[self.i..].starts_with(lit.as_bytes()) {
            self.i += lit.len();
            true
        } else {
            false
        }
    }

    fn expect(&mut self, lit: &str) {
        self.skip();
        if !self.eat(lit) {
            panic!(
                "Strings.cpp: expected {lit:?} near {:?}",
                &self.src[self.i..(self.i + 60).min(self.src.len())]
            );
        }
    }

    /// One or more adjacent C string literals, joined.
    fn strings(&mut self) -> String {
        let mut out = String::new();
        self.skip();
        assert!(
            self.s[self.i] == b'"',
            "Strings.cpp: expected a string literal near {:?}",
            &self.src[self.i..(self.i + 60).min(self.src.len())]
        );
        while self.s[self.i] == b'"' {
            self.i += 1;
            loop {
                let c = self.s[self.i];
                match c {
                    b'"' => {
                        self.i += 1;
                        break;
                    }
                    b'\\' => {
                        self.i += 1;
                        let e = self.s[self.i];
                        self.i += 1;
                        match e {
                            b'n' => out.push('\n'),
                            b't' => out.push('\t'),
                            b'r' => out.push('\r'),
                            b'"' => out.push('"'),
                            b'\'' => out.push('\''),
                            b'\\' => out.push('\\'),
                            b'0' if !self.s[self.i].is_ascii_digit() => out.push('\0'),
                            b'x' | b'u' => panic!(
                                "Strings.cpp: \\{} escapes are not supported by the reader",
                                e as char
                            ),
                            _ => panic!("Strings.cpp: unsupported escape \\{}", e as char),
                        }
                    }
                    b'\n' => panic!("Strings.cpp: newline inside a string literal"),
                    _ => {
                        let ch = self.src[self.i..].chars().next().unwrap();
                        out.push(ch);
                        self.i += ch.len_utf8();
                    }
                }
            }
            self.skip();
        }
        out
    }
}

/// `(key, th, en)` for each `s(Str::Key, "th", "en");` between `Table build() {` and `return t;`.
fn parse_calls(cpp: &str) -> Vec<(String, String, String)> {
    let head = cpp
        .find("Table build() {")
        .expect("Table build() not found in Strings.cpp");
    let lambda = head
        + cpp[head..]
            .find("auto s = ")
            .expect("the `auto s = ` helper moved");
    let mut lx = Lex {
        s: cpp.as_bytes(),
        src: cpp,
        i: lambda
            + cpp[lambda..]
                .find('\n')
                .expect("end of the helper lambda line"),
    };
    let mut calls = Vec::new();
    loop {
        lx.skip();
        if lx.eat("return t;") {
            break;
        }
        assert!(
            lx.eat("s(Str::"),
            "Strings.cpp: unexpected statement near {:?}",
            &cpp[lx.i..(lx.i + 60).min(cpp.len())]
        );
        let start = lx.i;
        while lx.s[lx.i].is_ascii_alphanumeric() || lx.s[lx.i] == b'_' {
            lx.i += 1;
        }
        let key = cpp[start..lx.i].to_owned();
        lx.expect(",");
        let th = lx.strings();
        lx.expect(",");
        let en = lx.strings();
        lx.expect(")");
        lx.expect(";");
        calls.push((key, th, en));
    }
    calls
}
