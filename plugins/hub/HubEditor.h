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

// Track / App Audio rows come in three widths (docs/ux-roadmap.md 5.2): Full (wide pills and
// level sliders), Mid (shorter pills, slimmer sliders), Narrow (two lines: the name, then both
// pills; levels move to the "..." panel). The status word is always written out.
enum class RowTier { Full, Mid, Narrow };
struct RowCols { RowTier tier; float pillW, levelW; };
RowCols rowCols(float rowWidth);

class TrackRow : public juce::Component {
public:
    explicit TrackRow(HubEditor& ed);
    static int heightFor(float width);
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
    Badge warn_, state_, delay_;   // delay_: viewers delay (ms), shown when set
    juce::uint32 lastUserMs_[2] = { 0, 0 };
    bool updating_ = false;
    juce::Rectangle<float> nameArea_, hpIcon_, vwIcon_, hpValue_, vwValue_;
};

// One HEARASIDE App Audio, laid out like a TrackRow: program, you hear / viewers hear and their
// levels (through its headphone slot); on/off and recording in the "..." menu.
class SourceRow : public juce::Component {
public:
    explicit SourceRow(HubEditor& ed);
    static int heightFor(float width);
    void update(const SourceView& v, int shadeIndex);
    int index() const noexcept { return view_.index; }
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void refreshTexts();

private:
    void sendLevel(LevelSlider& s, ssbus::ParamId id);
    void showMenu();
    void pickApp();

    HubEditor& ed_;
    SourceView view_;
    int shade_ = 0;
    AudiblePill mon_ { icons::Icon::Headphones }, str_ { icons::Icon::Broadcast };
    LevelSlider hp_, vw_;
    MeterBar meter_ { 3.0f };
    IconButton more_ { icons::Icon::More };
    Badge state_, delay_;
    juce::uint32 lastUserMs_[2] = { 0, 0 };
    bool updating_ = false;
    juce::Rectangle<float> nameArea_, appArea_, hpIcon_, vwIcon_, hpValue_, vwValue_;
};

class HubEditor : public EditorShell, private juce::Timer {
public:
    explicit HubEditor(HubProcessor&);
    ~HubEditor() override;

    HubProcessor& proc() noexcept { return proc_; }
    LookAndFeel& lnf() noexcept { return lnf_; }
    void showTrackPanel(int slot, juce::Component& anchor);
    void showSourceLevels(int index, juce::Component& anchor);
    void showShare();
    // sub-panels by name ("share", "settings", "sync", "track"), also rendered by ui-snapshot
    enum class Panel { Share, Settings, Sync, Track, Levels, SourceLevels, Setup };
    std::unique_ptr<juce::Component> createPanel(Panel, int slot = -1);
    void askRename(const juce::String& title, const juce::String& current, std::function<void(juce::String)> done);

private:
    struct Content : juce::Component {
        explicit Content(HubEditor& e) : ed(e) { setOpaque(true); }
        void paint(juce::Graphics& g) override { ed.paintContent(g); }
        void resized() override { ed.layout(); }
        HubEditor& ed;
    };
    // getting-started checklist (empty state), track rows, then the App Audio rows under a title
    struct ListContent : juce::Component {
        void paint(juce::Graphics&) override;
        juce::Rectangle<float> startBox, sourcesHead;
        juce::String stepText(int i) const;
        bool steps[3] = { false, false, false };
        juce::String step1Detail;   // the step for the DAW in use
    };
    enum class Mode { Compact, Regular, Wide };

    void paintContent(juce::Graphics&);
    void layout();
    void layoutHeader(bool compact);
    void layoutTracksCard(bool compact);
    void layoutStrip(juce::Rectangle<float> card, bool withSummary);
    float stripHeight(float width, bool withSummary) const;
    void layoutList();
    void updateMode(float width, float height);
    void showLevels();
    void showSetup();
    struct Lists { juce::StringArray you, viewers, onlyViewers; };
    Lists summaryLists() const;
    int startHeight(float width) const;
    int summaryLineCount() const;
    juce::String obsLine() const;
    void updateStart();
    void paintSummaryLines(juce::Graphics&, juce::Rectangle<float>, bool stacked);   // stacked: label over value + latency
    void timerCallback() override;
    FnChangeListener procListener_;
    void lookChanged() override;
    void refreshTexts();
    void syncTracks();
    void syncSources();
    void showSettings();
    void showSync();
    void updateSyncBanner();
    bool paramOn(const char* id) const;
    void toggleParam(const char* id);

    HubProcessor& proc_;
    Content content_ { *this };
    Backdrop backdrop_;

    StatusChip obsChip_ { 36.0f };
    StatusChip dawChip_ { 36.0f };
    MuteButton mute_;
    IconButton settingsButton_ { icons::Icon::Sliders };
    IconButton shareButton_ { icons::Icon::Link };
    StatusChip shareChip_ { 36.0f };
    juce::TextButton syncButton_;
    IconButton syncMore_ { icons::Icon::More };
    Banner syncBanner_;
    juce::String syncText_;

    Banner previewBanner_, panicBanner_;
    juce::Viewport viewport_;
    ListContent list_;
    juce::OwnedArray<TrackRow> rows_;
    std::vector<TrackView> views_;
    juce::OwnedArray<SourceRow> sourceRows_;
    std::vector<SourceView> srcViews_;

    MeterBar meterL_ { 6.0f }, meterR_ { 6.0f };
    LevelSlider masterSlider_, headphoneSlider_;
    Switch limiter_;
    PrimaryButton preview_ { icons::Icon::Headphones };
    IconButton levelsButton_ { icons::Icon::Sliders };   // compact: stream level, headphones, limiter
    IconButton setupButton_ { icons::Icon::Check };      // setup check
    Banner silentBanner_;                                // viewers hear nothing (click: setup check)
    struct ClickListener : juce::MouseListener {
        std::function<void()> fn;
        void mouseUp(const juce::MouseEvent& e) override { if (fn && !e.mods.isPopupMenu()) fn(); }
    } silentClick_;
    juce::TextButton startHide_;
    Mode mode_ = Mode::Regular;
    bool condensed_ = false;          // right column too short for the full cards
    bool obsInHeader_ = true;         // else the OBS state is written in the viewers card
    bool shareChipOn_ = false;
    bool showStart_ = false, hadOnlyViewers_ = false;
    float wordmarkSize_ = 17.0f;
    bool tracksSubtitle_ = true;
    int chipWidths_[3] = { 0, 0, 0 };
    std::unique_ptr<DbSliderLink> masterLink_, headphoneLink_;

    // painted geometry
    juce::Rectangle<float> header_, wordmark_, tracksCard_, tracksTitle_, columns_;
    juce::Rectangle<float> streamCard_, streamTitle_, lufsBox_, meterLabels_, masterLabel_, limiterText_;
    juce::Rectangle<float> summaryCard_, summaryTitle_, youBox_, youText_, hpLabel_, viewersBox_, latencyBox_;
    juce::Rectangle<float> messageCard_, stripCard_, stripTitle_, stripLines_, onlyBox_;
    bool lastPreview_ = false, lastPanic_ = false, lastLimiter_ = false;
    int slowTick_ = 0;
};

} // namespace hearaside
