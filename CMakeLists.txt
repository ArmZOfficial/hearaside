cmake_minimum_required(VERSION 3.25)

project(Hearaside VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
endif()

if(MSVC)
    # Static runtime: plug-ins must not depend on a VC++ redistributable being installed.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /MP /permissive- /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

option(HEARASIDE_BUILD_PLUGINS "Build the JUCE plug-ins (Track + Hub)" ON)
option(HEARASIDE_BUILD_OBS     "Build the OBS source plug-in" ON)
option(HEARASIDE_BUILD_TOOLS   "Build bus-inspector and other tools" ON)
option(HEARASIDE_BUILD_TESTS   "Build unit / integration tests" ON)
set(HEARASIDE_VST2_SDK "" CACHE PATH
    "Folder that contains pluginterfaces/vst2.x/aeffect.h from a *licensed* VST 2 SDK. Empty = no VST2 build.")

set(SS_EXTERNAL ${CMAKE_CURRENT_SOURCE_DIR}/external)

if(HEARASIDE_BUILD_TESTS)
    enable_testing()   # before any subdirectory registers tests (plugins/ adds the host tests)
endif()

add_subdirectory(libs)

if(HEARASIDE_BUILD_PLUGINS)
    add_subdirectory(${SS_EXTERNAL}/JUCE ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
    add_subdirectory(plugins)
endif()

if(HEARASIDE_BUILD_OBS)
    add_subdirectory(obs/hearaside-obs)
endif()

if(HEARASIDE_BUILD_TOOLS)
    add_subdirectory(tools/bus-inspector)
endif()

if(HEARASIDE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
