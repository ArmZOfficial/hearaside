// UI preferences shared by Track and Hub (language, theme, size, motion, glass, colours).
// Lives inside a CallOutBox; extra plug-in specific rows can be stacked above it.
#pragma once

#include "Components.h"
#include "../Settings.h"
#include "../Strings.h"

namespace hearaside {

// A label on the left, a control on the right; used by every settings-style panel.
class FormRow : public juce::Component {
public:
    FormRow(Str label, juce::Component& control, int controlWidth = 150)
        : label_(label), control_(control), controlWidth_(controlWidth) { addAndMakeVisible(control_); }
    void setCaption(const juce::String& c) { caption_ = c; repaint(); }
    void paint(juce::Graphics& g) override;
    void resized() override;
private:
    Str label_;
    juce::String caption_;
    juce::Component& control_;
    int controlWidth_;
};

class UiSettingsPanel : public juce::Component, private juce::ChangeListener {
public:
    UiSettingsPanel();
    ~UiSettingsPanel() override;
    int idealHeight() const { return 6 * 40; }
    void resized() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();

    SharedSettings settings_;
    juce::ComboBox language_, theme_, size_, colours_;
    Switch motion_;
    juce::Slider glass_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::OwnedArray<FormRow> rows_;
    bool refreshing_ = false;
};

} // namespace hearaside
