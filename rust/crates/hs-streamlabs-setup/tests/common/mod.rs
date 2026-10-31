//! A fake Streamlabs Desktop folder: resources\app.asar (a real asar layout: pickle header + JSON + file data),
//! resources\app.asar.unpacked\...\obs.dll and the obs-plugins\64bit folder next to it.
#![allow(dead_code)]

use hs_streamlabs_setup::asar::EDITS;
use hs_streamlabs_setup::json::{self, Json};
use hs_streamlabs_setup::sha256;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

pub struct Tmp(pub PathBuf);

impl Tmp {
    pub fn new(tag: &str) -> Tmp {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!(
            "hs-sl-{tag}-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        Tmp(p)
    }
}

impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// The renderer bundle: every anchor once, between filler (and a non-ASCII character)
pub fn bundle(tag: &str) -> Vec<u8> {
    let mut s = format!("/* renderer {tag} é ภาษาไทย */\nvar a=1;");
    for (anchor, _) in EDITS {
        s.push_str("\nfiller();");
        s.push_str(anchor);
    }
    s.push_str("\nend();");
    s.into_bytes()
}

/// An original app.asar. `integrity` gives the bundle (and the files after it) an integrity record.
pub fn asar(bundle: &[u8], integrity: bool) -> Vec<u8> {
    let first = b"hello first file".to_vec();
    let last: Vec<u8> = (0..=255u8).collect();
    let inner = b"inner".to_vec();
    let (o_bundle, o_last, o_inner) = (
        first.len(),
        first.len() + bundle.len(),
        first.len() + bundle.len() + last.len(),
    );
    let integ = |d: &[u8]| {
        if integrity {
            format!(
                r#","integrity":{{"algorithm":"SHA256","hash":"{}","blockSize":4194304,"blocks":["{}"]}}"#,
                sha256::hex(d),
                sha256::hex(d)
            )
        } else {
            String::new()
        }
    };
    let header = format!(
        r#"{{"files":{{"first.txt":{{"size":{},"offset":"0"}},"bundles":{{"files":{{"renderer.abc123.js":{{"size":{}{},"offset":"{o_bundle}"}}}}}},"last.bin":{{"size":{}{},"offset":"{o_last}"}},"node_modules":{{"files":{{"m":{{"files":{{"inner.js":{{"size":{},"offset":"{o_inner}"}}}}}}}}}},"native.node":{{"size":99,"unpacked":true}},"ünï.txt":{{"size":0,"offset":"{}"}}}}}}"#,
        first.len(),
        bundle.len(),
        integ(bundle),
        last.len(),
        integ(&last),
        inner.len(),
        o_inner + inner.len(),
    );
    with_header(&header, &[first, bundle.to_vec(), last, inner].concat())
}

/// pickle: 4, size of the header pickle, payload size, JSON length; then the JSON, zero padding to 4, the file data
pub fn with_header(json: &str, data: &[u8]) -> Vec<u8> {
    let len = json.len() as u32;
    let pad = (4 - len % 4) % 4;
    let payload = 4 + len + pad;
    let mut v = Vec::new();
    for w in [4, payload + 4, payload, len] {
        v.extend_from_slice(&w.to_le_bytes());
    }
    v.extend_from_slice(json.as_bytes());
    v.extend(std::iter::repeat_n(0u8, pad as usize));
    v.extend_from_slice(data);
    v
}

/// The header and every packed file's bytes by path, read back with the pickle layout (independent of the tool)
pub fn unpack(asar: &[u8]) -> (Json, BTreeMap<String, Vec<u8>>) {
    let w = |i: usize| u32::from_le_bytes(asar[i * 4..i * 4 + 4].try_into().unwrap()) as usize;
    let header = json::parse(std::str::from_utf8(&asar[16..16 + w(3)]).unwrap());
    let base = 8 + w(1);
    assert_eq!(
        base,
        16 + w(3) + (4 - w(3) % 4) % 4,
        "header pickle sizes agree"
    );
    let mut out = BTreeMap::new();
    fn walk(
        node: &Json,
        prefix: &str,
        asar: &[u8],
        base: usize,
        out: &mut BTreeMap<String, Vec<u8>>,
    ) {
        let Some(Json::Obj(files)) = node.get("files") else {
            return;
        };
        for (name, e) in files {
            if e.get("files").is_some() {
                walk(e, &format!("{prefix}{name}/"), asar, base, out);
            } else if let Some(off) = e.get("offset") {
                let (o, n) = (
                    off.as_text().parse::<usize>().unwrap(),
                    e.get("size").unwrap().as_i64() as usize,
                );
                out.insert(
                    format!("{prefix}{name}"),
                    asar[base + o..base + o + n].to_vec(),
                );
            }
        }
    }
    walk(&header, "", asar, base, &mut out);
    (header, out)
}

pub struct Tree {
    pub tmp: Tmp,
    pub root: PathBuf,
    pub dll: PathBuf,
}

impl Tree {
    pub fn new(asar: Option<&[u8]>) -> Tree {
        let tmp = Tmp::new("t");
        let root = tmp.0.join("Streamlabs OBS");
        let res = root.join("resources");
        fs::create_dir_all(
            res.join("app.asar.unpacked/node_modules/obs-studio-node/obs-plugins/64bit"),
        )
        .unwrap();
        fs::write(
            res.join("app.asar.unpacked/node_modules/obs-studio-node/obs.dll"),
            b"obs",
        )
        .unwrap();
        if let Some(a) = asar {
            fs::write(res.join("app.asar"), a).unwrap();
        }
        fs::create_dir_all(tmp.0.join("setup")).unwrap();
        let dll = tmp.0.join("setup").join("hearaside-obs.dll");
        fs::write(&dll, b"MZ fake hearaside-obs.dll").unwrap();
        Tree { tmp, root, dll }
    }
    pub fn res(&self, name: &str) -> PathBuf {
        self.root.join("resources").join(name)
    }
    pub fn asar(&self) -> Vec<u8> {
        fs::read(self.res("app.asar")).unwrap()
    }
    pub fn plugin(&self) -> PathBuf {
        self.res("app.asar.unpacked")
            .join("node_modules")
            .join("obs-studio-node")
            .join("obs-plugins")
            .join("64bit")
            .join("hearaside-obs.dll")
    }
    /// every file under the Streamlabs folder: relative path -> SHA-256 (the state file: just its size, the time differs)
    pub fn snapshot(&self) -> BTreeMap<String, String> {
        fn walk(dir: &Path, root: &Path, out: &mut BTreeMap<String, String>) {
            for e in fs::read_dir(dir).unwrap() {
                let p = e.unwrap().path();
                let rel = p
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                if p.is_dir() {
                    out.insert(format!("{rel}/"), String::new());
                    walk(&p, root, out);
                } else if rel.ends_with(".hearaside-state") {
                    let s = fs::read_to_string(&p).unwrap();
                    out.insert(rel, format!("state: size {}", s.split(' ').next().unwrap()));
                } else {
                    out.insert(rel, sha256::hex(&fs::read(&p).unwrap()));
                }
            }
        }
        let mut out = BTreeMap::new();
        walk(&self.root, &self.root, &mut out);
        out
    }
}
