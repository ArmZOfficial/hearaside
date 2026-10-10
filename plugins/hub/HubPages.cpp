// Hub sub-pages, part 1: Settings, Setup check, Auto-sync vocal (+ the page factory).
#include "HubPages.h"
#include "HubPagesShared.h"
#include "Host.h"
#include "ui/AccountPanel.h"

namespace hearaside {

using ssbus::ParamId;

// =============================================================================================
// Settings

void HubSettingsPage::Nav::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat();
    if (on) { g.setColour(p.ink); g.fillRoundedRectangle(r, 12.0f); }
    else if (highlighted || down) { g.setColour(p.hairline1.withMultipliedAlpha(down ? 2.0f : 1.0f)); g.fillRoundedRectangle(r, 12.0f); }
    auto in = r.reduced(12.0f, 0.0f);
    icons::draw(g, icon, in.removeFromLeft(18.0f).withSizeKeepingCentre(18.0f, 18.0f), on ? p.onInk : p.ink2);
    in.removeFromLeft(12.0f);
    g.setColour(on ? p.onInk : p.ink2);
    g.setFont(uiFont(13.5f, Weight::Medium));
    g.drawText(getButtonText(), in, juce::Justification::centredLeft, true);
}

namespace {

// One section's rows; owns its controls. Built fresh when the section or the language changes.
class SectionBody : public juce::Component, private juce::Timer {
public:
    SectionBody(HubEditor& ed, int section) : ed_(ed), section_(section) {
        if (section != 0 && section != 3) addAndMakeVisible(list_);
        auto& proc = ed_.proc();
        auto& st = ed_.settings();
        if (section == 0) {
            appearance_ = std::make_unique<AppearanceSettings>();
            addAndMakeVisible(*appearance_);
        } else if (section == 1) {
            // Peak ceiling: slider + value
            auto& ceil = *proc.params().getParameter(hubparam::Ceiling);
            ceiling_.slider.setRange(-6.0, 0.0, 0.1);
            ceiling_.slider.setDoubleClickReturnValue(true, -1.0);
            ceiling_.slider.setTooltip({});
            ceilingLink_ = std::make_unique<DbSliderLink>(ceiling_.slider, ceil);
            ceilingValueLink_ = std::make_unique<ParamValueLink>(ceiling_.value, ceil);
            list_.add(std::make_unique<SettingRow>(tr(Str::Ceiling), tr(Str::CeilingCap), &ceiling_, 280, 26));
            // Sync safety: stepper (0..2 blocks)
            auto& sync = *proc.params().getParameter(hubparam::SyncSafety);
            stepper_.setUnit(tr(Str::Blocks));
            stepper_.setValue(juce::roundToInt(paramPlain(sync)));
            stepper_.onChange = [&sync](int v) { setParamWithGesture(sync, float(v)); };
            list_.add(std::make_unique<SettingRow>(tr(Str::SyncSafety), tr(Str::SyncSafetyHint), &stepper_, 180, 34));
            // Stem names: 2 x 2 fields
            for (int i = 0; i < ssbus::kMaxStems; ++i) {
                auto* f = stems_.add(new TextField(trf(Str::StemN, { juce::String(i + 1) })));
                f->setText(proc.stemName(i), false);
                f->setTitle(trf(Str::StemNameN, { juce::String(i + 1) }));
                f->onReturnKey = f->onFocusLost = [&proc, i, f] { proc.setStemName(i, f->getText()); };
                stemGrid_.addAndMakeVisible(f);
            }
            stemGrid_.fields = &stems_;
            list_.add(std::make_unique<SettingRow>(tr(Str::StemNames), tr(Str::StemNamesCap), &stemGrid_, 280, (ssbus::kMaxStems + 1) / 2 * 46 - 8));
        } else if (section == 2) {
            list_.add(std::make_unique<SettingRow>("OBS", tr(Str::ObsRowCap), &obs_, 280, 26));
            bus_.setText(proc.busName(), false);
            bus_.setTitle(tr(Str::BusName));
            bus_.onReturnKey = bus_.onFocusLost = [this] { ed_.proc().setBusName(bus_.getText()); };
            list_.add(std::make_unique<SettingRow>(tr(Str::BusName), tr(Str::BusNameCap), &bus_, 280, 38));
            list_.add(std::make_unique<SettingRow>(tr(Str::LatencyToObs), tr(Str::DelayToObsCap), &delay_, 280, 26));
            // permanent links (your Vercel site) - not in the mock, prompt 3.10 item 7
            shareBase_.setPlaceholder("https://....vercel.app");
            shareBase_.setText(st->shareBase(), false);
            shareBase_.setTitle(tr(Str::ShareBase));
            shareBase_.onReturnKey = shareBase_.onFocusLost = [this] { ed_.settings()->setShareBase(shareBase_.getText()); shareBase_.setText(ed_.settings()->shareBase(), false); };
            list_.add(std::make_unique<SettingRow>(tr(Str::ShareBase), tr(Str::ShareBaseCap), &shareBase_, 280, 38));
            permanent_.setOn(st->permanentLinks(), false);
            permanent_.setTitle(tr(Str::PermanentLinks));
            permanent_.onClick = [this] { auto& s = ed_.settings(); s->setPermanentLinks(!s->permanentLinks()); permanent_.setOn(s->permanentLinks(), true); };
            list_.add(std::make_unique<SettingRow>(tr(Str::PermanentLinks), tr(Str::SharePermanent), &permanent_, 44, 26));
            // REST API (Stream Deck, scripts)
            rest_.sw.setOn(st->restApi(), false);
            rest_.sw.setTitle(tr(Str::RestApi));
            rest_.sw.onClick = [this] { auto& s = ed_.settings(); s->setRestApi(!s->restApi()); rest_.sw.setOn(s->restApi(), true); refreshRest(); };
            rest_.copy.onClick = [this] {
                juce::SystemClipboard::copyTextToClipboard(ed_.settings()->restApiKey());
                ed_.toast(tr(Str::Copied));
            };
            list_.add(std::make_unique<SettingRow>(tr(Str::RestApi), tr(Str::RestApiCapShort), &rest_, 280, 34));
            refreshRest();
        } else if (section == 3) {
            accountPanel_ = std::make_unique<AccountPanel>(AccountPanel::Mode::Full);
            accountPanel_->onToast = [this](const juce::String& msg) { ed_.toast(msg); };
            addAndMakeVisible(*accountPanel_);
        } else if (section == 4) {
            version_.setText(trf(Str::VersionN, { juce::String(HEARASIDE_VERSION) }));
            version_.setJustification(juce::Justification::centredRight);
            list_.add(std::make_unique<SettingRow>("HEARASIDE", tr(Str::AppTagline), &version_, 280, 26));
            showAgain_.setButtonText(tr(Str::ShowAgain));
            showAgain_.onClick = [this] {
                ed_.settings()->setFlag("startHidden", false);
                ed_.settings()->setFlag("triedPreview", false);
                ed_.showPage(HubEditor::Page::Main);
            };
            list_.add(std::make_unique<SettingRow>(tr(Str::GettingStarted), tr(Str::GettingStartedCap), &showAgainBox_, 280, 38));
            showAgainBox_.button = &showAgain_;
            showAgainBox_.addAndMakeVisible(showAgain_);
            setup_.setButtonText(tr(Str::Open));
            setup_.onClick = [this] { ed_.showPage(HubEditor::Page::Setup); };
            setupBox_.button = &setup_;
            setupBox_.addAndMakeVisible(setup_);
            list_.add(std::make_unique<SettingRow>(tr(Str::SetupTitle), tr(Str::SetupCheckCap), &setupBox_, 280, 38));
            reset_.setButtonText(tr(Str::ResetWord));
            reset_.setStyle(GhostButton::Style::Danger);
            reset_.onClick = [this] { resetAll(); };
            resetBox_.button = &reset_;
            resetBox_.addAndMakeVisible(reset_);
            list_.add(std::make_unique<SettingRow>(tr(Str::ResetSettings), tr(Str::ResetSettingsCap), &resetBox_, 280, 38));
        }
        startTimerHz(4);
        timerCallback();
    }

    int idealHeight(int width) const {
        if (appearance_) return appearance_->idealHeight(width);
        if (accountPanel_) return accountPanel_->idealHeight(width);
        return list_.idealHeight(width);
    }
    void resized() override {
        list_.setBounds(getLocalBounds());
        if (appearance_) appearance_->setBounds(getLocalBounds());
        if (accountPanel_) accountPanel_->setBounds(getLocalBounds());
    }

private:
    struct StemGrid : juce::Component {
        juce::OwnedArray<TextField>* fields = nullptr;
        void resized() override {
            if (fields == nullptr) return;
            const int w = (getWidth() - 8) / 2;
            for (int i = 0; i < fields->size(); ++i) (*fields)[i]->setBounds((i % 2) * (w + 8), (i / 2) * 46, w, 38);
        }
    };
    struct SliderValue : juce::Component {
        LevelSlider slider { 0.0 };
        EditableValue value { EditableValue::Kind::Db, -12.0f, 0.0f };
        SliderValue() { addAndMakeVisible(slider); addAndMakeVisible(value); }
        void resized() override {
            auto r = getLocalBounds();
            value.setBounds(r.removeFromRight(64));
            slider.setBounds(r.withTrimmedRight(10).withSizeKeepingCentre(r.getWidth() - 10, 20));
        }
    };
    struct RightButton : juce::Component {   // a button at the right end of the 280 column
        GhostButton* button = nullptr;
        void resized() override { if (button) button->setBounds(getWidth() - button->idealWidth(), 0, button->idealWidth(), getHeight()); }
    };
    struct RestRow : juce::Component {
        Switch sw;
        GhostButton copy { tr(Str::CopyApiKey) };
        RestRow() { copy.setSmall(true); addAndMakeVisible(sw); addAndMakeVisible(copy); }
        void resized() override {
            sw.setBounds(getWidth() - 44, (getHeight() - 26) / 2, 44, 26);
            copy.setBounds(getWidth() - 44 - 12 - copy.idealWidth(), (getHeight() - 30) / 2, copy.idealWidth(), 30);
        }
    };

    void refreshRest() {
        auto& s = ed_.settings();
        rest_.copy.setVisible(s->restApi());
        if (list_.rows().size() >= 6)
            list_.rows()[5]->setTexts(tr(Str::RestApi), s->restApi() ? "http://127.0.0.1:" + juce::String(ed_.proc().control().running() ? ed_.proc().control().port() : ControlServer::kFirstPort) + "/api/v1"
                                                                      : tr(Str::RestApiCapShort));
        rest_.resized();
    }

    void resetAll() {
        auto& s = ed_.settings();
        auto& proc = ed_.proc();
        s->setThemeMode(ThemeMode::Auto);
        s->setGlassAlpha(theme::glass::alphaDefault);
        s->setHostColours(true);
        s->setReduceMotion(false);
        s->setUiScale(1.0f);
        setParamWithGesture(*proc.params().getParameter(hubparam::Ceiling), -1.0f);
        setParamWithGesture(*proc.params().getParameter(hubparam::SyncSafety), 0.0f);
        for (int i = 0; i < ssbus::kMaxStems; ++i) proc.setStemName(i, {});
        ed_.toast(tr(Str::ResetSettings));
    }

    void timerCallback() override {
        auto& proc = ed_.proc();
        if (ceilingLink_) { ceilingLink_->update(); ceilingValueLink_->update(); }
        if (section_ == 1) stepper_.setValue(juce::roundToInt(paramPlain(*proc.params().getParameter(hubparam::SyncSafety))));
        if (section_ == 2) {
            const bool on = proc.obsConnected();
            obs_.setText(on ? tr(Str::ConnectedWord) : tr(Str::NotConnected));
            obs_.setDot(on ? Dot::Ok : Dot::Warn);
            obs_.setJustification(juce::Justification::centredRight);
            delay_.setText(trf(Str::AboutMs, { juce::String(proc.latency().total(), 1) }));
            delay_.setJustification(juce::Justification::centredRight);
            if (!bus_.hasKeyboardFocus(true) && bus_.getText() != proc.busName()) bus_.setText(proc.busName(), false);
        }
    }

    HubEditor& ed_;
    int section_;
    SettingList list_;
    std::unique_ptr<AppearanceSettings> appearance_;
    std::unique_ptr<AccountPanel> accountPanel_;
    SliderValue ceiling_;
    std::unique_ptr<DbSliderLink> ceilingLink_;
    std::unique_ptr<ParamValueLink> ceilingValueLink_;
    Stepper stepper_ { 0, 2 };
    juce::OwnedArray<TextField> stems_;
    StemGrid stemGrid_;
    TextLine obs_ { 13.0f, Weight::Regular, TextLine::Tone::Ink2 }, delay_ { 14.0f, Weight::Medium, TextLine::Tone::Ink };
    TextLine version_ { 13.0f };
    TextField bus_, shareBase_;
    Switch permanent_;
    RestRow rest_;
    GhostButton showAgain_, setup_, reset_;
    RightButton showAgainBox_, setupBox_, resetBox_;
};

constexpr int kSections = 5;
const Str kSectionTitles[kSections] = { Str::SecAppearance, Str::SecAudio, Str::SecConnection, Str::AccountTitle, Str::SecAbout };
const Str kSectionCaps[kSections] = { Str::SecAppearanceCap, Str::SecAudioCap, Str::SecConnectionCap, Str::AccountOptional, Str::SecAboutCap };
const icons::Icon kSectionIcons[kSections] = { icons::Icon::Appearance, icons::Icon::AudioBars, icons::Icon::Connection, icons::Icon::Person, icons::Icon::Info };

} // namespace

HubSettingsPage::HubSettingsPage(HubEditor& e) : HubPage(e) {
    for (int i = 0; i < kSections; ++i) {
        auto* n = nav_.add(new Nav(kSectionIcons[i]));
        n->onClick = [this, i] { select(i); };
        addAndMakeVisible(n);
    }
    addAndMakeVisible(scroll_);
    scroll_.setContent(holder_);
    setTitle(tr(Str::SettingsSections));
    refreshTexts();
    select(0);
}

HubSettingsPage::~HubSettingsPage() { body_.reset(); }

void HubSettingsPage::refreshTexts() {
    for (int i = 0; i < nav_.size(); ++i) { nav_[i]->setButtonText(tr(kSectionTitles[i])); nav_[i]->setTitle(tr(kSectionTitles[i])); }
    build();
    repaint();
}

void HubSettingsPage::select(int s) {
    section_ = juce::jlimit(0, kSections - 1, s);
    for (int i = 0; i < nav_.size(); ++i) { nav_[i]->on = i == section_; nav_[i]->repaint(); }
    build();
    scroll_.setViewPosition(0, 0);
    repaint();
}

void HubSettingsPage::build() {
    body_.reset();
    body_ = std::make_unique<SectionBody>(ed, section_);
    holder_.addAndMakeVisible(*body_);
    layoutSection();
}

void HubSettingsPage::layoutSection() {
    if (body_ == nullptr) return;
    const int w = scroll_.contentWidth();
    const int h = static_cast<SectionBody*>(body_.get())->idealHeight(w);
    holder_.setSize(w, h + 8);
    body_->setBounds(0, 0, w, h);
}

std::vector<GlassCard> HubSettingsPage::cards() const { return { { navCard_, theme::radius::card }, { card_, theme::radius::card } }; }

void HubSettingsPage::tick() {}

void HubSettingsPage::resized() {
    auto r = getLocalBounds().toFloat();
    const bool narrow = r.getWidth() < 640.0f;
    if (narrow) {   // Compact: the sections become a row of buttons above the card
        navCard_ = r.removeFromTop(64.0f);
        r.removeFromTop(10.0f);
        auto n = navCard_.reduced(10.0f, 9.0f);
        const float w = n.getWidth() / float(nav_.size());
        for (auto* b : nav_) b->setBounds(n.removeFromLeft(w).reduced(2.0f, 0.0f).toNearestInt());
        note_ = {};
    } else {
        navCard_ = r.removeFromLeft(236.0f);
        r.removeFromLeft(16.0f);
        auto n = navCard_.reduced(12.0f);
        for (auto* b : nav_) { b->setBounds(n.removeFromTop(46.0f).toNearestInt()); n.removeFromTop(4.0f); }
        const float nh = wrappedHeight(uiFont(12.0f), tr(Str::SettingsScope), n.getWidth() - 24.0f, 6.0f) + 24.0f;
        note_ = n.removeFromBottom(nh);
    }
    card_ = r;
    auto c = card_.withTrimmedLeft(narrow ? 16.0f : 24.0f).withTrimmedTop(narrow ? 16.0f : 24.0f).withTrimmedBottom(8.0f);
    title_ = c.removeFromTop(24.0f + 4.0f + 17.0f + 8.0f).withTrimmedRight(28.0f);
    scroll_.setBounds(c.withTrimmedRight(6.0f).toNearestInt());
    scroll_.setFadeColour(paletteOf(*this).paper.overlaidWith(paletteOf(*this).glass));
    layoutSection();
}

void HubSettingsPage::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    if (!note_.isEmpty()) {
        g.setColour(p.inset);
        g.fillRoundedRectangle(note_, 12.0f);
        drawWrapped(g, tr(Str::SettingsScope), uiFont(12.0f), p.graphite, note_.reduced(12.0f), 6.0f);
    }
    auto t = title_;
    g.setColour(p.ink);
    g.setFont(uiFont(20.0f, Weight::SemiBold));
    g.drawText(tr(kSectionTitles[section_]), t.removeFromTop(24.0f), juce::Justification::centredLeft, true);
    t.removeFromTop(4.0f);
    g.setColour(p.graphite);
    g.setFont(uiFont(12.5f));
    g.drawText(tr(kSectionCaps[section_]), t.removeFromTop(17.0f), juce::Justification::centredLeft, true);
}

// =============================================================================================
// Setup check: what is ready, what to fix (docs/ux-roadmap.md 4.1)

namespace {

class SetupPage : public HubPage, private juce::Timer {
public:
    explicit SetupPage(HubEditor& e) : HubPage(e) {
        copy_.setButtonText(tr(Str::CopyReport));
        copy_.setTooltip(tr(Str::CopyReportTip));
        copy_.onClick = [this] {
            juce::SystemClipboard::copyTextToClipboard(ed.proc().diagnostics());
            ed.toast(tr(Str::Copied));
        };
        addAndMakeVisible(copy_);
        addAndMakeVisible(scroll_);
        scroll_.setContent(list_);
        list_.page = this;
        items_ = check();
        startTimerHz(2);
    }
    std::vector<GlassCard> cards() const override { return { { card_, theme::radius::card } }; }
    void resized() override {
        card_ = getLocalBounds().toFloat();
        auto c = card_.reduced(24.0f, 20.0f);
        auto foot = c.removeFromBottom(38.0f);
        copy_.setBounds(foot.removeFromLeft(float(copy_.idealWidth())).toNearestInt());
        c.removeFromBottom(12.0f);
        mastering_ = c.removeFromBottom(wrappedHeight(uiFont(12.0f), tr(Str::SetupMastering), c.getWidth(), 6.0f) + 4.0f);
        c.removeFromBottom(12.0f);
        scroll_.setBounds(c.withTrimmedRight(-14.0f).toNearestInt());
        scroll_.setFadeColour(paletteOf(*this).paper.overlaidWith(paletteOf(*this).glass));
        layoutList();
    }
    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        drawWrapped(g, tr(Str::SetupMastering), uiFont(12.0f), p.graphite, mastering_, 6.0f);
    }

private:
    struct Item {
        int level = 0;   // 0 fine, 1 should be fixed, 2 the viewers are affected
        juce::String what, you, viewers, fix;
    };
    using Line = std::tuple<juce::String, juce::Font, juce::Colour>;
    struct List : juce::Component {
        SetupPage* page = nullptr;
        void paint(juce::Graphics& g) override { page->paintList(g); }
    };

    std::vector<Line> lines(const Item& it, const theme::Palette& p) const {
        std::vector<Line> out { { it.what, uiFont(13.5f, it.level > 0 ? Weight::SemiBold : Weight::Regular), p.ink } };
        if (it.level == 0) return out;
        if (it.you.isNotEmpty()) out.push_back({ tr(Str::MonLabel) + ": " + it.you, uiFont(12.0f), p.graphite });
        if (it.viewers.isNotEmpty()) out.push_back({ tr(Str::StrLabel) + ": " + it.viewers, uiFont(12.0f), p.graphite });
        if (it.fix.isNotEmpty()) out.push_back({ juce::String(juce::CharPointer_UTF8("\xe2\x86\x92 ")) + it.fix, uiFont(12.0f, Weight::Medium), p.ink });
        return out;
    }
    float itemHeight(const Item& it, float w) const {
        float h = 20.0f;
        for (const auto& [text, font, colour] : lines(it, paletteOf(*this))) h += wrappedHeight(font, text, w - 24.0f - 34.0f, 3.0f) + 3.0f;
        return juce::jmax(46.0f, h);
    }
    void layoutList() {
        const float w = float(scroll_.contentWidth());
        float h = 0.0f;
        for (const auto& it : items_) h += itemHeight(it, w) + 8.0f;
        list_.setSize(juce::roundToInt(w), juce::roundToInt(h));
        list_.repaint();
    }
    void paintList(juce::Graphics& g) {
        const auto& p = paletteOf(*this);
        const float w = float(list_.getWidth());
        float y = 0.0f;
        for (const auto& it : items_) {
            const float h = itemHeight(it, w);
            const juce::Rectangle<float> box(0.0f, y, w, h);
            y += h + 8.0f;
            g.setColour(it.level == 2 ? p.warnBg : p.inset);
            g.fillRoundedRectangle(box, 14.0f);
            g.setColour(it.level == 2 ? p.warn.withAlpha(0.35f) : p.hairline1);
            g.drawRoundedRectangle(box.reduced(0.5f), 14.0f, 1.0f);
            auto in = box.reduced(12.0f, 10.0f);
            auto icon = in.removeFromLeft(24.0f).removeFromTop(22.0f);
            if (it.level == 0) {
                g.setColour(p.ok);
                g.fillEllipse(icon.withSizeKeepingCentre(20.0f, 20.0f));
                icons::draw(g, icons::Icon::Check, icon.withSizeKeepingCentre(12.0f, 12.0f), juce::Colours::white, 2.6f);
            } else {
                icons::draw(g, it.level == 2 ? icons::Icon::SpeakerOff : icons::Icon::Warning, icon.withSizeKeepingCentre(18.0f, 18.0f), it.level == 2 ? p.danger : p.warn, 2.0f);
            }
            in.removeFromLeft(10.0f);
            for (const auto& [text, font, colour] : lines(it, p)) {
                const float th = wrappedHeight(font, text, in.getWidth(), 3.0f);
                drawWrapped(g, text, font, colour, in.removeFromTop(th), 3.0f);
                in.removeFromTop(3.0f);
            }
        }
    }

    std::vector<Item> check() const {
        auto& proc_ = ed.proc();
        std::vector<Item> out;
        const bool owner = proc_.connected() && proc_.engine().role() == ssengine::HubEngine::Role::Owner;
        const auto ts = proc_.tracks();
        const auto ss = proc_.sources();
        out.push_back(owner ? Item { 0, tr(Str::SetupHubOk) } : Item { 2, tr(Str::SecondHubTitle), {}, tr(Str::SetupNothing), tr(Str::SetupHubFix) });
        if (!ts.empty() || !ss.empty()) out.push_back({ 0, tr(Str::SetupTracksOk).replace("%n", juce::String(int(ts.size() + ss.size()))) });
        else out.push_back({ 1, tr(Str::SetupTracksBad), tr(Str::SetupTracksYou), {}, tr(Str::StartStep1) });
        if (proc_.obsConnected()) out.push_back({ 0, tr(Str::ObsConnected) });
        else out.push_back({ 2, tr(Str::ObsNotConnected), {}, tr(Str::SetupNothing), tr(Str::SetupObsFix) });
        if (!proc_.viewersSilent()) out.push_back({ 0, tr(Str::SetupSignalOk) });
        else out.push_back({ 2, tr(Str::SetupSignalBad).replace("%t", proc_.silentTrack()), {}, tr(Str::SetupNothing), tr(Str::SetupSignalFix) });
        juce::StringArray rate, bypass, ahead;
        for (const auto& v : ts) {
            if (v.hubStatus & ssbus::kHubStatusRateMismatch) rate.add(v.name);
            if (v.bypassed) bypass.add(v.name);
            if (v.hubStatus & ssbus::kHubStatusAhead) ahead.add(v.name);
        }
        if (rate.isEmpty()) out.push_back({ 0, tr(Str::SetupRateOk) });
        else out.push_back({ 2, tr(Str::SetupRateBad) + " " + rate.joinIntoString(", "), {}, tr(Str::SetupRateViewers), tr(Str::SetupRateFix) });
        if (bypass.isEmpty()) out.push_back({ 0, tr(Str::SetupBypassOk) });
        else out.push_back({ 1, tr(Str::SetupBypassBad) + " " + bypass.joinIntoString(", "), tr(Str::SetupBypassYou), {}, tr(Str::SetupBypassFix) });
        if (ahead.isEmpty()) out.push_back({ 0, tr(Str::SetupAheadOk) });
        else out.push_back({ 1, tr(Str::SetupAheadBad) + " " + ahead.joinIntoString(", "), {}, tr(Str::AheadWarning), tr(Str::SyncSafetyHint) });
        bool captureFailed = false;
        for (const auto& s : ss) captureFailed = captureFailed || (s.on() && s.capture == 4);   // AppCapture::State::Failed
        if (!captureFailed) out.push_back({ 0, tr(Str::SetupAppOk) });
        else out.push_back({ 1, tr(Str::AppFailed), tr(Str::SetupAppYou), tr(Str::SetupAppYou), tr(Str::SetupAppFix) });
        if (!proc_.sharing()) out.push_back({ 0, tr(Str::SetupShareOff) });
        else if (proc_.share().tunnel() == ShareServer::Tunnel::Missing || proc_.share().tunnel() == ShareServer::Tunnel::Failed)
            out.push_back({ 1, tr(Str::SetupShareLan), {}, {}, tr(Str::TunnelMissing) });
        else if (proc_.permanentLinksSet() && proc_.directory().state() == ShareDirectory::State::Unreachable)
            out.push_back({ 1, tr(Str::ShareDirOffline), {}, {}, tr(Str::SetupShareFix) });
        else out.push_back({ 0, tr(Str::SetupShareOk) });
        return out;
    }

    void timerCallback() override {
        auto now = check();
        bool same = now.size() == items_.size();
        for (size_t i = 0; same && i < now.size(); ++i) same = now[i].level == items_[i].level && now[i].what == items_[i].what;
        if (!same) { items_ = std::move(now); layoutList(); }
    }

    ScrollArea scroll_;
    List list_;
    GhostButton copy_;
    std::vector<Item> items_;
    juce::Rectangle<float> card_, mastering_;
};

// =============================================================================================
// Auto-sync vocal (prompt 3.4 "Sync")

class SyncPage : public HubPage {
public:
    explicit SyncPage(HubEditor& e) : HubPage(e) {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &mic_, &ref_, &start_, &cancel_, &again_, &check_, &byHand_ })
            addChildComponent(c);
        for (int i = 0; i < 3; ++i) {
            auto* b = checks_.add(new CheckRow());
            b->onClick = [b] { b->on = !b->on; b->repaint(); };
            addAndMakeVisible(b);
        }
        mic_.setVisible(true);
        ref_.setVisible(true);
        start_.onClick = [this] { startMeasuring(); };
        again_.onClick = [this] { startMeasuring(); };
        cancel_.onClick = [this] { ed.proc().cancelAutoSync(); tick(); };
        check_.onClick = [this] { ed.proc().setParam(hubparam::Preview, 1.0f); };
        byHand_.onClick = [this] { ed.showPage(HubEditor::Page::Tracks); };
        byHand_.setVisible(true);
        start_.setIcon(icons::Icon::Play);
        refreshTexts();
        fillTracks();
        tick();
    }

    std::vector<GlassCard> cards() const override { return { { card_, theme::radius::card }, { delaysCard_, theme::radius::card }, { tipsCard_, theme::radius::card } }; }

    void refreshTexts() override {
        const Str texts[3] = { Str::SyncCheck1, Str::SyncCheck2, Str::SyncCheck3 };
        for (int i = 0; i < 3; ++i) { checks_[i]->text = tr(texts[i]); checks_[i]->setTitle(tr(texts[i])); }
        start_.setButtonText(tr(Str::SyncStartMeasuring));
        cancel_.setButtonText(tr(Str::Cancel));
        again_.setButtonText(tr(Str::MeasureAgain));
        check_.setButtonText(tr(Str::SyncCheckWith));
        byHand_.setButtonText(tr(Str::SetDelaysByHand));
        mic_.setTitle(tr(Str::SyncMic));
        ref_.setTitle(tr(Str::SyncRef));
        repaint();
    }

    void tick() override {
        using P = HubProcessor::SyncPhase;
        const auto st = ed.proc().autoSync();
        if (st.phase != phase_) { phase_ = st.phase; phaseStart_ = juce::Time::getMillisecondCounter(); resized(); }
        repaint();
    }

    void resized() override {
        auto r = getLocalBounds().toFloat();
        const bool narrow = r.getWidth() < 700.0f;
        if (!narrow) {
            auto right = r.removeFromRight(310.0f);
            r.removeFromRight(16.0f);
            delaysCard_ = right.removeFromTop(juce::jmin(right.getHeight() * 0.4f, 58.0f + 24.0f * float(juce::jmin(6, int(ed.trackViews().size()) + int(ed.sourceViews().size()))) + 30.0f));
            right.removeFromTop(16.0f);
            tipsCard_ = right;
            auto d = delaysCard_.reduced(22.0f, 20.0f);
            byHand_.setBounds(d.removeFromBottom(18.0f).withWidth(float(byHand_.idealWidth())).toNearestInt());
        } else {
            delaysCard_ = tipsCard_ = {};
            byHand_.setVisible(false);
        }
        card_ = r;
        auto c = card_.reduced(24.0f);
        // 1 choose the tracks
        step1_ = c.removeFromTop(26.0f);
        c.removeFromTop(12.0f);
        auto line = c.removeFromTop(18.0f + 4.0f + 42.0f).withTrimmedLeft(44.0f);
        labels_ = line.removeFromTop(18.0f);
        line.removeFromTop(4.0f);
        mic_.setBounds(line.removeFromLeft((line.getWidth() - 12.0f) * 0.5f).toNearestInt());
        line.removeFromLeft(12.0f);
        ref_.setBounds(line.toNearestInt());
        c.removeFromTop(22.0f);
        sep1_ = c.removeFromTop(1.0f);
        c.removeFromTop(22.0f);
        // 2 get ready
        step2_ = c.removeFromTop(26.0f);
        c.removeFromTop(10.0f);
        auto checks = c.withTrimmedLeft(44.0f);
        for (auto* b : checks_) { b->setBounds(checks.removeFromTop(44.0f).toNearestInt()); checks.removeFromTop(10.0f); }
        c.removeFromTop(3.0f * 54.0f);
        c.removeFromTop(12.0f);
        sep2_ = c.removeFromTop(1.0f);
        c.removeFromTop(22.0f);
        // 3 measure
        step3_ = c.removeFromTop(26.0f);
        c.removeFromTop(12.0f);
        measure_ = c.withTrimmedLeft(44.0f);
        using P = HubProcessor::SyncPhase;
        const bool busy = phase_ == P::Countdown || phase_ == P::Reference || phase_ == P::Microphone;
        start_.setVisible(!busy && phase_ != P::Done && phase_ != P::Failed);
        cancel_.setVisible(busy);
        again_.setVisible(phase_ == P::Done || phase_ == P::Failed);
        check_.setVisible(phase_ == P::Done);
        auto m = measure_;
        // the two hint lines go beside the button when they fit, else under it
        const auto hintFont = uiFont(12.5f);
        const float hintW = juce::jmax(textWidth(hintFont, tr(Str::SyncTakes)), textWidth(hintFont, tr(Str::SyncViewersNothing)));
        hintBelow_ = m.getWidth() - 208.0f < hintW + 4.0f;
        if (!busy && phase_ != P::Done && phase_ != P::Failed) {
            start_.setBounds(m.removeFromTop(50.0f).withWidth(juce::jmin(190.0f, m.getWidth())).toNearestInt());
        } else if (busy) {
            cancel_.setBounds(m.withTrimmedTop(76.0f).removeFromTop(36.0f).withWidth(float(cancel_.idealWidth())).toNearestInt());
        } else {
            // a failure message can wrap to several lines (Thai, narrow window): the buttons go under it
            const float errorH = juce::jmax(36.0f, wrappedHeight(hintFont, tr(ed.proc().autoSync().error), m.getWidth(), 4.0f));
            auto row = m.withTrimmedTop(phase_ == P::Done ? 110.0f : 22.0f + errorH + 6.0f).removeFromTop(36.0f);
            again_.setBounds(row.removeFromLeft(float(again_.idealWidth())).toNearestInt());
            row.removeFromLeft(16.0f);
            check_.setBounds(row.removeFromLeft(float(check_.idealWidth())).withSizeKeepingCentre(float(check_.idealWidth()), 20.0f).toNearestInt());
        }
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto stepHead = [&](juce::Rectangle<float> r, int n, Str title) {
            const auto circle = r.removeFromLeft(26.0f).withSizeKeepingCentre(26.0f, 26.0f);
            g.setColour(p.ink);
            g.fillEllipse(circle);
            g.setColour(p.onInk);
            g.setFont(uiFont(13.0f, Weight::SemiBold));
            g.drawText(juce::String(n), circle, juce::Justification::centred, false);
            r.removeFromLeft(18.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(16.5f, Weight::SemiBold));
            g.drawText(tr(title), r, juce::Justification::centredLeft, true);
        };
        stepHead(step1_, 1, Str::SyncStep1);
        stepHead(step2_, 2, Str::SyncStep2);
        stepHead(step3_, 3, Str::SyncStep3);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f));
        g.drawText(tr(Str::SyncMic), labels_.withWidth(float(mic_.getWidth())), juce::Justification::centredLeft, true);
        g.drawText(tr(Str::SyncRef), labels_.withLeft(float(ref_.getX())), juce::Justification::centredLeft, true);
        g.setColour(p.hairline2);
        g.fillRect(sep1_);
        g.fillRect(sep2_);

        // step 3 by phase
        using P = HubProcessor::SyncPhase;
        const auto st = ed.proc().autoSync();
        auto m = measure_;
        if (phase_ == P::Idle) {
            auto t = hintBelow_ ? m.withTrimmedTop(62.0f).removeFromTop(40.0f)
                                : m.withTrimmedLeft(208.0f).removeFromTop(50.0f).withSizeKeepingCentre(m.getWidth() - 208.0f, 40.0f);
            g.setColour(p.ink2);
            g.setFont(uiFont(12.5f));
            g.drawText(tr(Str::SyncTakes), t.removeFromTop(20.0f), juce::Justification::centredLeft, true);
            g.drawText(tr(Str::SyncViewersNothing), t, juce::Justification::centredLeft, true);
        } else if (phase_ == P::Countdown || phase_ == P::Reference || phase_ == P::Microphone) {
            const auto elapsed = float(juce::Time::getMillisecondCounter() - phaseStart_) / 3500.0f;
            float pct = phase_ == P::Countdown ? 0.0f : phase_ == P::Reference ? juce::jmin(0.5f, elapsed * 0.5f) : 0.5f + juce::jmin(0.5f, elapsed * 0.5f);
            const auto text = phase_ == P::Countdown ? trf(Str::SyncStartingIn, { juce::String(ed.proc().syncCountdown()) })
                                                     : phase_ == P::Reference ? tr(Str::SyncMeasuringRef) : tr(Str::SyncMeasuringMic);
            auto row = m.removeFromTop(22.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(14.0f, Weight::Medium));
            g.drawText(text, row, juce::Justification::centredLeft, true);
            g.drawText(juce::String(juce::roundToInt(pct * 100.0f)) + "%", row, juce::Justification::centredRight, false);
            m.removeFromTop(8.0f);
            auto bar = m.removeFromTop(6.0f);
            g.setColour(p.sliderRail);
            g.fillRoundedRectangle(bar, 3.0f);
            g.setColour(p.ink);
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * pct), 3.0f);
            m.removeFromTop(10.0f);
            drawStatusDot(g, { m.getX() + 4.0f, m.getY() + 9.0f }, Dot::Warn, p);
            g.setColour(p.ink2);
            g.setFont(uiFont(12.5f));
            g.drawText(tr(Str::SyncViewersUntil), m.removeFromTop(18.0f).withTrimmedLeft(14.0f), juce::Justification::centredLeft, true);
        } else if (phase_ == P::Done) {
            const int ms = juce::roundToInt(std::abs(st.deltaMs));
            g.setColour(p.ink);
            g.setFont(uiFont(40.0f, Weight::SemiBold));
            g.drawText(juce::String(ms) + " ms", m.removeFromTop(48.0f), juce::Justification::centredLeft, false);
            g.setFont(uiFont(14.0f, Weight::Medium));
            g.drawText(trf(st.deltaMs >= 0.0 ? Str::SyncLateBy : Str::SyncEarlyBy, { juce::String(ms) }), m.removeFromTop(22.0f), juce::Justification::centredLeft, true);
            g.setColour(p.graphite);
            g.setFont(uiFont(12.5f));
            g.drawText(tr(st.deltaMs >= 0.0 ? Str::SyncFixedMusic : Str::SyncFixedVocal), m.removeFromTop(22.0f), juce::Justification::centredLeft, true);
        } else if (phase_ == P::Failed) {
            g.setColour(p.danger);
            g.setFont(uiFont(14.0f, Weight::SemiBold));
            g.drawText(tr(Str::SyncFailedTitle), m.removeFromTop(22.0f), juce::Justification::centredLeft, true);
            drawWrapped(g, tr(st.error), uiFont(12.5f), p.ink2, m.removeFromTop(juce::jmax(36.0f, wrappedHeight(uiFont(12.5f), tr(st.error), m.getWidth(), 4.0f))), 4.0f);
        }

        // right: current delays, tips
        if (!delaysCard_.isEmpty()) {
            auto d = delaysCard_.reduced(22.0f, 20.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(15.0f, Weight::SemiBold));
            g.drawText(tr(Str::CurrentDelays), d.removeFromTop(20.0f), juce::Justification::centredLeft, true);
            d.removeFromTop(8.0f);
            d.removeFromBottom(26.0f);
            g.setFont(uiFont(12.5f));
            auto line = [&](const juce::String& name, float ms) {
                if (d.getHeight() < 20.0f) return;
                auto row = d.removeFromTop(24.0f);
                g.setColour(p.ink2);
                g.drawText(name, row.withTrimmedRight(60.0f), juce::Justification::centredLeft, true);
                g.setColour(p.ink);
                g.drawText(valuetext::formatMs(ms), row, juce::Justification::centredRight, false);
            };
            for (const auto& v : ed.trackViews()) line(v.name, v.delayMs);
            for (const auto& s : ed.sourceViews()) line(HubEditor::programLabel(s.app), s.delayMs);
        }
        if (!tipsCard_.isEmpty()) {
            auto t = tipsCard_.reduced(22.0f, 20.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(15.0f, Weight::SemiBold));
            g.drawText(tr(Str::IfItDoesntWork), t.removeFromTop(20.0f), juce::Justification::centredLeft, true);
            t.removeFromTop(8.0f);
            const std::pair<Str, Str> tips[] = { { Str::TipSilentTitle, Str::TipSilent }, { Str::TipNoMicTitle, Str::TipNoMic },
                                                 { Str::TipWeakTitle, Str::TipWeak }, { Str::TipUnsteadyTitle, Str::TipUnsteady } };
            for (const auto& [title, text] : tips) {
                g.setColour(p.hairline2);
                g.fillRect(t.removeFromTop(1.0f));
                t.removeFromTop(12.0f);
                g.setColour(p.ink);
                g.setFont(uiFont(13.0f, Weight::SemiBold));
                g.drawText(tr(title), t.removeFromTop(18.0f), juce::Justification::centredLeft, true);
                t.removeFromTop(4.0f);
                const float th = wrappedHeight(uiFont(12.0f), tr(text), t.getWidth(), 6.0f);
                drawWrapped(g, tr(text), uiFont(12.0f), p.graphite, t.removeFromTop(th), 6.0f);
                t.removeFromTop(12.0f);
            }
        }
    }

private:
    struct CheckRow : juce::Button {
        CheckRow() : juce::Button({}) { setWantsKeyboardFocus(true); setMouseCursor(juce::MouseCursor::PointingHandCursor); setFocusShape(*this, 12.0f); }
        void paintButton(juce::Graphics& g, bool highlighted, bool) override {
            const auto& p = paletteOf(*this);
            auto r = getLocalBounds().toFloat();
            g.setColour(p.inset.overlaidWith(p.ink.withAlpha(highlighted ? 0.03f : 0.0f)));
            g.fillRoundedRectangle(r, 12.0f);
            auto c = r.withTrimmedLeft(12.0f).removeFromLeft(22.0f).withSizeKeepingCentre(22.0f, 22.0f);
            if (on) {
                g.setColour(p.ink);
                g.fillEllipse(c);
                icons::draw(g, icons::Icon::Check, c.reduced(5.0f), p.onInk, 2.4f);
            } else {
                g.setColour(p.hairline3.withMultipliedAlpha(1.6f));
                g.drawEllipse(c.reduced(0.75f), 1.5f);
            }
            g.setColour(p.ink);
            g.setFont(uiFont(13.5f));
            g.drawText(text, r.withTrimmedLeft(48.0f), juce::Justification::centredLeft, true);
        }
        bool on = false;
        juce::String text;
    };

    void fillTracks() {
        int mic = -1, ref = -1;
        bool refSource = false;
        ed.proc().defaultSyncTracks(mic, ref, refSource);
        juce::StringArray micItems, refItems;
        micSlots_.clear();
        refIds_.clear();
        for (const auto& v : ed.trackViews()) {
            micItems.add(v.name);
            micSlots_.push_back(v.slot);
            refItems.add(v.name);
            refIds_.push_back({ v.slot, false });
        }
        for (const auto& s : ed.sourceViews()) {
            refItems.add(HubEditor::programLabel(s.app));
            refIds_.push_back({ s.index, true });
        }
        mic_.setItems(micItems);
        ref_.setItems(refItems);
        for (size_t i = 0; i < micSlots_.size(); ++i) if (micSlots_[i] == mic) mic_.setSelected(int(i));
        for (size_t i = 0; i < refIds_.size(); ++i) if (refIds_[i].first == ref && refIds_[i].second == refSource) ref_.setSelected(int(i));
        start_.setEnabled(!micSlots_.empty() && !refIds_.empty());
    }

    void startMeasuring() {
        if (micSlots_.empty() || refIds_.empty()) { ed.toast(tr(Str::SyncNeedTracks), 3600); return; }
        const auto refId = refIds_[size_t(juce::jlimit(0, int(refIds_.size()) - 1, ref_.selected()))];
        ed.proc().startAutoSync(micSlots_[size_t(juce::jlimit(0, int(micSlots_.size()) - 1, mic_.selected()))], refId.first, refId.second, 3000);
        tick();
    }

    Dropdown mic_, ref_;
    juce::OwnedArray<CheckRow> checks_;
    PrimaryButton start_ { icons::Icon::Play, 50.0f };
    GhostButton cancel_, again_;
    LinkButton check_ { {}, 12.5f }, byHand_ { {}, 12.5f };
    std::vector<int> micSlots_;
    std::vector<std::pair<int, bool>> refIds_;
    HubProcessor::SyncPhase phase_ = HubProcessor::SyncPhase::Idle;
    juce::uint32 phaseStart_ = 0;
    bool hintBelow_ = false;
    juce::Rectangle<float> card_, delaysCard_, tipsCard_, step1_, step2_, step3_, labels_, sep1_, sep2_, measure_;
};

} // namespace

std::unique_ptr<HubPage> makeHubPage(HubEditor& ed, HubEditor::Page page, int slot) {
    std::unique_ptr<HubPage> p;
    switch (page) {
        case HubEditor::Page::Settings: p = std::make_unique<HubSettingsPage>(ed); break;
        case HubEditor::Page::Setup:    p = std::make_unique<SetupPage>(ed); break;
        case HubEditor::Page::Sync:     p = std::make_unique<SyncPage>(ed); break;
        case HubEditor::Page::Share:    p = makeSharePage(ed); break;
        case HubEditor::Page::Track:    p = makeTrackPage(ed, slot); break;
        case HubEditor::Page::Tracks:   p = makeTracksPage(ed); break;
        case HubEditor::Page::Programs: p = makeProgramsPage(ed); break;
        case HubEditor::Page::Main:     break;
    }
    return p;
}

} // namespace hearaside
