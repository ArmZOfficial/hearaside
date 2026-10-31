//! The Rust tool and the C++ hearaside-streamlabs.exe, step by step on identical fake Streamlabs folders: same exit code,
//! same output, same files afterwards (every byte of app.asar, the backup, the plug-in, the markers).
//!
//! Runs when SS_STREAMLABS_CPP points at the C++ exe (cmake target `hearaside-streamlabs`, e.g.
//! build\rust-core-full\obs\streamlabs-setup\hearaside-streamlabs_artefacts\Release\hearaside-streamlabs.exe); otherwise it
//! prints "skipped". The C++ tool asks Windows whether Streamlabs is running, so it is skipped too while a real
//! "Streamlabs OBS.exe" runs.

mod common;

use common::*;
use hs_streamlabs_setup::{Outcome, run, streamlabs_running};
use std::fs;
use std::path::Path;
use std::process::Command;

enum Step {
    Run(&'static [&'static str]),
    Do(fn(&Tree)),
}
use Step::{Do, Run};

fn args(t: &Tree, a: &[&str]) -> Vec<String> {
    a.iter()
        .map(|a| match *a {
            "ROOT" => t.root.display().to_string(),
            "DLL" => t.dll.display().to_string(),
            "NODLL" => t.tmp.0.join("nothing.dll").display().to_string(),
            "REL" => "relative-folder".into(),
            other => other.to_string(),
        })
        .collect()
}

fn rust(t: &Tree, a: &[&str]) -> Outcome {
    run(&args(t, a), &|| false)
}

fn cpp(exe: &Path, t: &Tree, a: &[&str]) -> Outcome {
    let o = Command::new(exe)
        .args(args(t, a))
        .output()
        .expect("start the C++ tool");
    Outcome {
        code: o.status.code().unwrap(),
        stdout: String::from_utf8_lossy(&o.stdout).replace("\r\n", "\n"),
        stderr: String::from_utf8_lossy(&o.stderr).replace("\r\n", "\n"),
    }
}

/// the folder names differ between the two trees
fn neutral(t: &Tree, s: &str) -> String {
    s.replace(&t.root.display().to_string(), "<ROOT>")
        .replace(&t.tmp.0.display().to_string(), "<TMP>")
}

/// returns the exit codes of the Run steps (both tools gave these)
fn check(exe: &Path, name: &str, asar: Option<&[u8]>, steps: &[Step]) -> Vec<i32> {
    let mut codes = Vec::new();
    let (r, c) = (Tree::new(asar), Tree::new(asar));
    for (n, step) in steps.iter().enumerate() {
        let at = format!("{name}, step {n}");
        match step {
            Do(f) => {
                f(&r);
                f(&c);
            }
            Run(a) => {
                let (ro, co) = (rust(&r, a), cpp(exe, &c, a));
                codes.push(co.code);
                assert_eq!(
                    ro.code, co.code,
                    "{at}: exit code of {a:?}: rust {ro:?}, C++ {co:?}"
                );
                assert_eq!(ro.stdout, co.stdout, "{at}: stdout of {a:?}");
                assert_eq!(
                    neutral(&r, &ro.stderr),
                    neutral(&c, &co.stderr),
                    "{at}: stderr of {a:?}"
                );
            }
        }
        let (rs, cs) = (r.snapshot(), c.snapshot());
        for k in rs.keys().chain(cs.keys()) {
            assert_eq!(
                rs.get(k),
                cs.get(k),
                "{at}: {k} differs (rust left, C++ right)"
            );
        }
    }
    codes
}

fn update(t: &Tree) {
    fs::write(t.res("app.asar"), asar(&bundle("v2"), true)).unwrap();
}
fn delete_backup(t: &Tree) {
    fs::remove_file(t.res("app.asar.before-hearaside")).unwrap();
}
fn remove_plugin(t: &Tree) {
    fs::remove_file(t.plugin()).unwrap();
}
fn old_plugin(t: &Tree) {
    fs::write(t.plugin(), b"an older plug-in").unwrap();
}
fn remove_plugin_dir(t: &Tree) {
    fs::remove_dir(t.plugin().parent().unwrap()).unwrap();
}
fn patched_backup(t: &Tree) {
    fs::copy(t.res("app.asar"), t.res("app.asar.before-hearaside")).unwrap();
}
fn remove_marker(t: &Tree) {
    fs::remove_file(t.root.join("force-local-bundles")).unwrap();
}

/// whitespace, escapes, single quotes, ints and doubles, a number offset, an unpacked "yes", entries that are not objects,
/// top-level extras and trailing text: the header the C++ tool reads and writes back
fn odd_header() -> Vec<u8> {
    let b = bundle("odd");
    let n = b.len();
    let h = format!(
        "{{ \"files\" : {{\n  \"first.txt\": {{\"size\": 4, \"offset\": \"0\", \"executable\": true}},\n  \"bundles\": {{\"files\": {{\"renderer.x.js\": {{\"size\": {n}, \"offset\": \"4\", \"integrity\": {{\"algorithm\":\"SHA256\",\"hash\":\"00\",\"blockSize\":1,\"blocks\":[]}}}}}}}},\n  \"last.bin\": {{\"size\": 3, \"offset\": \"{o3}\"}},\n  \"as-int.bin\": {{\"size\": 2, \"offset\": {o4}}},\n  \"u.node\": {{\"size\": 9, \"unpacked\": \"yes\", \"offset\": \"{o3}\"}},\n  \"esc\\u00e9\\/x\\t\": {{\"size\": 0, \"offset\": \"1\"}},\n  \"not-an-entry\": 1.5, \"big\": {{\"size\": 5000000000, \"offset\": \"{o5}\"}},\n  \"arr\": [1, 2.0e3, -7, 'single', null] }}, \"extra\": {{\"k\": [true, false, null], \"\\u00e9\": \"\\ud83d\\ude00\"}}, \"n\": 12345678901 }} trailing garbage",
        o3 = 4 + n,
        o4 = 4 + n + 3,
        o5 = 4 + n + 5,
    );
    with_header(
        &h,
        &[b"abcd".to_vec(), b, b"xyz".to_vec(), b"12".to_vec()].concat(),
    )
}

#[test]
fn rust_matches_the_cpp_tool() {
    let Some(exe) = std::env::var_os("SS_STREAMLABS_CPP") else {
        println!("SS_STREAMLABS_CPP not set: skipped");
        return;
    };
    let exe = Path::new(&exe);
    assert!(
        exe.is_file(),
        "SS_STREAMLABS_CPP does not point at a file: {exe:?}"
    );
    if streamlabs_running() {
        println!("Streamlabs Desktop is running: skipped");
        return;
    }
    let orig = asar(&bundle("v1"), true);
    let orig_plain = asar(&bundle("v1"), false);

    // (the expected codes make sure the scenarios are not "both tools fail the same way" by accident)
    let codes = check(
        exe,
        "fresh, idempotent, uninstall",
        Some(&orig),
        &[
            Run(&["check", "ROOT"]),
            Run(&["install", "ROOT", "DLL"]),
            Run(&["repair", "ROOT", "DLL"]),
            Run(&["install", "ROOT", "DLL"]),
            Run(&["check", "ROOT"]),
            Run(&["uninstall", "ROOT"]),
            Run(&["uninstall", "ROOT"]),
            Run(&["repair", "ROOT", "DLL"]),
        ],
    );
    assert_eq!(codes, [0; 8]);
    let codes = check(
        exe,
        "no integrity record",
        Some(&orig_plain),
        &[
            Run(&["install", "ROOT", "DLL"]),
            Run(&["uninstall", "ROOT"]),
        ],
    );
    assert_eq!(codes, [0; 2]);
    let codes = check(
        exe,
        "odd header",
        Some(&odd_header()),
        &[
            Run(&["check", "ROOT"]),
            Run(&["install", "ROOT", "DLL"]),
            Run(&["repair", "ROOT", "DLL"]),
            Run(&["uninstall", "ROOT"]),
        ],
    );
    assert_eq!(codes, [0; 4]);
    let codes = check(
        exe,
        "streamlabs update then repair",
        Some(&orig),
        &[
            Run(&["install", "ROOT", "DLL"]),
            Do(update),
            Run(&["check", "ROOT"]),
            Run(&["repair", "ROOT", "DLL"]),
            Run(&["repair", "ROOT", "DLL"]),
            Run(&["uninstall", "ROOT"]),
        ],
    );
    assert_eq!(codes, [0; 5]);
    let codes = check(
        exe,
        "streamlabs update then uninstall",
        Some(&orig),
        &[
            Run(&["install", "ROOT", "DLL"]),
            Do(update),
            Run(&["uninstall", "ROOT"]),
        ],
    );
    assert_eq!(codes, [0; 2]);
    let codes = check(
        exe,
        "repair puts back what is gone",
        Some(&orig),
        &[
            Run(&["install", "ROOT", "DLL"]),
            Do(remove_plugin),
            Run(&["repair", "ROOT", "DLL"]),
            Do(old_plugin),
            Run(&["repair", "ROOT", "DLL"]),
            Do(remove_marker),
            Run(&["repair", "ROOT", "DLL"]),
        ],
    );
    assert_eq!(codes, [0; 4]);
    let codes = check(
        exe,
        "streamlabs missing",
        None,
        &[
            Run(&["check", "ROOT"]),
            Run(&["install", "ROOT", "DLL"]),
            Run(&["repair", "ROOT", "DLL"]),
            Run(&["uninstall", "ROOT"]),
            Run(&["bogus", "ROOT"]),
        ],
    );
    assert_eq!(codes, [4; 5]);
    let codes = check(
        exe,
        "usage",
        Some(&orig),
        &[
            Run(&[]),
            Run(&["check"]),
            Run(&["install"]),
            Run(&["install", "ROOT"]),
            Run(&["repair", "ROOT"]),
            Run(&["bogus", "ROOT"]),
            Run(&["bogus", "ROOT", "DLL"]),
        ],
    );
    assert_eq!(codes, [1; 7]);
    let codes = check(
        exe,
        "plug-in missing",
        Some(&orig),
        &[
            Run(&["install", "ROOT", "NODLL"]),
            Run(&["repair", "ROOT", "NODLL"]),
        ],
    );
    assert_eq!(codes, [4; 2]);
    let codes = check(
        exe,
        "plug-in folder missing",
        Some(&orig),
        &[Do(remove_plugin_dir), Run(&["install", "ROOT", "DLL"])],
    );
    assert_eq!(codes, [5]);
    let codes = check(
        exe,
        "backup missing / backup patched",
        Some(&orig),
        &[
            Run(&["install", "ROOT", "DLL"]),
            Do(delete_backup),
            Run(&["install", "ROOT", "DLL"]),
            Run(&["repair", "ROOT", "DLL"]),
            Do(patched_backup),
            Run(&["install", "ROOT", "DLL"]),
            Run(&["uninstall", "ROOT"]),
        ],
    );
    assert_eq!(codes, [0, 5, 0, 5, 0]);

    // unsupported UI code: an anchor gone, an anchor twice
    let b = String::from_utf8(bundle("v1")).unwrap();
    let gone = asar(
        b.replacen(hs_streamlabs_setup::asar::EDITS[3].0, "x", 1)
            .as_bytes(),
        true,
    );
    let twice = asar(
        (b.clone() + hs_streamlabs_setup::asar::EDITS[0].0).as_bytes(),
        true,
    );
    for (name, a) in [("anchor gone", gone), ("anchor twice", twice)] {
        let codes = check(
            exe,
            name,
            Some(&a),
            &[
                Run(&["check", "ROOT"]),
                Run(&["install", "ROOT", "DLL"]),
                Run(&["uninstall", "ROOT"]),
            ],
        );
        assert_eq!(codes, [3, 3, 0], "{name}");
    }

    // malformed app.asar
    let json = |h: &str| with_header(h, b"data");
    let bad: Vec<(&str, Vec<u8>)> = vec![
        ("not JSON", json("this is not json at all")),
        (
            "truncated JSON",
            json(r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":4,"off"#),
        ),
        ("array", json("[1,2,3]")),
        ("empty object", json("{}")),
        ("empty header", json("")),
        (
            "renderer unpacked",
            json(r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":4,"unpacked":true}}}}}"#),
        ),
        (
            "two renderers",
            json(
                r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":4,"offset":"0"},"renderer.b.js":{"size":4,"offset":"4"}}}}}"#,
            ),
        ),
        (
            "data shorter than the header says",
            with_header(
                r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":400,"offset":"0"}}}}}"#,
                b"short",
            ),
        ),
        ("too short", vec![4, 0, 0, 0, 1]),
        ("empty file", vec![]),
        ("header longer than the file", {
            let mut v = json("{}");
            v[12..16].copy_from_slice(&1000u32.to_le_bytes());
            v
        }),
    ];
    for (name, a) in &bad {
        let codes = check(
            exe,
            name,
            Some(a),
            &[
                Run(&["check", "ROOT"]),
                Run(&["install", "ROOT", "DLL"]),
                Run(&["uninstall", "ROOT"]),
            ],
        );
        assert!(codes[0] == 3 || codes[0] == 5, "{name}: {codes:?}");
        assert_eq!(codes[0], codes[1], "{name}");
    }
}
