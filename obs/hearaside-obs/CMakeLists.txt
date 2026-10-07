# HEARASIDE OBS Source (GPLv2-compatible: links libobs). Reads the Stream Mix / stems from the
# HEARASIDE bus, compensates clock drift and converts the sample rate (ssdsp::StreamConsumer).

set(HEARASIDE_OBS_DIR "C:/Program Files/obs-studio" CACHE PATH
    "OBS Studio install folder (Windows: obs.dll is turned into an import library at configure time)")

if(WIN32)
    set(obs_dll "${HEARASIDE_OBS_DIR}/bin/64bit/obs.dll")
    if(NOT EXISTS "${obs_dll}")
        message(WARNING "HEARASIDE: ${obs_dll} not found - skipping the OBS plug-in (set HEARASIDE_OBS_DIR)")
        return()
    endif()
    get_filename_component(msvc_bin "${CMAKE_LINKER}" DIRECTORY)
    find_program(HEARASIDE_DUMPBIN dumpbin HINTS "${msvc_bin}" REQUIRED)

    # obs.dll exports -> obs.def -> obs.lib (OBS does not ship an import library)
    set(obs_def "${CMAKE_CURRENT_BINARY_DIR}/obs.def")
    set(obs_lib "${CMAKE_CURRENT_BINARY_DIR}/obs.lib")
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
    add_library(hearaside_libobs SHARED IMPORTED)
    set_target_properties(hearaside_libobs PROPERTIES IMPORTED_IMPLIB "${obs_lib}" IMPORTED_LOCATION "${obs_dll}")
else()
    find_package(libobs REQUIRED)
    add_library(hearaside_libobs ALIAS OBS::libobs)
endif()

# libobs' obs-config.h includes the build-generated obsconfig.h; plug-ins only need the defaults
set(obsconfig_dir "${CMAKE_CURRENT_BINARY_DIR}/obsconfig")
file(WRITE "${obsconfig_dir}/obsconfig.h"
    "#pragma once\n#define OBS_DATA_PATH \"../../data\"\n#define OBS_PLUGIN_PATH \"../../obs-plugins/64bit\"\n"
    "#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

# control dock page, embedded as bytes (served on 127.0.0.1 by the plug-in)
set(dock_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/dock_page.inc")
add_custom_command(
    OUTPUT "${dock_inc}"
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/dock.html -DOUT=${dock_inc} -DNAME=kDockPage
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS src/dock.html embed.cmake
    COMMENT "Embedding the HEARASIDE OBS control dock")

add_library(hearaside-obs MODULE src/plugin.cpp src/http_server.cpp src/remote.cpp "${dock_inc}")
target_include_directories(hearaside-obs PRIVATE ${SS_EXTERNAL}/obs-studio/libobs "${obsconfig_dir}"
                           "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(hearaside-obs PRIVATE ssdsp hearaside_libobs)
if(WIN32)
    target_link_libraries(hearaside-obs PRIVATE ws2_32 shell32)
endif()
target_compile_definitions(hearaside-obs PRIVATE HEARASIDE_VERSION="${PROJECT_VERSION}")
set_target_properties(hearaside-obs PROPERTIES PREFIX "")
if(MSVC)
    target_compile_options(hearaside-obs PRIVATE /wd4200 /wd4244 /wd4267)   # libobs headers
endif()
