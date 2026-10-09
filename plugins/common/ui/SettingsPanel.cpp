#include "SettingsPanel.h"

namespace hearaside {

namespace {
constexpr float kPadY = 16.0f, kGap = 16.0f;
const juce::Font titleFont() { return uiFont(14.0f, Weight::Medium); }
const juce::Font captionFont() { return uiFont(12.0f); }
}

SettingRow::SettingRow(const juce::String& title, const juce::String& caption, juce::Component* control, int controlW, int controlH)
    : title_(title), caption_(caption), control_(control), controlW_(controlW), controlH_(controlH) {
    setTitle(title);
    if (control_ != nullptr) addAndMakeVisible(*control_);
}

int SettingRow::idealHeight(int width) const {
    const bool st = stacked(width);
    const float textW = st ? float(width) : float(width) - float(controlW_) - kGap;
    float text = titleFont().getHeight();
    if (caption_.isNotEmpty()) text += 3.0f + wrappedHeight(captionFont(), caption_, textW, 3.0f);
    const float h = st ? text + (control_ != nullptr ? 10.0f + float(controlH_) : 0.0f) : juce::jmax(text, float(controlH_));
    return juce::roundToInt(h + kPadY * 2.0f);
}

void SettingRow::resized() {
    if (control_ == nullptr) return;
    if (stacked(getWidth())) {
        control_->setBounds(0, getHeight() - juce::roundToInt(kPadY) - controlH_, getWidth(), controlH_);
    } else {
        control_->setBounds(getWidth() - controlW_, (getHeight() - controlH_) / 2, controlW_, controlH_);
    }
}

void SettingRow::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const bool st = stacked(getWidth());
    const float textW = st ? float(getWidth()) : float(getWidth()) - float(controlW_) - kGap;
    float textH = titleFont().getHeight() + (caption_.isNotEmpty() ? 3.0f + wrappedHeight(captionFont(), caption_, textW, 3.0f) : 0.0f);
    float y = st ? kPadY : (float(getHeight()) - textH) * 0.5f;
    g.setColour(hasTitleColour_ ? titleColour_ : p.ink);
    g.setFont(titleFont());
    g.drawText(title_, juce::Rectangle<float>(0.0f, y, textW, titleFont().getHeight()), juce::Justification::centredLeft, true);
    y += titleFont().getHeight() + 3.0f;
    if (caption_.isNotEmpty()) drawWrapped(g, caption_, captionFont(), p.graphite, { 0.0f, y, textW, 400.0f }, 3.0f);
    if (divider_) {
        g.setColour(p.hairline2);
        g.fillRect(0.0f, float(getHeight()) - 1.0f, float(getWidth()), 1.0f);
    }
}

Field::Field(const juce::String& title, juce::Component* control, int controlH, juce::Component* value, int valueW)
    : title_(title), control_(control), value_(value), controlH_(controlH), valueW_(valueW) {
    setTitle(title);
    if (control_ != nullptr) addAndMakeVisible(*control_);
    if (value_ != nullptr) addAndMakeVisible(*value_);
}

int Field::idealHeight(int width) const {
    float h = 26.0f + 6.0f + float(controlH_);
    if (caption_.isNotEmpty()) h += 6.0f + wrappedHeight(captionFont(), caption_, float(width), 6.0f);
    return juce::roundToInt(h);
}

void Field::resized() {
    if (value_ != nullptr) value_->setBounds(getWidth() - valueW_, 0, valueW_, 26);
    if (control_ != nullptr) control_->setBounds(0, 32, getWidth(), controlH_);
}

void Field::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    g.setColour(p.ink);
    g.setFont(uiFont(13.0f, Weight::Medium));
    g.drawText(title_, juce::Rectangle<float>(0.0f, 0.0f, float(getWidth() - (value_ != nullptr ? valueW_ : 0)), 26.0f), juce::Justification::centredLeft, true);
    if (caption_.isNotEmpty())
        drawWrapped(g, caption_, captionFont(), p.graphite, { 0.0f, 32.0f + float(controlH_) + 6.0f, float(getWidth()), 200.0f }, 6.0f);
}

SettingRow& SettingList::add(std::unique_ptr<SettingRow> r) {
    addAndMakeVisible(*r);
    return *rows_.add(r.release());
}

int SettingList::idealHeight(int width) const {
    int h = 0;
    for (auto* r : rows_) if (r->isVisible()) h += r->idealHeight(width);
    return h;
}

void SettingList::resized() {
    int y = 0;
    SettingRow* last = nullptr;
    for (auto* r : rows_) {
        if (!r->isVisible()) continue;
        const int h = r->idealHeight(getWidth());
        r->setBounds(0, y, getWidth(), h);
        r->setDivider(true);
        last = r;
        y += h;
    }
    if (last != nullptr) last->setDivider(false);
}

// ---------------------------------------------------------------------------------------------
AppearanceSettings::AppearanceSettings() {
    addAndMakeVisible(list_);
    for (auto* sc : { &language_, &theme_, &colours_ }) sc->setTall(true);
    language_.onChange = [this](int i) { if (!refreshing_) settings_->setLanguage(i == 0 ? Language::English : Language::Thai); };
    theme_.onChange = [this](int i) { if (!refreshing_) settings_->setThemeMode(i == 1 ? ThemeMode::Light : i == 2 ? ThemeMode::Dark : ThemeMode::Auto); };
    colours_.onChange = [this](int i) { if (!refreshing_) settings_->setHostColours(i == 0); };
    size_.onChange = [this](int i) { if (!refreshing_) settings_->setUiScale(1.0f + 0.25f * float(i)); };
    glass_.slider.setRange(0.35, 0.95, 0.01);
    glass_.slider.setDoubleClickReturnValue(true, 0.62);
    glass_.slider.setTooltip({});
    glass_.slider.onValueChange = [this] { if (!refreshing_) settings_->setGlassAlpha(float(glass_.slider.getValue())); };
    glass_.value.onCommit = [this](float v) { settings_->setGlassAlpha(v); };
    motion_.onClick = [this] { settings_->setReduceMotion(!settings_->reduceMotion()); };
    settings_->addChangeListener(this);
    build();
    refresh();
}

AppearanceSettings::~AppearanceSettings() { settings_->removeChangeListener(this); }

void AppearanceSettings::build() {
    list_.clear();
    list_.add(std::make_unique<SettingRow>(tr(Str::LanguageWord), tr(Str::LanguageCap), &language_, 280, 40));
    list_.add(std::make_unique<SettingRow>(tr(Str::ThemeWord), tr(Str::ThemeCap), &theme_, 280, 40));
    list_.add(std::make_unique<SettingRow>(tr(Str::UiSize), tr(Str::UiSizeCap), &size_, 140, 38));
    list_.add(std::make_unique<SettingRow>(tr(Str::GlassOpacity), tr(Str::GlassCap), &glass_, 280, 26));
    list_.add(std::make_unique<SettingRow>(tr(Str::TrackColours), tr(Str::ColoursCap), &colours_, 280, 40));
    list_.add(std::make_unique<SettingRow>(tr(Str::ReduceMotion), tr(Str::ReduceMotionCap), &motion_, 44, 26));
}

void AppearanceSettings::refresh() {
    const juce::ScopedValueSetter<bool> svs(refreshing_, true);
    language_.setSegments({ { "English" }, { juce::String(juce::CharPointer_UTF8("ไทย")) } });
    language_.setSelected(settings_->language() == Language::English ? 0 : 1);
    theme_.setSegments({ { tr(Str::ThemeAuto) }, { tr(Str::ThemeLight) }, { tr(Str::ThemeDark) } });
    const auto m = settings_->themeMode();
    theme_.setSelected(m == ThemeMode::Light ? 1 : m == ThemeMode::Dark ? 2 : 0);
    colours_.setSegments({ { tr(Str::ColoursFromDaw) }, { tr(Str::ColoursMono) } });
    colours_.setSelected(settings_->hostColours() ? 0 : 1);
    size_.setItems({ "100%", "125%", "150%", "175%", "200%" });
    size_.setSelected(juce::jlimit(0, 4, juce::roundToInt((settings_->uiScale() - 1.0f) / 0.25f)));
    glass_.slider.setValue(settings_->glassAlpha(), juce::dontSendNotification);
    glass_.value.setValue(settings_->glassAlpha());
    motion_.setOn(settings_->reduceMotion(), false);
    motion_.setTitle(tr(Str::ReduceMotion));
    // texts follow the language
    const auto& rows = list_.rows();
    if (rows.size() == 6) {
        rows[0]->setTexts(tr(Str::LanguageWord), tr(Str::LanguageCap));
        rows[1]->setTexts(tr(Str::ThemeWord), tr(Str::ThemeCap));
        rows[2]->setTexts(tr(Str::UiSize), tr(Str::UiSizeCap));
        rows[3]->setTexts(tr(Str::GlassOpacity), tr(Str::GlassCap));
        rows[4]->setTexts(tr(Str::TrackColours), tr(Str::ColoursCap));
        rows[5]->setTexts(tr(Str::ReduceMotion), tr(Str::ReduceMotionCap));
    }
    list_.resized();
}

} // namespace hearaside
