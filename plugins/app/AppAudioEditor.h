// HEARASIDE App Audio editor (prompt 3.7, design export "AppAudio", 440 x 600): header with the
// capture switch, the source picker (programs, whole computer, someone's link), status + meter,
// level and delay, and how to record the program with the DAW in use.
#pragma once

#include "AppAudioProcessor.h"
#include "ui/Backdrop.h"
#include "ui/EditorShell.h"
#include "ui/ScrollArea.h"
#include "ui/SettingsPanel.h"

namespace hearaside {

class AppAudioEditor : public EditorShell, private juce::Timer {
public:
    explicit AppAudioEditor(AppAudioProcessor&);
    ~AppAudioEditor() override;
    static constexpr int kWidth = 440, kHeight = 600;

    void showReceiveLink();   // "Receive someone's link…" (also used by ui-snapshot)

private:
    struct Content : juce::Component {
        explicit Content(AppAudioEditor& e) : ed(e) { setOpaque(true); }
        void paint(juce::Graphics& g) override { ed.paintContent(g); }
        void resized() override { ed.layout(); }
        AppAudioEditor& ed;
    };
    struct Body : juce::Component {
        explicit Body(AppAudioEditor& e) : ed(e) {}
        void paint(juce::Graphics& g) override { ed.paintBody(g); }
        AppAudioEditor& ed;
    };

    void paintContent(juce::Graphics&);
    void paintBody(juce::Graphics&);
    void layout();
    int layoutBody(int width);   // returns the height
    void timerCallback() override;
    void lookChanged() override;
    void refreshTexts();
    Menu sourceMenu();
    void showMore();
    struct Status { juce::String text; Dot dot; };
    Status status() const;

    AppAudioProcessor& proc_;
    Content content_ { *this };
    Body body_ { *this };
    Backdrop backdrop_;
    ScrollArea scroll_;

    IconButton more_ { icons::Icon::More };
    LabelledSwitch power_;
    SourcePicker source_;
    TextLine status_ { 12.5f, Weight::Regular, TextLine::Tone::Ink2 };
    MeterBar meter_ { 4.0f, true };
    LevelSlider level_;
    juce::Slider delay_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    EditableValue levelValue_ { EditableValue::Kind::Db, -30.0f, 12.0f }, delayValue_ { EditableValue::Kind::Ms, 0.0f, 500.0f };
    Field levelField_ { {}, &level_, 20, &levelValue_ };
    Field delayField_ { {}, &delay_, 20, &delayValue_ };
    Switch only_;
    NumberedSteps steps_;
    Disclosure otherDaws_;
    bool allDaws_ = false;
    std::unique_ptr<DbSliderLink> levelLink_, delayLink_;
    std::unique_ptr<ParamValueLink> levelValueLink_, delayValueLink_;
    FnChangeListener procListener_;

    // painted geometry (content / body coordinates)
    juce::Rectangle<float> header_, card_, wordmark_, title_, divider_, recordTitle_, onlyBox_, stepsBox_, stepsTitle_, others_, note_;
    std::tuple<bool, int, int, juce::String> lastKey_ {};
};

} // namespace hearaside
