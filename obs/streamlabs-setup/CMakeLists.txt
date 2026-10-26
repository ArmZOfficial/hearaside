# hearaside-streamlabs.exe: puts the HEARASIDE source into Streamlabs Desktop (installer component "streamlabs")
juce_add_console_app(hearaside-streamlabs PRODUCT_NAME "hearaside-streamlabs")
target_sources(hearaside-streamlabs PRIVATE main.cpp)
target_compile_definitions(hearaside-streamlabs PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    HEARASIDE_STREAMLABS_TAG="${HEARASIDE_STREAMLABS_TAG}")
target_link_libraries(hearaside-streamlabs PRIVATE
    juce::juce_core juce::juce_cryptography
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
