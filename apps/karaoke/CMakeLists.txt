# HAS Karaoke (.exe): Standalone Windows Karaoke App (JUCE GUI app)
juce_add_gui_app(HearasideKaraoke
    PRODUCT_NAME "HAS Karaoke"
    COMPANY_NAME "HEARASIDE"
    COMPANY_WEBSITE "https://github.com/ArmZOfficial/hearaside"
    BUNDLE_ID "com.hearaside.karaoke"
    VERSION "${PROJECT_VERSION}"
)

target_sources(HearasideKaraoke PRIVATE
    Main.cpp
    MainWindow.h
    MainWindow.cpp
    AudioSettings.h
    AudioSettings.cpp
    VocalChain.h
    VocalChain.cpp
    JoinClient.h
    JoinClient.cpp
    PreflightChecker.h
    PreflightChecker.cpp
    MusicPlayer.h
    MusicPlayer.cpp
    RoomHost.h
    RoomHost.cpp
    UpdateChecker.h
    UpdateChecker.cpp
    ${CMAKE_SOURCE_DIR}/plugins/app/AppCapture.cpp
)

target_include_directories(HearasideKaraoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/plugins/app
)

target_compile_definitions(HearasideKaraoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    HEARASIDE_VERSION="${PROJECT_VERSION}"
    HEARASIDE_SHARE_BASE="${HEARASIDE_SHARE_BASE}"
)

set(HEARASIDE_ASIO_SDK "" CACHE PATH "Path to Steinberg ASIO SDK directory")
if(HEARASIDE_ASIO_SDK)
    target_compile_definitions(HearasideKaraoke PRIVATE JUCE_ASIO=1)
    target_include_directories(HearasideKaraoke PRIVATE "${HEARASIDE_ASIO_SDK}")
endif()

target_link_libraries(HearasideKaraoke PRIVATE
    hearaside_common
    ssengine
    ssdsp
    ssnet
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

if(WIN32)
    target_link_libraries(HearasideKaraoke PRIVATE
        ole32 mmdevapi avrt winhttp crypt32
    )
endif()
