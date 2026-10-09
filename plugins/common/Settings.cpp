#include "Settings.h"
#include "Strings.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside {

namespace {
juce::PropertiesFile::Options fileOptions() {
    juce::PropertiesFile::Options o;
    // $HEARASIDE_SETTINGS_NAME: tools (ui-snapshot) keep their own file, never the user's
    const auto name = juce::SystemStats::getEnvironmentVariable("HEARASIDE_SETTINGS_NAME", {}).trim();
    o.applicationName = name.isNotEmpty() ? name : juce::String("settings");
    o.folderName = "HEARASIDE";
    o.filenameSuffix = ".xml";
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    o.millisecondsBeforeSaving = 500;
    o.processLock = nullptr;
    return o;
}
}

Settings::Settings() {
    file_ = std::make_unique<juce::PropertiesFile>(fileOptions());
    language_ = file_->getValue("language", "en") == "th" ? Language::Thai : Language::English;   // English for new users (S6)
    const auto t = file_->getValue("theme", "auto");
    theme_ = t == "light" ? ThemeMode::Light : t == "dark" ? ThemeMode::Dark : ThemeMode::Auto;
    scale_ = juce::jlimit(1.0f, 2.0f, (float) file_->getDoubleValue("uiScale", 1.0));
    scaleChosen_ = file_->containsKey("uiScale") && !file_->getBoolValue("uiScaleAuto", false);
    reduceMotion_ = file_->getBoolValue("reduceMotion", false);
    glassAlpha_ = juce::jlimit(0.35f, 0.95f, (float) file_->getDoubleValue("glassAlpha", 0.62));
    hostColours_ = file_->getBoolValue("hostColours", true);
    setCurrentLanguage(language_);
}

Settings::~Settings() {
    if (file_) file_->saveIfNeeded();
}

void Settings::save() {
    file_->setValue("language", language_ == Language::English ? "en" : "th");
    file_->setValue("theme", theme_ == ThemeMode::Light ? "light" : theme_ == ThemeMode::Dark ? "dark" : "auto");
    file_->setValue("uiScale", (double) scale_);
    file_->setValue("uiScaleAuto", !scaleChosen_);
    file_->setValue("reduceMotion", reduceMotion_);
    file_->setValue("glassAlpha", (double) glassAlpha_);
    file_->setValue("hostColours", hostColours_);
    sendChangeMessage();
}

void Settings::setLanguage(Language l) {
    if (l == language_) return;
    language_ = l;
    setCurrentLanguage(l);
    save();
}
void Settings::setThemeMode(ThemeMode m) { if (m != theme_) { theme_ = m; save(); } }
void Settings::setUiScale(float s) {
    s = juce::jlimit(1.0f, 2.0f, s);
    scaleChosen_ = true;
    if (s != scale_) { scale_ = s; save(); }
    else file_->setValue("uiScaleAuto", false);
}
void Settings::setUiScaleAuto(float s) {
    s = juce::jlimit(1.0f, 2.0f, s);
    if (s != scale_) { scale_ = s; save(); }
}

#ifndef HEARASIDE_SHARE_BASE
#define HEARASIDE_SHARE_BASE ""
#endif

juce::String Settings::shareBase() const {
    return file_->containsKey("shareBase") ? file_->getValue("shareBase") : juce::String(HEARASIDE_SHARE_BASE);
}
void Settings::setShareBase(const juce::String& url) {
    auto u = url.trim().trimCharactersAtEnd("/");
    if (u.isNotEmpty() && !u.startsWithIgnoreCase("https://") && !u.startsWithIgnoreCase("http://")) u = "https://" + u;
    file_->setValue("shareBase", u);
}

juce::Point<int> Settings::windowSize(const juce::String& key) const {
    const auto v = juce::StringArray::fromTokens(file_->getValue("window." + key), "x", "");
    if (v.size() != 2) return {};
    return { v[0].getIntValue(), v[1].getIntValue() };
}
void Settings::setWindowSize(const juce::String& key, juce::Point<int> s) {
    if (key.isEmpty() || s.x <= 0 || s.y <= 0) return;
    file_->setValue("window." + key, juce::String(s.x) + "x" + juce::String(s.y));
}
void Settings::setReduceMotion(bool b) { if (b != reduceMotion_) { reduceMotion_ = b; save(); } }
void Settings::setGlassAlpha(float a) {
    a = juce::jlimit(0.35f, 0.95f, a);
    if (a != glassAlpha_) { glassAlpha_ = a; save(); }
}
void Settings::setHostColours(bool b) { if (b != hostColours_) { hostColours_ = b; save(); } }

bool Settings::isDark() const {
    switch (theme_) {
        case ThemeMode::Light: return false;
        case ThemeMode::Dark:  return true;
        case ThemeMode::Auto:  break;
    }
    return juce::Desktop::getInstance().isDarkModeActive();
}

} // namespace hearaside
