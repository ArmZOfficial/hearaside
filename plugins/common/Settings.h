// Per-user UI preferences shared by every HEARASIDE window in this process
// (language, theme, size, motion, glass opacity). Stored in the user's app-data folder.
#pragma once

#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

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

    // UI size never picked by the user (fresh install): the first editor fits it to the screen.
    bool uiScaleChosen() const noexcept { return scaleChosen_; }
    void setUiScaleAuto(float);   // like setUiScale, but still counts as not chosen

    // Last window size per plug-in ("hub", "track", "app") in content units; {0, 0} = none yet.
    // Saved quietly (no change message: other windows don't need to relayout).
    juce::Point<int> windowSize(const juce::String& key) const;
    void setWindowSize(const juce::String& key, juce::Point<int> contentSize);

    // Permanent share links: the share web site (your Vercel deployment). Empty = links change every
    // time sharing starts (cloudflared quick tunnel). Default: the HEARASIDE_SHARE_BASE build option.
    juce::String shareBase() const;
    void setShareBase(const juce::String&);
    bool permanentLinks() const { return !flag("permanentLinksOff"); }
    void setPermanentLinks(bool on) { setFlag("permanentLinksOff", !on); }

    // Listen link password: if non-empty, web listeners must provide this password.
    juce::String listenPassword() const { return file_->getValue("listenPassword"); }
    void setListenPassword(const juce::String& p) { file_->setValue("listenPassword", p); }

    // Friend approval: require host confirmation in Hub before accepting new friend audio.
    bool friendApprovalRequired() const { return flag("friendApprovalRequired"); }
    void setFriendApprovalRequired(bool on) { setFlag("friendApprovalRequired", on); }

    // REST API on 127.0.0.1 (docs/rest-api.md): off until switched on; the key is per user.
    bool restApi() const { return flag("restApi"); }
    void setRestApi(bool on) { setFlag("restApi", on); }
    juce::String restApiKey() const { return file_->getValue("restApiKey"); }
    void setRestApiKey(const juce::String& k) { file_->setValue("restApiKey", k); }

    // Line up friends: friends slower than this are not waited for (ms, 100 .. 1000).
    int lineUpLimitMs() const { return juce::jlimit(100, 1000, file_->getIntValue("lineUpLimitMs", 600)); }
    void setLineUpLimitMs(int ms) { file_->setValue("lineUpLimitMs", juce::jlimit(100, 1000, ms)); }

    // Virtual mic: the output device the Hub plays the Stream Mix into (VB-Cable "CABLE Input" for
    // TikTok LIVE Studio). "" = off.
    juce::String virtualMicDevice() const { return file_->getValue("virtualMicDevice"); }
    void setVirtualMicDevice(const juce::String& d) { file_->setValue("virtualMicDevice", d); }

    // One-off facts about this user ("triedPreview", "startHidden"...), saved quietly.
    bool flag(const juce::String& key) const { return file_->getBoolValue("flag." + key, false); }
    void setFlag(const juce::String& key, bool on) { if (flag(key) != on) file_->setValue("flag." + key, on); }

    // Audio device state (e.g. for standalone HAS Karaoke app)
    juce::String audioDeviceState(const juce::String& key = "karaoke") const { return file_->getValue("audioDeviceState." + key); }
    void setAudioDeviceState(const juce::String& key, const juce::String& stateXml) { file_->setValue("audioDeviceState." + key, stateXml); }

    // Resolved theme (Auto follows the OS).
    bool isDark() const;

    // Called (message thread) after Language, Theme, UI size, Glass, Track colours or Reduce motion
    // changed: the account syncs them when "Sync appearance" is on (A6 item 1). Never anything that
    // changes the sound.
    static inline std::function<void(const Settings&)> appearanceChanged;

private:
    void save();

    std::unique_ptr<juce::PropertiesFile> file_;
    Language  language_ = Language::English;
    ThemeMode theme_ = ThemeMode::Auto;
    float     scale_ = 1.0f;
    bool      reduceMotion_ = false;
    float     glassAlpha_ = 0.62f;
    bool      hostColours_ = true;
    bool      scaleChosen_ = false;
};

// Bus used by new instances: "Main", or $HEARASIDE_DEFAULT_BUS (tests, a second DAW session).
inline juce::String defaultBusName() {
    const auto env = juce::SystemStats::getEnvironmentVariable("HEARASIDE_DEFAULT_BUS", {}).trim();
    return env.isNotEmpty() ? env : juce::String("Main");
}

// One instance per process while any HEARASIDE editor/processor holds it.
using SharedSettings = juce::SharedResourcePointer<Settings>;

} // namespace hearaside
