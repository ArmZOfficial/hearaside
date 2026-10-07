#include "Settings.h"
#include "Strings.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside {

namespace {
juce::PropertiesFile::Options fileOptions() {
    juce::PropertiesFile::Options o;
    o.applicationName = "settings";
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
    language_ = file_->getValue("language", "th") == "en" ? Language::English : Language::Thai;
    const auto t = file_->getValue("theme", "auto");
    theme_ = t == "light" ? ThemeMode::Light : t == "dark" ? ThemeMode::Dark : ThemeMode::Auto;
    scale_ = juce::jlimit(1.0f, 2.0f, (float) file_->getDoubleValue("uiScale", 1.0));
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
    if (s != scale_) { scale_ = s; save(); }
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
