#pragma once

#include "TrackProcessor.h"
#include "ui/Backdrop.h"
#include "ui/Components.h"
#include "ui/EditorShell.h"

namespace hearaside {

class TrackEditor : public EditorShell, private juce::Timer {
public:
    explicit TrackEditor(TrackProcessor&);
    ~TrackEditor() override;

    static constexpr int kWidth = 380, kHeight = 664;

private:
    struct Content : juce::Component {
        explicit Content(TrackEditor& e) : ed(e) { setOpaque(true); }
        void paint(juce::Graphics& g) override { ed.paintContent(g); }
        void resized() override { ed.layout(); }
        TrackEditor& ed;
    };

    void paintContent(juce::Graphics&);
    void layout();
    void timerCallback() override;
    FnChangeListener procListener_;
    void lookChanged() override;
    void refreshStatus();
    void refreshTexts();
    void toggle(const char* paramId);
    void showFineSettings();
    bool paramOn(const char* id) const;

    TrackProcessor& proc_;
    Content content_ { *this };
    Backdrop backdrop_;

    StatusChip chip_ { 30.0f };
    IconButton fineButton_ { icons::Icon::Sliders };
    Banner banner_;
    MeterBar inputMeter_ { 4.0f };
    ToggleRow monRow_ { icons::Icon::Headphones };
    ToggleRow strRow_ { icons::Icon::Broadcast };
    LevelSlider headphoneSlider_, viewersSlider_;
    juce::Slider delaySlider_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label delayValue_;
    juce::Label footer_;
    std::unique_ptr<DbSliderLink> headphoneLink_, viewersLink_;
    std::unique_ptr<juce::SliderParameterAttachment> delayAttachment_;

    // painted areas
    juce::Rectangle<float> card_, wordmark_, nameRow_, inputLabel_, summary_;
    juce::Rectangle<float> headLabel_, viewLabel_, delayLabel_;
    bool lastMon_ = false, lastStr_ = false;
    int slowTick_ = 0;
    juce::String lastName_;
};

} // namespace hearaside
