//! Gives the update check the product version (`HEARASIDE_VERSION`, the `project(Hearaside VERSION x.y.z)` of the root
//! CMakeLists, which the C++ karaoke target defines from the same line). Same reader as hs-obs's build.rs.
use std::path::PathBuf;

fn main() {
    let cmake =
        PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../../../CMakeLists.txt");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed={}", cmake.display());
    let text = std::fs::read_to_string(&cmake)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", cmake.display()));
    let version = text
        .lines()
        .find_map(|l| {
            let rest = l.trim().strip_prefix("project(Hearaside VERSION ")?;
            Some(rest.split_whitespace().next()?.to_string())
        })
        .expect("no `project(Hearaside VERSION x.y.z` line in the root CMakeLists.txt");
    println!("cargo:rustc-env=HEARASIDE_VERSION={version}");
}
