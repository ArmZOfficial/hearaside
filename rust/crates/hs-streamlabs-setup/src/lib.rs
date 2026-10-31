//! hearaside-streamlabs.exe: puts the HEARASIDE source into Streamlabs Desktop, or takes it out again.
//! Port of obs/streamlabs-setup/main.cpp; the command line, exit codes, files and messages are the installer's contract
//! (installer/windows/hearaside.iss runs it, and a scheduled task runs "repair").
//!   hearaside-streamlabs check     <Streamlabs folder>                 0 = can be added (2 = yes, but it is running)
//!   hearaside-streamlabs install   <Streamlabs folder> <hearaside-obs.dll>
//!   hearaside-streamlabs repair    <Streamlabs folder> <hearaside-obs.dll>   install again after a Streamlabs update
//!   hearaside-streamlabs uninstall <Streamlabs folder>
//! Streamlabs only offers and keeps built-in source types, so besides the plug-in (the OBS one: it fills Streamlabs'
//! libobs layout itself) the renderer bundle in resources\app.asar gets seven small additions; the original is kept as
//! app.asar.before-hearaside, and <folder>\force-local-bundles makes Streamlabs run that bundle instead of the one it
//! downloads at start-up. A Streamlabs update replaces all of it: "repair" puts it back, from the new original, once
//! Streamlabs is closed.
//! Exit codes: 0 ok, 1 usage, 2 Streamlabs is running, 3 Streamlabs version not supported, 4 not found, 5 failed.

pub mod asar;
pub mod json;
pub mod sha256;

use asar::{Asar, is_patched, patch_asar, patched_text};
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub struct Failure {
    pub code: i32,
    pub message: String,
}

fn fail(code: i32, message: impl Into<String>) -> Failure {
    Failure {
        code,
        message: message.into(),
    }
}

/// What the tool printed and returned
#[derive(Debug, Default, PartialEq)]
pub struct Outcome {
    pub code: i32,
    pub stdout: String,
    pub stderr: String,
}

struct Paths {
    root: PathBuf,
    asar: PathBuf,
    backup: PathBuf,
    temp: PathBuf,
    obs_dll: PathBuf,
    plugin: PathBuf,
    force_local: PathBuf,
    state: PathBuf,
}

impl Paths {
    fn new(root: PathBuf) -> Paths {
        let resources = root.join("resources");
        let obs_dll: PathBuf = resources
            .join("app.asar.unpacked")
            .join("node_modules")
            .join("obs-studio-node")
            .join("obs.dll");
        let plugin = obs_dll
            .with_file_name("obs-plugins")
            .join("64bit")
            .join("hearaside-obs.dll");
        Paths {
            asar: resources.join("app.asar"),
            backup: resources.join("app.asar.before-hearaside"),
            temp: resources.join("app.asar.hearaside-tmp"),
            force_local: root.join("force-local-bundles"),
            state: resources.join("app.asar.hearaside-state"),
            obs_dll,
            plugin,
            root,
        }
    }
}

/// `juce::File(path).getFullPathName()`: absolute, no trailing separator
fn full_path(s: &str) -> PathBuf {
    let p = Path::new(s);
    std::path::absolute(p)
        .unwrap_or_else(|_| p.to_path_buf())
        .components()
        .collect()
}

#[cfg(windows)]
pub fn streamlabs_running() -> bool {
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::System::Diagnostics::ToolHelp::{
        CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW,
        TH32CS_SNAPPROCESS,
    };
    // SAFETY: the snapshot handle is checked before use and closed once; PROCESSENTRY32W is plain data, all-zero is
    // valid, and dwSize is set as Process32FirstW/NextW require before they fill it.
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snap == INVALID_HANDLE_VALUE {
            return false;
        }
        let mut e: PROCESSENTRY32W = std::mem::zeroed();
        e.dwSize = size_of::<PROCESSENTRY32W>() as u32;
        let mut found = false;
        let mut ok = Process32FirstW(snap, &mut e);
        while ok != 0 && !found {
            let len = e
                .szExeFile
                .iter()
                .position(|&c| c == 0)
                .unwrap_or(e.szExeFile.len());
            found = String::from_utf16_lossy(&e.szExeFile[..len])
                .eq_ignore_ascii_case("Streamlabs OBS.exe");
            ok = Process32NextW(snap, &mut e);
        }
        CloseHandle(snap);
        found
    }
}

#[cfg(not(windows))]
pub fn streamlabs_running() -> bool {
    false
}

/// app.asar as we left it (size + time): a Streamlabs update or repair changes it
fn asar_stamp(asar: &Path) -> String {
    let m = fs::metadata(asar).ok();
    let size = m.as_ref().map_or(0, |m| m.len());
    let ms = m.and_then(|m| m.modified().ok()).map_or(0, |t| {
        match t.duration_since(std::time::UNIX_EPOCH) {
            Ok(d) => d.as_millis() as i64,
            Err(e) => -(e.duration().as_millis() as i64),
        }
    });
    format!("{size} {ms}")
}

fn same_file(a: &Path, b: &Path) -> bool {
    let hash = |p: &Path| -> Option<String> {
        let mut f = File::open(p).ok()?;
        let mut h = sha256::Sha256::default();
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = f.read(&mut buf).ok()?;
            if n == 0 {
                return Some(h.finish());
            }
            h.update(&buf[..n]);
        }
    };
    matches!((fs::metadata(a), fs::metadata(b)), (Ok(x), Ok(y)) if x.len() == y.len())
        && hash(a).is_some_and(|x| hash(b) == Some(x))
}

/// `juce::File::deleteFile()`: true when it is gone afterwards
fn delete_file(p: &Path) -> bool {
    match fs::symlink_metadata(p) {
        Err(_) => true,
        Ok(m) if m.is_dir() => fs::remove_dir(p).is_ok(),
        Ok(_) => fs::remove_file(p).is_ok(),
    }
}

fn install(p: &Paths, dll: &Path) -> Result<String, Failure> {
    if !dll.is_file() {
        return Err(fail(4, format!("plug-in not found: {}", dll.display())));
    }
    // An unpatched app.asar is the newest original (first install, or Streamlabs updated): it becomes the
    // backup. A patched one was made from the backup we kept. Never put an older original into a newer Streamlabs.
    if !is_patched(&p.asar)? {
        patch_asar(&p.asar, &p.temp)?;
        if !(delete_file(&p.backup) && fs::copy(&p.asar, &p.backup).is_ok()) {
            return Err(fail(5, "cannot keep the original app.asar"));
        }
    } else {
        if !p.backup.is_file() {
            return Err(fail(5, "app.asar is patched but its original is missing"));
        }
        patch_asar(&p.backup, &p.temp)?;
    }
    // std replaces an existing destination (MoveFileExW with MOVEFILE_REPLACE_EXISTING on Windows)
    fs::rename(&p.temp, &p.asar).map_err(|_| fail(5, "cannot replace app.asar"))?;
    if !(delete_file(&p.plugin) && fs::copy(dll, &p.plugin).is_ok()) {
        return Err(fail(
            5,
            format!("cannot copy the plug-in to {}", p.plugin.display()),
        ));
    }
    let created = p.force_local.exists()
        || (p
            .force_local
            .parent()
            .is_none_or(|d| fs::create_dir_all(d).is_ok())
            && File::create(&p.force_local).is_ok());
    if !created {
        return Err(fail(
            5,
            format!("cannot create {}", p.force_local.display()),
        ));
    }
    // the C++ tool's replaceWithText() failure was not checked either
    let _ = fs::write(&p.state, asar_stamp(&p.asar));
    Ok("HEARASIDE source installed in Streamlabs Desktop".into())
}

/// Quick when nothing changed (the scheduled task runs it every few minutes)
fn installed(p: &Paths, dll: &Path) -> bool {
    fs::read(&p.state).is_ok_and(|s| String::from_utf8_lossy(&s) == asar_stamp(&p.asar))
        && p.force_local.exists()
        && p.plugin.is_file()
        && same_file(&p.plugin, dll)
}

fn uninstall(p: &Paths) -> Result<String, Failure> {
    delete_file(&p.plugin);
    if p.backup.is_file() {
        if is_patched(&p.asar)? {
            // our patch of that backup: put it back
            if !(delete_file(&p.asar) && fs::rename(&p.backup, &p.asar).is_ok()) {
                return Err(fail(5, "cannot restore the original app.asar"));
            }
        } else {
            delete_file(&p.backup); // Streamlabs was updated since: the backup is older than what is there now
        }
    }
    delete_file(&p.force_local);
    delete_file(&p.state);
    Ok("Streamlabs Desktop restored".into())
}

/// 0 = HEARASIDE can be added (or is): the renderer is patched, or every anchor matches once
fn check(p: &Paths, running: &dyn Fn() -> bool) -> Result<i32, Failure> {
    let mut f =
        File::open(&p.asar).map_err(|_| fail(5, format!("cannot read {}", p.asar.display())))?;
    let asar = Asar::read(&mut f)?;
    if !asar.patched() {
        patched_text(asar.text)?; // fails with 3 when an anchor is missing
    }
    Ok(if running() { 2 } else { 0 })
}

/// `args` is argv without the program name. `running` says whether Streamlabs Desktop is running (`streamlabs_running`).
pub fn run(args: &[String], running: &dyn Fn() -> bool) -> Outcome {
    const USAGE: &str = "usage: hearaside-streamlabs check|install|repair|uninstall <Streamlabs folder> [hearaside-obs.dll]";
    if args.len() < 2 {
        return Outcome {
            code: 1,
            stderr: format!("{USAGE}\n"),
            ..Outcome::default()
        };
    }
    let (cmd, p) = (args[0].as_str(), Paths::new(full_path(&args[1])));
    let mut out = Outcome::default();
    let result = (|| -> Result<i32, Failure> {
        if !p.asar.is_file() || !p.obs_dll.is_file() {
            return Err(fail(
                4,
                format!("Streamlabs Desktop not found in {}", p.root.display()),
            ));
        }
        match cmd {
            "uninstall" => {
                if running() {
                    return Err(fail(2, "close Streamlabs Desktop first"));
                }
                out.stdout = uninstall(&p)? + "\n";
                Ok(0)
            }
            "check" => check(&p, running),
            "install" | "repair" if args.len() >= 3 => {
                let dll = full_path(&args[2]);
                if running() {
                    return Err(fail(2, "close Streamlabs Desktop first"));
                }
                if cmd == "repair" && installed(&p, &dll) {
                    return Ok(0);
                }
                out.stdout = install(&p, &dll)? + "\n";
                Ok(0)
            }
            _ => {
                out.stderr = "unknown command\n".into();
                Ok(1)
            }
        }
    })();
    out.code = result.unwrap_or_else(|f| {
        let _ = fs::remove_file(&p.temp);
        out.stderr = format!("{}\n", f.message);
        f.code
    });
    out
}
