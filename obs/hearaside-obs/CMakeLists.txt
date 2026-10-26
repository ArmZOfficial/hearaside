# HEARASIDE OBS Source (GPLv2-compatible: links libobs). Reads the Stream Mix / stems from the
# HEARASIDE bus, compensates clock drift and converts the sample rate (ssdsp::StreamConsumer).
# Built twice on Windows: for OBS Studio, and for Streamlabs Desktop, whose libobs is a fork with a different
# obs_source_info layout (extra callbacks mid-struct), so a plug-in built for one crashes the other.

set(HEARASIDE_OBS_DIR "C:/Program Files/obs-studio" CACHE PATH
    "OBS Studio install folder (Windows: obs.dll is turned into an import library at configure time)")
set(HEARASIDE_STREAMLABS_DIR "C:/Program Files/Streamlabs OBS" CACHE PATH
    "Streamlabs Desktop install folder (its obs.dll); the Streamlabs flavour is skipped when it is missing")
set(HEARASIDE_STREAMLABS_TAG "31.1.2sl19b5" CACHE STRING
    "stream-labs/obs-studio tag of that obs.dll and of external/obs-studio-streamlabs (scripts/fetch-deps.ps1)")

# libobs' obs-config.h includes the build-generated obsconfig.h; plug-ins only need the defaults
set(obsconfig_dir "${CMAKE_CURRENT_BINARY_DIR}/obsconfig")
file(WRITE "${obsconfig_dir}/obsconfig.h"
    "#pragma once\n#define OBS_DATA_PATH \"../../data\"\n#define OBS_PLUGIN_PATH \"../../obs-plugins/64bit\"\n"
    "#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

# obs.dll exports -> <name>.def -> <name>.lib (neither host ships an import library)
function(hearaside_import_libobs name obs_dll)
    get_filename_component(msvc_bin "${CMAKE_LINKER}" DIRECTORY)
    find_program(HEARASIDE_DUMPBIN dumpbin HINTS "${msvc_bin}" REQUIRED)
    set(obs_def "${CMAKE_CURRENT_BINARY_DIR}/${name}.def")
    set(obs_lib "${CMAKE_CURRENT_BINARY_DIR}/${name}.lib")
    if(NOT EXISTS "${obs_lib}" OR "${obs_dll}" IS_NEWER_THAN "${obs_lib}")
        execute_process(COMMAND "${HEARASIDE_DUMPBIN}" /nologo /exports "${obs_dll}"
                        OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "dumpbin failed on ${obs_dll}")
        endif()
        string(REPLACE "\n" ";" lines "${dump}")
        set(def "LIBRARY obs\nEXPORTS\n")
        foreach(line IN LISTS lines)
            if(line MATCHES "^ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +([A-Za-z_][A-Za-z0-9_]*)")
                string(APPEND def "    ${CMAKE_MATCH_1}\n")
            endif()
        endforeach()
        file(WRITE "${obs_def}" "${def}")
        execute_process(COMMAND "${CMAKE_AR}" /nologo /def:${obs_def} /machine:x64 /out:${obs_lib} RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "lib.exe could not create ${obs_lib}")
        endif()
    endif()
    add_library(${name} SHARED IMPORTED)
    set_target_properties(${name} PROPERTIES IMPORTED_IMPLIB "${obs_lib}" IMPORTED_LOCATION "${obs_dll}")
endfunction()

function(hearaside_obs_plugin target libobs include_dir)
    add_library(${target} MODULE src/plugin.cpp src/remote.cpp)
    target_include_directories(${target} PRIVATE "${include_dir}" "${obsconfig_dir}")
    target_link_libraries(${target} PRIVATE ssdsp ${libobs})
    target_compile_definitions(${target} PRIVATE HEARASIDE_VERSION="${PROJECT_VERSION}")
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME hearaside-obs)
    if(MSVC)
        target_compile_options(${target} PRIVATE /wd4200 /wd4244 /wd4267)   # libobs headers
    endif()
endfunction()

if(NOT WIN32)
    find_package(libobs REQUIRED)
    hearaside_obs_plugin(hearaside-obs OBS::libobs "${SS_EXTERNAL}/obs-studio/libobs")
    return()
endif()

set(obs_dll "${HEARASIDE_OBS_DIR}/bin/64bit/obs.dll")
if(EXISTS "${obs_dll}")
    hearaside_import_libobs(hearaside_libobs "${obs_dll}")
    hearaside_obs_plugin(hearaside-obs hearaside_libobs "${SS_EXTERNAL}/obs-studio/libobs")
else()
    message(WARNING "HEARASIDE: ${obs_dll} not found - skipping the OBS plug-in (set HEARASIDE_OBS_DIR)")
endif()

# Streamlabs flavour: only when the installed Streamlabs runs the fork tag the headers were fetched for
set(slobs_dll "${HEARASIDE_STREAMLABS_DIR}/resources/app.asar.unpacked/node_modules/obs-studio-node/obs.dll")
set(slobs_include "${SS_EXTERNAL}/obs-studio-streamlabs/libobs")
if(NOT EXISTS "${slobs_dll}" OR NOT EXISTS "${slobs_include}/obs-source.h")
    message(STATUS "HEARASIDE: Streamlabs Desktop or external/obs-studio-streamlabs missing - no Streamlabs plug-in")
    return()
endif()
file(STRINGS "${slobs_dll}" slobs_tags REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+sl[0-9a-z]+$")
if(NOT HEARASIDE_STREAMLABS_TAG IN_LIST slobs_tags)
    message(WARNING "HEARASIDE: Streamlabs runs libobs ${slobs_tags}, the headers are ${HEARASIDE_STREAMLABS_TAG} - "
                    "no Streamlabs plug-in (update the tag in scripts/fetch-deps.ps1 and HEARASIDE_STREAMLABS_TAG)")
    return()
endif()
hearaside_import_libobs(hearaside_libobs_streamlabs "${slobs_dll}")
hearaside_obs_plugin(hearaside-obs-streamlabs hearaside_libobs_streamlabs "${slobs_include}")
set_target_properties(hearaside-obs-streamlabs PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/streamlabs")
set(HEARASIDE_STREAMLABS_PLUGIN ON PARENT_SCOPE)
message(STATUS "HEARASIDE: building the Streamlabs plug-in for libobs ${HEARASIDE_STREAMLABS_TAG}")
