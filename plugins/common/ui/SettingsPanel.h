// Settings rows (prompt 3.4 "Settings"): name + description on the left, a control 280 wide on
// the right, 16 px above and below, a hairline between rows. In narrow windows the control goes
// under the text. AppearanceSettings is the "Appearance" group shared by Hub, Track and App Audio.
#pragma once

#include "Controls.h"
#include "Overlay.h"
#include "../Settings.h"
#include "../Strings.h"

namespace hearaside {

class SettingRow : public juce::Component {
public:
    SettingRow(const juce::String& title, const juce::String& caption, juce::Component* control, int controlW = 280, int controlH = 36);
    void setTexts(const juce::String& title, const juce::String& caption) { title_ = title; caption_ = caption; setTitle(title); repaint(); }
    void setTitleColour(juce::Colour c) { titleColour_ = c; hasTitleColour_ = true; repaint(); }
    void setDivider(bool d) { divider_ = d; repaint(); }
    int idealHeight(int width) const;
    bool stacked(int width) const { return width < 520; }
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    juce::String title_, caption_;
    juce::Component* control_;
    int controlW_, controlH_;
    bool divider_ = true, hasTitleColour_ = false;
    juce::Colour titleColour_;
};

// A labelled control stacked top to bottom (the plug-ins' fields): "Headphone level   0.0 dB",
// the control, then an optional grey caption.
class Field : public juce::Component {
public:
    Field(const juce::String& title, juce::Component* control, int controlH, juce::Component* value = nullptr, int valueW = 72);
    void setTexts(const juce::String& title, const juce::String& caption) { title_ = title; caption_ = caption; repaint(); }
    void setControlHeight(int h) { controlH_ = h; }
    int idealHeight(int width) const;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    juce::String title_, caption_;
    juce::Component *control_, *value_;
    int controlH_, valueW_;
};

// A column of rows; height follows the rows.
class SettingList : public juce::Component {
public:
    SettingRow& add(std::unique_ptr<SettingRow>);
    void clear() { rows_.clear(); }
    int idealHeight(int width) const;
    void resized() override;
    juce::OwnedArray<SettingRow>& rows() noexcept { return rows_; }
private:
    juce::OwnedArray<SettingRow> rows_;
};

// Language, theme, UI size, glass, track colours, reduce motion (follows the shared Settings).
class AppearanceSettings : public juce::Component, private juce::ChangeListener {
public:
    AppearanceSettings();
    ~AppearanceSettings() override;
    int idealHeight(int width) const { return list_.idealHeight(width); }
    void resized() override { list_.setBounds(getLocalBounds()); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();
    void build();

    SharedSettings settings_;
    SettingList list_;
    SegmentedControl language_, theme_, colours_;
    Dropdown size_;
    struct Glass : juce::Component {
        LevelSlider slider { 0.95 };
        EditableValue value { EditableValue::Kind::Percent, 0.35f, 0.95f };
        Glass() { addAndMakeVisible(slider); addAndMakeVisible(value); }
        void resized() override {
            auto r = getLocalBounds();
            value.setBounds(r.removeFromRight(50));
            slider.setBounds(r.withTrimmedRight(10).withSizeKeepingCentre(r.getWidth() - 10, 20));
        }
    } glass_;
    Switch motion_;
    bool refreshing_ = false;
};

} // namespace hearaside
