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
)

target_include_directories(HearasideKaraoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(HearasideKaraoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    HEARASIDE_VERSION="${PROJECT_VERSION}"
    HEARASIDE_SHARE_BASE="${HEARASIDE_SHARE_BASE}"
)

target_link_libraries(HearasideKaraoke PRIVATE
    hearaside_common
    ssengine
    ssdsp
    juce::juce_audio_devices
    juce::juce_audio_utils
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
