// Per-user UI preferences shared by every HEARASIDE window in this process
// (language, theme, size, motion, glass opacity). Stored in the user's app-data folder.
#pragma once

#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

namespace hearaside {

enum class Language { Thai, English };
enum class ThemeMode { Auto, Light, Dark };

class Settings : public juce::ChangeBroadcaster {
public:
    Settings();
    ~Settings() override;

    Language  language() const noexcept { return language_; }
    ThemeMode themeMode() const noexcept { return theme_; }
    float     uiScale() const noexcept { return scale_; }          // 1.0 .. 2.0
    bool      reduceMotion() const noexcept { return reduceMotion_; }
    float     glassAlpha() const noexcept { return glassAlpha_; }  // 0.35 .. 0.95
    bool      hostColours() const noexcept { return hostColours_; } // track colours from the DAW

    void setLanguage(Language);
    void setThemeMode(ThemeMode);
    void setUiScale(float);
    void setReduceMotion(bool);
    void setGlassAlpha(float);
    void setHostColours(bool);

    // Resolved theme (Auto follows the OS).
    bool isDark() const;

private:
    void save();

    std::unique_ptr<juce::PropertiesFile> file_;
    Language  language_ = Language::Thai;
    ThemeMode theme_ = ThemeMode::Auto;
    float     scale_ = 1.0f;
    bool      reduceMotion_ = false;
    float     glassAlpha_ = 0.62f;
    bool      hostColours_ = true;
};

// Bus used by new instances: "Main", or $HEARASIDE_DEFAULT_BUS (tests, a second DAW session).
inline juce::String defaultBusName() {
    const auto env = juce::SystemStats::getEnvironmentVariable("HEARASIDE_DEFAULT_BUS", {}).trim();
    return env.isNotEmpty() ? env : juce::String("Main");
}

// One instance per process while any HEARASIDE editor/processor holds it.
using SharedSettings = juce::SharedResourcePointer<Settings>;

} // namespace hearaside
