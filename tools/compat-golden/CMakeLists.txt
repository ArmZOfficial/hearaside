add_executable(hs_compat_golden main.cpp)
target_link_libraries(hs_compat_golden PRIVATE ssengine ssdsp ssbus)
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
    tools/compat-golden/main.cpp tools/compat-golden/CMakeLists.txt tools/compat-golden/provenance.h.in)
    file(SHA256 ${CMAKE_SOURCE_DIR}/${source} source_hash)
    if(HS_GOLDEN_HASHES)
        string(APPEND HS_GOLDEN_HASHES ",\n")
    endif()
    string(APPEND HS_GOLDEN_HASHES "    \"${source}\": \"${source_hash}\"")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/${source})
endforeach()
configure_file(provenance.h.in provenance.h @ONLY)
target_include_directories(hs_compat_golden PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME compat_golden_determinism COMMAND hs_compat_golden --self-test)
add_test(NAME compat_golden_core COMMAND hs_compat_golden --compare ${CMAKE_SOURCE_DIR}/compat/golden/core)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
        AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "19.44.35229.0")
    add_test(NAME compat_golden_core_exact COMMAND hs_compat_golden --check ${CMAKE_SOURCE_DIR}/compat/golden/core
        CONFIGURATIONS Release)
endif()
