//! Cross-process / cross-language bus tests (PLAN R1.5). Rust writer + Rust reader always run. The
//! C++ directions run when `SS_IPC_CPP` points at the built `ss_ipc_test` executable (the tests print
//! a note and skip otherwise); CI sets it after building the C++ tests.
use std::path::PathBuf;
use std::process::{Command, Output};

fn rust_exe() -> &'static str {
    env!("CARGO_BIN_EXE_hs-ipc-test")
}

fn cpp_exe() -> Option<PathBuf> {
    let p = PathBuf::from(std::env::var_os("SS_IPC_CPP")?);
    assert!(p.is_file(), "SS_IPC_CPP does not point at a file: {p:?}");
    Some(p)
}

fn check(name: &str, out: Output) {
    let text = String::from_utf8_lossy(&out.stdout);
    println!("{name}: {text}");
    assert!(out.status.success(), "{name} failed:\n{text}");
    assert!(text.contains("IPC TEST PASSED"), "{name}:\n{text}");
}

#[test]
fn rust_writer_rust_reader() {
    check("rust->rust", Command::new(rust_exe()).output().unwrap());
}

#[test]
fn cpp_writer_rust_reader() {
    let Some(cpp) = cpp_exe() else {
        println!("SS_IPC_CPP not set: skipped");
        return;
    };
    check(
        "cpp->rust",
        Command::new(rust_exe())
            .arg("--writer-cmd")
            .arg(cpp)
            .output()
            .unwrap(),
    );
}

#[test]
fn rust_writer_cpp_reader() {
    let Some(cpp) = cpp_exe() else {
        println!("SS_IPC_CPP not set: skipped");
        return;
    };
    check(
        "rust->cpp",
        Command::new(cpp)
            .env("HS_IPC_WRITER", rust_exe())
            .output()
            .unwrap(),
    );
}
