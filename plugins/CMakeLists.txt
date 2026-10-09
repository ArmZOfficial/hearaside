# HEARASIDE Track + Hub (JUCE 8). VST3 always; VST2 only when HEARASIDE_VST2_SDK points at a
# licensed VST 2 SDK (folder containing pluginterfaces/vst2.x/aeffect.h).

set(hearaside_formats VST3)
# Permanent share links: your deployment of web/share-vercel (e.g. https://my-hearaside.vercel.app).
# Empty = none until set in the Hub's settings; nothing is ever sent to a site you didn't choose.
set(HEARASIDE_SHARE_BASE "" CACHE STRING "Default share web site for permanent links")
if(HEARASIDE_VST2_SDK)
    if(NOT EXISTS "${HEARASIDE_VST2_SDK}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR "HEARASIDE_VST2_SDK=${HEARASIDE_VST2_SDK} does not contain pluginterfaces/vst2.x/aeffect.h")
    endif()
    juce_set_vst2_sdk_path("${HEARASIDE_VST2_SDK}")
    list(APPEND hearaside_formats VST)
    message(STATUS "HEARASIDE: building VST2 as well (${HEARASIDE_VST2_SDK})")
endif()

juce_add_binary_data(hearaside_assets
    NAMESPACE HearasideAssets
    SOURCES
        ${SS_EXTERNAL}/fonts/Anuphan-400.ttf
        ${SS_EXTERNAL}/fonts/Anuphan-500.ttf
        ${SS_EXTERNAL}/fonts/Anuphan-600.ttf)
set_target_properties(hearaside_assets PROPERTIES POSITION_INDEPENDENT_CODE ON)

# web pages of the Hub's share links (listen / send)
juce_add_binary_data(hearaside_web
    HEADER_NAME BinaryData_web.h
    NAMESPACE HearasideWeb
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/hub/web/listen.html
        ${CMAKE_CURRENT_SOURCE_DIR}/hub/web/send.html)
set_target_properties(hearaside_web PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(hearaside_common_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/common/Settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/common/Strings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/common/ui/LookAndFeel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/common/ui/Backdrop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/common/ui/Components.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/common/ui/Icons.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/common/ui/SettingsPanel.cpp)

function(hearaside_add_plugin target)
    cmake_parse_arguments(P "" "PRODUCT_NAME;PLUGIN_CODE;DESCRIPTION" "SOURCES" ${ARGN})
    juce_add_plugin(${target}
        PRODUCT_NAME            "${P_PRODUCT_NAME}"
        COMPANY_NAME            "HEARASIDE"
        COMPANY_WEBSITE         "https://github.com/ArmZOfficial/hearaside"
        BUNDLE_ID               "com.hearaside.${target}"
        DESCRIPTION             "${P_DESCRIPTION}"
        PLUGIN_MANUFACTURER_CODE Hrsd
        PLUGIN_CODE             ${P_PLUGIN_CODE}
        FORMATS                 ${hearaside_formats}
        IS_SYNTH                FALSE
        NEEDS_MIDI_INPUT        FALSE
        NEEDS_MIDI_OUTPUT       FALSE
        IS_MIDI_EFFECT          FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES         Fx Tools
        VST2_CATEGORY           kPlugCategEffect
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(${target} PRIVATE ${P_SOURCES} ${hearaside_common_sources})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        # keep VST3 class IDs independent of the VST2 build so projects never change identity
        JUCE_VST3_CAN_REPLACE_VST2=0
        HEARASIDE_VERSION="${PROJECT_VERSION}"
        HEARASIDE_SHARE_BASE="${HEARASIDE_SHARE_BASE}")
    target_link_libraries(${target}
        PRIVATE
            hearaside_assets
            ssengine
            juce::juce_audio_utils
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

hearaside_add_plugin(HearasideTrack
    PRODUCT_NAME "HEARASIDE Track"
    PLUGIN_CODE  HrTk
    DESCRIPTION  "Per-track switch: what you hear vs what your viewers hear"
    SOURCES
        track/TrackProcessor.cpp
        track/TrackEditor.cpp)

hearaside_add_plugin(HearasideHub
    PRODUCT_NAME "HEARASIDE Hub"
    PLUGIN_CODE  HrHb
    DESCRIPTION  "Master-bus hub: builds the viewers' mix and sends it to OBS"
    SOURCES
        hub/HubProcessor.cpp
        hub/HubEditor.cpp
        hub/ShareServer.cpp
        hub/ShareDirectory.cpp
        hub/ControlServer.cpp
        hub/HubRestApi.cpp
        app/AppCapture.cpp)   # program list for the App Audio rows
target_include_directories(HearasideHub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HearasideHub PRIVATE hearaside_web)

# App Audio: an effect that adds what one program (or the whole computer) plays to its track
# (WASAPI process loopback, Windows 10 2004+), and prints it to WAV takes. Same plug-in code as
# the earlier instrument version, so saved projects keep finding it.
if(WIN32)
    target_link_libraries(HearasideHub PRIVATE ole32 mmdevapi avrt)
    hearaside_add_plugin(HearasideAppAudio
        PRODUCT_NAME "HEARASIDE App Audio"
        PLUGIN_CODE  HrAp
        DESCRIPTION  "Brings a program's audio (YouTube, Spotify...) into any DAW track and prints it"
        SOURCES
            app/AppAudioProcessor.cpp
            app/AppCapture.cpp
            app/LinkReceiver.cpp
            app/TakeRecorder.cpp)
    target_link_libraries(HearasideAppAudio PRIVATE ssdsp ole32 mmdevapi avrt winhttp)
endif()
# ui-snapshot: renders both editors to PNG (design QA without a DAW)
if(HEARASIDE_BUILD_TOOLS)
    juce_add_console_app(hearaside_ui_snapshot PRODUCT_NAME "ui-snapshot")
    target_sources(hearaside_ui_snapshot PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/ui-snapshot/main.cpp
        track/TrackProcessor.cpp track/TrackEditor.cpp
        hub/HubProcessor.cpp hub/HubEditor.cpp hub/ShareServer.cpp hub/ShareDirectory.cpp hub/ControlServer.cpp hub/HubRestApi.cpp app/AppCapture.cpp
        ${hearaside_common_sources})
    target_include_directories(hearaside_ui_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hearaside_ui_snapshot PRIVATE
        HEARASIDE_NO_PLUGIN_ENTRY=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0
        HEARASIDE_VERSION="${PROJECT_VERSION}")
    target_link_libraries(hearaside_ui_snapshot PRIVATE
        hearaside_assets hearaside_web ssengine juce::juce_audio_utils juce::juce_gui_extra
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(WIN32)   # App Audio editor + --test-app-audio
        target_sources(hearaside_ui_snapshot PRIVATE app/AppAudioProcessor.cpp app/LinkReceiver.cpp app/TakeRecorder.cpp)
        target_link_libraries(hearaside_ui_snapshot PRIVATE ole32 mmdevapi avrt winhttp)
    endif()
endif()

if(HEARASIDE_BUILD_TOOLS AND HEARASIDE_BUILD_TESTS)
    add_test(NAME rest_api COMMAND hearaside_ui_snapshot --test-rest)   # docs/rest-api.md
    add_test(NAME share_directory COMMAND hearaside_ui_snapshot --test-directory)   # permanent links, Hub side
endif()

# plugin-host-test: loads the built binaries like a DAW (VST3, and VST2 when enabled)
if(HEARASIDE_BUILD_TESTS)
    juce_add_console_app(hearaside_plugin_host_test PRODUCT_NAME "plugin-host-test")
    target_sources(hearaside_plugin_host_test PRIVATE ${CMAKE_SOURCE_DIR}/tools/plugin-host-test/main.cpp)
    target_compile_definitions(hearaside_plugin_host_test PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=$<IF:$<BOOL:${HEARASIDE_VST2_SDK}>,1,0>
        JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0)
    if(HEARASIDE_VST2_SDK)
        target_include_directories(hearaside_plugin_host_test PRIVATE "${HEARASIDE_VST2_SDK}")
    endif()
    target_link_libraries(hearaside_plugin_host_test PRIVATE ssbus juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics
        juce::juce_recommended_config_flags)
    add_dependencies(hearaside_plugin_host_test HearasideTrack_VST3 HearasideHub_VST3)
    set(app_vst3 "")
    set(app_vst2 "")
    if(WIN32)
        add_dependencies(hearaside_plugin_host_test HearasideAppAudio_VST3)
        set(app_vst3 $<TARGET_FILE:HearasideAppAudio_VST3>)
        if(HEARASIDE_VST2_SDK)
            add_dependencies(hearaside_plugin_host_test HearasideAppAudio_VST)
            set(app_vst2 $<TARGET_FILE:HearasideAppAudio_VST>)
        endif()
    endif()
    add_test(NAME plugin_host_vst3
             COMMAND hearaside_plugin_host_test $<TARGET_FILE:HearasideTrack_VST3> $<TARGET_FILE:HearasideHub_VST3> ${app_vst3})
    if(HEARASIDE_VST2_SDK)
        add_dependencies(hearaside_plugin_host_test HearasideTrack_VST HearasideHub_VST)
        add_test(NAME plugin_host_vst2
                 COMMAND hearaside_plugin_host_test $<TARGET_FILE:HearasideTrack_VST> $<TARGET_FILE:HearasideHub_VST> ${app_vst2})
    endif()
endif()
