//! Builds libopus from `external/opus` with its own CMake project and the options of `libs/CMakeLists.txt`, and the
//! SpeexDSP echo canceller + preprocessor from `external/speexdsp` with the switches of `ss_speex_echo`, so the
//! codec and the canceller are the same code as in the C++ build (`compat/golden/net` checks the bytes).
use std::path::Path;

fn main() {
    let ext = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../external");
    println!("cargo:rerun-if-changed=build.rs");

    // ---- opus (BSD-3) ----
    let opus = ext.join("opus");
    for d in ["CMakeLists.txt", "cmake", "include", "src", "celt", "silk"] {
        println!("cargo:rerun-if-changed={}", opus.join(d).display());
    }
    let mut cfg = cmake::Config::new(&opus);
    // Desktop shells may omit PROCESSOR_ARCHITECTURE; Opus must still select the
    // target's SIMD code, including when CMake cached an empty system processor.
    let target_processor =
        std::env::var("CARGO_CFG_TARGET_ARCH").expect("Cargo target architecture");
    let target_config =
        Path::new(&std::env::var("OUT_DIR").expect("OUT_DIR")).join("opus-target.cmake");
    std::fs::write(
        &target_config,
        format!("set(CMAKE_SYSTEM_PROCESSOR \"{target_processor}\")\n"),
    )
    .expect("Opus target configuration");
    cfg.profile("Release") // the same optimised code in debug test builds as in the plug-ins
        .define("CMAKE_PROJECT_Opus_INCLUDE", &target_config)
        .define("OPUS_BUILD_SHARED_LIBRARY", "OFF")
        .define("OPUS_BUILD_TESTING", "OFF")
        .define("OPUS_BUILD_PROGRAMS", "OFF")
        .define("OPUS_INSTALL_PKG_CONFIG_FILE", "OFF")
        .define("OPUS_INSTALL_CMAKE_CONFIG_MODULE", "OFF")
        .define("CMAKE_INSTALL_LIBDIR", "lib"); // not lib64 (GNUInstallDirs on some Linux distributions)
    let crt_static = std::env::var("CARGO_CFG_TARGET_FEATURE")
        .is_ok_and(|f| f.split(',').any(|x| x == "crt-static"));
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        // the CRT of the Rust side (rust/.cargo/config.toml: +crt-static); opus picks its own CRT from this option
        cfg.static_crt(crt_static)
            .define("OPUS_STATIC_RUNTIME", if crt_static { "ON" } else { "OFF" });
    }
    let dst = cfg.build();
    println!(
        "cargo:rustc-link-search=native={}",
        dst.join("lib").display()
    );
    println!("cargo:rustc-link-lib=static=opus");

    // ---- speexdsp echo canceller & preprocessor (BSD) ----
    let spx = ext.join("speexdsp");
    let mut b = cc::Build::new();
    for f in [
        "mdf.c",
        "preprocess.c",
        "filterbank.c",
        "fftwrap.c",
        "kiss_fft.c",
        "kiss_fftr.c",
        "buffer.c",
        "scal.c",
    ] {
        b.file(spx.join("libspeexdsp").join(f));
    }
    println!(
        "cargo:rerun-if-changed={}",
        spx.join("libspeexdsp").display()
    );
    println!("cargo:rerun-if-changed={}", spx.join("include").display());
    b.include(spx.join("include"))
        .include(spx.join("libspeexdsp"))
        .define("FLOATING_POINT", None)
        .define("USE_KISS_FFT", None)
        // speex_warning() is an fprintf(stderr) from inside speex_echo_cancellation (filter reset): no syscalls on
        // the audio thread (the C++ build kept it)
        .define("DISABLE_WARNINGS", None)
        .define("EXPORT", "")
        .opt_level(2)
        .warnings(false);
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        b.define("inline", "__inline");
    }
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        // speexdsp_types.h needs the header autotools would generate outside Windows
        let out = Path::new(&std::env::var("OUT_DIR").expect("OUT_DIR")).join("speex_config");
        std::fs::create_dir_all(&out).expect("speex_config dir");
        std::fs::write(
            out.join("speexdsp_config_types.h"),
            "#ifndef __SPEEX_TYPES_H__\n#define __SPEEX_TYPES_H__\n#include <stdint.h>\n\
             typedef int16_t spx_int16_t;\ntypedef uint16_t spx_uint16_t;\ntypedef int32_t spx_int32_t;\n\
             typedef uint32_t spx_uint32_t;\n#endif\n",
        )
        .expect("speexdsp_config_types.h");
        b.include(&out);
    }
    b.compile("hs_speex_echo");
}
