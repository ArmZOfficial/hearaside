// HEARASIDE Track editor (prompt 3.6, design export "TrackPlugin", 380 x 640): header card with
// the Hub status, the track name (double-click to rename), two big switches, the summary
// sentence, levels and the viewers delay in a scroll area, and Fine settings as a sheet over
// the card.
#pragma once

#include "TrackProcessor.h"
#include "ui/Backdrop.h"
#include "ui/EditorShell.h"
#include "ui/ScrollArea.h"
#include "ui/SettingsPanel.h"

namespace hearaside {

class TrackEditor : public EditorShell, private juce::Timer {
public:
    explicit TrackEditor(TrackProcessor&);
    ~TrackEditor() override;

    static constexpr int kWidth = 380, kHeight = 640;

    void openFineSettings(bool open);   // also used by ui-snapshot
    bool fineSettingsOpen() const { return sheet_.isVisible(); }

private:
    struct Content : juce::Component {
        explicit Content(TrackEditor& e) : ed(e) { setOpaque(true); }
        void paint(juce::Graphics& g) override { ed.paintContent(g); }
        void resized() override { ed.layout(); }
        TrackEditor& ed;
    };
    struct Body : juce::Component {
        void resized() override;
        std::vector<Field*> fields;
    };
    // Fine settings: a sheet over the card (prompt 3.6 item 9)
    struct Sheet : juce::Component {
        explicit Sheet(TrackEditor& e);
        void paint(juce::Graphics&) override;
        void resized() override;
        void refreshTexts();
        void sync();   // from the processor (name, bus, stem names)
        TrackEditor& ed;
        BackButton back;
        ScrollArea scroll;
        Body body;
        TextField name, bus;
        PanSlider pan { true };
        EditableValue panValue { EditableValue::Kind::Pan, -100.0f, 100.0f };
        Dropdown stem;
        Switch solo;
        struct SoloRow : juce::Component {
            Switch* sw = nullptr;
            juce::String title, caption;
            void paint(juce::Graphics&) override;
            void resized() override { sw->setBounds(getWidth() - 44, (getHeight() - 26) / 2, 44, 26); }
        } soloRow;
        Field nameField, panField, stemField, busField;
        Field soloField;
        AppearanceSettings appearance;
        Field appearanceField;
    };

    void paintContent(juce::Graphics&);
    void layout();
    void timerCallback() override;
    FnChangeListener procListener_;
    void lookChanged() override;
    void refreshStatus();
    void refreshTexts();
    void toggle(const char* paramId);
    bool paramOn(const char* id) const;
    Str summaryKey() const;

    TrackProcessor& proc_;
    Content content_ { *this };
    Backdrop backdrop_;

    StatusChip chip_ { 30.0f };
    IconButton fineButton_ { icons::Icon::Sliders };
    Banner banner_;
    InlineName name_ { 16.0f, Weight::SemiBold };
    MeterBar inputMeter_ { 4.0f, true };
    ToggleRow monRow_ { icons::Icon::Headphones };
    ToggleRow strRow_ { icons::Icon::Broadcast };
    ScrollArea scroll_;
    Body body_;
    LevelSlider headphoneSlider_, viewersSlider_;
    juce::Slider delaySlider_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    EditableValue headValue_ { EditableValue::Kind::Db, -30.0f, 6.0f }, viewValue_ { EditableValue::Kind::Db, -30.0f, 6.0f };
    EditableValue delayValue_ { EditableValue::Kind::Ms, 0.0f, 500.0f };
    Field headField_ { {}, &headphoneSlider_, 20, &headValue_ };
    Field viewField_ { {}, &viewersSlider_, 20, &viewValue_ };
    Field delayField_ { {}, &delaySlider_, 20, &delayValue_ };
    TextLine footer_ { 11.0f };
    std::unique_ptr<DbSliderLink> headphoneLink_, viewersLink_, delayLink_;
    std::unique_ptr<ParamValueLink> headValueLink_, viewValueLink_, delayValueLink_;
    Sheet sheet_ { *this };

    // painted areas
    juce::Rectangle<float> header_, card_, wordmark_, dot_, inputLabel_, summary_, footerLine_;
    bool lastMon_ = false, lastStr_ = false;
    int slowTick_ = 0;
};

} // namespace hearaside
