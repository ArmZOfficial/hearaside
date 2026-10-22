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
add_library(ssengine STATIC
    ssengine/src/track_publisher.cpp
    ssengine/src/hub_engine.cpp
    ssengine/src/hub_friends.cpp
    ssengine/src/friend_reader.cpp)
target_include_directories(ssengine PUBLIC ssengine/include)
target_link_libraries(ssengine PUBLIC ssbus ssdsp)

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

# ---- ssnet: network wire formats, codecs, and echo cancellation (MIT, no JUCE) -------------
add_library(ssnet STATIC
    ssnet/src/opus_codec.cpp
    ssnet/src/aec.cpp)
target_include_directories(ssnet PUBLIC ssnet/include)
target_link_libraries(ssnet PUBLIC opus ss_speex_echo)


