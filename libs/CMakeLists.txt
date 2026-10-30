# ---- ssbus: shared memory protocol (MIT, no JUCE) ------------------------------------------
add_library(ssbus STATIC
    ssbus/src/bus.cpp
    ssbus/src/shm_win.cpp
    ssbus/src/shm_posix.cpp)
target_include_directories(ssbus PUBLIC ssbus/include)
if(WIN32)
    target_link_libraries(ssbus PUBLIC advapi32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(ssbus PUBLIC rt pthread)
endif()

# ---- speexdsp resampler (BSD) ---------------------------------------------------------------
add_library(ss_speex_resampler STATIC ${SS_EXTERNAL}/speexdsp/libspeexdsp/resample.c)
target_include_directories(ss_speex_resampler
    PUBLIC  ${SS_EXTERNAL}/speexdsp/include/speex
    PRIVATE ${SS_EXTERNAL}/speexdsp/libspeexdsp ${SS_EXTERNAL}/speexdsp/include)
target_compile_definitions(ss_speex_resampler
    PUBLIC  OUTSIDE_SPEEX RANDOM_PREFIX=hearaside
    PRIVATE FLOATING_POINT EXPORT=)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64" AND NOT APPLE)
    target_compile_definitions(ss_speex_resampler PRIVATE USE_SSE)
endif()
if(MSVC)
    target_compile_options(ss_speex_resampler PRIVATE /wd4244 /wd4305 /wd4267)
endif()

# ---- ssdsp: DSP blocks + consumer (resampler / drift) ---------------------------------------
add_library(ssdsp STATIC ssdsp/src/consumer.cpp)
target_include_directories(ssdsp PUBLIC ssdsp/include)
target_link_libraries(ssdsp PUBLIC ssbus PRIVATE ss_speex_resampler)

# ---- ssengine: Track publisher + Hub engine -------------------------------------------------
if(HEARASIDE_USE_RUST_CORE)
    # The same API over the Rust core: cargo builds the hs-ffi static library, ssengine keeps only the adapter.
    find_program(HEARASIDE_CARGO cargo REQUIRED)
    set(HS_RUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../rust)
    set(HS_RUST_TARGET_DIR ${CMAKE_BINARY_DIR}/rust-target)
    if(WIN32)
        set(HS_FFI_LIB ${HS_RUST_TARGET_DIR}/release/hs_ffi.lib)
    else()
        set(HS_FFI_LIB ${HS_RUST_TARGET_DIR}/release/libhs_ffi.a)
    endif()
    file(GLOB_RECURSE HS_RUST_SOURCES CONFIGURE_DEPENDS ${HS_RUST_DIR}/crates/*.rs ${HS_RUST_DIR}/crates/*/Cargo.toml)
    # RUSTFLAGS from the environment would replace rust/.cargo/config.toml (and drop +crt-static) without a word
    add_custom_command(
        OUTPUT ${HS_FFI_LIB}
        COMMAND ${CMAKE_COMMAND} -E env --unset=RUSTFLAGS
                ${HEARASIDE_CARGO} build -p hs-ffi --release --locked --target-dir ${HS_RUST_TARGET_DIR}
        WORKING_DIRECTORY ${HS_RUST_DIR}
        DEPENDS ${HS_RUST_SOURCES} ${HS_RUST_DIR}/Cargo.lock ${HS_RUST_DIR}/Cargo.toml
                ${HS_RUST_DIR}/.cargo/config.toml ${SS_EXTERNAL}/speexdsp/libspeexdsp/resample.c
        COMMENT "cargo build hs-ffi: the Rust core"
        VERBATIM)
    add_custom_target(hs_ffi_build DEPENDS ${HS_FFI_LIB})
    add_library(hs_ffi STATIC IMPORTED GLOBAL)
    set_target_properties(hs_ffi PROPERTIES IMPORTED_LOCATION ${HS_FFI_LIB})
    add_dependencies(hs_ffi hs_ffi_build)
    if(WIN32)
        # what `cargo rustc -- --print native-static-libs` lists (+ bcrypt / advapi32, harmless)
        target_link_libraries(hs_ffi INTERFACE ws2_32 userenv bcrypt ntdll advapi32 kernel32 dbghelp)
        if(MSVC)
            # the Rust lib is built against the static release CRT (libcmt); a Debug build links libcmtd instead
            target_link_options(hs_ffi INTERFACE $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmt>)
        endif()
    else()
        target_link_libraries(hs_ffi INTERFACE pthread dl m)
    endif()

    add_library(ssengine STATIC ssengine/src/rust_core.cpp)
    target_include_directories(ssengine PUBLIC ssengine/include PRIVATE ${HS_RUST_DIR}/crates/hs-ffi/include)
    target_compile_definitions(ssengine PUBLIC HEARASIDE_USE_RUST_CORE=1)
    target_link_libraries(ssengine PUBLIC ssbus ssdsp hs_ffi)
else()
    add_library(ssengine STATIC
        ssengine/src/track_publisher.cpp
        ssengine/src/hub_engine.cpp
        ssengine/src/hub_friends.cpp
        ssengine/src/friend_reader.cpp)
    target_include_directories(ssengine PUBLIC ssengine/include)
    target_link_libraries(ssengine PUBLIC ssbus ssdsp)
endif()

# ---- opus: audio codec (BSD-3) -------------------------------------------------------------
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_FILE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${SS_EXTERNAL}/opus ${CMAKE_CURRENT_BINARY_DIR}/opus EXCLUDE_FROM_ALL)

# ---- speexdsp echo canceller & preprocessor (BSD) -------------------------------------------
add_library(ss_speex_echo STATIC
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/mdf.c
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/preprocess.c
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/filterbank.c
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/fftwrap.c
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/kiss_fft.c
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/kiss_fftr.c
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/buffer.c
    ${SS_EXTERNAL}/speexdsp/libspeexdsp/scal.c)
target_include_directories(ss_speex_echo
    PUBLIC  ${SS_EXTERNAL}/speexdsp/include
    PRIVATE ${SS_EXTERNAL}/speexdsp/libspeexdsp)
target_compile_definitions(ss_speex_echo
    PUBLIC  FLOATING_POINT USE_KISS_FFT
    PRIVATE EXPORT= inline=__inline)
if(MSVC)
    target_compile_options(ss_speex_echo PRIVATE /wd4244 /wd4305 /wd4267 /wd4018)
endif()
if(NOT WIN32)
    # speexdsp_types.h needs the header autotools would generate outside Windows
    set(INCLUDE_STDINT "#include <stdint.h>")
    set(SIZE16 int16_t)
    set(USIZE16 uint16_t)
    set(SIZE32 int32_t)
    set(USIZE32 uint32_t)
    configure_file(${SS_EXTERNAL}/speexdsp/include/speex/speexdsp_config_types.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/speex_config/speexdsp_config_types.h @ONLY)
    target_include_directories(ss_speex_echo PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/speex_config)
endif()

# ---- mbedtls: crypto & TLS backend for libdatachannel (Apache-2.0) -------------------------
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SS_EXTERNAL}/mbedtls ${CMAKE_CURRENT_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)
if(NOT TARGET MbedTLS::MbedTLS AND TARGET mbedtls)
    add_library(MbedTLS::MbedTLS ALIAS mbedtls)
endif()

# ---- libdatachannel: WebRTC DataChannel (MPL-2.0) -------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${SS_EXTERNAL}/libdatachannel ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel EXCLUDE_FROM_ALL)

# ---- ssnet: network wire formats, codecs, echo cancellation, WebRTC (MIT, no JUCE) --------
add_library(ssnet STATIC
    ssnet/src/opus_codec.cpp
    ssnet/src/aec.cpp
    ssnet/src/rtc.cpp)
target_include_directories(ssnet PUBLIC ssnet/include)
target_link_libraries(ssnet PUBLIC opus ss_speex_echo datachannel-static)


