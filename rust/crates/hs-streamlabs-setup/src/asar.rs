//! app.asar: its header, the renderer bundle (Streamlabs' UI code) and the patch that adds the HEARASIDE source type.

use crate::Failure;
use crate::json::{self, Json};
use crate::sha256;
use std::fs::File;
use std::io::{self, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;

pub const SOURCE_ID: &str = "hearaside_source";

/// {anchor, replacement}: each anchor must be found exactly once in the original renderer bundle
pub const EDITS: [(&str, &str); 7] = [
    // Windows types a scene collection may contain (SourcesNode.removeUnsupported / load)
    (
        r#""spout_capture","smart_browser_source"],macSources="#,
        r#""spout_capture","smart_browser_source","hearaside_source"],macSources="#,
    ),
    // "Add Source": the allowlist, filtered by what libobs has loaded
    (
        r#"{description:"Spout2 capture",value:"spout_capture"}].filter("#,
        r#"{description:"Spout2 capture",value:"spout_capture"},{description:"HEARASIDE",value:"hearaside_source"}].filter("#,
    ),
    // name, text, icon and group in the "Add Source" showcase (without a group it is not listed)
    (
        r#"group:"capture"},smart_browser_source:{name:"Reactive Source""#,
        r#"group:"capture"},hearaside_source:{name:"HEARASIDE",shortDesc:"DAW audio",description:"What your viewers hear from the HEARASIDE Hub in your DAW: the Stream Mix or a stem, the same source as in OBS.",icon:"fas fa-microphone",group:"av"},smart_browser_source:{name:"Reactive Source""#,
    ),
    // audio-only sources open Advanced Audio, whose panel takes the first property for a device list:
    // no device panel for HEARASIDE, and its own Properties (bus, output, buffer) open as for a video source
    (
        r#"hasDevices=!((null==source?void 0:source.video)||"soundtrack_source"===(null==source?void 0:source.type))"#,
        r#"hasDevices=!((null==source?void 0:source.video)||"soundtrack_source"===(null==source?void 0:source.type)||"hearaside_source"===(null==source?void 0:source.type))"#,
    ),
    (
        r#"!(null==source?void 0:source.video)&&(null==source?void 0:source.hasProps())?AudioService.actions.showAdvancedSettings(source.sourceId)"#,
        r#"!(null==source?void 0:source.video)&&"hearaside_source"!==(null==source?void 0:source.type)&&(null==source?void 0:source.hasProps())?AudioService.actions.showAdvancedSettings(source.sourceId)"#,
    ),
    (
        r#""scene"!==item.type?item.video?"#,
        r#""scene"!==item.type?item.video||"hearaside_source"===item.type?"#,
    ),
    (
        r#"showProperties(){this.options.showAudioMixerMenu||!this.source.video?this.audioService.actions.showAdvancedSettings(this.source.sourceId)"#,
        r#"showProperties(){this.options.showAudioMixerMenu||!this.source.video&&"hearaside_source"!==this.source.type?this.audioService.actions.showAdvancedSettings(this.source.sourceId)"#,
    ),
];

fn fail(code: i32, message: impl Into<String>) -> Failure {
    Failure {
        code,
        message: message.into(),
    }
}

fn find(hay: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    hay.get(from..)?
        .windows(needle.len())
        .position(|w| w == needle)
        .map(|p| p + from)
}

/// Every anchor once, in order, each searched in the text the earlier edits left. Bytes, not text: a bundle that is
/// not clean UTF-8 comes out byte for byte (the C++ tool round-tripped it through juce::String).
pub fn patched_text(mut text: Vec<u8>) -> Result<Vec<u8>, Failure> {
    for (anchor, replacement) in EDITS {
        let a = anchor.as_bytes();
        let Some(first) = find(&text, a, 0).filter(|&f| find(&text, a, f + 1).is_none()) else {
            return Err(fail(
                3,
                "this Streamlabs version is not supported (its UI code changed)",
            ));
        };
        text.splice(first..first + a.len(), replacement.bytes());
    }
    Ok(text)
}

/// `{algorithm, hash, blockSize, blocks}`: asar's integrity record of a file
fn integrity(data: &[u8]) -> Json {
    const BLOCK: usize = 4 * 1024 * 1024;
    let blocks: Vec<Json> = if data.is_empty() {
        vec![Json::Str(sha256::hex(&[]))]
    } else {
        data.chunks(BLOCK)
            .map(|c| Json::Str(sha256::hex(c)))
            .collect()
    };
    Json::Obj(vec![
        ("algorithm".into(), Json::Str("SHA256".into())),
        ("hash".into(), Json::Str(sha256::hex(data))),
        ("blockSize".into(), Json::Int(BLOCK as i64)),
        ("blocks".into(), Json::Arr(blocks)),
    ])
}

/// Where a file entry sits in the header: the keys to follow from the root (`files`/<key>/`files`/<key> ...).
type Chain = Vec<String>;

fn collect_files(node: &Json, chain: &mut Chain, prefix: &str, out: &mut Vec<(String, Chain)>) {
    let Some(Json::Obj(files)) = node.get("files") else {
        return;
    };
    for (name, entry) in files {
        if !matches!(entry, Json::Obj(_)) {
            continue;
        }
        chain.push(name.clone());
        if entry.get("files").is_some() {
            collect_files(entry, chain, &format!("{prefix}{name}/"), out);
        } else if entry.get("offset").is_some() && !entry.get("unpacked").is_some_and(Json::truthy)
        {
            out.push((format!("{prefix}{name}"), chain.clone()));
        }
        chain.pop();
    }
}

fn entry_mut<'a>(root: &'a mut Json, chain: &[String]) -> &'a mut Json {
    let mut cur = root;
    for key in chain {
        cur = cur
            .get_mut("files")
            .and_then(|f| f.get_mut(key))
            .expect("the chain was collected from this header");
    }
    cur
}

/// An app.asar's header and its renderer bundle
pub struct Asar {
    pub header: Json,
    /// where the file data starts, and the bundle's offset / size in it
    pub base: i64,
    pub off: i64,
    pub size: i64,
    files: Vec<(String, Chain)>,
    bundle: Chain,
    pub text: Vec<u8>,
}

impl Asar {
    pub fn read(file: &mut File) -> Result<Asar, Failure> {
        let mut head = [0u8; 16];
        file.read_exact(&mut head)
            .map_err(|_| fail(5, "app.asar is too short"))?;
        let word = |i: usize| {
            u32::from_le_bytes([
                head[i * 4],
                head[i * 4 + 1],
                head[i * 4 + 2],
                head[i * 4 + 3],
            ])
        };
        let mut raw = Vec::new();
        // a short read is not an error here (JUCE read what there was); it only makes the JSON fail to parse
        Read::by_ref(file)
            .take(u64::from(word(3)))
            .read_to_end(&mut raw)
            .map_err(|_| fail(5, "app.asar is too short"))?;
        let raw = raw.split(|&b| b == 0).next().unwrap_or_default();
        let raw = raw.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(raw);
        let header = json::parse(&String::from_utf8_lossy(raw));
        let base = 8 + i64::from(word(1));

        let mut files = Vec::new();
        collect_files(&header, &mut Vec::new(), "", &mut files);
        let mut bundle = None;
        for (name, chain) in &files {
            if name.starts_with("bundles/renderer.") && name.ends_with(".js") {
                if bundle.is_some() {
                    return Err(fail(3, "more than one renderer bundle in app.asar"));
                }
                bundle = Some(chain.clone());
            }
        }
        let bundle = bundle.ok_or_else(|| fail(3, "no renderer bundle in app.asar"))?;

        let mut me = Asar {
            header,
            base,
            off: 0,
            size: 0,
            files,
            bundle,
            text: Vec::new(),
        };
        let entry = entry_mut(&mut me.header, &me.bundle);
        me.off = entry
            .get("offset")
            .map_or(0, |o| json::large_int_value(&o.as_text()));
        me.size = entry.get("size").map_or(0, Json::as_i64);
        // JUCE: a negative size means "to the end of the file"
        let _ = file.seek(SeekFrom::Start(u64::try_from(base + me.off).unwrap_or(0)));
        let mut text = Vec::new();
        let _ = match u64::try_from(me.size) {
            Ok(n) => Read::by_ref(file).take(n).read_to_end(&mut text),
            Err(_) => file.read_to_end(&mut text),
        };
        me.text = text;
        Ok(me)
    }

    pub fn patched(&self) -> bool {
        find(&self.text, SOURCE_ID.as_bytes(), 0).is_some()
    }
}

pub fn is_patched(asar: &Path) -> Result<bool, Failure> {
    let mut f = File::open(asar).map_err(|_| fail(5, format!("cannot read {}", asar.display())))?;
    Ok(Asar::read(&mut f)?.patched())
}

/// src (an original app.asar) -> dst with the renderer bundle patched; every other file byte for byte
pub fn patch_asar(src: &Path, dst: &Path) -> Result<(), Failure> {
    let mut input =
        File::open(src).map_err(|_| fail(5, format!("cannot read {}", src.display())))?;
    let mut asar = Asar::read(&mut input)?;
    if asar.patched() {
        return Err(fail(
            5,
            format!("{} is not an original app.asar", src.display()),
        ));
    }
    let patched = patched_text(std::mem::take(&mut asar.text))?;
    let (base, off, size) = (asar.base, asar.off, asar.size);
    let delta = patched.len() as i64 - size;

    let bundle = entry_mut(&mut asar.header, &asar.bundle);
    bundle.set("size", Json::Int(patched.len() as i64));
    if bundle.get("integrity").is_some() {
        bundle.set("integrity", integrity(&patched));
    }
    for (_, chain) in &asar.files {
        let entry = entry_mut(&mut asar.header, chain);
        let o = entry
            .get("offset")
            .map_or(0, |o| json::large_int_value(&o.as_text()));
        if o > off {
            entry.set("offset", Json::Str((o + delta).to_string()));
        }
    }

    let js = asar.header.to_text();
    let js_len = js.len() as u32;
    let pad = (4 - js_len % 4) % 4;
    let payload = 4 + js_len + pad;
    let _ = std::fs::remove_file(dst);
    let file = File::create(dst).map_err(|_| fail(5, format!("cannot write {}", dst.display())))?;
    let mut out = BufWriter::with_capacity(1 << 20, file);
    for word in [4, payload + 4, payload, js_len] {
        out.write_all(&word.to_le_bytes()).map_err(write_failed)?;
    }
    out.write_all(js.as_bytes()).map_err(write_failed)?;
    out.write_all(&[0u8; 3][..pad as usize])
        .map_err(write_failed)?;

    let src_len = input.metadata().map_or(0, |m| m.len() as i64);
    copy(&mut input, &mut out, base, off)?; // files before the bundle
    out.write_all(&patched).map_err(write_failed)?;
    copy(
        &mut input,
        &mut out,
        base + off + size,
        src_len - (base + off + size),
    )?; // files after it, unchanged
    out.flush().map_err(write_failed)
}

fn write_failed(e: io::Error) -> Failure {
    fail(5, format!("writing failed: {e}"))
}

fn copy(input: &mut File, out: &mut impl Write, from: i64, count: i64) -> Result<(), Failure> {
    if count <= 0 {
        return Ok(());
    }
    let ended = || fail(5, "app.asar ended early");
    input
        .seek(SeekFrom::Start(u64::try_from(from).unwrap_or(0)))
        .map_err(|_| ended())?;
    let n = io::copy(&mut Read::by_ref(input).take(count as u64), out).map_err(write_failed)?;
    if n < count as u64 {
        Err(ended())
    } else {
        Ok(())
    }
}
