#include "HubEditor.h"
#include "Host.h"
#include "app/AppCapture.h"
#include "ui/SettingsPanel.h"

namespace hearaside {

using ssbus::ParamId;

namespace {

constexpr float kGap = 12.0f, kMoreW = 26.0f, kPadL = 16.0f, kPadR = 12.0f, kNameMinFull = 150.0f, kNameMinMid = 110.0f;
constexpr float kStreamCardH = 300.0f, kStripLineH = 20.0f;
constexpr float kSummaryFullH = 18.0f + 24.0f + 12.0f + 96.0f + 8.0f + 62.0f + 8.0f + 44.0f + 18.0f;   // without the latency box
constexpr int kSourcesHeadH = 52;
constexpr const char* kSystemAudio = "*system*";   // AppAudioProcessor::kSystemAudio

juce::String programLabel(const juce::String& exe) {
    if (exe == kSystemAudio) return tr(Str::AppSystem);
    if (exe == "*link*") return tr(Str::AppLinkIn);
    if (exe.startsWith("http")) return tr(Str::AppLinkFrom) + " " + juce::URL(exe).getDomain();
    return exe.endsWithIgnoreCase(".exe") ? exe.dropLastCharacters(4) : exe;
}

juce::String clock(double seconds) {
    const int s = juce::roundToInt(seconds);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

juce::Colour shadeFor(const theme::Palette& p, int index) {
    const juce::Colour shades[] = { p.trackShade1, p.trackShade2, p.trackShade3, p.trackShade4, p.trackShade5 };
    return shades[juce::jmax(0, index) % 5];
}

juce::String minusText(const juce::String& s) { return s.replace("-", juce::String(juce::CharPointer_UTF8("\xe2\x88\x92"))); }

void drawTextBlock(juce::Graphics& g, const juce::String& text, juce::Font f, juce::Colour c, juce::Rectangle<float> r,
                   float lineSpacing = 2.0f) {
    drawWrapped(g, text, f, c, r, lineSpacing);
}

// ---- per-track fine settings (remote: everything goes through the Track's mailbox) ----------
class HubTrackPanel : public juce::Component, private juce::Timer {
public:
    HubTrackPanel(HubEditor& ed, int slot) : ed_(ed), slot_(slot) {
        name_.setFont(uiFont(13.0f));
        name_.setIndents(10, 6);
        name_.onReturnKey = name_.onFocusLost = [this] {
            if (name_.getText().trim().isNotEmpty() && name_.getText() != lastName_) ed_.proc().rename(slot_, name_.getText());
        };
        pan_.setRange(-100.0, 100.0, 1.0);
        pan_.setDoubleClickReturnValue(true, 0.0);
        pan_.onValueChange = [this] { if (!updating_) { touched_ = now(); ed_.proc().send(slot_, ParamId::StrPan, float(pan_.getValue()) / 100.0f); } };
        delay_.setRange(0.0, 500.0, 1.0);
        delay_.setSkewFactorFromMidPoint(100.0);
        delay_.setDoubleClickReturnValue(true, 0.0);
        delay_.setTooltip(tr(Str::DelayTip));
        delay_.onValueChange = [this] { if (!updating_) { touched_ = now(); ed_.proc().send(slot_, ParamId::StrDelayMs, float(delay_.getValue())); } };
        stem_.addItem(tr(Str::StemNone), 1);
        for (int i = 0; i < ssbus::kMaxStems; ++i)
            stem_.addItem(ed_.proc().stemName(i).isNotEmpty() ? ed_.proc().stemName(i) : "Stem " + juce::String(i + 1), i + 2);
        stem_.onChange = [this] { if (!updating_) { touched_ = now(); ed_.proc().send(slot_, ParamId::StemIndex, float(stem_.getSelectedId() - 2)); } };
        solo_.onClick = [this] { touched_ = now(); ed_.proc().send(slot_, ParamId::StrSolo, solo_.isOn() ? 0.0f : 1.0f); };
        hp_.onValueChange = [this] { if (!updating_) { touched_ = now(); ed_.proc().send(slot_, ParamId::MonTrimDb, LevelSlider::sliderToDb(hp_.getValue())); } };
        vw_.onValueChange = [this] { if (!updating_) { touched_ = now(); ed_.proc().send(slot_, ParamId::StrGainDb, LevelSlider::sliderToDb(vw_.getValue())); } };
        hp_.setTooltip(tr(Str::HeadphoneTip));
        for (auto* r : rows()) addAndMakeVisible(r);
        setSize(360, int(rows().size()) * 42 + 16);
        timerCallback();
        startTimerHz(15);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        for (auto* row : rows()) row->setBounds(r.removeFromTop(42));
    }

private:
    static juce::uint32 now() { return juce::Time::getMillisecondCounter(); }
    std::vector<FormRow*> rows() { return { &hpRow_, &vwRow_, &nameRow_, &panRow_, &delayRow_, &stemRow_, &soloRow_ }; }

    void timerCallback() override {
        const auto views = ed_.proc().tracks();
        const auto it = std::find_if(views.begin(), views.end(), [this](const TrackView& v) { return v.slot == slot_; });
        if (it == views.end()) return;
        const juce::ScopedValueSetter<bool> svs(updating_, true);
        if (!name_.hasKeyboardFocus(true) && it->name != lastName_) { lastName_ = it->name; name_.setText(it->name, false); }
        const bool idle = now() - touched_ > 400 && !juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
        if (idle) {
            hp_.setValue(LevelSlider::dbToSlider(it->trimDb), juce::dontSendNotification);
            vw_.setValue(LevelSlider::dbToSlider(it->gainDb), juce::dontSendNotification);
            pan_.setValue(it->pan * 100.0, juce::dontSendNotification);
            delay_.setValue(it->delayMs, juce::dontSendNotification);
            stem_.setSelectedId(it->stem + 2, juce::dontSendNotification);
        }
        solo_.setOn(it->solo, true);
        hp_.setEnabled(it->mon);
        vw_.setEnabled(it->str);
        hpRow_.setCaption(formatDb(LevelSlider::sliderToDb(hp_.getValue())));
        vwRow_.setCaption(formatDb(LevelSlider::sliderToDb(vw_.getValue())));
        panRow_.setCaption(juce::roundToInt(pan_.getValue()) == 0 ? juce::String("C")
                           : (pan_.getValue() < 0 ? "L" : "R") + juce::String(std::abs(juce::roundToInt(pan_.getValue()))));
        delayRow_.setCaption(juce::String(juce::roundToInt(delay_.getValue())) + " ms");
    }

    HubEditor& ed_;
    int slot_;
    juce::String lastName_;
    juce::TextEditor name_;
    juce::Slider pan_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider delay_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::ComboBox stem_;
    Switch solo_;
    LevelSlider hp_, vw_;
    FormRow hpRow_ { Str::HeadphoneLevel, hp_, 170 }, vwRow_ { Str::ViewersLevel, vw_, 170 };
    FormRow nameRow_ { Str::RenameDisplay, name_, 170 }, panRow_ { Str::Pan, pan_, 170 }, delayRow_ { Str::ViewersDelay, delay_, 170 },
            stemRow_ { Str::Stem, stem_, 170 }, soloRow_ { Str::StreamSolo, solo_, 50 };
    bool updating_ = false;
    juce::uint32 touched_ = 0;
};

// ---- one App Audio's headphone / viewers levels (narrow rows have no sliders) -----------------
class HubSourceLevelsPanel : public juce::Component, private juce::Timer {
public:
    HubSourceLevelsPanel(HubEditor& ed, int index) : ed_(ed), index_(index) {
        hp_.onValueChange = [this] { send(hp_, ParamId::MonTrimDb); };
        vw_.onValueChange = [this] { send(vw_, ParamId::StrGainDb); };
        hp_.setTooltip(tr(Str::HeadphoneTip));
        for (auto* r : { &hpRow_, &vwRow_ }) addAndMakeVisible(r);
        setSize(360, 2 * 42 + 16);
        timerCallback();
        startTimerHz(15);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(8);
        for (auto* row : { &hpRow_, &vwRow_ }) row->setBounds(r.removeFromTop(42));
    }

private:
    void send(LevelSlider& s, ParamId id) {
        if (updating_ || slot_ < 0) return;
        touched_ = juce::Time::getMillisecondCounter();
        ed_.proc().send(slot_, id, LevelSlider::sliderToDb(s.getValue()));
    }
    void timerCallback() override {
        const auto views = ed_.proc().sources();
        const auto it = std::find_if(views.begin(), views.end(), [this](const SourceView& v) { return v.index == index_; });
        if (it == views.end()) return;
        slot_ = it->slot;
        const juce::ScopedValueSetter<bool> svs(updating_, true);
        if (juce::Time::getMillisecondCounter() - touched_ > 400 && !juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown()) {
            hp_.setValue(LevelSlider::dbToSlider(it->trimDb), juce::dontSendNotification);
            vw_.setValue(LevelSlider::dbToSlider(it->gainDb), juce::dontSendNotification);
        }
        hp_.setEnabled(slot_ >= 0 && it->mon);
        vw_.setEnabled(slot_ >= 0 && it->str);
        hpRow_.setCaption(formatDb(LevelSlider::sliderToDb(hp_.getValue())));
        vwRow_.setCaption(formatDb(LevelSlider::sliderToDb(vw_.getValue())));
    }
    HubEditor& ed_;
    int index_, slot_ = -1;
    LevelSlider hp_, vw_;
    FormRow hpRow_ { Str::HeadphoneLevel, hp_, 170 }, vwRow_ { Str::ViewersLevel, vw_, 170 };
    bool updating_ = false;
    juce::uint32 touched_ = 0;
};

// ---- levels (compact Hub): stream level, headphone master, peak protection -------------------
class HubLevelsPanel : public juce::Component, private juce::Timer {
public:
    explicit HubLevelsPanel(HubProcessor& p) : proc_(p) {
        master_ = std::make_unique<DbSliderLink>(masterSlider_, *proc_.params().getParameter(hubparam::Master));
        phones_ = std::make_unique<DbSliderLink>(phonesSlider_, *proc_.params().getParameter(hubparam::Headphones));
        masterSlider_.setTitle(tr(Str::Master));
        phonesSlider_.setTitle(tr(Str::HeadphoneMaster));
        phonesSlider_.setTooltip(tr(Str::HeadphoneMasterTip));
        limiter_.setTitle(tr(Str::Limiter));
        limiter_.onClick = [this] { proc_.setParam(hubparam::LimiterOn, on(hubparam::LimiterOn) ? 0.0f : 1.0f); timerCallback(); };
        for (auto* r : { &masterRow_, &phonesRow_, &limiterRow_ }) addAndMakeVisible(r);
        limiterRow_.setCaption(tr(Str::LimiterCaption));
        phonesRow_.setCaption(tr(Str::HeadphoneMasterTip).upToFirstOccurrenceOf(" (", false, false));
        setSize(360, 3 * 48 + 16);
        timerCallback();
        startTimerHz(15);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(8);
        for (auto* row : { &masterRow_, &phonesRow_, &limiterRow_ }) row->setBounds(r.removeFromTop(48));
    }

private:
    bool on(const char* id) const { return proc_.params().getRawParameterValue(id)->load() > 0.5f; }
    void timerCallback() override {
        master_->update();
        phones_->update();
        limiter_.setOn(on(hubparam::LimiterOn), isShowing());
        masterRow_.setCaption(formatDb(float(masterSlider_.getValue())));
    }
    HubProcessor& proc_;
    LevelSlider masterSlider_, phonesSlider_;
    Switch limiter_;
    std::unique_ptr<DbSliderLink> master_, phones_;
    FormRow masterRow_ { Str::Master, masterSlider_, 170 }, phonesRow_ { Str::HeadphoneMaster, phonesSlider_, 170 },
            limiterRow_ { Str::Limiter, limiter_, 50 };
};

// ---- Hub settings (gear): bus, sync, ceiling, stem names + UI preferences --------------------
class HubSettingsPanel : public juce::Component {
public:
    explicit HubSettingsPanel(HubProcessor& p) : proc_(p) {
        bus_.setText(proc_.busName(), false);
        bus_.onReturnKey = bus_.onFocusLost = [this] { proc_.setBusName(bus_.getText()); };
        shareBase_.setText(settings_->shareBase(), false);
        shareBase_.setTextToShowWhenEmpty("https://....vercel.app", paletteOf(*this).graphite);
        shareBase_.onReturnKey = shareBase_.onFocusLost = [this] { settings_->setShareBase(shareBase_.getText()); shareBase_.setText(settings_->shareBase(), false); };
        shareBaseRow_.setCaption(tr(Str::ShareBaseHint));
        permanent_.setOn(settings_->permanentLinks(), false);
        permanent_.setTitle(tr(Str::PermanentLinks));
        permanent_.onClick = [this] { settings_->setPermanentLinks(!settings_->permanentLinks()); permanent_.setOn(settings_->permanentLinks(), true); };
        permanentRow_.setCaption(tr(Str::SharePermanent));
        restOn_.setOn(settings_->restApi(), false);
        restOn_.setTitle(tr(Str::RestApi));
        restOn_.onClick = [this] { settings_->setRestApi(!settings_->restApi()); restOn_.setOn(settings_->restApi(), true); refreshRest(); };
        copyKey_.setButtonText(tr(Str::CopyApiKey));
        copyKey_.onClick = [this] {
            juce::SystemClipboard::copyTextToClipboard(settings_->restApiKey());
            copyKey_.setButtonText(tr(Str::Copied));
        };
        inner_.addAndMakeVisible(copyKey_);
        refreshRest();
        sync_.addItemList({ "0", "1 " + tr(Str::Blocks), "2 " + tr(Str::Blocks) }, 1);
        syncAttachment_ = std::make_unique<juce::ComboBoxParameterAttachment>(*proc_.params().getParameter(hubparam::SyncSafety), sync_);
        syncRow_.setCaption(tr(Str::SyncSafetyHint));
        ceiling_.setRange(-12.0, 0.0, 0.1);
        ceilingAttachment_ = std::make_unique<juce::SliderParameterAttachment>(*proc_.params().getParameter(hubparam::Ceiling), ceiling_);
        ceiling_.onValueChange = [this] { ceilingRow_.setCaption(minusText(juce::String(ceiling_.getValue(), 1)) + " dBFS"); };
        ceiling_.onValueChange();
        for (auto* e : { &bus_, &shareBase_ }) { e->setFont(uiFont(13.0f)); e->setIndents(10, 6); }
        for (auto* r : { &busRow_, &syncRow_, &ceilingRow_, &shareBaseRow_, &permanentRow_, &restRow_ }) inner_.addAndMakeVisible(r);
        for (int i = 0; i < ssbus::kMaxStems; ++i) {
            auto* e = stems_.add(new juce::TextEditor());
            e->setFont(uiFont(13.0f));
            e->setIndents(10, 6);
            e->setText(proc_.stemName(i), false);
            e->setTextToShowWhenEmpty("Stem " + juce::String(i + 1), paletteOf(*this).graphite);
            e->onReturnKey = e->onFocusLost = [this, i, e] { proc_.setStemName(i, e->getText()); };
            inner_.addAndMakeVisible(e);
        }
        inner_.addAndMakeVisible(ui_);
        const int h = 6 * 48 + 40 + 30 + 4 * 40 + 20 + ui_.idealHeight() + 16;
        inner_.setSize(380, h);
        viewport_.setViewedComponent(&inner_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        setSize(396, juce::jmin(h, 540));
        inner_.onResize = [this] { layoutInner(); };
        layoutInner();
    }

    void resized() override { viewport_.setBounds(getLocalBounds()); }

private:
    struct Inner : juce::Component {
        std::function<void()> onResize;
        void resized() override { if (onResize) onResize(); }
        void paint(juce::Graphics& g) override {
            const auto& p = paletteOf(*this);
            g.setColour(p.ink);
            g.setFont(uiFont(13.0f, Weight::Medium));
            g.drawText(tr(Str::StemNames), stemTitle, juce::Justification::centredLeft, false);
            g.setColour(p.hairline2);
            g.fillRect(divider);
        }
        juce::Rectangle<int> stemTitle, divider;
    };

    void layoutInner() {
        auto r = inner_.getLocalBounds().reduced(8);
        for (auto* row : { &busRow_, &syncRow_, &ceilingRow_, &shareBaseRow_, &permanentRow_, &restRow_ }) row->setBounds(r.removeFromTop(48));
        copyKey_.setBounds(r.removeFromTop(40).reduced(0, 4).removeFromRight(200));
        inner_.stemTitle = r.removeFromTop(30);
        for (int i = 0; i < stems_.size(); i += 2) {
            auto line = r.removeFromTop(40).reduced(0, 5);
            stems_[i]->setBounds(line.removeFromLeft(line.getWidth() / 2 - 4));
            if (i + 1 < stems_.size()) stems_[i + 1]->setBounds(line.withTrimmedLeft(4));
        }
        r.removeFromTop(10);
        inner_.divider = r.removeFromTop(1);
        r.removeFromTop(9);
        ui_.setBounds(r);
    }

    HubProcessor& proc_;
    juce::Viewport viewport_;
    Inner inner_;
    juce::TextEditor bus_;
    juce::ComboBox sync_;
    juce::Slider ceiling_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    std::unique_ptr<juce::ComboBoxParameterAttachment> syncAttachment_;
    std::unique_ptr<juce::SliderParameterAttachment> ceilingAttachment_;
    juce::OwnedArray<juce::TextEditor> stems_;
    // REST API: the address while on (the key is written when the Hub starts the server)
    void refreshRest() {
        restRow_.setCaption(settings_->restApi() ? juce::String("http://127.0.0.1:" + juce::String(proc_.control().running() ? proc_.control().port() : ControlServer::kFirstPort) + "/api/v1")
                                                 : tr(Str::RestApiCap));
        copyKey_.setEnabled(settings_->restApi());
    }

    SharedSettings settings_;
    juce::TextEditor shareBase_;
    Switch permanent_, restOn_;
    juce::TextButton copyKey_;
    FormRow busRow_ { Str::BusName, bus_, 170 }, syncRow_ { Str::SyncSafety, sync_, 170 }, ceilingRow_ { Str::Ceiling, ceiling_, 170 },
            shareBaseRow_ { Str::ShareBase, shareBase_, 200 }, permanentRow_ { Str::PermanentLinks, permanent_, 50 },
            restRow_ { Str::RestApi, restOn_, 50 };
    UiSettingsPanel ui_;
};

// ---- auto sync: pick the mic and the music track, measure, show the result -------------------
class HubSyncPanel : public juce::Component, private juce::Timer {
public:
    explicit HubSyncPanel(HubProcessor& p) : proc_(p) {
        for (const auto& s : proc_.sources())
            ref_.addItem("App Audio: " + s.name + (s.app.isNotEmpty() ? " (" + programLabel(s.app) + ")" : juce::String()), kSourceId + s.index);
        for (const auto& v : proc_.tracks()) {
            mic_.addItem(v.name, v.slot + 1);
            ref_.addItem(v.name, v.slot + 1);
        }
        int mic = -1, ref = -1;
        bool refSource = false;
        proc_.defaultSyncTracks(mic, ref, refSource);
        mic_.setSelectedId(mic + 1, juce::dontSendNotification);
        ref_.setSelectedId(refSource ? kSourceId + ref : ref + 1, juce::dontSendNotification);
        start_.onClick = [this] {
            if (busy()) proc_.cancelAutoSync();
            else {
                const int r = ref_.getSelectedId();
                proc_.startAutoSync(mic_.getSelectedId() - 1, r >= kSourceId ? r - kSourceId : r - 1, r >= kSourceId, 3000);
            }
            timerCallback();
        };
        for (auto* c : std::initializer_list<juce::Component*> { &micRow_, &refRow_, &start_ }) addAndMakeVisible(c);
        // as tall as the steps need, + room for the status lines
        const float howH = wrappedHeight(uiFont(12.0f), tr(Str::SyncHow), 420.0f - 32.0f, 2.0f);
        setSize(420, juce::roundToInt(16.0f + 26.0f + 6.0f + howH + 4.0f + 8.0f + 2.0f * 42.0f + 10.0f + 40.0f + 10.0f + 64.0f + 16.0f));
        timerCallback();
        startTimerHz(10);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(16);
        title_ = r.removeFromTop(26).toFloat();
        r.removeFromTop(6);
        how_ = r.removeFromTop(int(wrappedHeight(uiFont(12.0f), tr(Str::SyncHow), float(r.getWidth()), 2.0f)) + 4).toFloat();
        r.removeFromTop(8);
        micRow_.setBounds(r.removeFromTop(42));
        refRow_.setBounds(r.removeFromTop(42));
        r.removeFromTop(10);
        start_.setBounds(r.removeFromTop(40));
        r.removeFromTop(10);
        status_ = r.toFloat();
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText(tr(Str::SyncTitle), title_, juce::Justification::centredLeft, true);
        drawWrapped(g, tr(Str::SyncHow), uiFont(12.0f), p.graphite, how_, 2.0f);
        drawWrapped(g, statusText(), uiFont(13.0f, Weight::Medium), p.ink, status_, 2.0f);
    }

private:
    static constexpr int kSourceId = 1000;   // combo ids of App Audio entries

    bool busy() const {
        const auto ph = proc_.autoSync().phase;
        return ph == HubProcessor::SyncPhase::Countdown || ph == HubProcessor::SyncPhase::Reference || ph == HubProcessor::SyncPhase::Microphone;
    }

    juce::String statusText() const { return syncStatusText(proc_); }

public:
    static juce::String syncStatusText(const HubProcessor& proc) {
        const auto st = proc.autoSync();
        using P = HubProcessor::SyncPhase;
        switch (st.phase) {
            case P::Idle:       return {};
            case P::Countdown:  return tr(Str::SyncCountdown) + " " + juce::String(proc.syncCountdown());
            case P::Reference:  return tr(Str::SyncMeasuringRef);
            case P::Microphone: return tr(Str::SyncMeasuringMic);
            case P::Failed:     return tr(st.error);
            case P::Done: {
                const auto ms = juce::String(std::abs(st.deltaMs), 1) + " ms";
                return (st.deltaMs >= 0.0 ? tr(Str::SyncLate) : tr(Str::SyncEarly)) + " " + ms + " - " + tr(Str::SyncInTime);
            }
        }
        return {};
    }

private:
    void timerCallback() override {
        const bool b = busy();
        start_.setButtonText(b ? tr(Str::SyncCancel) : tr(Str::SyncStart));
        start_.setActive(b);
        mic_.setEnabled(!b);
        ref_.setEnabled(!b);
        const auto text = statusText();
        if (text != lastStatus_) { lastStatus_ = text; repaint(); }
    }

    HubProcessor& proc_;
    juce::ComboBox mic_, ref_;
    FormRow micRow_ { Str::SyncMic, mic_, 200 }, refRow_ { Str::SyncRef, ref_, 200 };
    PrimaryButton start_ { icons::Icon::Headphones, 40.0f };
    juce::Rectangle<float> title_, how_, status_;
    juce::String lastStatus_;
};

// ---- Setup check: what is ready, what to fix (docs/ux-roadmap.md 4.1) -----------------------------
// Every line reads the real state and says what happened, what it means for you and for the
// viewers, and how to fix it. The same nine lines are always there, so the panel never jumps.
class HubSetupPanel : public juce::Component, private juce::Timer {
public:
    explicit HubSetupPanel(HubProcessor& p) : proc_(p) {
        copy_.setButtonText(tr(Str::CopyReport));
        copy_.setTooltip(tr(Str::CopyReportTip));
        copy_.onClick = [this] {
            juce::SystemClipboard::copyTextToClipboard(proc_.diagnostics());
            copy_.setButtonText(tr(Str::Copied));
        };
        addAndMakeVisible(copy_);
        items_ = check();
        setSize(kW, juce::roundToInt(contentHeight()));
        startTimerHz(2);
    }

    void resized() override { copy_.setBounds(getLocalBounds().reduced(18).removeFromBottom(32).removeFromLeft(220)); }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto r = getLocalBounds().toFloat().reduced(18.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(16.0f, Weight::SemiBold));
        g.drawText(tr(Str::SetupTitle), r.removeFromTop(26.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(8.0f);
        for (const auto& it : items_) {
            const float h = itemHeight(it);
            auto box = r.removeFromTop(h);
            r.removeFromTop(6.0f);
            if (it.level > 0) drawInset(g, box, theme::radius::small, p, it.level == 2);
            auto in = box.reduced(10.0f, 6.0f);
            auto icon = in.removeFromLeft(22.0f).removeFromTop(20.0f);
            if (it.level == 0) {
                g.setColour(p.ink);
                g.fillEllipse(icon.withSizeKeepingCentre(18.0f, 18.0f));
                icons::draw(g, icons::Icon::Check, icon.withSizeKeepingCentre(12.0f, 12.0f), p.onInk, 2.4f);
            } else {
                icons::draw(g, it.level == 2 ? icons::Icon::SpeakerOff : icons::Icon::Warning, icon.withSizeKeepingCentre(17.0f, 17.0f), p.ink, 2.0f);
            }
            in.removeFromLeft(8.0f);
            for (const auto& [text, font, colour] : lines(it, p)) {
                const float th = wrappedHeight(font, text, in.getWidth(), 2.0f);
                drawWrapped(g, text, font, colour, in.removeFromTop(th), 2.0f);
                in.removeFromTop(2.0f);
            }
        }
    }

private:
    static constexpr int kW = 440;
    struct Item {
        int level = 0;   // 0 fine, 1 should be fixed, 2 the viewers are affected
        juce::String what, you, viewers, fix;
    };
    using Line = std::tuple<juce::String, juce::Font, juce::Colour>;

    std::vector<Line> lines(const Item& it, const theme::Palette& p) const {
        std::vector<Line> out { { it.what, uiFont(13.0f, it.level > 0 ? Weight::SemiBold : Weight::Regular), p.ink } };
        if (it.level == 0) return out;
        if (it.you.isNotEmpty()) out.push_back({ tr(Str::MonLabel) + ": " + it.you, uiFont(12.0f), p.graphite });
        if (it.viewers.isNotEmpty()) out.push_back({ tr(Str::StrLabel) + ": " + it.viewers, uiFont(12.0f), p.graphite });
        if (it.fix.isNotEmpty()) out.push_back({ juce::String(juce::CharPointer_UTF8("\xe2\x86\x92 ")) + it.fix, uiFont(12.0f, Weight::Medium), p.ink });
        return out;
    }
    float itemHeight(const Item& it) const {
        const auto& p = paletteOf(*this);
        float h = 12.0f;
        for (const auto& [text, font, colour] : lines(it, p)) h += wrappedHeight(font, text, float(kW) - 36.0f - 20.0f - 30.0f, 2.0f) + 2.0f;
        return juce::jmax(32.0f, h);
    }
    float contentHeight() const {
        float h = 18.0f + 26.0f + 8.0f;
        for (const auto& it : items_) h += itemHeight(it) + 6.0f;
        return h + 8.0f + 32.0f + 18.0f + 60.0f;   // + room for longer texts later
    }

    std::vector<Item> check() const {
        std::vector<Item> out;
        const bool owner = proc_.connected() && proc_.engine().role() == ssengine::HubEngine::Role::Owner;
        const auto ts = proc_.tracks();
        const auto ss = proc_.sources();
        out.push_back(owner ? Item { 0, tr(Str::SetupHubOk) }
                            : Item { 2, tr(Str::SecondHubTitle), {}, tr(Str::SetupNothing), tr(Str::SetupHubFix) });
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
        if (!same) { items_ = std::move(now); repaint(); }
    }

    HubProcessor& proc_;
    juce::TextButton copy_;
    std::vector<Item> items_;
};

// ---- share links (like LISTENTO) ------------------------------------------------------------
// With a share web site set (Settings), the links are permanent (https://<site>/l/<token>) and a
// backup link (the tunnel or Wi-Fi address of today) is shown below them.
class HubSharePanel : public juce::Component, private juce::Timer {
public:
    explicit HubSharePanel(HubProcessor& p) : proc_(p) {
        on_.onClick = [this] { proc_.setSharing(!proc_.sharing()); timerCallback(); };
        on_.setTitle(tr(Str::ShareTitle));
        for (auto* e : { &listenUrl_, &sendUrl_, &backupUrl_ }) {
            e->setReadOnly(true);
            e->setFont(uiFont(13.0f));
            e->setIndents(10, 7);
            e->setCaretVisible(false);
        }
        listenUrl_.setTitle(tr(Str::ShareListen));
        sendUrl_.setTitle(tr(Str::ShareSend));
        backupUrl_.setTitle(tr(Str::ShareBackup));
        auto copier = [](juce::TextEditor& from, juce::TextButton& b) {
            juce::SystemClipboard::copyTextToClipboard(from.getText());
            b.setButtonText(tr(Str::Copied));
            juce::Component::SafePointer<juce::TextButton> sp(&b);
            juce::Timer::callAfterDelay(1500, [sp] { if (sp) sp->setButtonText(tr(Str::Copy)); });
        };
        copyListen_.onClick = [this, copier] { copier(listenUrl_, copyListen_); };
        copySend_.onClick = [this, copier] { copier(sendUrl_, copySend_); };
        copyBackup_.onClick = [this, copier] { copier(backupUrl_, copyBackup_); };
        // on this computer: localhost (no tunnel round trip, and a secure context for the player)
        openListen_.onClick = [this] { if (proc_.share().running()) juce::URL(proc_.share().localListenUrl()).launchInDefaultBrowser(); };
        install_.onClick = [this] {
            juce::SystemClipboard::copyTextToClipboard("winget install --id Cloudflare.cloudflared");
            install_.setButtonText(tr(Str::Copied));
        };
        for (auto* b : { &copyListen_, &copySend_, &copyBackup_, &openListen_, &install_ })
            b->setButtonText(tr(b == &openListen_ ? Str::OpenLink : b == &install_ ? Str::CopyInstall : Str::Copy));
        for (juce::Component* c : std::initializer_list<juce::Component*> { &on_, &listenUrl_, &sendUrl_, &backupUrl_, &copyListen_, &copySend_,
                                                                            &copyBackup_, &openListen_, &install_ })
            addAndMakeVisible(c);
        setSize(460, 640);
        timerCallback();
        startTimerHz(4);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(18);
        auto top = r.removeFromTop(34);
        on_.setBounds(top.removeFromRight(50).withSizeKeepingCentre(50, 30));
        title_ = top.toFloat();
        r.removeFromTop(8);
        permanent_ = r.removeFromTop(40).toFloat();
        r.removeFromTop(8);
        auto block = [&](juce::Rectangle<float>& box, juce::Rectangle<float>& text, juce::TextEditor& url,
                         juce::TextButton& a, juce::TextButton* b) {
            box = r.removeFromTop(148).toFloat();
            auto in = box.reduced(14.0f, 12.0f);
            text = in.removeFromTop(40.0f);
            in.removeFromTop(8.0f);
            url.setBounds(in.removeFromTop(36.0f).toNearestInt());
            in.removeFromTop(8.0f);
            auto row = in.removeFromTop(32.0f);
            a.setBounds(row.removeFromLeft(110.0f).toNearestInt());
            row.removeFromLeft(8.0f);
            if (b) b->setBounds(row.removeFromLeft(80.0f).toNearestInt());
            r.removeFromTop(12);
        };
        block(listenBox_, listenText_, listenUrl_, copyListen_, &openListen_);
        statusL_ = listenBox_.reduced(14.0f, 12.0f).removeFromBottom(32.0f).withTrimmedLeft(210.0f);
        block(sendBox_, sendText_, sendUrl_, copySend_, nullptr);
        statusS_ = sendBox_.reduced(14.0f, 12.0f).removeFromBottom(32.0f).withTrimmedLeft(130.0f);
        // backup: today's tunnel / Wi-Fi link, for when the share web site is down
        backupLabel_ = r.removeFromTop(20).toFloat();
        auto row = r.removeFromTop(36);
        copyBackup_.setBounds(row.removeFromRight(90));
        row.removeFromRight(8);
        backupUrl_.setBounds(row);
        r.removeFromTop(12);
        tunnel_ = r.removeFromTop(58).toFloat();
        r.removeFromTop(6);
        install_.setBounds(r.removeFromTop(32).removeFromLeft(200));
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        g.setColour(p.ink);
        g.setFont(uiFont(16.0f, Weight::SemiBold));
        g.drawText(tr(Str::ShareTitle), title_, juce::Justification::centredLeft, true);
        const bool on = proc_.sharing();
        drawWrapped(g, permanentText(), uiFont(12.5f, Weight::Medium), p.ink, permanent_, 2.0f);
        auto box = [&](juce::Rectangle<float> b, juce::Rectangle<float> t, Str title, Str cap, const juce::String& status, juce::Rectangle<float> st) {
            drawInset(g, b, theme::radius::small, p);
            g.setColour(on ? p.ink : p.muted);
            g.setFont(uiFont(14.0f, Weight::Medium));
            g.drawText(tr(title), t.removeFromTop(18.0f), juce::Justification::centredLeft, true);
            g.setColour(p.graphite);
            g.setFont(uiFont(11.5f));
            g.drawFittedText(tr(cap), t.toNearestInt(), juce::Justification::topLeft, 2, 0.85f);
            g.setColour(p.ink);
            g.setFont(uiFont(12.0f, Weight::Medium));
            g.drawText(status, st, juce::Justification::centredRight, true);
        };
        const auto& sh = proc_.share();
        box(listenBox_, listenText_, Str::ShareListen, Str::ShareListenCap,
            on ? tr(Str::Listeners) + " " + juce::String(sh.listeners()) : juce::String(), statusL_);
        box(sendBox_, sendText_, Str::ShareSend, Str::ShareSendCap,
            on ? (sh.senderActive() ? tr(Str::SenderOn) : tr(Str::SenderOff)) : juce::String(), statusS_);
        if (backupUrl_.isVisible()) {
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f));
            g.drawText(tr(Str::ShareBackup), backupLabel_, juce::Justification::centredLeft, true);
        }
        if (on) {
            Str t = Str::TunnelMissing;
            switch (sh.tunnel()) {
                case ShareServer::Tunnel::Ready:    t = Str::TunnelReady; break;
                case ShareServer::Tunnel::Starting: t = Str::TunnelStarting; break;
                case ShareServer::Tunnel::Failed:   t = Str::TunnelFailed; break;
                default: break;
            }
            drawWrapped(g, tr(t), uiFont(12.0f), sh.tunnel() == ShareServer::Tunnel::Ready ? p.ink : p.graphite, tunnel_, 2.0f);
        }
    }

private:
    // what the permanent link is doing right now (what happened -> what it means -> what to do)
    juce::String permanentText() const {
        if (!proc_.permanentLinksSet()) return tr(Str::ShareNoBase);
        if (!proc_.sharing()) return tr(Str::SharePermanent);
        if (proc_.share().tunnel() == ShareServer::Tunnel::Missing || proc_.share().tunnel() == ShareServer::Tunnel::Failed)
            return tr(Str::ShareDirNeedsTunnel);
        switch (proc_.directory().state()) {
            case ShareDirectory::State::Online:      return tr(Str::ShareDirOnline);
            case ShareDirectory::State::Unreachable: return tr(Str::ShareDirOffline);
            case ShareDirectory::State::Registering:
            case ShareDirectory::State::Off:         break;
        }
        return tr(Str::ShareDirRegistering);
    }

    void timerCallback() override {
        const bool on = proc_.sharing();
        on_.setOn(on, isShowing());
        const auto& sh = proc_.share();
        const bool live = on && sh.running();
        const bool permanent = proc_.permanentLinksSet();
        // the permanent link can be copied any time (send it once); the others only exist while sharing
        const auto l = permanent ? proc_.permanentUrl(true) : live ? sh.listenUrl() : juce::String();
        const auto s = permanent ? proc_.permanentUrl(false) : live ? sh.sendUrl() : juce::String();
        const auto b = permanent && live ? sh.listenUrl() : juce::String();
        if (listenUrl_.getText() != l) listenUrl_.setText(l, false);
        if (sendUrl_.getText() != s) sendUrl_.setText(s, false);
        if (backupUrl_.getText() != b) backupUrl_.setText(b, false);
        for (auto* c : std::initializer_list<juce::Component*> { &listenUrl_, &copyListen_ }) c->setEnabled(l.isNotEmpty());
        for (auto* c : std::initializer_list<juce::Component*> { &sendUrl_, &copySend_ }) c->setEnabled(s.isNotEmpty());
        openListen_.setEnabled(live);
        backupUrl_.setVisible(b.isNotEmpty());
        copyBackup_.setVisible(b.isNotEmpty());
        install_.setVisible(on && sh.tunnel() == ShareServer::Tunnel::Missing);
        repaint();
    }

    HubProcessor& proc_;
    Switch on_;
    juce::TextEditor listenUrl_, sendUrl_, backupUrl_;
    juce::TextButton copyListen_, copySend_, copyBackup_, openListen_, install_;
    juce::Rectangle<float> title_, permanent_, listenBox_, listenText_, sendBox_, sendText_, statusL_, statusS_, backupLabel_, tunnel_;
};

} // namespace

// =============================================================================================
// row geometry (shared by TrackRow, SourceRow and the column header)

RowCols rowCols(float w) {
    const float fixed = kPadL + kMoreW + 6.0f + kPadR + 3.0f * kGap;
    if (w - fixed - 2.0f * 118.0f - 190.0f >= kNameMinFull) return { RowTier::Full, 118.0f, 190.0f };
    if (w - fixed - 2.0f * 104.0f - 126.0f >= kNameMinMid) return { RowTier::Mid, 104.0f, 126.0f };
    return { RowTier::Narrow, 0.0f, 0.0f };
}

namespace {

// Badges right to left along a name line; one that would leave the name under 48 px gets no room.
// Returns where the name has to stop.
float placeBadges(juce::Rectangle<float> line, std::initializer_list<Badge*> badges, Badge* warn) {
    float right = line.getRight();
    auto place = [&](Badge* b, float w) {
        if (right - w < line.getX() + 48.0f) { b->setBounds({}); return; }
        b->setBounds(juce::Rectangle<float>(right - w, line.getCentreY() - 9.0f, w, 18.0f).toNearestInt());
        right -= w + 6.0f;
    };
    for (auto* b : badges) if (b->isVisible()) place(b, float(b->idealWidth()));
    if (warn != nullptr && warn->isVisible()) place(warn, 18.0f);
    return right;
}

// two compact lines (headphones, viewers): [icon] [slider] [value]
void layoutLevels(juce::Rectangle<float> level, juce::Rectangle<float>& hpIcon, juce::Rectangle<float>& hpValue, juce::Slider& hp,
                  juce::Rectangle<float>& vwIcon, juce::Rectangle<float>& vwValue, juce::Slider& vw) {
    auto line = [&](juce::Rectangle<float> l, juce::Rectangle<float>& icon, juce::Rectangle<float>& value, juce::Slider& s) {
        icon = l.removeFromLeft(14.0f);
        l.removeFromLeft(6.0f);
        value = l.removeFromRight(50.0f);
        l.removeFromRight(4.0f);
        s.setBounds(l.withSizeKeepingCentre(l.getWidth(), 20.0f).toNearestInt());
    };
    line(level.removeFromTop(level.getHeight() * 0.5f), hpIcon, hpValue, hp);
    line(level, vwIcon, vwValue, vw);
}

// narrow rows: both pills side by side on their own line
void layoutPills(juce::Rectangle<float> r, AudiblePill& mon, AudiblePill& str) {
    const auto line = r.withSizeKeepingCentre(r.getWidth(), juce::jmin(44.0f, r.getHeight()));
    const float pw = (line.getWidth() - kGap) * 0.5f;
    mon.setBounds(line.withWidth(pw).toNearestInt());
    str.setBounds(line.withTrimmedLeft(pw + kGap).toNearestInt());
}

} // namespace

// =============================================================================================
// Badge

int Badge::idealWidth() const {
    return text_.isEmpty() ? 18 : juce::roundToInt(textWidth(uiFont(11.0f, Weight::Medium), text_) + 14.0f);
}

void Badge::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat();
    if (text_.isEmpty()) {
        icons::draw(g, icon_, r.withSizeKeepingCentre(15.0f, 15.0f), p.ink);
        return;
    }
    g.setColour(p.ink.withAlpha(0.08f));
    g.fillRoundedRectangle(r.reduced(0.5f), r.getHeight() * 0.5f);
    g.setColour(p.ink2);
    g.setFont(uiFont(11.0f, Weight::Medium));
    g.drawText(text_, r, juce::Justification::centred, false);
}

// =============================================================================================
// TrackRow

TrackRow::TrackRow(HubEditor& ed) : ed_(ed) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &mon_, &str_, &hp_, &vw_, &in_, &more_, &warn_, &state_, &delay_ })
        addAndMakeVisible(c);
    delay_.setVisible(false);
    mon_.onClick = [this] { ed_.proc().send(view_.slot, ParamId::Mon, view_.mon ? 0.0f : 1.0f); };
    str_.onClick = [this] { ed_.proc().send(view_.slot, ParamId::Str, view_.str ? 0.0f : 1.0f); };
    hp_.onValueChange = [this] { sendLevel(hp_, ParamId::MonTrimDb); };
    vw_.onValueChange = [this] { sendLevel(vw_, ParamId::StrGainDb); };
    more_.onClick = [this] { showMenu(); };
    refreshTexts();
}

void TrackRow::refreshTexts() {
    more_.setTooltip(tr(Str::AdvancedSettings));
    hp_.setTooltip(tr(Str::HeadphoneTip));
    vw_.setTooltip(tr(Str::ViewersLevel));
}

void TrackRow::sendLevel(LevelSlider& s, ParamId id) {
    if (updating_) return;
    lastUserMs_[&s == &hp_ ? 0 : 1] = juce::Time::getMillisecondCounter();
    ed_.proc().send(view_.slot, id, LevelSlider::sliderToDb(s.getValue()));
    repaint(&s == &hp_ ? hpValue_.toNearestInt() : vwValue_.toNearestInt());
}

// "370 ms" next to the name while a viewers delay is set (auto sync or by hand). True when it changed.
static bool showDelay(Badge& b, float ms) {
    const auto text = ms >= 0.5f ? juce::String(juce::roundToInt(ms)) + " ms" : juce::String();
    if (text == b.getTitle()) return false;
    b.setTitle(text);
    b.setText(text);
    b.setTooltip(tr(Str::ViewersDelay));
    b.setVisible(text.isNotEmpty());
    return true;
}

void TrackRow::update(const TrackView& v, int shadeIndex) {
    const bool nameChanged = showDelay(delay_, v.delayMs) || v.name != view_.name || v.colourARGB != view_.colourARGB || shadeIndex != shade_;
    const bool textChanged = nameChanged || v.mon != view_.mon || v.str != view_.str || view_.slot < 0;
    const double hpBefore = hp_.getValue(), vwBefore = vw_.getValue();
    view_ = v;
    shade_ = shadeIndex;
    mon_.setOn(v.mon);
    str_.setOn(v.str);
    if (textChanged) {   // accessibility titles: no string building while nothing changes
        mon_.setTitle(tr(Str::MonLabel) + " " + v.name + ": " + (v.mon ? tr(Str::StateOn) : tr(Str::StateOff)));
        str_.setTitle(tr(Str::StrLabel) + " " + v.name + ": " + (v.str ? tr(Str::StateOn) : tr(Str::StateOff)));
        hp_.setTitle(tr(Str::HeadphoneLevel) + " " + v.name);
        vw_.setTitle(tr(Str::ViewersLevel) + " " + v.name);
    }
    hp_.setEnabled(v.mon);
    vw_.setEnabled(v.str);

    // Sliders show the Track's real value, except while (or just after) the user moves them.
    const auto now = juce::Time::getMillisecondCounter();
    const juce::ScopedValueSetter<bool> svs(updating_, true);
    if (!hp_.isMouseButtonDown() && now - lastUserMs_[0] > 400) hp_.setValue(LevelSlider::dbToSlider(v.trimDb), juce::dontSendNotification);
    if (!vw_.isMouseButtonDown() && now - lastUserMs_[1] > 400) vw_.setValue(LevelSlider::dbToSlider(v.gainDb), juce::dontSendNotification);
    in_.setLevel(meterPosition(v.peakIn));

    // status badges
    juce::String warnTip;
    if (v.hubStatus & ssbus::kHubStatusRateMismatch) warnTip = tr(Str::RateMismatch);
    else if (v.hubStatus & ssbus::kHubStatusAhead) warnTip = tr(Str::AheadWarning);
    else if (v.bypassed) warnTip = tr(Str::BypassedTip);
    warn_.setVisible(warnTip.isNotEmpty());
    warn_.setTooltip(warnTip);
    juce::String badge, badgeTip;
    if (!v.active) { badge = tr(Str::Inactive); badgeTip = tr(Str::InactiveTip); }
    else if (v.solo) { badge = tr(Str::SoloBadge); badgeTip = tr(Str::StreamSolo); }
    state_.setVisible(badge.isNotEmpty());
    state_.setText(badge);
    state_.setTooltip(badgeTip);
    setAlpha(v.active ? 1.0f : 0.5f);
    if (nameChanged) resized();
    if (textChanged || hp_.getValue() != hpBefore || vw_.getValue() != vwBefore)
        repaint(hpValue_.getUnion(vwValue_).getUnion(hpIcon_).getUnion(vwIcon_).toNearestInt().expanded(2));
    if (nameChanged) repaint();
}

void TrackRow::resized() {
    const auto cols = rowCols(float(getWidth()));
    const bool narrow = cols.tier == RowTier::Narrow;
    auto r = getLocalBounds().toFloat().reduced(0.0f, 10.0f).withTrimmedLeft(kPadL).withTrimmedRight(kPadR);
    hp_.setVisible(!narrow);
    vw_.setVisible(!narrow);
    mon_.setPrefix(narrow ? tr(Str::PillPrefixYou) : juce::String());
    str_.setPrefix(narrow ? tr(Str::PillPrefixViewers) : juce::String());
    if (narrow) {   // [dot name  badges meter ...] / [you hear][viewers hear]
        auto top = r.removeFromTop(30.0f);
        more_.setBounds(top.removeFromRight(kMoreW).withSizeKeepingCentre(kMoreW, kMoreW).toNearestInt());
        top.removeFromRight(8.0f);
        in_.setBounds(top.removeFromRight(48.0f).withSizeKeepingCentre(48.0f, 6.0f).toNearestInt());
        top.removeFromRight(8.0f);
        nameArea_ = top.withRight(placeBadges(top, { &state_, &delay_ }, &warn_));
        layoutPills(r, mon_, str_);
        hpIcon_ = vwIcon_ = hpValue_ = vwValue_ = {};
        return;
    }
    auto level = r.removeFromRight(cols.levelW);
    r.removeFromRight(kGap);
    str_.setBounds(r.removeFromRight(cols.pillW).withSizeKeepingCentre(cols.pillW, 44.0f).toNearestInt());
    r.removeFromRight(kGap);
    mon_.setBounds(r.removeFromRight(cols.pillW).withSizeKeepingCentre(cols.pillW, 44.0f).toNearestInt());
    r.removeFromRight(kGap);

    // name column: [dot name  badges ...] [more]
    more_.setBounds(r.removeFromRight(kMoreW).withSizeKeepingCentre(kMoreW, kMoreW).toNearestInt());
    r.removeFromRight(6.0f);
    auto top = r.removeFromTop(r.getHeight() * 0.55f);
    const float mw = juce::jmin(72.0f, r.getWidth() - 18.0f);
    in_.setBounds(r.withTrimmedLeft(18.0f).withWidth(mw).withSizeKeepingCentre(mw, 6.0f).toNearestInt());
    nameArea_ = top.withRight(placeBadges(top, { &state_, &delay_ }, &warn_));
    layoutLevels(level, hpIcon_, hpValue_, hp_, vwIcon_, vwValue_, vw_);
}

int TrackRow::heightFor(float width) {
    return juce::roundToInt(rowCols(width).tier == RowTier::Narrow ? theme::layout::rowNarrowH : theme::layout::rowH);
}

void TrackRow::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    drawInset(g, getLocalBounds().toFloat().reduced(0.5f), theme::radius::row, p);

    auto n = nameArea_;
    const auto dot = n.removeFromLeft(9.0f).withSizeKeepingCentre(9.0f, 9.0f);
    const bool useHost = SharedSettings()->hostColours() && (view_.colourARGB >> 24) != 0;
    g.setColour(useHost ? juce::Colour(view_.colourARGB).withAlpha(1.0f) : shadeFor(p, shade_));
    g.fillEllipse(dot);
    n.removeFromLeft(9.0f);
    g.setColour(p.ink);
    g.setFont(uiFont(14.0f, Weight::Medium));
    g.drawText(view_.name, n, juce::Justification::centredLeft, true);

    if (hpIcon_.isEmpty()) return;   // narrow row: the levels are in the "..." panel
    icons::draw(g, icons::Icon::Headphones, hpIcon_.withSizeKeepingCentre(14.0f, 14.0f), view_.mon ? p.graphite : p.muted);
    icons::draw(g, icons::Icon::Broadcast, vwIcon_.withSizeKeepingCentre(14.0f, 14.0f), view_.str ? p.graphite : p.muted);
    g.setFont(uiFont(12.0f));
    g.setColour(view_.mon ? p.ink : p.muted);
    g.drawText(formatDb(float(hp_.getValue())), hpValue_, juce::Justification::centredRight, false);
    g.setColour(view_.str ? p.ink : p.muted);
    g.drawText(formatDb(float(vw_.getValue())), vwValue_, juce::Justification::centredRight, false);
}

void TrackRow::mouseUp(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) showMenu();
}

void TrackRow::showMenu() {
    juce::PopupMenu m;
    m.setLookAndFeel(&ed_.lnf());
    m.addItem(1, tr(Str::StreamSolo), true, view_.solo);
    m.addItem(2, tr(Str::AdvancedSettings));
    m.addItem(3, tr(Str::RenameDisplay));
    juce::Component::SafePointer<TrackRow> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&more_), [self](int r) {
        if (self == nullptr || r == 0) return;
        auto& row = *self;
        if (r == 1) row.ed_.proc().send(row.view_.slot, ParamId::StrSolo, row.view_.solo ? 0.0f : 1.0f);
        if (r == 2) row.ed_.showTrackPanel(row.view_.slot, row.more_);
        if (r == 3) {
            const int slot = row.view_.slot;
            auto* ed = &row.ed_;
            ed->askRename(tr(Str::RenameDisplay), row.view_.name, [ed, slot](juce::String name) { ed->proc().rename(slot, name); });
        }
    });
}

// =============================================================================================
// SourceRow

SourceRow::SourceRow(HubEditor& ed) : ed_(ed) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &mon_, &str_, &hp_, &vw_, &meter_, &more_, &state_, &delay_ })
        addAndMakeVisible(c);
    delay_.setVisible(false);
    mon_.onClick = [this] { if (view_.slot >= 0) ed_.proc().send(view_.slot, ParamId::Mon, view_.mon ? 0.0f : 1.0f); };
    str_.onClick = [this] { if (view_.slot >= 0) ed_.proc().send(view_.slot, ParamId::Str, view_.str ? 0.0f : 1.0f); };
    hp_.onValueChange = [this] { sendLevel(hp_, ParamId::MonTrimDb); };
    vw_.onValueChange = [this] { sendLevel(vw_, ParamId::StrGainDb); };
    more_.onClick = [this] { showMenu(); };
    refreshTexts();
}

void SourceRow::refreshTexts() {
    more_.setTooltip(tr(Str::AdvancedSettings));
    hp_.setTooltip(tr(Str::HeadphoneTip));
    vw_.setTooltip(tr(Str::ViewersLevel));
}

void SourceRow::sendLevel(LevelSlider& s, ParamId id) {
    if (updating_ || view_.slot < 0) return;
    lastUserMs_[&s == &hp_ ? 0 : 1] = juce::Time::getMillisecondCounter();
    ed_.proc().send(view_.slot, id, LevelSlider::sliderToDb(s.getValue()));
    repaint(&s == &hp_ ? hpValue_.toNearestInt() : vwValue_.toNearestInt());
}

void SourceRow::update(const SourceView& v, int shadeIndex) {
    const bool textChanged = v.name != view_.name || v.app != view_.app || v.colourARGB != view_.colourARGB || v.flags != view_.flags
                          || v.capture != view_.capture || v.mon != view_.mon || v.str != view_.str || shadeIndex != shade_ || view_.index < 0;
    const double hpBefore = hp_.getValue(), vwBefore = vw_.getValue();
    const bool recBefore = view_.recording();
    const int secBefore = int(view_.recordSec);
    view_ = v;
    shade_ = shadeIndex;
    mon_.setOn(v.mon);
    str_.setOn(v.str);
    const bool linked = v.slot >= 0;   // an older App Audio has no headphone slot
    for (juce::Component* c : std::initializer_list<juce::Component*> { &mon_, &str_ }) c->setEnabled(linked);
    hp_.setEnabled(linked && v.mon);
    vw_.setEnabled(linked && v.str);
    if (textChanged) {
        mon_.setTitle(tr(Str::MonLabel) + " " + v.name + ": " + (v.mon ? tr(Str::StateOn) : tr(Str::StateOff)));
        str_.setTitle(tr(Str::StrLabel) + " " + v.name + ": " + (v.str ? tr(Str::StateOn) : tr(Str::StateOff)));
        hp_.setTitle(tr(Str::HeadphoneLevel) + " " + v.name);
        vw_.setTitle(tr(Str::ViewersLevel) + " " + v.name);
    }
    const auto now = juce::Time::getMillisecondCounter();
    {
        const juce::ScopedValueSetter<bool> svs(updating_, true);
        if (!hp_.isMouseButtonDown() && now - lastUserMs_[0] > 400) hp_.setValue(LevelSlider::dbToSlider(v.trimDb), juce::dontSendNotification);
        if (!vw_.isMouseButtonDown() && now - lastUserMs_[1] > 400) vw_.setValue(LevelSlider::dbToSlider(v.gainDb), juce::dontSendNotification);
    }
    meter_.setLevel(meterPosition(v.peak));

    // badges: switched off, recording (with its length), recording with the DAW, lost audio
    juce::String badge, tip;
    if (!v.active) { badge = tr(Str::Inactive); tip = tr(Str::InactiveTip); }
    else if (!v.on()) { badge = tr(Str::PowerOff); tip = tr(Str::PowerTip); }
    else if (v.recording()) { badge = "REC " + clock(v.recordSec); tip = tr(Str::Recording); }
    else if (v.flags & ssbus::kSrcFollowRec) { badge = "REC"; tip = tr(Str::FollowRecordTip); }
    else if (v.flags & ssbus::kSrcDropped) { badge = "!"; tip = tr(Str::TakeDropped); }
    const bool badgeChanged = showDelay(delay_, v.delayMs) || badge != state_.getTitle() || v.recording() != recBefore
                           || int(v.recordSec) != secBefore;
    state_.setVisible(badge.isNotEmpty());
    state_.setText(badge);
    state_.setTitle(badge);
    state_.setTooltip(tip);
    setAlpha(v.active ? 1.0f : 0.5f);
    if (textChanged || badgeChanged) { resized(); repaint(); }
    else if (hp_.getValue() != hpBefore || vw_.getValue() != vwBefore)
        repaint(hpValue_.getUnion(vwValue_).toNearestInt().expanded(2));
}

void SourceRow::resized() {
    const auto cols = rowCols(float(getWidth()));
    const bool narrow = cols.tier == RowTier::Narrow;
    auto r = getLocalBounds().toFloat().reduced(0.0f, 10.0f).withTrimmedLeft(kPadL).withTrimmedRight(kPadR);
    hp_.setVisible(!narrow);
    vw_.setVisible(!narrow);
    mon_.setPrefix(narrow ? tr(Str::PillPrefixYou) : juce::String());
    str_.setPrefix(narrow ? tr(Str::PillPrefixViewers) : juce::String());
    juce::Rectangle<float> top, bottom;
    if (narrow) {   // [dot name  badges ...] / [program v] [meter] / [you hear][viewers hear]
        top = r.removeFromTop(30.0f);
        more_.setBounds(top.removeFromRight(kMoreW).withSizeKeepingCentre(kMoreW, kMoreW).toNearestInt());
        top.removeFromRight(8.0f);
        r.removeFromTop(4.0f);
        bottom = r.removeFromTop(24.0f).withTrimmedLeft(18.0f);
        layoutPills(r, mon_, str_);
        hpIcon_ = vwIcon_ = hpValue_ = vwValue_ = {};
    } else {
        auto level = r.removeFromRight(cols.levelW);
        r.removeFromRight(kGap);
        str_.setBounds(r.removeFromRight(cols.pillW).withSizeKeepingCentre(cols.pillW, 44.0f).toNearestInt());
        r.removeFromRight(kGap);
        mon_.setBounds(r.removeFromRight(cols.pillW).withSizeKeepingCentre(cols.pillW, 44.0f).toNearestInt());
        r.removeFromRight(kGap);
        more_.setBounds(r.removeFromRight(kMoreW).withSizeKeepingCentre(kMoreW, kMoreW).toNearestInt());
        r.removeFromRight(6.0f);
        top = r.removeFromTop(r.getHeight() * 0.55f);
        bottom = r.withTrimmedLeft(18.0f);   // second line: [program v] [meter]
        layoutLevels(level, hpIcon_, hpValue_, hp_, vwIcon_, vwValue_, vw_);
    }
    nameArea_ = top.withRight(placeBadges(top, { &state_, &delay_ }, nullptr));
    const auto label = view_.app.isEmpty() ? tr(Str::AppPick) : programLabel(view_.app);
    // the program chip gets the room it needs; the meter only shows when there is space left
    const float aw = juce::jmin(bottom.getWidth(), textWidth(uiFont(12.0f, Weight::Medium), label) + 34.0f);
    appArea_ = bottom.removeFromLeft(aw).withSizeKeepingCentre(aw, 20.0f);
    bottom.removeFromLeft(8.0f);
    meter_.setVisible(bottom.getWidth() >= 30.0f);
    const float mw = juce::jmin(72.0f, bottom.getWidth());
    meter_.setBounds(bottom.withSizeKeepingCentre(bottom.getWidth(), 6.0f).withWidth(mw).toNearestInt());
}

int SourceRow::heightFor(float width) {
    // narrow rows add a line for the program chip
    return juce::roundToInt(rowCols(width).tier == RowTier::Narrow ? theme::layout::rowNarrowH + 28.0f : theme::layout::rowH);
}

void SourceRow::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    drawInset(g, getLocalBounds().toFloat().reduced(0.5f), theme::radius::row, p);

    auto n = nameArea_;
    const auto dot = n.removeFromLeft(9.0f).withSizeKeepingCentre(9.0f, 9.0f);
    const bool useHost = SharedSettings()->hostColours() && (view_.colourARGB >> 24) != 0;
    g.setColour(useHost ? juce::Colour(view_.colourARGB).withAlpha(1.0f) : shadeFor(p, shade_));
    g.fillEllipse(dot);
    n.removeFromLeft(9.0f);
    g.setColour(p.ink);
    g.setFont(uiFont(14.0f, Weight::Medium));
    g.drawText(view_.name, n, juce::Justification::centredLeft, true);

    // program chooser chip; greyed while the program isn't delivering sound
    const bool running = view_.capture == 2;   // AppCapture::State::Running
    const bool hover = isMouseOver() && appArea_.contains(getMouseXYRelative().toFloat());
    g.setColour(p.ink.withAlpha(hover ? 0.12f : 0.06f));
    g.fillRoundedRectangle(appArea_, appArea_.getHeight() * 0.5f);
    auto a = appArea_.reduced(9.0f, 0.0f);
    const auto t = a.removeFromRight(8.0f).withSizeKeepingCentre(8.0f, 5.0f);
    juce::Path tri;
    tri.addTriangle(t.getX(), t.getY(), t.getRight(), t.getY(), t.getCentreX(), t.getBottom());
    g.setColour(p.graphite);
    g.fillPath(tri);
    g.setColour(view_.app.isEmpty() || (view_.on() && !running) ? p.graphite : p.ink);
    g.setFont(uiFont(12.0f, Weight::Medium));
    g.drawText(view_.app.isEmpty() ? tr(Str::AppPick) : programLabel(view_.app), a.withTrimmedRight(4.0f), juce::Justification::centredLeft, true);

    const bool linked = view_.slot >= 0;
    if (hpIcon_.isEmpty()) return;   // narrow row: the levels are in the "..." menu
    icons::draw(g, icons::Icon::Headphones, hpIcon_.withSizeKeepingCentre(14.0f, 14.0f), linked && view_.mon ? p.graphite : p.muted);
    icons::draw(g, icons::Icon::Broadcast, vwIcon_.withSizeKeepingCentre(14.0f, 14.0f), linked && view_.str ? p.graphite : p.muted);
    g.setFont(uiFont(12.0f));
    g.setColour(linked && view_.mon ? p.ink : p.muted);
    g.drawText(formatDb(float(hp_.getValue())), hpValue_, juce::Justification::centredRight, false);
    g.setColour(linked && view_.str ? p.ink : p.muted);
    g.drawText(formatDb(float(vw_.getValue())), vwValue_, juce::Justification::centredRight, false);
}

void SourceRow::mouseMove(const juce::MouseEvent& e) {
    setMouseCursor(appArea_.contains(e.position) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint(appArea_.toNearestInt().expanded(1));
}

void SourceRow::mouseUp(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) showMenu();
    else if (appArea_.contains(e.position)) pickApp();
}

void SourceRow::pickApp() {
    const auto apps = AppCapture::listAudioApps();
    juce::PopupMenu m;
    m.setLookAndFeel(&ed_.lnf());
    m.addItem(1, tr(Str::AppSystem), true, view_.app == kSystemAudio);
    m.addSeparator();
    bool listed = false;
    for (size_t i = 0; i < apps.size(); ++i) {
        const bool cur = juce::String(apps[i].exe).equalsIgnoreCase(view_.app);
        listed = listed || cur;
        m.addItem(int(i) + 2, juce::String(apps[i].name), true, cur);
    }
    if (!listed && view_.app.isNotEmpty() && view_.app != kSystemAudio && view_.app != "*link*") m.addItem(1000, programLabel(view_.app), true, true);
    m.addSeparator();
    m.addItem(999, tr(Str::AppLinkIn), true, view_.app == "*link*");
    juce::Component::SafePointer<SourceRow> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(appArea_.toNearestInt())), [self, apps](int r) {
        if (self == nullptr || r == 0 || r == 1000) return;
        const juce::String exe = r == 1 ? juce::String(kSystemAudio) : r == 999 ? juce::String("*link*") : juce::String(apps[size_t(r - 2)].exe);
        self->ed_.proc().chooseSourceApp(self->view_.index, exe);
    });
}

void SourceRow::showMenu() {
    juce::PopupMenu m;
    m.setLookAndFeel(&ed_.lnf());
    m.addItem(1, tr(Str::PowerTip), true, view_.on());
    m.addSeparator();
    m.addItem(2, view_.recording() ? tr(Str::RecordStop) : tr(Str::RecordTip));
    m.addItem(3, tr(Str::FollowRecord), true, (view_.flags & ssbus::kSrcFollowRec) != 0);
    m.addItem(4, tr(Str::OpenFolder));
    m.addSeparator();
    m.addItem(5, tr(Str::RowLevels), view_.slot >= 0);
    juce::Component::SafePointer<SourceRow> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&more_), [self](int r) {
        if (self == nullptr || r == 0) return;
        auto& v = self->view_;
        auto& proc = self->ed_.proc();
        if (r == 1) proc.sendSource(v.index, ssbus::SourceParam::On, v.on() ? 0.0f : 1.0f);
        if (r == 2) proc.sendSource(v.index, ssbus::SourceParam::Record, v.recording() ? 0.0f : 1.0f);
        if (r == 3) proc.sendSource(v.index, ssbus::SourceParam::FollowRecord, (v.flags & ssbus::kSrcFollowRec) ? 0.0f : 1.0f);
        if (r == 4) {
            const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HEARASIDE").getChildFile("Recordings");
            dir.createDirectory();
            dir.revealToUser();
        }
        if (r == 5) self->ed_.showSourceLevels(v.index, self->more_);
    });
}

void HubEditor::ListContent::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    if (!startBox.isEmpty()) {   // getting started: three steps that tick themselves
        drawInset(g, startBox, theme::radius::row, p, true);
        auto r = startBox.reduced(18.0f, 14.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText(tr(Str::StartTitle), r.removeFromTop(24.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(6.0f);
        for (int i = 0; i < 3; ++i) {
            const auto text = stepText(i);
            const float h = juce::jmax(24.0f, wrappedHeight(uiFont(13.0f), text, r.getWidth() - 34.0f, 2.0f));
            auto line = r.removeFromTop(h);
            const auto dot = line.removeFromLeft(24.0f).withHeight(24.0f).reduced(1.0f);
            if (steps[i]) {
                g.setColour(p.ink);
                g.fillEllipse(dot);
                icons::draw(g, icons::Icon::Check, dot.reduced(5.0f), p.onInk, 2.2f);
            } else {
                g.setColour(p.ink2);
                g.drawEllipse(dot, 1.2f);
                g.setFont(uiFont(12.0f, Weight::SemiBold));
                g.drawText(juce::String(i + 1), dot, juce::Justification::centred, false);
            }
            line.removeFromLeft(10.0f);
            drawWrapped(g, text, uiFont(13.0f), steps[i] ? p.graphite : p.ink, line.withTrimmedTop(juce::jmax(0.0f, (24.0f - uiFont(13.0f).getHeight()) * 0.5f)), 2.0f);
            r.removeFromTop(8.0f);
        }
    }
    if (!sourcesHead.isEmpty()) {
        auto h = sourcesHead.withTrimmedLeft(4.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText(tr(Str::SourcesTitle), h.removeFromTop(24.0f), juce::Justification::bottomLeft, true);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::SourcesSubtitle), h.withTrimmedTop(2.0f), juce::Justification::topLeft, true);
    }
}

juce::String HubEditor::ListContent::stepText(int i) const {
    const Str texts[] = { Str::StartStep1, Str::StartStep2, Str::StartStep3 };
    auto t = tr(texts[i]);
    if (i == 0 && !steps[0] && step1Detail.isNotEmpty()) t << "\n" << step1Detail;   // how, in the DAW in use
    return t;
}

// =============================================================================================
// HubEditor

HubEditor::HubEditor(HubProcessor& p)
    : EditorShell(p, 1040, 790, int(theme::layout::hubMinW), int(theme::layout::hubMinH), "hub"), proc_(p) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &obsChip_, &dawChip_, &mute_, &settingsButton_, &previewBanner_,
                                                                        &panicBanner_, &viewport_, &meterL_, &meterR_, &masterSlider_,
                                                                        &limiter_, &preview_, &headphoneSlider_ })
        content_.addAndMakeVisible(c);
    viewport_.setViewedComponent(&list_, false);
    viewport_.setScrollBarsShown(true, false);
    previewBanner_.setVisible(false);
    panicBanner_.setVisible(false);

    mute_.onClick = [this] { toggleParam(hubparam::Panic); };
    preview_.onClick = [this] { toggleParam(hubparam::Preview); };
    limiter_.onClick = [this] { toggleParam(hubparam::LimiterOn); };
    settingsButton_.onClick = [this] { showSettings(); };
    shareButton_.onClick = [this] { showShare(); };
    levelsButton_.onClick = [this] { showLevels(); };
    setupButton_.onClick = [this] { showSetup(); };
    content_.addAndMakeVisible(setupButton_);
    content_.addChildComponent(silentBanner_);
    silentBanner_.setInterceptsMouseClicks(true, false);
    silentBanner_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    silentClick_.fn = [this] { showSetup(); };
    silentBanner_.addMouseListener(&silentClick_, false);
    content_.addAndMakeVisible(shareButton_);
    content_.addChildComponent(shareChip_);
    content_.addChildComponent(levelsButton_);
    syncButton_.onClick = [this] {
        const auto ph = proc_.autoSync().phase;
        if (ph == HubProcessor::SyncPhase::Countdown || ph == HubProcessor::SyncPhase::Reference || ph == HubProcessor::SyncPhase::Microphone)
            proc_.cancelAutoSync();
        else
            proc_.startAutoSync();   // one press: guess the mic and the music, count down, measure, set the delays
        updateSyncBanner();
    };
    syncMore_.onClick = [this] { showSync(); };
    content_.addAndMakeVisible(syncButton_);
    content_.addAndMakeVisible(syncMore_);
    content_.addChildComponent(syncBanner_);
    list_.addChildComponent(startHide_);
    startHide_.onClick = [this] {
        settings_->setFlag("startHidden", true);
        updateStart();
    };
    masterLink_ = std::make_unique<DbSliderLink>(masterSlider_, *proc_.params().getParameter(hubparam::Master));
    headphoneLink_ = std::make_unique<DbSliderLink>(headphoneSlider_, *proc_.params().getParameter(hubparam::Headphones));

    procListener_.fn = [this] { layout(); content_.repaint(); };
    proc_.stateChanged.addChangeListener(&procListener_);
    refreshTexts();
    syncTracks();
    syncSources();
    updateStart();
    setContent(content_);
    startTimerHz(30);
}

HubEditor::~HubEditor() {
    proc_.stateChanged.removeChangeListener(&procListener_);
}

bool HubEditor::paramOn(const char* id) const { return proc_.params().getRawParameterValue(id)->load() > 0.5f; }

void HubEditor::toggleParam(const char* id) { proc_.setParam(id, paramOn(id) ? 0.0f : 1.0f); timerCallback(); }

void HubEditor::lookChanged() {
    backdrop_.invalidate();
    refreshTexts();
    for (auto* r : rows_) { r->refreshTexts(); r->resized(); }
    for (auto* r : sourceRows_) { r->refreshTexts(); r->resized(); }
    layout();
    content_.repaint();
}

// The step "put HEARASIDE Track last" for the DAW in use.
static juce::String step1For(Daw daw) {
    switch (daw) {
        case Daw::StudioOne: return tr(Str::StartStep1StudioOne);
        case Daw::Cubase:    return tr(Str::StartStep1Cubase);
        case Daw::Reaper:    return tr(Str::StartStep1Reaper);
        case Daw::FlStudio:  return tr(Str::StartStep1Fl);
        case Daw::Ableton:   return tr(Str::StartStep1Ableton);
        case Daw::Other:     break;
    }
    return {};
}

void HubEditor::refreshTexts() {
    mute_.setButtonText(paramOn(hubparam::Panic) ? tr(Str::MuteOn) : tr(Str::MuteOff));
    mute_.setTitle(mute_.getButtonText());
    preview_.setButtonText(paramOn(hubparam::Preview) ? tr(Str::PreviewOn) : tr(Str::PreviewOff));
    preview_.setTitle(preview_.getButtonText());
    limiter_.setTitle(tr(Str::Limiter));
    masterSlider_.setTitle(tr(Str::Master));
    headphoneSlider_.setTitle(tr(Str::HeadphoneMaster));
    headphoneSlider_.setTooltip(tr(Str::HeadphoneMasterTip));
    settingsButton_.setTooltip(tr(Str::Settings));
    shareButton_.setTooltip(tr(Str::ShareTip));
    shareButton_.setTitle(tr(Str::ShareTip));
    levelsButton_.setTooltip(tr(Str::LevelsTip));
    setupButton_.setTooltip(tr(Str::SetupTitle));
    setupButton_.setTitle(tr(Str::SetupTitle));
    silentBanner_.set(Banner::Style::Warning, icons::Icon::SpeakerOff, tr(Str::ViewersSilentBanner));
    levelsButton_.setTitle(tr(Str::LevelsTitle));
    syncButton_.setButtonText(tr(Str::SyncButton));
    syncButton_.setTooltip(tr(Str::SyncTip));
    syncMore_.setTooltip(tr(Str::SyncOptions));
    syncMore_.setTitle(tr(Str::SyncOptions));
    syncText_ = "-";   // re-render the banner in the new language
    settingsButton_.setTitle(tr(Str::Settings));
    startHide_.setButtonText(tr(Str::StartHide));
    list_.step1Detail = step1For(currentDaw());
    previewBanner_.set(Banner::Style::Dark, icons::Icon::Headphones, tr(Str::BannerPreview));
    panicBanner_.set(Banner::Style::Outline, icons::Icon::SpeakerOff, tr(Str::BannerPanic));
}

void HubEditor::askRename(const juce::String& title, const juce::String& current, std::function<void(juce::String)> done) {
    auto* aw = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon, this);
    aw->setLookAndFeel(&lnf_);
    aw->addTextEditor("name", current, tr(Str::NewName));
    aw->addButton(tr(Str::Done), 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton(tr(Str::Cancel), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true, juce::ModalCallbackFunction::create([aw, done](int result) {
        if (result == 1) {
            const auto name = aw->getTextEditorContents("name").trim();
            if (name.isNotEmpty()) done(name);
        }
    }), true);
}

std::unique_ptr<juce::Component> HubEditor::createPanel(Panel which, int slot) {
    std::unique_ptr<juce::Component> panel;
    switch (which) {
        case Panel::Share:        panel = std::make_unique<HubSharePanel>(proc_); break;
        case Panel::Settings:     panel = std::make_unique<HubSettingsPanel>(proc_); break;
        case Panel::Sync:         panel = std::make_unique<HubSyncPanel>(proc_); break;
        case Panel::Track:        panel = std::make_unique<HubTrackPanel>(*this, slot); break;
        case Panel::Levels:       panel = std::make_unique<HubLevelsPanel>(proc_); break;
        case Panel::SourceLevels: panel = std::make_unique<HubSourceLevelsPanel>(*this, slot); break;
        case Panel::Setup:        panel = std::make_unique<HubSetupPanel>(proc_); break;
    }
    panel->setLookAndFeel(&lnf_);
    recolourTextEditors(*panel);
    return panel;
}

void HubEditor::showTrackPanel(int slot, juce::Component& anchor) { launchPanel(createPanel(Panel::Track, slot), anchor); }
void HubEditor::showSourceLevels(int index, juce::Component& anchor) { launchPanel(createPanel(Panel::SourceLevels, index), anchor); }
void HubEditor::showSync() { launchPanel(createPanel(Panel::Sync), syncMore_); }
void HubEditor::showShare() { launchPanel(createPanel(Panel::Share), shareButton_); }
void HubEditor::showSettings() { launchPanel(createPanel(Panel::Settings), settingsButton_); }
void HubEditor::showLevels() { launchPanel(createPanel(Panel::Levels), levelsButton_); }
void HubEditor::showSetup() { launchPanel(createPanel(Panel::Setup), setupButton_.isVisible() ? static_cast<juce::Component&>(setupButton_) : silentBanner_); }

void HubEditor::updateSyncBanner() {
    const auto st = proc_.autoSync();
    using P = HubProcessor::SyncPhase;
    const bool busy = st.phase == P::Countdown || st.phase == P::Reference || st.phase == P::Microphone;
    const bool recent = (st.phase == P::Done || st.phase == P::Failed) && juce::Time::getMillisecondCounter() - proc_.syncFinishedMs() < 12000;
    const auto text = busy || recent ? HubSyncPanel::syncStatusText(proc_) : juce::String();
    syncButton_.setButtonText(busy ? tr(Str::SyncCancel) : tr(Str::SyncButton));
    if (text == syncText_ && syncBanner_.isVisible() == text.isNotEmpty()) return;
    syncText_ = text;
    syncBanner_.set(busy ? Banner::Style::Dark : st.phase == P::Failed ? Banner::Style::Warning : Banner::Style::Outline,
                    busy ? icons::Icon::Headphones : st.phase == P::Failed ? icons::Icon::Warning : icons::Icon::Broadcast, text);
    syncBanner_.setVisible(text.isNotEmpty());
    layout();
    content_.repaint();
}

void HubEditor::syncTracks() {
    views_ = proc_.tracks();
    bool structure = views_.size() != size_t(rows_.size());
    for (size_t i = 0; !structure && i < views_.size(); ++i) structure = rows_[int(i)]->slot() != views_[i].slot;
    if (structure) {
        rows_.clear();
        for (size_t i = 0; i < views_.size(); ++i) list_.addAndMakeVisible(rows_.add(new TrackRow(*this)));
    }
    for (size_t i = 0; i < views_.size(); ++i) rows_[int(i)]->update(views_[i], views_[i].slot);
    if (structure) { updateStart(); layoutList(); content_.repaint(); }
}

void HubEditor::syncSources() {
    srcViews_ = proc_.sources();
    const auto& views = srcViews_;
    bool structure = views.size() != size_t(sourceRows_.size());
    for (size_t i = 0; !structure && i < views.size(); ++i) structure = sourceRows_[int(i)]->index() != views[i].index;
    if (structure) {
        sourceRows_.clear();
        for (size_t i = 0; i < views.size(); ++i) list_.addAndMakeVisible(sourceRows_.add(new SourceRow(*this)));
    }
    for (size_t i = 0; i < views.size(); ++i) sourceRows_[int(i)]->update(views[i], views[i].index + 3);
    if (structure) { updateStart(); layoutList(); }
}

// Who hears what, from the rows (solo only applies to the viewers).
HubEditor::Lists HubEditor::summaryLists() const {
    Lists l;
    bool solo = false;
    for (const auto& v : views_) solo = solo || (v.solo && v.str);
    auto add = [&](const juce::String& name, bool mon, bool viewers) {
        if (mon) l.you.add(name);
        if (viewers) l.viewers.add(name);
        if (viewers && !mon) l.onlyViewers.add(name);
    };
    for (const auto& v : views_)
        if (v.active) add(v.name, v.mon, v.str && (!solo || v.solo));
    for (const auto& s : srcViews_)   // App Audio: only while it is switched on and has a slot
        if (s.active && s.on() && s.slot >= 0) add(s.name, s.mon, s.str && !solo);
    return l;
}

// Getting started: tracks are there, OBS listens, the viewers' mix was heard once.
void HubEditor::updateStart() {
    const bool s1 = !views_.empty() || !srcViews_.empty(), s2 = proc_.obsConnected(), s3 = settings_->flag("triedPreview");
    const bool show = !s1 || (!(s2 && s3) && !settings_->flag("startHidden"));
    if (show == showStart_ && s1 == list_.steps[0] && s2 == list_.steps[1] && s3 == list_.steps[2]) return;
    showStart_ = show;
    list_.steps[0] = s1;
    list_.steps[1] = s2;
    list_.steps[2] = s3;
    layoutList();
    list_.repaint();
}

int HubEditor::startHeight(float w) const {
    float h = 14.0f + 24.0f + 6.0f;
    for (int i = 0; i < 3; ++i)
        h += juce::jmax(24.0f, wrappedHeight(uiFont(13.0f), list_.stepText(i), w - 36.0f - 34.0f, 2.0f)) + 8.0f;
    return juce::roundToInt(h - 8.0f + 14.0f);
}

void HubEditor::timerCallback() {
    const bool preview = paramOn(hubparam::Preview), panic = paramOn(hubparam::Panic), lim = paramOn(hubparam::LimiterOn);
    if (preview != lastPreview_ || panic != lastPanic_) {
        lastPreview_ = preview;
        lastPanic_ = panic;
        preview_.setActive(preview);
        mute_.setActive(panic);
        refreshTexts();
        previewBanner_.setVisible(preview);
        panicBanner_.setVisible(panic);
        if (preview) { settings_->setFlag("triedPreview", true); updateStart(); }
        layout();
        content_.repaint();
    }
    if (lim != lastLimiter_ || limiter_.isOn() != lim) { lastLimiter_ = lim; limiter_.setOn(lim, content_.isShowing()); }
    masterLink_->update();
    headphoneLink_->update();

    syncTracks();
    syncSources();
    updateSyncBanner();
    if (auto* bus = proc_.engine().bus()) {
        const auto& sh = bus->streamHeader;
        meterL_.setLevel(meterPosition(ssbus::bitsFloat(sh.peakBits[0][0].load(std::memory_order_relaxed))));
        meterR_.setLevel(meterPosition(ssbus::bitsFloat(sh.peakBits[0][1].load(std::memory_order_relaxed))));
    }

    if (++slowTick_ % 6 == 0) {   // ~5 Hz: chips, LUFS, summary text
        const bool obs = proc_.obsConnected();
        obsChip_.set(obs ? tr(Str::ObsConnected) : tr(Str::ObsNotConnected), obs ? StatusChip::Dot::Solid : StatusChip::Dot::None);
        {
            // DAW latency as plug-ins can see it: the host buffer (audio-interface latency is not exposed)
            const auto li = proc_.latency();
            dawChip_.set(li.block > 0 ? "DAW " + juce::String(li.block) + " · " + juce::String(li.dawMs, 1) + " ms"
                                      : juce::String(juce::CharPointer_UTF8("DAW \xe2\x80\x93")),
                         StatusChip::Dot::None);
        }
        {   // sharing: "Sharing · Listening 2"
            const auto& sh = proc_.share();
            const bool on = proc_.sharing() && sh.running();
            if (on) shareChip_.set(tr(Str::SharingChip) + " · " + tr(Str::Listeners) + " " + juce::String(sh.listeners()), StatusChip::Dot::Solid);
            if (on != shareChipOn_) { shareChipOn_ = on; layout(); }
        }
        // the header is laid out again only when a chip's text changes its width
        const int widths[3] = { obsChip_.idealWidth(), dawChip_.idealWidth(), shareChip_.idealWidth() };
        if (!std::equal(std::begin(widths), std::end(widths), std::begin(chipWidths_))) {
            std::copy(std::begin(widths), std::end(widths), std::begin(chipWidths_));
            layout();
        }
        const bool only = !summaryLists().onlyViewers.isEmpty();
        if (only != hadOnlyViewers_) { hadOnlyViewers_ = only; layout(); }
        updateStart();
        content_.repaint(lufsBox_.toNearestInt().expanded(2));
        content_.repaint(summaryCard_.toNearestInt());
        content_.repaint(stripCard_.toNearestInt());
        content_.repaint(masterLabel_.toNearestInt());
    }
}

// ---------------------------------------------------------------------------------------------
// layout (docs/ux-roadmap.md 5.1): Compact = one column; Regular / Wide = tracks on the left,
// the viewers' cards on the right. Short windows get the condensed right column.

void HubEditor::updateMode(float w, float h) {
    const float cb = theme::layout::compactBelow, wa = theme::layout::wideAbove, hy = theme::layout::hysteresis * 0.5f;
    if (w < cb - hy) mode_ = Mode::Compact;
    else if (w > wa + hy) mode_ = Mode::Wide;
    else if (w >= cb + hy && w <= wa - hy) mode_ = Mode::Regular;
    else if (w < cb + hy) { if (mode_ == Mode::Wide) mode_ = Mode::Regular; }   // between Compact and Regular: keep
    else if (mode_ == Mode::Compact) mode_ = Mode::Regular;                       // between Regular and Wide: keep
    // header + the full viewers card + the full summary card (latency box optional)
    const float need = 2.0f * theme::space::hubPad + 60.0f + theme::space::cardGap + kStreamCardH + theme::space::cardGap + kSummaryFullH;
    if (h < need - hy) condensed_ = true;
    else if (h > need + hy) condensed_ = false;
}

void HubEditor::layoutHeader(bool compact) {
    auto h = header_.withTrimmedLeft(compact ? 16.0f : 24.0f).withTrimmedRight(10.0f);
    wordmarkSize_ = compact || header_.getWidth() < 720.0f ? 13.0f : 17.0f;
    const float wmW = wordmarkSize_ * 10.0f;
    wordmark_ = h.removeFromLeft(wmW).withSizeKeepingCentre(wmW, wordmarkSize_ * 1.8f);
    h.removeFromLeft(8.0f);
    // what fits is decided by importance (Mute stream always stays), then placed right to left in
    // the usual order; whatever does not fit is hidden (OBS then shows in the viewers card)
    struct Item { juce::Component* c; float w, height; bool wanted; int importance; };
    const Item items[] = { { &settingsButton_, 36.0f, 36.0f, true, 1 }, { &shareButton_, 36.0f, 36.0f, true, 2 },
                           { &setupButton_, 36.0f, 36.0f, true, 3 }, { &mute_, float(mute_.idealWidth()), 40.0f, true, 0 },
                           { &obsChip_, float(obsChip_.idealWidth()), 36.0f, true, 4 },
                           { &shareChip_, float(shareChip_.idealWidth()), 36.0f, shareChipOn_, 6 }, { &dawChip_, float(dawChip_.idealWidth()), 36.0f, true, 5 } };
    bool shown[std::size(items)] = {};
    float used = 0.0f;
    for (int rank = 0; rank <= 6; ++rank)
        for (size_t i = 0; i < std::size(items); ++i)
            if (items[i].importance == rank && items[i].wanted && used + items[i].w + (used > 0.0f ? 8.0f : 0.0f) <= h.getWidth()) {
                used += items[i].w + (used > 0.0f ? 8.0f : 0.0f);
                shown[i] = true;
            }
    bool first = true;
    for (size_t i = 0; i < std::size(items); ++i) {
        const auto& it = items[i];
        it.c->setVisible(shown[i]);
        if (!shown[i]) continue;
        if (!first) h.removeFromRight(8.0f);
        it.c->setBounds(h.removeFromRight(it.w).withSizeKeepingCentre(it.w, it.height).toNearestInt());
        first = false;
    }
    obsInHeader_ = obsChip_.isVisible();
}

// The viewers' card when there is little room: title + LUFS, meters, (who hears what), preview + levels.
float HubEditor::stripHeight(float, bool withSummary) const {
    const auto lines = float(summaryLineCount());
    const float summary = withSummary ? lines * kStripLineH + (lines - 1.0f) * 4.0f + 12.0f : 0.0f;
    return 16.0f + 44.0f + 10.0f + 18.0f + 12.0f + summary + 44.0f + 16.0f;
}

int HubEditor::summaryLineCount() const { return summaryLists().onlyViewers.isEmpty() ? 2 : 3; }

void HubEditor::layoutStrip(juce::Rectangle<float> card, bool withSummary) {
    auto c = card.reduced(16.0f);
    auto top = c.removeFromTop(44.0f);
    lufsBox_ = top.removeFromRight(110.0f);
    stripTitle_ = top.withTrimmedRight(8.0f);
    c.removeFromTop(10.0f);
    auto meters = c.removeFromTop(18.0f);
    meterLabels_ = meters.removeFromLeft(18.0f);
    meterL_.setBounds(meters.removeFromTop(6.0f).toNearestInt());
    meters.removeFromTop(6.0f);
    meterR_.setBounds(meters.removeFromTop(6.0f).toNearestInt());
    c.removeFromTop(12.0f);
    stripLines_ = {};
    if (withSummary) {
        const auto lines = float(summaryLineCount());
        stripLines_ = c.removeFromTop(lines * kStripLineH + (lines - 1.0f) * 4.0f);
        c.removeFromTop(12.0f);
    }
    auto row = c.removeFromTop(44.0f);
    levelsButton_.setBounds(row.removeFromRight(44.0f).toNearestInt());
    row.removeFromRight(8.0f);
    preview_.setBounds(row.toNearestInt());
}

void HubEditor::layoutTracksCard(bool compact) {
    const float padX = compact ? 14.0f : 20.0f;
    auto c = tracksCard_.withTrimmedTop(compact ? 16.0f : 22.0f).withTrimmedBottom(compact ? 14.0f : 20.0f).reduced(padX, 0.0f);
    // title (+ subtitle) on the left, the sync buttons on the right; they get a line of their own when narrow
    const float sw = textWidth(uiFont(13.0f, Weight::Medium), syncButton_.getButtonText()) + 32.0f;
    const float titleW = textWidth(uiFont(20.0f, Weight::SemiBold), tr(Str::TracksTitle));
    const bool stacked = c.getWidth() < titleW + 24.0f + sw + 6.0f + 34.0f;
    auto head = c.removeFromTop(stacked ? 26.0f + 8.0f + 34.0f : 48.0f);
    auto syncRow = stacked ? head.removeFromBottom(34.0f) : head;
    syncMore_.setBounds(syncRow.removeFromRight(34.0f).withSizeKeepingCentre(34.0f, 34.0f).toNearestInt());
    syncRow.removeFromRight(6.0f);
    const float bw = juce::jmin(sw, syncRow.getWidth());
    syncButton_.setBounds(syncRow.withLeft(syncRow.getRight() - bw).withSizeKeepingCentre(bw, 34.0f).toNearestInt());
    tracksTitle_ = stacked ? head.removeFromTop(26.0f) : head.withRight(float(syncButton_.getX()) - 12.0f).withTrimmedLeft(4.0f);
    tracksSubtitle_ = !stacked;
    c.removeFromTop(4.0f);
    const bool bannersHere = !compact;   // compact: preview / panic sit above the viewers card
    for (auto* bn : { &silentBanner_, &syncBanner_, &previewBanner_, &panicBanner_ }) {
        if (!bn->isVisible() || (!bannersHere && bn != &syncBanner_)) continue;
        const int h = bn->idealHeight(int(c.getWidth()));
        bn->setBounds(c.removeFromTop(float(h)).toNearestInt());
        c.removeFromTop(12.0f);
    }
    const float rowW = c.getWidth() - float(viewport_.getScrollBarThickness()) - 4.0f;
    columns_ = rowCols(rowW).tier == RowTier::Narrow ? juce::Rectangle<float>() : c.removeFromTop(20.0f);
    if (!columns_.isEmpty()) c.removeFromTop(8.0f);
    viewport_.setBounds(c.toNearestInt());
    layoutList();
}

void HubEditor::layout() {
    const auto all = content_.getLocalBounds().toFloat();
    if (all.isEmpty()) return;
    updateMode(all.getWidth(), all.getHeight());
    const bool compact = mode_ == Mode::Compact;
    const float pad = compact ? theme::layout::compactPad : theme::space::hubPad;
    const float gap = compact ? theme::layout::compactPad : theme::space::cardGap;
    auto r = all.reduced(pad);

    header_ = r.removeFromTop(compact ? 52.0f : 60.0f);
    r.removeFromTop(gap);
    layoutHeader(compact);

    const bool showMain = proc_.connected() && proc_.engine().role() != ssengine::HubEngine::Role::Secondary;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &viewport_, &syncButton_, &syncMore_, &meterL_, &meterR_, &preview_ })
        c->setVisible(showMain);
    if (!showMain) {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &previewBanner_, &panicBanner_, &syncBanner_, &silentBanner_, &masterSlider_, &limiter_,
                                                                            &headphoneSlider_, &levelsButton_ })
            c->setVisible(false);
        messageCard_ = r.withSizeKeepingCentre(juce::jmin(600.0f, r.getWidth()), juce::jmin(250.0f, r.getHeight()));
        tracksCard_ = streamCard_ = summaryCard_ = stripCard_ = {};
        backdrop_.setCards({ { header_, theme::radius::header }, { messageCard_, theme::radius::card } });
        return;
    }
    previewBanner_.setVisible(lastPreview_);
    panicBanner_.setVisible(lastPanic_);
    silentBanner_.setVisible(proc_.viewersSilent());

    streamCard_ = summaryCard_ = stripCard_ = {};
    if (compact) {
        // what reaches the viewers comes first: mute / preview / silence banners, then the viewers card
        for (auto* bn : { &silentBanner_, &panicBanner_, &previewBanner_ }) {
            if (!bn->isVisible()) continue;
            bn->setBounds(r.removeFromTop(float(bn->idealHeight(int(r.getWidth())))).toNearestInt());
            r.removeFromTop(gap);
        }
        stripCard_ = r.removeFromTop(stripHeight(r.getWidth(), true));
        layoutStrip(stripCard_, true);
        r.removeFromTop(gap);
        tracksCard_ = r;
    } else {
        const float rightW = mode_ == Mode::Wide ? 340.0f : 300.0f;
        auto right = r.removeFromRight(rightW);
        r.removeFromRight(gap);
        tracksCard_ = r;
        if (condensed_) {
            stripCard_ = right.removeFromTop(stripHeight(rightW, false));
            layoutStrip(stripCard_, false);
            right.removeFromTop(gap);
            const auto lines = float(summaryLineCount());
            const float latency = proc_.latency().obs ? kStripLineH : wrappedHeight(uiFont(12.0f), tr(Str::ObsHint), rightW - 36.0f, 2.0f);
            summaryCard_ = right.withHeight(juce::jmin(right.getHeight(), 18.0f + 24.0f + 10.0f + lines * (16.0f + kStripLineH + 6.0f) + latency + 18.0f));
            summaryTitle_ = summaryCard_.reduced(18.0f).removeFromTop(24.0f);
            stripLines_ = {};
        } else {
            streamCard_ = right.removeFromTop(kStreamCardH);
            auto c = streamCard_.reduced(18.0f);
            auto top = c.removeFromTop(56.0f);
            lufsBox_ = top.removeFromRight(118.0f);
            streamTitle_ = top;
            c.removeFromTop(14.0f);
            auto meters = c.removeFromTop(18.0f);
            meterLabels_ = meters.removeFromLeft(18.0f);
            meterL_.setBounds(meters.removeFromTop(6.0f).toNearestInt());
            meters.removeFromTop(6.0f);
            meterR_.setBounds(meters.removeFromTop(6.0f).toNearestInt());
            c.removeFromTop(14.0f);
            masterLabel_ = c.removeFromTop(18.0f);
            c.removeFromTop(6.0f);
            masterSlider_.setBounds(c.removeFromTop(22.0f).toNearestInt());
            c.removeFromTop(14.0f);
            auto limRow = c.removeFromTop(36.0f);
            limiter_.setBounds(limRow.removeFromRight(50.0f).withSizeKeepingCentre(50.0f, 30.0f).toNearestInt());
            limiterText_ = limRow;
            c.removeFromTop(14.0f);
            preview_.setBounds(c.removeFromTop(48.0f).toNearestInt());
            right.removeFromTop(gap);

            summaryCard_ = right;
            auto s = summaryCard_.reduced(18.0f);
            summaryTitle_ = s.removeFromTop(24.0f);
            s.removeFromTop(12.0f);
            youBox_ = s.removeFromTop(96.0f);
            {
                auto in = youBox_.reduced(14.0f, 10.0f);
                youText_ = in.removeFromTop(38.0f);
                in.removeFromTop(4.0f);
                hpLabel_ = in.removeFromTop(16.0f);
                in.removeFromTop(2.0f);
                headphoneSlider_.setBounds(in.withSizeKeepingCentre(in.getWidth(), 20.0f).toNearestInt());
            }
            s.removeFromTop(8.0f);
            viewersBox_ = s.removeFromTop(62.0f);
            s.removeFromTop(8.0f);
            onlyBox_ = {};
            if (hadOnlyViewers_ && s.getHeight() >= 44.0f) {
                onlyBox_ = s.removeFromTop(44.0f);
                s.removeFromTop(8.0f);
            }
            latencyBox_ = s.removeFromTop(juce::jmin(s.getHeight(), 58.0f));
        }
    }
    const bool strip = !stripCard_.isEmpty();
    masterSlider_.setVisible(!strip);
    limiter_.setVisible(!strip);
    headphoneSlider_.setVisible(!strip);
    levelsButton_.setVisible(strip);
    layoutTracksCard(compact);

    std::vector<GlassCard> cards { { header_, theme::radius::header }, { tracksCard_, theme::radius::card } };
    for (auto* c : { &streamCard_, &stripCard_, &summaryCard_ })
        if (!c->isEmpty()) cards.push_back({ *c, theme::radius::card });
    backdrop_.setCards(std::move(cards));
}

void HubEditor::layoutList() {
    const int gapY = int(theme::space::rowGap);
    auto total = [&](float w) {
        int t = showStart_ ? startHeight(w) + gapY : 0;
        t += rows_.size() * (TrackRow::heightFor(w) + gapY);
        if (!sourceRows_.isEmpty()) t += gapY + kSourcesHeadH + sourceRows_.size() * (SourceRow::heightFor(w) + gapY);
        return t - gapY;
    };
    float w = float(viewport_.getWidth());
    if (total(w) > viewport_.getHeight()) w -= float(viewport_.getScrollBarThickness() + 4);
    const int wi = juce::jmax(1, int(w));
    int y = 0;
    list_.startBox = {};
    list_.sourcesHead = {};
    startHide_.setVisible(false);
    if (showStart_) {
        const int h = startHeight(w);
        list_.startBox = juce::Rectangle<float>(0.0f, 0.0f, w, float(h));
        if (list_.steps[0]) {   // can be hidden once there are tracks
            const int bw = juce::roundToInt(textWidth(uiFont(13.0f, Weight::Medium), startHide_.getButtonText()) + 28.0f);
            startHide_.setBounds(wi - bw - 12, 12, bw, int(theme::layout::minTarget));
            startHide_.setVisible(true);
        }
        y = h + gapY;
    }
    for (auto* row : rows_) {
        const int h = TrackRow::heightFor(w);
        row->setBounds(0, y, wi, h);
        y += h + gapY;
    }
    if (!sourceRows_.isEmpty()) {
        y += gapY;
        list_.sourcesHead = juce::Rectangle<float>(0.0f, float(y), w, float(kSourcesHeadH - gapY));
        y += kSourcesHeadH;
        for (auto* row : sourceRows_) {
            const int h = SourceRow::heightFor(w);
            row->setBounds(0, y, wi, h);
            y += h + gapY;
        }
    }
    list_.setSize(wi, juce::jmax(1, y - gapY));
    list_.repaint();
}

// "In your headphones: a, b" lines (condensed / compact viewers card and summary)
void HubEditor::paintSummaryLines(juce::Graphics& g, juce::Rectangle<float> area, bool stacked) {
    const auto& p = lnf_.pal();
    const auto l = summaryLists();
    struct Line { Str label; juce::String text; };
    std::vector<Line> lines {
        { Str::MonLabel, lastPreview_ ? tr(Str::SummaryPreviewing) : (l.you.isEmpty() ? tr(Str::None) : l.you.joinIntoString(", ")) },
        { Str::StrLabel, lastPanic_ ? tr(Str::SummaryPanic) : (l.viewers.isEmpty() ? tr(Str::None) : l.viewers.joinIntoString(", ")) } };
    if (!l.onlyViewers.isEmpty()) lines.push_back({ Str::SummaryOnlyViewers, l.onlyViewers.joinIntoString(", ") });
    const auto lf = uiFont(12.0f), vf = uiFont(13.0f, Weight::Medium);
    float labelW = 0.0f;
    for (const auto& ln : lines) labelW = juce::jmax(labelW, textWidth(lf, tr(ln.label)));
    labelW = juce::jmin(labelW, area.getWidth() * 0.42f);
    for (const auto& ln : lines) {
        auto row = area.removeFromTop(stacked ? 16.0f + kStripLineH : kStripLineH);
        area.removeFromTop(stacked ? 6.0f : 4.0f);
        g.setColour(p.graphite);
        g.setFont(lf);
        const auto label = stacked ? row.removeFromTop(16.0f) : row.removeFromLeft(labelW);
        if (!stacked) row.removeFromLeft(10.0f);
        g.drawFittedText(tr(ln.label), label.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour(p.ink);
        g.setFont(vf);
        g.drawFittedText(ln.text, row.toNearestInt(), juce::Justification::centredLeft, 1, 0.9f);
    }
    if (stacked && area.getHeight() >= kStripLineH) {   // + how late the stream is
        const auto li = proc_.latency();
        g.setColour(p.graphite);
        g.setFont(lf);
        if (li.obs) {
            auto row = area.removeFromTop(kStripLineH);
            g.drawFittedText(tr(Str::LatencyToObs), row.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            g.setColour(p.ink);
            g.setFont(uiFont(12.0f, Weight::SemiBold));
            g.drawText(juce::String(li.total(), 1) + " ms", row, juce::Justification::centredRight, false);
        } else {
            drawWrapped(g, tr(Str::ObsHint), lf, p.graphite, area, 2.0f);
        }
    }
}

void HubEditor::paintContent(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    backdrop_.paint(g, content_.getLocalBounds(), p, settings_->glassAlpha(), lnf_.isDark());

    g.setColour(p.ink);
    drawWordmark(g, wordmark_, "HUB", wordmarkSize_, p.ink);

    const bool secondary = proc_.engine().role() == ssengine::HubEngine::Role::Secondary;
    if (!proc_.connected() || secondary) {
        auto c = messageCard_.reduced(juce::jmin(28.0f, messageCard_.getWidth() * 0.06f));
        g.setColour(p.ink);
        g.setFont(uiFont(20.0f, Weight::SemiBold));
        const juce::String title = secondary ? tr(Str::SecondHubTitle) : tr(Str::HubBusError).upToFirstOccurrenceOf(" ", false, false);
        const juce::String body = secondary ? tr(Str::SecondHubBody) : tr(Str::HubBusError);
        g.drawFittedText(title, c.removeFromTop(30.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        c.removeFromTop(10.0f);
        drawTextBlock(g, body, uiFont(14.0f), p.graphite, c, 4.0f);
        return;
    }

    // ---- tracks card ------------------------------------------------------------------------
    {
        auto t = tracksTitle_;
        g.setColour(p.ink);
        g.setFont(uiFont(20.0f, Weight::SemiBold));
        g.drawText(tr(Str::TracksTitle), t.removeFromTop(26.0f), juce::Justification::centredLeft, true);
        if (tracksSubtitle_) {
            g.setColour(p.graphite);
            g.setFont(uiFont(13.0f));
            g.drawText(tr(Str::TracksSubtitle), t.withTrimmedTop(4.0f), juce::Justification::topLeft, true);
        }

        // column header (aligned with the rows; narrow rows label their own pills)
        if (!columns_.isEmpty()) {
            const auto cols = rowCols(float(list_.getWidth()));
            auto c = columns_.withWidth(float(list_.getWidth())).withTrimmedLeft(kPadL).withTrimmedRight(kPadR);
            auto level = c.removeFromRight(cols.levelW);
            c.removeFromRight(kGap);
            auto strCol = c.removeFromRight(cols.pillW);
            c.removeFromRight(kGap);
            auto monCol = c.removeFromRight(cols.pillW);
            g.setFont(uiFont(12.0f));
            g.setColour(p.graphite);
            g.drawText(tr(Str::ColTrack), c, juce::Justification::centredLeft, true);
            auto head = [&](juce::Rectangle<float> col, icons::Icon icon, const juce::String& text) {
                const float tw = juce::jmin(textWidth(uiFont(12.0f), text), col.getWidth() - 20.0f);
                const float x = col.getCentreX() - (14.0f + 6.0f + tw) * 0.5f;
                icons::draw(g, icon, { x, col.getCentreY() - 7.0f, 14.0f, 14.0f }, p.graphite);
                g.drawFittedText(text, juce::Rectangle<float>(x + 20.0f, col.getY(), tw + 2.0f, col.getHeight()).toNearestInt(),
                                 juce::Justification::centredLeft, 1, 0.85f);
            };
            head(monCol, icons::Icon::Headphones, tr(Str::MonLabel));
            head(strCol, icons::Icon::Broadcast, tr(Str::StrLabel));
            g.drawFittedText(cols.tier == RowTier::Full ? tr(Str::HeadphoneLevel) + " / " + tr(Str::ViewersLevel) : tr(Str::LevelsColumn),
                             level.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
        }
    }

    auto paintLufs = [&](juce::Rectangle<float> box) {
        drawInset(g, box, theme::radius::small, p);
        auto lb = box.reduced(12.0f, 8.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(11.0f));
        g.drawFittedText(tr(Str::Loudness), lb.removeFromTop(14.0f).toNearestInt(), juce::Justification::centredRight, 1, 0.8f);
        juce::String lufs;
        if (lastPanic_) lufs = tr(Str::LoudMuted);
        else if (auto* bus = proc_.engine().bus()) {
            const float v = ssbus::bitsFloat(bus->streamHeader.loudnessSBits.load(std::memory_order_relaxed));
            lufs = v < -70.0f ? tr(Str::LoudSilent) : minusText(juce::String(v, 1));
        }
        g.setColour(p.ink);
        g.setFont(uiFont(22.0f, Weight::SemiBold));
        g.drawFittedText(lufs, lb.toNearestInt(), juce::Justification::centredRight, 1, 0.8f);
    };
    auto paintMeterLabels = [&] {
        auto ml = meterLabels_;
        g.setColour(p.graphite);
        g.setFont(uiFont(10.0f));
        g.drawText("L", ml.removeFromTop(6.0f).expanded(0, 3), juce::Justification::centredLeft, false);
        ml.removeFromTop(6.0f);
        g.drawText("R", ml.expanded(0, 3), juce::Justification::centredLeft, false);
    };

    // ---- viewers card (full) -----------------------------------------------------------------
    if (!streamCard_.isEmpty()) {
        auto t = streamTitle_;
        g.setColour(p.ink);
        g.setFont(uiFont(17.0f, Weight::SemiBold));
        g.drawFittedText(tr(Str::StreamTitle), t.removeFromTop(24.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(obsInHeader_ ? tr(Str::StreamSubtitle) : obsLine(), t.withTrimmedTop(4.0f), juce::Justification::topLeft, true);
        paintLufs(lufsBox_);
        paintMeterLabels();

        g.setColour(p.ink);
        g.setFont(uiFont(13.0f));
        g.drawText(tr(Str::Master), masterLabel_, juce::Justification::centredLeft, true);
        g.setFont(uiFont(12.0f));
        g.drawText(formatDb(float(masterSlider_.getValue())), masterLabel_, juce::Justification::centredRight, false);

        auto lt = limiterText_;
        g.setFont(uiFont(13.0f));
        g.drawText(tr(Str::Limiter), lt.removeFromTop(18.0f), juce::Justification::centredLeft, true);
        g.setColour(p.graphite);
        g.setFont(uiFont(11.0f));
        g.drawText(tr(Str::LimiterCaption), lt, juce::Justification::centredLeft, true);
    }

    // ---- viewers card (strip: compact / short windows) ------------------------------------------
    if (!stripCard_.isEmpty()) {
        auto t = stripTitle_;
        g.setColour(p.ink);
        g.setFont(uiFont(17.0f, Weight::SemiBold));
        g.drawFittedText(tr(Str::StreamTitle), t.removeFromTop(24.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawFittedText(obsInHeader_ ? tr(Str::StreamSubtitle) : obsLine(), t.withTrimmedTop(2.0f).toNearestInt(), juce::Justification::topLeft, 1, 0.85f);
        paintLufs(lufsBox_);
        paintMeterLabels();
        if (!stripLines_.isEmpty()) paintSummaryLines(g, stripLines_, false);
    }

    // ---- summary card ------------------------------------------------------------------------
    if (!summaryCard_.isEmpty()) {
        g.setColour(p.ink);
        g.setFont(uiFont(17.0f, Weight::SemiBold));
        g.drawText(tr(Str::SummaryTitle), summaryTitle_, juce::Justification::centredLeft, true);
    }
    if (!summaryCard_.isEmpty() && condensed_) {
        paintSummaryLines(g, summaryCard_.reduced(18.0f).withTrimmedTop(24.0f + 10.0f), true);
    } else if (!summaryCard_.isEmpty()) {
        const auto lists = summaryLists();
        const auto youText = lastPreview_ ? tr(Str::SummaryPreviewing) : (lists.you.isEmpty() ? tr(Str::None) : lists.you.joinIntoString(", "));
        const auto viewersText = lastPanic_ ? tr(Str::SummaryPanic) : (lists.viewers.isEmpty() ? tr(Str::None) : lists.viewers.joinIntoString(", "));

        drawInset(g, youBox_, theme::radius::small, p);
        auto yt = youText_;
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::SummaryYou), yt.removeFromTop(16.0f), juce::Justification::centredLeft, true);
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawFittedText(youText, yt.toNearestInt(), juce::Justification::centredLeft, 1, 0.9f);
        g.setColour(p.graphite);
        g.setFont(uiFont(11.0f));
        g.drawText(tr(Str::HeadphoneMaster), hpLabel_, juce::Justification::centredLeft, true);
        g.setColour(p.ink);
        g.setFont(uiFont(12.0f));
        g.drawText(formatDb(float(headphoneSlider_.getValue())), hpLabel_, juce::Justification::centredRight, false);

        drawInset(g, viewersBox_, theme::radius::small, p);
        auto vt = viewersBox_.reduced(14.0f, 10.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::SummaryViewers), vt.removeFromTop(16.0f), juce::Justification::centredLeft, true);
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawFittedText(viewersText, vt.toNearestInt(), juce::Justification::centredLeft, 2, 0.9f);

        if (!onlyBox_.isEmpty()) {   // the usual surprise: viewers get a track you don't hear
            drawInset(g, onlyBox_, theme::radius::small, p, true);
            auto ot = onlyBox_.reduced(14.0f, 6.0f);
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f));
            g.drawText(tr(Str::SummaryOnlyViewers), ot.removeFromTop(16.0f), juce::Justification::centredLeft, true);
            g.setColour(p.ink);
            g.setFont(uiFont(13.0f, Weight::Medium));
            g.drawFittedText(lists.onlyViewers.joinIntoString(", "), ot.toNearestInt(), juce::Justification::centredLeft, 1, 0.9f);
        }

        if (latencyBox_.getHeight() > 30.0f) {
            drawInset(g, latencyBox_, theme::radius::small, p);
            auto lt = latencyBox_.reduced(14.0f, 8.0f);
            const auto li = proc_.latency();
            auto ms = [](double v) { return juce::String(v, 1); };
            auto line1 = lt.removeFromTop(lt.getHeight() * 0.5f);
            if (li.obs) {
                g.setColour(p.graphite);
                g.setFont(uiFont(12.0f));
                g.drawText(tr(Str::LatencyToObs), line1, juce::Justification::centredLeft, true);
                g.setColour(p.ink);
                g.setFont(uiFont(13.0f, Weight::SemiBold));
                g.drawText(ms(li.total()) + " ms", line1, juce::Justification::centredRight, false);
                juce::String parts = "DAW " + ms(li.dawMs);
                if (li.trackFxMs >= 0.5) parts << " + " << tr(Str::TrackFx) << " " << ms(li.trackFxMs);
                if (li.masterFxMs >= 0.5) parts << " + " << tr(Str::MasterFx) << " " << ms(li.masterFxMs);
                parts << " + Hub " << ms(li.hubMs);
                parts << " + OBS " << ms(li.obsMs);
                g.setColour(p.graphite);
                g.setFont(uiFont(11.0f));
                g.drawFittedText(parts, lt.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            } else {
                g.setColour(p.graphite);
                g.setFont(uiFont(12.0f));
                g.drawText(tr(Str::DawBuffer), line1, juce::Justification::centredLeft, true);
                g.setColour(p.ink);
                g.setFont(uiFont(12.0f, Weight::SemiBold));
                g.drawText(juce::String(li.block) + " " + tr(Str::Samples) + " = " + ms(li.dawMs) + " ms", line1,
                           juce::Justification::centredRight, false);
                g.setColour(p.graphite);
                g.setFont(uiFont(11.0f));
                g.drawFittedText(tr(Str::ObsHint), lt.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
            }
        }
    }
}

juce::String HubEditor::obsLine() const { return proc_.obsConnected() ? tr(Str::ObsConnected) : tr(Str::ObsNotConnected); }

} // namespace hearaside
