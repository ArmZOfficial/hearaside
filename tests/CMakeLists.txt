add_executable(ss_unit_tests
    unit/main.cpp
    unit/test_bus.cpp
    unit/test_dsp.cpp
    unit/test_engine.cpp
    unit/test_friends.cpp
    unit/test_jitter.cpp
    unit/test_autosync.cpp
    unit/test_paced_send.cpp
    unit/test_net.cpp
    unit/test_rt_safety.cpp)
target_link_libraries(ss_unit_tests PRIVATE ssengine ssdsp ssbus ssnet)
add_test(NAME unit COMMAND ss_unit_tests)

# CPU of the Hub audio thread, C++ vs Rust core (PLAN-rust-migration R2.6); a tool, not a test
add_executable(ss_hub_bench bench/hub_cpu_bench.cpp)
target_link_libraries(ss_hub_bench PRIVATE ssengine ssdsp ssbus)

# Writes compat/golden/net for hs-net (PLAN-rust-migration R3.1); a tool, not a test
add_executable(ss_net_golden golden/net_golden.cpp)
target_link_libraries(ss_net_golden PRIVATE ssnet)

add_executable(ss_ipc_test integration/ipc_test.cpp)
target_link_libraries(ss_ipc_test PRIVATE ssbus)
add_test(NAME ipc COMMAND ss_ipc_test)

# OBS hotkey remote commands, the C++ side of the Rust hs-obs cross-language tests (SS_REMOTE_CPP); a tool, not a test
add_executable(ss_remote_probe integration/remote_probe.cpp ../obs/hearaside-obs/src/remote.cpp)
target_include_directories(ss_remote_probe PRIVATE ${CMAKE_SOURCE_DIR}/obs/hearaside-obs/src)
target_link_libraries(ss_remote_probe PRIVATE ssbus)

add_executable(hs-layout-dump ../tools/compat-dump/layout.cpp)
target_include_directories(hs-layout-dump PRIVATE ${CMAKE_SOURCE_DIR}/libs/ssbus/include)
add_test(NAME shm_v14_layout COMMAND hs-layout-dump --check
    ${CMAKE_SOURCE_DIR}/compat/fixtures/shm-v14-layout.json)

add_subdirectory(${CMAKE_SOURCE_DIR}/tools/compat-golden ${CMAKE_BINARY_DIR}/tools/compat-golden)
if(TARGET juce::juce_dsp)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tools/compat-vocal-golden ${CMAKE_BINARY_DIR}/tools/compat-vocal-golden)
endif()

# permanent share links, web side (web/share-vercel): the pages match plugins/hub/web, API tests
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME share_web_in_sync COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/web/share-vercel/scripts/sync-web.mjs --check)
    add_test(NAME share_api COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_SOURCE_DIR}/web/share-vercel/test/directory.test.js)
    add_test(NAME net_sim COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/net-sim/net-sim.test.mjs)   # the bad-line proxy for the friend page tests
    add_test(NAME web_worklet COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/web-friend/worklet.test.mjs)   # the friend page's audio worklet, no browser
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME compat_state_fixtures
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/compat/check_fixtures.py)
    add_test(NAME compat_golden_integrity
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/compat/check_golden.py)
    add_test(NAME compat_state_envelopes
             COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/compat -p test_fixtures.py)
endif()
if(Python3_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/design/gen_theme.py)
    add_test(NAME design_tokens_in_sync
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/design/gen_theme.py --check)
    add_test(NAME design_contrast   # WCAG 4.5:1 for every text colour (docs/ux-roadmap.md 6.2)
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/design/gen_theme.py --contrast)
endif()

if(HEARASIDE_BUILD_KARAOKE)
    juce_add_console_app(hearaside_karaoke_test PRODUCT_NAME "karaoke-test")
    target_sources(hearaside_karaoke_test PRIVATE
        unit/main.cpp
        unit/test_karaoke.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/AudioSettings.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/VocalChain.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/JoinClient.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/PreflightChecker.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/MusicPlayer.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/RoomHost.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/UpdateChecker.cpp
        ${CMAKE_SOURCE_DIR}/apps/karaoke/HasAsio.cpp
        ${CMAKE_SOURCE_DIR}/plugins/app/AppCapture.cpp
    )
    target_include_directories(hearaside_karaoke_test PRIVATE
        ${CMAKE_SOURCE_DIR}/apps/karaoke
        ${CMAKE_SOURCE_DIR}/plugins/app
        ${CMAKE_CURRENT_SOURCE_DIR}/unit
    )
    target_compile_definitions(hearaside_karaoke_test PRIVATE
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        HEARASIDE_VERSION="${PROJECT_VERSION}"
        HEARASIDE_SHARE_BASE="${HEARASIDE_SHARE_BASE}"
    )
    target_link_libraries(hearaside_karaoke_test PRIVATE
        hearaside_common
        ssengine
        ssdsp
        ssbus
        ssnet
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_gui_extra
        juce::juce_dsp
        juce::juce_recommended_config_flags
    )
    if(WIN32)
        target_link_libraries(hearaside_karaoke_test PRIVATE
            ole32 mmdevapi avrt winhttp crypt32
        )
    endif()
    add_test(NAME karaoke_loopback COMMAND hearaside_karaoke_test "karaoke:")
endif()

