#pragma once

#include "HubProcessor.h"
#include "ui/Backdrop.h"
#include "ui/Components.h"
#include "ui/EditorShell.h"

namespace hearaside {

class HubEditor;

// Small hover target with a tooltip (warning icon or text badge inside a track row).
class Badge : public juce::Component, public juce::SettableTooltipClient {
public:
    void setIcon(icons::Icon i) { icon_ = i; text_ = {}; repaint(); }
    void setText(const juce::String& t) { text_ = t; repaint(); }
    int idealWidth() const;
    void paint(juce::Graphics&) override;
private:
    icons::Icon icon_ = icons::Icon::Warning;
    juce::String text_;
};

class TrackRow : public juce::Component {
public:
    explicit TrackRow(HubEditor& ed);
    void update(const TrackView& v, int shadeIndex);
    int slot() const noexcept { return view_.slot; }
    const TrackView& view() const noexcept { return view_; }
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    void refreshTexts();

private:
    void sendLevel(LevelSlider& s, ssbus::ParamId id);
    void showMenu();

    HubEditor& ed_;
    TrackView view_;
    int shade_ = 0;
    AudiblePill mon_ { icons::Icon::Headphones }, str_ { icons::Icon::Broadcast };
    LevelSlider hp_, vw_;
    MeterBar in_ { 3.0f };
    IconButton more_ { icons::Icon::More };
    Badge warn_, state_;
    juce::uint32 lastUserMs_[2] = { 0, 0 };
    bool updating_ = false;
    juce::Rectangle<float> nameArea_, hpIcon_, vwIcon_, hpValue_, vwValue_;
};

class HubEditor : public EditorShell, private juce::Timer {
public:
    explicit HubEditor(HubProcessor&);
    ~HubEditor() override;

    HubProcessor& proc() noexcept { return proc_; }
    LookAndFeel& lnf() noexcept { return lnf_; }
    void showTrackPanel(int slot, juce::Component& anchor);
    void askRename(const juce::String& title, const juce::String& current, std::function<void(juce::String)> done);

private:
    struct Content : juce::Component {
        explicit Content(HubEditor& e) : ed(e) { setOpaque(true); }
        void paint(juce::Graphics& g) override { ed.paintContent(g); }
        void resized() override { ed.layout(); }
        HubEditor& ed;
    };
    struct ListContent : juce::Component {};

    void paintContent(juce::Graphics&);
    void layout();
    void layoutList();
    void timerCallback() override;
    FnChangeListener procListener_;
    void lookChanged() override;
    void refreshScenes();
    void refreshTexts();
    void syncTracks();
    void sceneMenu(int sceneIndex);
    void showSettings();
    bool paramOn(const char* id) const;
    void toggleParam(const char* id);

    HubProcessor& proc_;
    Content content_ { *this };
    Backdrop backdrop_;

    SceneBar scenes_;
    std::vector<int> sceneIndex_;   // SceneBar position -> scene slot
    StatusChip obsChip_ { 36.0f };
    StatusChip dawChip_ { 36.0f };
    MuteButton mute_;
    IconButton settingsButton_ { icons::Icon::Sliders };

    Banner previewBanner_, panicBanner_;
    juce::Viewport viewport_;
    ListContent list_;
    juce::OwnedArray<TrackRow> rows_;
    std::vector<TrackView> views_;

    MeterBar meterL_ { 6.0f }, meterR_ { 6.0f };
    LevelSlider masterSlider_, headphoneSlider_;
    Switch limiter_;
    PrimaryButton preview_ { icons::Icon::Headphones };
    juce::TextButton masteringButton_;
    FnChangeListener masteringListener_;
    std::unique_ptr<DbSliderLink> masterLink_, headphoneLink_;

    // painted geometry
    juce::Rectangle<float> header_, wordmark_, tracksCard_, tracksTitle_, columns_, emptyArea_;
    juce::Rectangle<float> streamCard_, streamTitle_, lufsBox_, meterLabels_, masterLabel_, limiterText_, masteringText_;
    juce::Rectangle<float> summaryCard_, summaryTitle_, youBox_, youText_, hpLabel_, viewersBox_, latencyBox_;
    juce::Rectangle<float> messageCard_;
    bool lastPreview_ = false, lastPanic_ = false, lastLimiter_ = false;
    int slowTick_ = 0;
};

} // namespace hearaside
