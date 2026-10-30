//! Builds the SpeexDSP resampler from `external/speexdsp` with the same switches as `libs/CMakeLists.txt`
//! (floating point, SSE on x86-64), under a symbol prefix of its own so it can sit in one binary with the
//! C++ copy during the hybrid phase.
use std::path::Path;

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../external/speexdsp");
    let src = root.join("libspeexdsp/resample.c");
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed=build.rs");
    let mut b = cc::Build::new();
    b.file(&src)
        .include(root.join("libspeexdsp"))
        .include(root.join("include/speex"))
        .include(root.join("include"))
        .define("OUTSIDE_SPEEX", None)
        .define("RANDOM_PREFIX", "hsdsp")
        .define("FLOATING_POINT", None)
        .define("EXPORT", "")
        .warnings(false);
    let arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    let os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if arch == "x86_64" && os != "macos" {
        b.define("USE_SSE", None);
    }
    b.compile("hs_speex_resampler");
}
