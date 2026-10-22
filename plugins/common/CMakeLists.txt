# HEARASIDE common static library: UI, Strings, Settings, Account, ShareServer
add_library(hearaside_common STATIC
    Settings.cpp
    Strings.cpp
    ValueText.cpp
    ui/LookAndFeel.cpp
    ui/Focus.cpp
    ui/Controls.cpp
    ui/Overlay.cpp
    ui/ScrollArea.cpp
    ui/Backdrop.cpp
    ui/Components.cpp
    ui/Icons.cpp
    ui/SettingsPanel.cpp
    AccountManager.cpp
    ui/AccountPanel.cpp
    ui/qrcodegen.cpp
    ShareServer.cpp
)

set_target_properties(hearaside_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(hearaside_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

target_compile_definitions(hearaside_common PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    HEARASIDE_VERSION="${PROJECT_VERSION}"
    HEARASIDE_SHARE_BASE="${HEARASIDE_SHARE_BASE}"
)

target_link_libraries(hearaside_common PUBLIC
    hearaside_assets
    hearaside_web
    ssbus
    ssnet
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

if(WIN32)
    target_link_libraries(hearaside_common PUBLIC
        ole32 mmdevapi avrt winhttp crypt32
    )
endif()
