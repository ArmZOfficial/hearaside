#include "SettingsPanel.h"

namespace hearaside {

void FormRow::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().withTrimmedRight(float(controlWidth_ + 12));
    if (caption_.isEmpty()) {
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f));
        g.drawFittedText(tr(label_), r.toNearestInt(), juce::Justification::centredLeft, 2);
        return;
    }
    const auto f1 = uiFont(13.0f), f2 = uiFont(11.0f);
    auto block = r.withSizeKeepingCentre(r.getWidth(), f1.getHeight() + f2.getHeight() + 2.0f);
    g.setColour(p.ink);
    g.setFont(f1);
    g.drawText(tr(label_), block.removeFromTop(f1.getHeight()), juce::Justification::centredLeft, true);
    g.setColour(p.graphite);
    g.setFont(f2);
    g.drawFittedText(caption_, block.withTrimmedTop(2.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
}

void FormRow::resized() {
    const int h = juce::jmin(getHeight(), 30);
    control_.setBounds(getWidth() - controlWidth_, (getHeight() - h) / 2, controlWidth_, h);
}

UiSettingsPanel::UiSettingsPanel() {
    rows_.add(new FormRow(Str::LanguageWord, language_));
    rows_.add(new FormRow(Str::ThemeWord, theme_));
    rows_.add(new FormRow(Str::UiSize, size_));
    rows_.add(new FormRow(Str::TrackColours, colours_));
    rows_.add(new FormRow(Str::GlassOpacity, glass_));
    rows_.add(new FormRow(Str::ReduceMotion, motion_, 50));
    for (auto* r : rows_) addAndMakeVisible(r);

    glass_.setRange(0.35, 0.95, 0.01);
    language_.onChange = [this] {
        if (!refreshing_) settings_->setLanguage(language_.getSelectedId() == 2 ? Language::English : Language::Thai);
    };
    theme_.onChange = [this] {
        if (refreshing_) return;
        const int id = theme_.getSelectedId();
        settings_->setThemeMode(id == 2 ? ThemeMode::Light : id == 3 ? ThemeMode::Dark : ThemeMode::Auto);
    };
    size_.onChange = [this] {
        if (!refreshing_ && size_.getSelectedId() > 0) settings_->setUiScale(float(size_.getSelectedId()) / 100.0f);
    };
    colours_.onChange = [this] { if (!refreshing_) settings_->setHostColours(colours_.getSelectedId() != 2); };
    glass_.onValueChange = [this] { if (!refreshing_) settings_->setGlassAlpha(float(glass_.getValue())); };
    motion_.onClick = [this] { settings_->setReduceMotion(!settings_->reduceMotion()); };
    settings_->addChangeListener(this);
    refresh();
}

UiSettingsPanel::~UiSettingsPanel() { settings_->removeChangeListener(this); }

void UiSettingsPanel::refresh() {
    const juce::ScopedValueSetter<bool> svs(refreshing_, true);
    language_.clear(juce::dontSendNotification);
    language_.addItem(juce::String(juce::CharPointer_UTF8("ไทย")), 1);
    language_.addItem("English", 2);
    language_.setSelectedId(settings_->language() == Language::English ? 2 : 1, juce::dontSendNotification);

    theme_.clear(juce::dontSendNotification);
    theme_.addItem(tr(Str::ThemeAuto), 1);
    theme_.addItem(tr(Str::ThemeLight), 2);
    theme_.addItem(tr(Str::ThemeDark), 3);
    const auto m = settings_->themeMode();
    theme_.setSelectedId(m == ThemeMode::Light ? 2 : m == ThemeMode::Dark ? 3 : 1, juce::dontSendNotification);

    size_.clear(juce::dontSendNotification);
    for (int pct : { 100, 125, 150, 175, 200 }) size_.addItem(juce::String(pct) + " %", pct);
    size_.setSelectedId(juce::roundToInt(settings_->uiScale() * 100.0f), juce::dontSendNotification);

    colours_.clear(juce::dontSendNotification);
    colours_.addItem(tr(Str::ColoursFromDaw), 1);
    colours_.addItem(tr(Str::ColoursMono), 2);
    colours_.setSelectedId(settings_->hostColours() ? 1 : 2, juce::dontSendNotification);

    glass_.setValue(settings_->glassAlpha(), juce::dontSendNotification);
    motion_.setOn(settings_->reduceMotion(), false);
    motion_.setTitle(tr(Str::ReduceMotion));
    for (auto* r : rows_) r->repaint();
}

void UiSettingsPanel::resized() {
    auto r = getLocalBounds();
    for (auto* row : rows_) row->setBounds(r.removeFromTop(40));
}

} // namespace hearaside
