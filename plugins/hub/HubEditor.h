// HEARASIDE Hub editor (prompt 3.3 - 3.5, design export "Main", "Compact" and the sub-pages).
// One window: the main screen (All tracks + What viewers hear + Right now) and full-window
// sub-pages (Share and friends, Settings, Track fine settings, Auto-sync vocal, Manage tracks,
// Manage program audio, Setup check) with Back and Mute always in the header.
#pragma once

#include "HubProcessor.h"
#include "ui/Backdrop.h"
#include "ui/EditorShell.h"
#include "ui/ScrollArea.h"
#include "ui/SettingsPanel.h"

namespace hearaside {

class HubEditor;

// What one row of the main screen shows: a Track, an App Audio (program) or a friend.
struct RowData {
    enum class Kind { Track, Program, Friend };
    Kind kind = Kind::Track;
    int slot = -1;        // bus slot: you / viewers / levels go through HubProcessor::send(slot, ...)
    int index = -1;       // App Audio index (sources()), friend id
    juce::String name;
    juce::uint32 colour = 0;
    int shade = 0;
    bool you = false, viewers = false, solo = false, active = true, linked = true;
    float hpDb = 0, vwDb = 0, pan = 0, delayMs = 0, meter = 0;
    int stem = -1;
    juce::String warnTip;   // rate mismatch / ahead / bypassed
    // program
    juce::String app;
    bool on = true, recording = false, follow = false;
    uint32_t capture = 0;
    double recordSec = 0;
    float latencyMs = 0;
    // friend (index = friend id)
    int friendState = 0;          // 0 hasn't opened the link, 1 singing, 2 offline
    float friendDelayMs = -1.0f;  // how late the voice arrives; < 0 = not measured yet
    bool inDaw = false;           // a DAW track carries this friend (S7)
    bool paired = false;          // ... and a Track at its end sends it on
    juce::String dawTrack;        // that DAW track's name
    bool overLimit = false;       // slower than the Line-up limit: not waited for
    int outSlot = -1;             // the Track at the end of the DAW channel
};

// One row (prompt 3.3): wide = grid 1fr | 48 | 48 | 206 | 32, 56 high; narrow = two lines
// (name + toggles / slider + value + ⋯) as in the Compact mock.
class ChannelRow : public juce::Component {
public:
    explicit ChannelRow(HubEditor&);
    void update(const RowData&);
    const RowData& data() const noexcept { return d_; }
    void setNarrow(bool n) { if (n != narrow_) { narrow_ = n; resized(); repaint(); } }
    bool isNarrow() const noexcept { return narrow_; }
    static int heightFor(bool narrow) { return narrow ? int(theme::layout::rowNarrowH) : int(theme::layout::rowH); }
    void startRename() { name_.startEditing(); }
    void flash();   // "go to the track": highlight for 1.2 s
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    void refreshTexts();
    MoreButton& moreButton() noexcept { return more_; }
    void openMenu() { showMenu(); }
    void flashDawTrack();   // a friend carried by a DAW track: go to the Track that sends it on

private:
    void showMenu();
    Menu programMenu();
    void sendLevel(float db);
    juce::String statusText(Dot& dot) const;

    HubEditor& ed_;
    RowData d_;
    bool narrow_ = false, first_ = true;
    InlineName name_;
    SourcePicker picker_ { true };
    TextLine status_ { 11.5f };
    MeterBar meter_ { 3.0f, true };
    AudibleToggle you_ { AudibleToggle::Side::You }, viewers_ { AudibleToggle::Side::Viewers };
    LevelSlider level_;
    EditableValue value_ { EditableValue::Kind::Db, -30.0f, 6.0f };
    MoreButton more_;
    juce::Rectangle<float> dot_, badges_, avatar_;
    juce::uint32 lastUserMs_ = 0, flashUntil_ = 0;
    bool updating_ = false;
};

// A sub-page of the Hub (fills the window under the header). Pages paint their own text; the
// glass cards come from cards() so the editor's backdrop blurs them once.
class HubPage : public juce::Component {
public:
    explicit HubPage(HubEditor& e) : ed(e) {}
    virtual std::vector<GlassCard> cards() const = 0;
    virtual void tick() {}           // ~10 Hz from the editor
    virtual void refreshTexts() {}
    HubEditor& ed;
};

class HubEditor : public EditorShell, private juce::Timer {
public:
    explicit HubEditor(HubProcessor&);
    ~HubEditor() override;

    enum class Page { Main, Share, Settings, Track, Sync, Tracks, Programs, Setup };
    void showPage(Page, int slot = -1);
    Page page() const noexcept { return page_; }
    enum class SettingsSection { Appearance, Audio, Connection, About };
    void showSettings(SettingsSection);

    HubProcessor& proc() noexcept { return proc_; }
    LookAndFeel& lnf() noexcept { return lnf_; }
    SharedSettings& settings() noexcept { return settings_; }
    bool levelsShowHeadphones() const noexcept { return levelHeadphones_; }
    void renameRow(int slot, const juce::String& name);   // S1: "" = back to the DAW's name, with a toast
    void renameFriend(uint32_t id, const juce::String& name);
    void goToFriend(uint32_t id);                          // scroll to the friend's row and flash it
    void showMixInDaw(uint32_t id, juce::Component& anchor);   // the three steps (S7): a popover next to `anchor`
    void goToTrack(int slot);                              // scroll to its row and flash it
    juce::Colour shadeColour(const RowData&) const;
    void layoutNow() { layout(); content_.repaint(); }
    const std::vector<TrackView>& trackViews() const noexcept { return views_; }
    const std::vector<SourceView>& sourceViews() const noexcept { return srcViews_; }

    // ui-snapshot: open a row's ⋯ menu / the OBS popover, pick a Compact tab
    void openRowMenu(int row) { if (row >= 0 && row < rows_.size()) rows_[row]->openMenu(); }
    void openObsPopover() { showObsPopover(); }
    void setCompactTab(int t) { tabs_.setSelected(t, true); }

    static juce::String programLabel(const juce::String& exe);
    static icons::Icon programIcon(const juce::String& exe);

private:
    struct Content : juce::Component {
        explicit Content(HubEditor& e) : ed(e) { setOpaque(true); }
        void paint(juce::Graphics& g) override { ed.paintContent(g); }
        void resized() override { ed.layout(); }
        HubEditor& ed;
    };
    // the scrolling list of the All tracks card: tracks, Program audio, Friends
    struct List : juce::Component {
        explicit List(HubEditor& e) : ed(e) {}
        void paint(juce::Graphics& g) override { ed.paintList(g); }
        HubEditor& ed;
    };
    // the right-hand cards' child controls live on the content directly
    enum class Mode { Compact, Regular, Wide };
    enum class Tab { Tracks, Levels, Summary };

    void paintContent(juce::Graphics&);
    void paintList(juce::Graphics&);
    void paintHeader(juce::Graphics&);
    void paintViewersCard(juce::Graphics&);
    void paintRightNow(juce::Graphics&, juce::Rectangle<float> area, bool lists);
    void layout();
    void layoutHeader();
    void layoutMain(juce::Rectangle<float> r);
    void layoutList();
    void layoutViewersCard(juce::Rectangle<float> card, bool condensed);
    void updateMode(float width);
    void timerCallback() override;
    void lookChanged() override;
    void refreshTexts();
    void syncRows();
    void updateStart();
    void showObsPopover();
    void showCompactMore();
    bool paramOn(const char* id) const;
    void toggleParam(const char* id);
    juce::String lufsText() const;
    struct Lists { juce::StringArray you, viewers, onlyViewers; };
    Lists summaryLists() const;
    juce::String obsLatencyText() const;

    HubProcessor& proc_;
    Content content_ { *this };
    List list_ { *this };
    Backdrop backdrop_;
    Page page_ = Page::Main;
    std::unique_ptr<HubPage> pageView_;
    Mode mode_ = Mode::Regular;
    Tab tab_ = Tab::Tracks;
    bool levelHeadphones_ = false, narrowRows_ = false;

    // header
    StatusChip obs_ { 36.0f };
    IconButton share_ { icons::Icon::Link }, settingsButton_ { icons::Icon::Sliders }, compactMore_ { icons::Icon::More };
    MuteButton mute_;
    BackButton back_ { true };
    // tracks card
    GhostButton manage_ { {}, GhostButton::Style::Ghost }, autoSync_ { {}, GhostButton::Style::Ghost };
    SegmentedControl levelMode_, tabs_;
    Banner silentBanner_, syncBanner_;
    struct ClickListener : juce::MouseListener {
        std::function<void()> fn;
        void mouseUp(const juce::MouseEvent& e) override { if (fn && !e.mods.isPopupMenu()) fn(); }
    } silentClick_;
    ScrollArea scroll_;
    juce::OwnedArray<ChannelRow> rows_;   // tracks, then programs (friends: S2)
    LinkButton manageTracksLink_, manageProgramsLink_, manageFriendsLink_;
    GhostButton startHide_ { {}, GhostButton::Style::Ghost };
    std::vector<TrackView> views_;
    std::vector<SourceView> srcViews_;
    // what viewers hear
    MeterBar meterL_ { 6.0f }, meterR_ { 6.0f };
    LevelSlider masterSlider_, headphoneSlider_;
    EditableValue masterValue_ { EditableValue::Kind::Db, -30.0f, 6.0f }, headphoneValue_ { EditableValue::Kind::Db, -30.0f, 6.0f };
    Switch limiter_;
    PrimaryButton preview_ { icons::Icon::Headphones };
    std::unique_ptr<DbSliderLink> masterLink_, headphoneLink_;
    std::unique_ptr<ParamValueLink> masterValueLink_, headphoneValueLink_;
    FnChangeListener procListener_;

    // getting started (empty state)
    bool showStart_ = false, steps_[3] = { false, false, false };
    juce::String step1Detail_;
    int pendingRevertSlot_ = -1;
    juce::uint32 pendingRevertUntil_ = 0;
    juce::String pendingRevertFrom_;

    // painted geometry (content coordinates unless noted)
    juce::Rectangle<float> header_, wordmark_, headerTitle_, divider_;
    juce::Rectangle<float> tracksCard_, tracksHead_, columns_, slidersSetLabel_, viewersCard_, rightNow_, bottomBar_, messageCard_;
    juce::Rectangle<float> lufs_, lufsLabel_, meterLabels_, masterLabel_, hpLabel_, sep_, limiterText_;
    juce::Rectangle<float> startBox_, programsHead_, friendsHead_;   // list coordinates
    bool condensed_ = false, rightNowLists_ = true, rightNowShown_ = true;
    bool lastPreview_ = false, lastPanic_ = false;
    int slowTick_ = 0, lastObsWidth_ = 0;
    uint32_t lastLineChanges_ = ~0u;
    int lastLineMs_ = 0;
    juce::String lastLufs_, lastSummary_;
};

} // namespace hearaside
