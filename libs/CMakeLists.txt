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
    PUBLIC  OUTSIDE_SPEEX RANDOM_PREFIX=ssplit
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
add_library(ssengine STATIC
    ssengine/src/track_publisher.cpp
    ssengine/src/hub_engine.cpp)
target_include_directories(ssengine PUBLIC ssengine/include)
target_link_libraries(ssengine PUBLIC ssbus ssdsp)
