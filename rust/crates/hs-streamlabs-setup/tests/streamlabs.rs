//! hearaside-streamlabs against a fake Streamlabs folder (see common/mod.rs). Streamlabs "running" is faked in these
//! tests, so a real Streamlabs on the machine does not matter.

mod common;

use common::*;
use hs_streamlabs_setup::asar::{EDITS, SOURCE_ID};
use hs_streamlabs_setup::json::Json;
use hs_streamlabs_setup::{Outcome, run, sha256};
use std::fs;

fn go(t: &Tree, args: &[&str]) -> Outcome {
    go_with(t, args, false)
}

fn go_with(t: &Tree, args: &[&str], running: bool) -> Outcome {
    let args: Vec<String> = args
        .iter()
        .map(|a| match *a {
            "ROOT" => t.root.display().to_string(),
            "DLL" => t.dll.display().to_string(),
            other => other.to_string(),
        })
        .collect();
    run(&args, &|| running)
}

fn original() -> Vec<u8> {
    asar(&bundle("v1"), true)
}

#[test]
fn fresh_install_patches_the_bundle_and_nothing_else() {
    let orig = original();
    let t = Tree::new(Some(&orig));
    let o = go(&t, &["install", "ROOT", "DLL"]);
    assert_eq!(
        (o.code, o.stdout.as_str(), o.stderr.as_str()),
        (0, "HEARASIDE source installed in Streamlabs Desktop\n", "")
    );

    // the original is kept; the plug-in is copied; the marker and the state file exist
    assert_eq!(fs::read(t.res("app.asar.before-hearaside")).unwrap(), orig);
    assert_eq!(fs::read(t.plugin()).unwrap(), fs::read(&t.dll).unwrap());
    assert!(t.root.join("force-local-bundles").is_file());
    let meta = fs::metadata(t.res("app.asar")).unwrap();
    let ms = meta
        .modified()
        .unwrap()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_millis();
    assert_eq!(
        fs::read_to_string(t.res("app.asar.hearaside-state")).unwrap(),
        format!("{} {ms}", meta.len())
    );
    assert!(!t.res("app.asar.hearaside-tmp").exists());

    // the bundle got the seven edits, with its size and integrity updated; every other file is byte for byte
    let (header, files) = unpack(&t.asar());
    let (old_header, old_files) = unpack(&orig);
    let name = "bundles/renderer.abc123.js";
    let mut expected = bundle("v1");
    for (a, r) in EDITS {
        let at = expected
            .windows(a.len())
            .position(|w| w == a.as_bytes())
            .unwrap();
        expected.splice(at..at + a.len(), r.bytes());
    }
    assert_eq!(files[name], expected);
    assert!(
        String::from_utf8(files[name].clone())
            .unwrap()
            .contains(SOURCE_ID)
    );
    for (k, v) in &old_files {
        if k != name {
            assert_eq!(&files[k], v, "{k}");
        }
    }
    let entry = header
        .get("files")
        .unwrap()
        .get("bundles")
        .unwrap()
        .get("files")
        .unwrap()
        .get("renderer.abc123.js")
        .unwrap();
    assert_eq!(entry.get("size"), Some(&Json::Int(expected.len() as i64)));
    let integ = entry.get("integrity").unwrap();
    assert_eq!(integ.get("hash"), Some(&Json::Str(sha256::hex(&expected))));
    assert_eq!(
        integ.get("blocks"),
        Some(&Json::Arr(vec![Json::Str(sha256::hex(&expected))]))
    );
    assert_eq!(integ.get("algorithm"), Some(&Json::Str("SHA256".into())));
    assert_eq!(integ.get("blockSize"), Some(&Json::Int(4 * 1024 * 1024)));
    // the packed file after the bundle moved; the unpacked one has no offset and is untouched
    let moved = |h: &Json, f: &str| {
        h.get("files")
            .unwrap()
            .get(f)
            .unwrap()
            .get("offset")
            .unwrap()
            .as_text()
            .parse::<i64>()
            .unwrap()
    };
    assert_eq!(
        moved(&header, "last.bin") - moved(&old_header, "last.bin"),
        expected.len() as i64 - bundle("v1").len() as i64
    );
    assert_eq!(
        header.get("files").unwrap().get("native.node"),
        old_header.get("files").unwrap().get("native.node")
    );
    // key order and the one-line JUCE format survive
    let text = std::str::from_utf8(
        &t.asar()[16..16 + u32::from_le_bytes(t.asar()[12..16].try_into().unwrap()) as usize],
    )
    .unwrap()
    .to_string();
    assert!(text.starts_with(r#"{"files": {"first.txt": {"size": 16, "offset": "0"}, "bundles": {"files": {"renderer.abc123.js": {"size": "#), "{text}");
    assert!(text.contains(r#""ünï.txt": {"size": 0, "offset": ""#));
}

#[test]
fn bundle_without_integrity_gets_none() {
    let t = Tree::new(Some(&asar(&bundle("v1"), false)));
    assert_eq!(go(&t, &["install", "ROOT", "DLL"]).code, 0);
    let (header, _) = unpack(&t.asar());
    let entry = header
        .get("files")
        .unwrap()
        .get("bundles")
        .unwrap()
        .get("files")
        .unwrap()
        .get("renderer.abc123.js")
        .unwrap();
    assert!(entry.get("integrity").is_none());
}

#[test]
fn repair_does_nothing_when_installed_and_install_again_gives_the_same_asar() {
    let t = Tree::new(Some(&original()));
    assert_eq!(go(&t, &["install", "ROOT", "DLL"]).code, 0);
    let (snap, patched) = (t.snapshot(), t.asar());
    let stamp = fs::metadata(t.res("app.asar")).unwrap().modified().unwrap();

    let o = go(&t, &["repair", "ROOT", "DLL"]);
    assert_eq!((o.code, o.stdout.as_str(), o.stderr.as_str()), (0, "", ""));
    assert_eq!(
        fs::metadata(t.res("app.asar")).unwrap().modified().unwrap(),
        stamp,
        "repair left app.asar alone"
    );
    assert_eq!(t.snapshot(), snap);

    // install again: made from the kept original, so the same bytes
    let o = go(&t, &["install", "ROOT", "DLL"]);
    assert_eq!(o.code, 0);
    assert_eq!(t.asar(), patched);
    assert_eq!(t.snapshot(), snap);
}

#[test]
fn repair_redoes_what_a_change_undid() {
    let t = Tree::new(Some(&original()));
    go(&t, &["install", "ROOT", "DLL"]);
    let snap = t.snapshot();
    for undo in [
        |t: &Tree| fs::remove_file(t.plugin()).unwrap(),
        |t: &Tree| fs::remove_file(t.root.join("force-local-bundles")).unwrap(),
        |t: &Tree| fs::remove_file(t.res("app.asar.hearaside-state")).unwrap(),
        |t: &Tree| fs::write(t.plugin(), b"an older plug-in").unwrap(),
    ] {
        undo(&t);
        assert_ne!(t.snapshot(), snap);
        assert_eq!(go(&t, &["repair", "ROOT", "DLL"]).code, 0);
        assert_eq!(t.snapshot(), snap);
    }
}

#[test]
fn repair_after_a_streamlabs_update_patches_the_new_original() {
    let t = Tree::new(Some(&original()));
    go(&t, &["install", "ROOT", "DLL"]);
    let newer = asar(&bundle("v2"), true);
    fs::write(t.res("app.asar"), &newer).unwrap(); // an update puts a new, unpatched app.asar there

    let o = go(&t, &["check", "ROOT"]);
    assert_eq!(o.code, 0);
    assert_eq!(go(&t, &["repair", "ROOT", "DLL"]).code, 0);
    assert_eq!(
        fs::read(t.res("app.asar.before-hearaside")).unwrap(),
        newer,
        "the new original is the backup"
    );
    let (_, files) = unpack(&t.asar());
    let text = String::from_utf8(files["bundles/renderer.abc123.js"].clone()).unwrap();
    assert!(text.contains("renderer v2") && text.contains(SOURCE_ID));
}

#[test]
fn streamlabs_missing_is_4_for_every_command() {
    // no folder, no app.asar, no obs.dll
    let none = Tree::new(None);
    for cmd in [
        vec!["check", "ROOT"],
        vec!["install", "ROOT", "DLL"],
        vec!["repair", "ROOT", "DLL"],
        vec!["uninstall", "ROOT"],
        vec!["bogus", "ROOT"],
    ] {
        let o = go(&none, &cmd);
        assert_eq!(o.code, 4, "{cmd:?}");
        assert_eq!(
            (o.stdout.as_str(), o.stderr.clone()),
            (
                "",
                format!("Streamlabs Desktop not found in {}\n", none.root.display())
            )
        );
    }
    let no_dll = Tree::new(Some(&original()));
    fs::remove_file(no_dll.res("app.asar.unpacked/node_modules/obs-studio-node/obs.dll")).unwrap();
    assert_eq!(go(&no_dll, &["check", "ROOT"]).code, 4);
    // even running Streamlabs does not come before "not found"
    assert_eq!(go_with(&none, &["uninstall", "ROOT"], true).code, 4);

    // the plug-in to install is missing
    let t = Tree::new(Some(&original()));
    fs::remove_file(&t.dll).unwrap();
    let o = go(&t, &["install", "ROOT", "DLL"]);
    assert_eq!(
        (o.code, o.stderr),
        (4, format!("plug-in not found: {}\n", t.dll.display()))
    );
    assert!(
        !t.res("app.asar.before-hearaside").exists(),
        "nothing was touched"
    );
}

#[test]
fn running_streamlabs_is_2_and_changes_nothing() {
    let t = Tree::new(Some(&original()));
    let snap = t.snapshot();
    for cmd in [
        vec!["install", "ROOT", "DLL"],
        vec!["repair", "ROOT", "DLL"],
        vec!["uninstall", "ROOT"],
    ] {
        let o = go_with(&t, &cmd, true);
        assert_eq!(
            (o.code, o.stderr.as_str()),
            (2, "close Streamlabs Desktop first\n"),
            "{cmd:?}"
        );
        assert_eq!(t.snapshot(), snap);
    }
    // check says "yes, but it is running"
    let o = go_with(&t, &["check", "ROOT"], true);
    assert_eq!((o.code, o.stderr.as_str()), (2, ""));
}

#[test]
fn check_is_0_for_original_and_patched_and_3_for_an_unknown_ui() {
    let t = Tree::new(Some(&original()));
    assert_eq!(go(&t, &["check", "ROOT"]).code, 0);
    go(&t, &["install", "ROOT", "DLL"]);
    assert_eq!(go(&t, &["check", "ROOT"]).code, 0);

    // each anchor missing once, and doubled, is "not supported"
    for (i, (anchor, _)) in EDITS.iter().enumerate() {
        let mut b = String::from_utf8(bundle("v1")).unwrap();
        b = b.replacen(anchor, "gone", 1);
        let missing = Tree::new(Some(&asar(b.as_bytes(), true)));
        let o = go(&missing, &["check", "ROOT"]);
        assert_eq!(
            (o.code, o.stderr.as_str()),
            (
                3,
                "this Streamlabs version is not supported (its UI code changed)\n"
            ),
            "missing {i}"
        );
        let b = String::from_utf8(bundle("v1")).unwrap() + anchor;
        let twice = Tree::new(Some(&asar(b.as_bytes(), true)));
        assert_eq!(go(&twice, &["check", "ROOT"]).code, 3, "doubled {i}");
        let o = go(&twice, &["install", "ROOT", "DLL"]);
        assert_eq!(o.code, 3);
        assert!(
            !twice.res("app.asar.before-hearaside").exists()
                && !twice.res("app.asar.hearaside-tmp").exists()
        );
    }
}

#[test]
fn uninstall_puts_everything_back() {
    let orig = original();
    let t = Tree::new(Some(&orig));
    let before = t.snapshot();
    go(&t, &["install", "ROOT", "DLL"]);
    let o = go(&t, &["uninstall", "ROOT"]);
    assert_eq!(
        (o.code, o.stdout.as_str(), o.stderr.as_str()),
        (0, "Streamlabs Desktop restored\n", "")
    );
    assert_eq!(t.asar(), orig);
    assert_eq!(t.snapshot(), before);
    // and again: nothing left to do, still fine
    assert_eq!(go(&t, &["uninstall", "ROOT"]).code, 0);
    assert_eq!(t.snapshot(), before);
}

#[test]
fn uninstall_after_an_update_drops_the_older_backup_and_keeps_the_new_app_asar() {
    let t = Tree::new(Some(&original()));
    go(&t, &["install", "ROOT", "DLL"]);
    let newer = asar(&bundle("v2"), true);
    fs::write(t.res("app.asar"), &newer).unwrap();
    assert_eq!(go(&t, &["uninstall", "ROOT"]).code, 0);
    assert_eq!(t.asar(), newer);
    assert!(!t.res("app.asar.before-hearaside").exists() && !t.plugin().exists());
    assert!(
        !t.root.join("force-local-bundles").exists() && !t.res("app.asar.hearaside-state").exists()
    );
}

#[test]
fn malformed_headers() {
    let json = |h: &str| with_header(h, b"data");
    let cases: Vec<(&str, Vec<u8>, i32, &str)> = vec![
        (
            "not JSON",
            json("this is not json at all"),
            3,
            "no renderer bundle in app.asar",
        ),
        (
            "truncated",
            json(r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":4,"off"#),
            3,
            "no renderer bundle in app.asar",
        ),
        (
            "array",
            json("[1,2,3]"),
            3,
            "no renderer bundle in app.asar",
        ),
        (
            "empty object",
            json("{}"),
            3,
            "no renderer bundle in app.asar",
        ),
        (
            "no renderer",
            json(r#"{"files":{"main.js":{"size":4,"offset":"0"}}}"#),
            3,
            "no renderer bundle in app.asar",
        ),
        (
            "unpacked renderer",
            json(r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":4,"unpacked":true}}}}}"#),
            3,
            "no renderer bundle in app.asar",
        ),
        ("short file", vec![4, 0, 0], 5, "app.asar is too short"),
        ("empty file", vec![], 5, "app.asar is too short"),
        (
            "short data",
            with_header(
                r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":400,"offset":"0"}}}}}"#,
                b"short",
            ),
            3,
            "this Streamlabs version is not supported (its UI code changed)",
        ),
        (
            "two renderers",
            with_header(
                r#"{"files":{"bundles":{"files":{"renderer.a.js":{"size":4,"offset":"0"},"renderer.b.js":{"size":4,"offset":"4"}}}}}"#,
                b"datadata",
            ),
            3,
            "more than one renderer bundle in app.asar",
        ),
    ];
    for (name, bytes, code, message) in cases {
        let t = Tree::new(Some(&bytes));
        for cmd in [vec!["check", "ROOT"], vec!["install", "ROOT", "DLL"]] {
            let o = go(&t, &cmd);
            assert_eq!(
                (o.code, o.stderr.trim_end()),
                (code, message),
                "{name} {cmd:?}"
            );
            assert!(
                !t.res("app.asar.hearaside-tmp").exists(),
                "{name}: the temporary file is removed"
            );
            if code != 0 {
                assert_eq!(t.asar(), bytes, "{name}: app.asar untouched");
                assert!(!t.res("app.asar.before-hearaside").exists(), "{name}");
            }
        }
    }
}

#[test]
fn patched_app_asar_without_its_original() {
    let t = Tree::new(Some(&original()));
    go(&t, &["install", "ROOT", "DLL"]);
    fs::remove_file(t.res("app.asar.before-hearaside")).unwrap();
    let o = go(&t, &["install", "ROOT", "DLL"]);
    assert_eq!(
        (o.code, o.stderr.as_str()),
        (5, "app.asar is patched but its original is missing\n")
    );
    // a backup that is itself patched is refused
    fs::copy(t.res("app.asar"), t.res("app.asar.before-hearaside")).unwrap();
    let o = go(&t, &["install", "ROOT", "DLL"]);
    assert_eq!(
        (o.code, o.stderr),
        (
            5,
            format!(
                "{} is not an original app.asar\n",
                t.res("app.asar.before-hearaside").display()
            )
        )
    );
    assert!(!t.res("app.asar.hearaside-tmp").exists());
}

#[test]
fn plugin_folder_missing_is_5() {
    let t = Tree::new(Some(&original()));
    fs::remove_dir(t.plugin().parent().unwrap()).unwrap();
    let o = go(&t, &["install", "ROOT", "DLL"]);
    assert_eq!(
        (o.code, o.stderr),
        (
            5,
            format!("cannot copy the plug-in to {}\n", t.plugin().display())
        )
    );
}

#[test]
fn usage_and_unknown_commands_are_1() {
    let t = Tree::new(Some(&original()));
    let usage = "usage: hearaside-streamlabs check|install|repair|uninstall <Streamlabs folder> [hearaside-obs.dll]\n";
    assert_eq!(
        (
            run(&[], &|| false).code,
            run(&[], &|| false).stderr.as_str()
        ),
        (1, usage)
    );
    assert_eq!(go(&t, &["check"]).stderr, usage);
    assert_eq!(go(&t, &["install"]).code, 1);
    for cmd in [
        vec!["bogus", "ROOT"],
        vec!["install", "ROOT"],
        vec!["repair", "ROOT"],
    ] {
        let o = go(&t, &cmd);
        assert_eq!(
            (o.code, o.stderr.as_str()),
            (1, "unknown command\n"),
            "{cmd:?}"
        );
    }
    assert!(!t.res("app.asar.before-hearaside").exists());
}

#[test]
fn a_trailing_slash_on_the_folder_is_fine() {
    let t = Tree::new(Some(&original()));
    let root = format!("{}{}", t.root.display(), std::path::MAIN_SEPARATOR);
    assert_eq!(run(&["check".into(), root], &|| false).code, 0);
}

/// The anchors and replacements here are copies of the raw strings in the C++ tool: while that file exists they must agree.
#[test]
fn edits_are_the_ones_in_main_cpp() {
    let cpp = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../obs/streamlabs-setup/main.cpp");
    let Ok(src) = fs::read_to_string(&cpp) else {
        println!("{} not found: skipped", cpp.display());
        return;
    };
    for (a, r) in EDITS {
        assert!(
            src.contains(&format!("R\"({a})\"")),
            "anchor missing in main.cpp: {a}"
        );
        assert!(
            src.contains(&format!("R\"({r})\"")),
            "replacement missing in main.cpp: {r}"
        );
    }
    assert_eq!(
        src.matches("R\"(").count(),
        EDITS.len() * 2,
        "main.cpp has other raw strings: new edit?"
    );
}
