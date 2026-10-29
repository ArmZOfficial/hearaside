add_executable(hs_compat_golden main.cpp)
target_link_libraries(hs_compat_golden PRIVATE ssengine ssdsp ssbus ${CMAKE_DL_LIBS})
target_compile_features(hs_compat_golden PRIVATE cxx_std_20)

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE HS_GOLDEN_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT HS_GOLDEN_COMMIT)
    set(HS_GOLDEN_COMMIT "unknown")
endif()
set(HS_GOLDEN_HASHES "")
foreach(source IN ITEMS
    libs/CMakeLists.txt
    libs/ssbus/include/ssbus/protocol.h libs/ssbus/include/ssbus/ring.h
    libs/ssbus/include/ssbus/bus.h libs/ssbus/src/bus.cpp
    libs/ssbus/src/shm_win.cpp libs/ssbus/src/shm_posix.cpp
    libs/ssdsp/include/ssdsp/dsp.h libs/ssdsp/include/ssdsp/consumer.h libs/ssdsp/src/consumer.cpp
    libs/ssengine/include/ssengine/engine.h libs/ssengine/include/ssengine/friend_reader.h
    libs/ssengine/src/track_publisher.cpp libs/ssengine/src/hub_engine.cpp
    libs/ssengine/src/hub_friends.cpp libs/ssengine/src/friend_reader.cpp
    external/speexdsp/libspeexdsp/resample.c external/speexdsp/libspeexdsp/resample_sse.h
    external/speexdsp/libspeexdsp/arch.h
    external/speexdsp/include/speex/speex_resampler.h
    tools/compat-golden/main.cpp tools/compat-golden/rt_audit.h tools/compat-golden/CMakeLists.txt tools/compat-golden/provenance.h.in)
    file(SHA256 ${CMAKE_SOURCE_DIR}/${source} source_hash)
    if(HS_GOLDEN_HASHES)
        string(APPEND HS_GOLDEN_HASHES ",\n")
    endif()
    string(APPEND HS_GOLDEN_HASHES "    \"${source}\": \"${source_hash}\"")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/${source})
endforeach()
# Net-sim arrival traces are recorded fixtures (tools/net-sim/record-trace.mjs), embedded byte for byte.
set(HS_NETSIM_TRACES good home mobile)
set(HS_NETSIM_ARRAYS "")
set(HS_NETSIM_TABLE "")
foreach(trace IN LISTS HS_NETSIM_TRACES)
    set(trace_file ${CMAKE_SOURCE_DIR}/compat/golden/netsim/traces/${trace}.trace)
    file(READ ${trace_file} trace_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," trace_bytes "${trace_hex}")
    string(APPEND HS_NETSIM_ARRAYS "inline constexpr unsigned char kTrace_${trace}[] = { ${trace_bytes} };\n")
    string(APPEND HS_NETSIM_TABLE "    { \"${trace}\", kTrace_${trace}, sizeof(kTrace_${trace}) },\n")
    file(SHA256 ${trace_file} trace_hash)
    string(APPEND HS_GOLDEN_HASHES ",\n    \"compat/golden/netsim/traces/${trace}.trace\": \"${trace_hash}\"")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${trace_file})
endforeach()
file(CONFIGURE OUTPUT netsim_traces.h CONTENT "#pragma once
#include <cstddef>
struct NetsimTrace { const char* name; const unsigned char* data; size_t size; };
${HS_NETSIM_ARRAYS}inline constexpr NetsimTrace kNetsimTraces[] = {
${HS_NETSIM_TABLE}};
" @ONLY)
configure_file(provenance.h.in provenance.h @ONLY)
target_include_directories(hs_compat_golden PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME compat_golden_determinism COMMAND hs_compat_golden --self-test)
add_test(NAME compat_golden_rt_safety COMMAND hs_compat_golden --rt-audit)
add_test(NAME compat_golden_core COMMAND hs_compat_golden --compare ${CMAKE_SOURCE_DIR}/compat/golden/core)
add_test(NAME compat_golden_netsim_determinism COMMAND hs_compat_golden --netsim --self-test)
add_test(NAME compat_golden_netsim COMMAND hs_compat_golden --netsim --compare ${CMAKE_SOURCE_DIR}/compat/golden/netsim)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
        AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "19.44.35229.0")
    add_test(NAME compat_golden_core_exact COMMAND hs_compat_golden --check ${CMAKE_SOURCE_DIR}/compat/golden/core
        CONFIGURATIONS Release)
endif()
# compat/golden/netsim was recorded with GCC 13.3.0 on Linux x86_64.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
        AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "13.3.0")
    add_test(NAME compat_golden_netsim_exact COMMAND hs_compat_golden --netsim --check ${CMAKE_SOURCE_DIR}/compat/golden/netsim)
endif()
