cmake_minimum_required(VERSION 3.25)
project(HearasideHostTest VERSION 1.0.10 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(HEARASIDE_VST2_SDK "" CACHE PATH "Licensed VST2 SDK for hosting VST2 plug-ins")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_subdirectory(../../external/JUCE JUCE EXCLUDE_FROM_ALL)
if(HEARASIDE_VST2_SDK)
    juce_set_vst2_sdk_path("${HEARASIDE_VST2_SDK}")
endif()
juce_add_console_app(hearaside_plugin_host_test PRODUCT_NAME "plugin-host-test")
target_sources(hearaside_plugin_host_test PRIVATE main.cpp
    ../../libs/ssbus/src/bus.cpp ../../libs/ssbus/src/shm_win.cpp ../../libs/ssbus/src/shm_posix.cpp)
target_include_directories(hearaside_plugin_host_test PRIVATE ../../libs/ssbus/include)
target_compile_definitions(hearaside_plugin_host_test PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=$<BOOL:${HEARASIDE_VST2_SDK}>
    JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0)
target_link_libraries(hearaside_plugin_host_test PRIVATE
    juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics
    juce::juce_recommended_config_flags)
if(WIN32)
    target_link_libraries(hearaside_plugin_host_test PRIVATE advapi32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(hearaside_plugin_host_test PRIVATE rt pthread)
endif()
if(MSVC)
    target_compile_options(hearaside_plugin_host_test PRIVATE /utf-8 /permissive-)
endif()
