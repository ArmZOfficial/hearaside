#include "AppAudioEditor.h"
#include "Host.h"
#include "Links.h"

namespace hearaside {

namespace {

constexpr float kPad = 12.0f, kGap = 10.0f, kHeaderH = 54.0f;

icons::Icon iconFor(const juce::String& exe) {
    if (exe == AppAudioProcessor::kSystemAudio) return icons::Icon::Monitor;
    if (FriendDirectory::isFriendApp(exe)) return icons::Icon::Person;
    if (exe == AppAudioProcessor::kLinkIn || links::parse(exe).kind != links::Kind::None) return icons::Icon::Link;
    return icons::Icon::Window;
}

juce::String dawName(Daw d) {
    switch (d) {
        case Daw::StudioOne: return "Studio One";
        case Daw::Cubase:    return "Cubase";
        case Daw::Reaper:    return "Reaper";
        case Daw::FlStudio:  return "FL Studio";
        case Daw::Ableton:   return "Ableton Live";
        case Daw::Other:     break;
    }
    return tr(Str::DawYours);
}

juce::StringArray stepsFor(Daw d) {
    switch (d) {
        case Daw::StudioOne: return { tr(Str::StudioOneStep1), tr(Str::StudioOneStep2), tr(Str::StudioOneStep3), tr(Str::StudioOneStep4) };
        case Daw::Cubase:    return { tr(Str::CubaseStep1), tr(Str::StudioOneStep3), tr(Str::CubaseStep2) };
        case Daw::Reaper:    return { tr(Str::ReaperStep1), tr(Str::StudioOneStep3), tr(Str::ReaperStep2) };
        default: break;
    }
    return { tr(Str::OtherDawStep1), tr(Str::StudioOneStep3), tr(Str::OtherDawStep2) };
}

// "Receive someone's link…" in a popover: paste, Connect, a message under the field when it's wrong
class ReceivePanel : public juce::Component {
public:
    explicit ReceivePanel(std::function<bool(const juce::String&)> connect) : connect_(std::move(connect)) {
        field_.setPlaceholder(juce::String(juce::CharPointer_UTF8("https://\xe2\x80\xa6/l/\xe2\x80\xa6")));
        field_.setTitle(tr(Str::LinkToReceive));
        const auto clip = juce::SystemClipboard::getTextFromClipboard().trim();
        if (links::isListen(clip)) field_.setText(clip, false);
        field_.onReturnKey = [this] { go(); };
        button_.onClick = [this] { go(); };
        addAndMakeVisible(field_);
        addAndMakeVisible(button_);
        setSize(320, 150);
    }
    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        g.setColour(p.ink);
        g.setFont(uiFont(14.0f, Weight::SemiBold));
        g.drawText(tr(Str::ReceiveTitle), juce::Rectangle<float>(0, 0, float(getWidth()), 20), juce::Justification::centredLeft, true);
        drawWrapped(g, tr(Str::ReceiveCap), uiFont(12.0f), p.graphite, { 0, 24, float(getWidth()), 40 }, 3.0f);
        if (error_.isNotEmpty()) {
            g.setColour(p.danger);
            g.setFont(uiFont(12.0f));
            g.drawText(error_, juce::Rectangle<float>(0, float(getHeight() - 18), float(getWidth()), 18), juce::Justification::centredLeft, true);
        }
    }
    void resized() override {
        auto r = getLocalBounds().withTrimmedTop(70).removeFromTop(38);
        button_.setBounds(r.removeFromRight(button_.idealWidth()));
        r.removeFromRight(8);
        field_.setBounds(r);
    }
    void visibilityChanged() override { if (isShowing()) field_.grabKeyboardFocus(); }
private:
    void go() {
        if (connect_(field_.getText().trim())) return;
        error_ = tr(Str::AppLinkBad);
        repaint();
    }
    std::function<bool(const juce::String&)> connect_;
    TextField field_;
    GhostButton button_ { tr(Str::Connect), GhostButton::Style::Solid };
    juce::String error_;
};

} // namespace

AppAudioEditor::AppAudioEditor(AppAudioProcessor& p)
    : EditorShell(p, kWidth, kHeight, int(theme::layout::appMinW), int(theme::layout::appMinH), "app", 800, 1400), proc_(p) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &more_, &power_, &source_, &status_, &meter_, &scroll_ })
        content_.addAndMakeVisible(c);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &levelField_, &delayField_, &only_, &steps_, &otherDaws_ })
        body_.addAndMakeVisible(c);
    scroll_.setContent(body_);
    power_.setPill(true);
    power_.onClick = [this] { proc_.setOn(!proc_.isOn()); timerCallback(); };
    more_.onClick = [this] { showMore(); };
    source_.buildMenu = [this] { return sourceMenu(); };
    only_.onClick = [this] {
        auto* prm = proc_.params().getParameter(appparam::Only);
        setParamWithGesture(*prm, prm->getValue() > 0.5f ? 0.0f : 1.0f);
        timerCallback();
    };
    otherDaws_.onClick = [this] { allDaws_ = !allDaws_; otherDaws_.setOpen(allDaws_); layout(); body_.repaint(); };

    auto& pr = proc_.params();
    levelLink_ = std::make_unique<DbSliderLink>(level_, *pr.getParameter(appparam::Level));
    levelValueLink_ = std::make_unique<ParamValueLink>(levelValue_, *pr.getParameter(appparam::Level));
    delay_.setRange(0.0, 200.0, 1.0);
    delay_.setDoubleClickReturnValue(true, 0.0);
    delay_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    delay_.setWantsKeyboardFocus(true);
    setFocusDrawnBySelf(delay_);
    delayLink_ = std::make_unique<DbSliderLink>(delay_, *pr.getParameter(appparam::Delay));
    delayValueLink_ = std::make_unique<ParamValueLink>(delayValue_, *pr.getParameter(appparam::Delay));

    procListener_.fn = [this] { timerCallback(); content_.repaint(); };
    proc_.stateChanged.addChangeListener(&procListener_);
    refreshTexts();
    setContent(content_);
    timerCallback();
    startTimerHz(15);
}

AppAudioEditor::~AppAudioEditor() { proc_.stateChanged.removeChangeListener(&procListener_); }

void AppAudioEditor::refreshTexts() {
    more_.setTooltip(tr(Str::More));
    more_.setTitle(tr(Str::More));
    power_.setTitle(tr(Str::PowerTip));
    power_.setTooltip(tr(Str::PowerTip));
    power_.switchButton().setTitle(tr(Str::ProgramCapture));
    levelField_.setTexts(tr(Str::AppLevel), {});
    delayField_.setTexts(tr(Str::AppDelay), tr(Str::AppDelayCap));
    level_.setTitle(tr(Str::AppLevel));
    delay_.setTitle(tr(Str::DelayInMs));
    delay_.setTooltip(tr(Str::DoubleClickToResetMs));
    only_.setTitle(tr(Str::ProgramOnly));
    otherDaws_.setButtonText(tr(Str::OtherDaws));
    otherDaws_.setTitle(tr(Str::OtherDaws));
    steps_.setSteps(stepsFor(currentDaw()));
    steps_.setTitle(trf(Str::SetUpIn, { dawName(currentDaw()) }));
}

void AppAudioEditor::lookChanged() {
    backdrop_.invalidate();
    refreshTexts();
    layout();
    content_.repaint();
    body_.repaint();
}

Menu AppAudioEditor::sourceMenu() {
    Menu m(270);
    m.header(tr(Str::AppPick));
    const auto cur = proc_.app();
    auto list = AppCapture::listAudioApps();   // fresh every time the list opens
    bool found = false;
    for (const auto& a : list) {
        const juce::String exe(a.exe);
        found = found || exe.equalsIgnoreCase(cur);
        m.check(AppAudioProcessor::appLabel(exe), exe.equalsIgnoreCase(cur), [this, exe] { proc_.setApp(exe); });
        m.last().icon = icons::Icon::Window;
    }
    // the chosen program isn't playing right now: keep it in the list
    if (cur.isNotEmpty() && !found && cur != AppAudioProcessor::kSystemAudio && cur != AppAudioProcessor::kLinkIn && !links::isListen(cur)) {
        m.check(AppAudioProcessor::appLabel(cur), true, [] {});
        m.last().icon = icons::Icon::Window;
    }
    m.check(tr(Str::AppSystem), cur == AppAudioProcessor::kSystemAudio, [this] { proc_.setApp(AppAudioProcessor::kSystemAudio); });
    m.last().icon = icons::Icon::Monitor;
    // friends of the Hub's room (prompt 3.10 item 1: they replace "Sent in via send link")
    {
        const auto friends = FriendDirectory::all();
        bool curIsFriend = false;
        for (const auto& f : friends) {
            const auto app = FriendDirectory::appFor(f.id);
            curIsFriend = curIsFriend || app == cur;
            m.check(trf(Str::FriendItem, { f.name }), app == cur, [this, app] { proc_.setApp(app); });
            m.last().icon = icons::Icon::Person;
            m.last().dot = f.live() ? Dot::Ok : f.state == ssbus::kFriendWaiting ? Dot::Warn : Dot::Muted;
        }
        if (friends.empty() && !FriendDirectory::isFriendApp(cur)) {   // no Hub or an empty room: say how
            m.item(tr(Str::SourceNeedsHub), [] {});
            m.last().enabled = false;
        }
        if (cur == AppAudioProcessor::kLinkIn) {   // an old project still listening to the single send-in link
            m.check(tr(Str::SentInLegacy), true, [] {});
            m.last().icon = icons::Icon::Link;
        }
        if (FriendDirectory::isFriendApp(cur) && !curIsFriend) {
            m.check(AppAudioProcessor::appLabel(cur), true, [] {});
            m.last().icon = icons::Icon::Person;
        }
    }
    if (links::isListen(cur)) {
        m.check(AppAudioProcessor::appLabel(cur), true, [] {});
        m.last().icon = icons::Icon::Link;
    }
    m.separator();
    m.item(tr(Str::AppLinkAsk), [this] { showReceiveLink(); });
    return m;
}

void AppAudioEditor::showReceiveLink() {
    auto panel = std::make_unique<ReceivePanel>([this](const juce::String& url) {
        if (!links::isListen(url)) return false;
        proc_.setApp(url);
        overlay().close();
        toast(tr(Str::AppLinkReceiving));
        return true;
    });
    overlay().showPopover(std::move(panel), source_, true);
}

void AppAudioEditor::showMore() {
    Menu m(220);
    m.item(tr(Str::HowToSetUp), [this] {
        if (!allDaws_) { allDaws_ = false; }
        scroll_.scrollToShow(juce::Rectangle<float>(recordTitle_.getUnion(stepsBox_)).toNearestInt());
    });
    overlay().showMenu(std::move(m), more_);
}

AppAudioEditor::Status AppAudioEditor::status() const {
    if (!proc_.isOn()) return { tr(Str::AppOffTrack), Dot::Muted };
    const auto in = proc_.input();
    const bool link = in != AppAudioProcessor::Input::Program;
    if (in == AppAudioProcessor::Input::Friend) {
        uint32_t fid = 0;
        proc_.friendInput(&fid);
        FriendDirectory::Info f;
        const bool known = FriendDirectory::find(fid, f);
        const auto name = known ? f.name : tr(Str::FriendWord);
        switch (proc_.captureState()) {
            case AppCapture::State::Running: {
                const int ms = juce::roundToInt(proc_.takeShiftMs());
                return { ms > 0 ? trf(Str::FriendTakeShift, { name, juce::String(ms) }) : trf(Str::ReceivingFriend, { name }), Dot::Ok };
            }
            case AppCapture::State::Failed:  return { trf(Str::FriendGone, { name }), Dot::Warn };
            case AppCapture::State::Idle:    return { tr(Str::AppNone), Dot::Muted };
            default:                         return { trf(Str::FriendWaiting, { name }), Dot::Warn };
        }
    }
    switch (proc_.captureState()) {
        case AppCapture::State::Idle:       return { tr(Str::AppNone), Dot::Muted };
        case AppCapture::State::Starting:   return { tr(Str::AppStarting), Dot::Muted };
        case AppCapture::State::Running:
            return { in == AppAudioProcessor::Input::LinkIn ? tr(Str::SenderOn) : link ? tr(Str::AppLinkReceiving) : tr(Str::AppRunning), Dot::Ok };
        case AppCapture::State::NotRunning:
            return { in == AppAudioProcessor::Input::LinkIn ? tr(Str::AppLinkWaiting) : link ? tr(Str::AppLinkOffline) : tr(Str::AppNotRunning), Dot::Warn };
        case AppCapture::State::Failed:     return { link ? tr(Str::AppLinkOffline) : tr(Str::AppFailed), link ? Dot::Warn : Dot::Rec };
    }
    return { {}, Dot::None };
}

void AppAudioEditor::timerCallback() {
    meter_.setLevel(proc_.isOn() ? meterPosition(proc_.peak()) : 0.0f);
    power_.setOn(proc_.isOn(), content_.isShowing());
    only_.setOn(proc_.params().getParameter(appparam::Only)->getValue() > 0.5f, content_.isShowing());
    levelLink_->update();
    delayLink_->update();
    levelValueLink_->update();
    delayValueLink_->update();
    const auto app = proc_.app();
    source_.set(iconFor(app), app.isNotEmpty() ? AppAudioProcessor::appLabel(app) : tr(Str::AppPick));
    const auto st = status();
    status_.setText(st.text);
    status_.setDot(st.dot);
    const bool on = proc_.isOn();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &source_, &levelField_, &delayField_ }) {
        c->setAlpha(on ? 1.0f : 0.4f);
        c->setEnabled(on);
    }
    const auto key = std::make_tuple(on, int(proc_.captureState()), juce::roundToInt(proc_.latencyMs()), app);
    if (key != lastKey_) { lastKey_ = key; content_.repaint(); }
}

void AppAudioEditor::layout() {
    const auto b = content_.getLocalBounds().toFloat();
    auto r = b.reduced(kPad);
    header_ = r.removeFromTop(kHeaderH);
    r.removeFromTop(kGap);
    card_ = r;
    backdrop_.setBlobs({ { { -60.0f, -110.0f, 420.0f, 260.0f }, true },
                         { { b.getWidth() - 220.0f, b.getHeight() - 240.0f, 320.0f, 300.0f }, false } });
    backdrop_.setCards({ { header_, 18.0f }, { card_, 20.0f } });

    auto h = header_.withTrimmedLeft(16.0f).withTrimmedRight(9.0f);
    const float pw = float(power_.idealWidth());
    power_.setBounds(h.removeFromRight(pw).withSizeKeepingCentre(pw, 36.0f).toNearestInt());
    h.removeFromRight(8.0f);
    more_.setBounds(h.removeFromRight(36.0f).withSizeKeepingCentre(36.0f, 36.0f).toNearestInt());
    wordmark_ = h.withSizeKeepingCentre(h.getWidth(), 26.0f);

    auto c = card_.reduced(18.0f, 16.0f);
    title_ = c.removeFromTop(19.0f + 3.0f + 17.0f);
    c.removeFromTop(12.0f);
    source_.setBounds(c.removeFromTop(48.0f).toNearestInt());
    c.removeFromTop(8.0f);
    auto st = c.removeFromTop(20.0f);
    meter_.setBounds(st.removeFromRight(90.0f).withSizeKeepingCentre(90.0f, 8.0f).toNearestInt());
    st.removeFromRight(8.0f);
    status_.setBounds(st.toNearestInt());
    c.removeFromTop(12.0f);
    scroll_.setBounds(c.withTrimmedRight(-14.0f).toNearestInt());
    scroll_.setFadeColour(lnf_.pal().paper.overlaidWith(lnf_.pal().glass));
    const int w = scroll_.contentWidth();
    body_.setSize(w, layoutBody(w));
}

int AppAudioEditor::layoutBody(int width) {
    const float w = float(width);
    float y = 4.0f;
    auto place = [&](juce::Component& c, int hh) { c.setBounds(0, juce::roundToInt(y), width, hh); y += float(hh); };
    place(levelField_, levelField_.idealHeight(width));
    y += 14.0f;
    place(delayField_, delayField_.idealHeight(width));
    y += 14.0f;
    divider_ = { 0.0f, y, w, 1.0f };
    y += 15.0f;
    recordTitle_ = { 0.0f, y, w, 20.0f };
    y += 32.0f;
    // "Program only" box
    const float capH = wrappedHeight(uiFont(12.0f), tr(Str::ProgramOnlyCap), w - 24.0f - 56.0f, 6.0f);
    onlyBox_ = { 0.0f, y, w, juce::jmax(50.0f, 12.0f + 18.0f + 2.0f + capH + 12.0f) };
    only_.setBounds(juce::Rectangle<float>(onlyBox_.getRight() - 12.0f - 44.0f, onlyBox_.getCentreY() - 13.0f, 44.0f, 26.0f).toNearestInt());
    y = onlyBox_.getBottom() + 12.0f;
    // steps for this DAW
    const int stepsH = steps_.idealHeight(width - 28);
    stepsBox_ = { 0.0f, y, w, 14.0f + 20.0f + 10.0f + float(stepsH) + 14.0f };
    stepsTitle_ = { 14.0f, y + 14.0f, w - 28.0f, 20.0f };
    steps_.setBounds(14, juce::roundToInt(y + 44.0f), width - 28, stepsH);
    y = stepsBox_.getBottom() + 12.0f;
    place(otherDaws_, 38);
    y += 10.0f;
    others_ = {};
    if (allDaws_) {
        float oh = 0.0f;
        for (auto d : { Daw::StudioOne, Daw::Cubase, Daw::Reaper }) if (d != currentDaw()) oh += 22.0f;
        others_ = { 0.0f, y, w, oh };
        y += oh + 8.0f;
    }
    const float nh = wrappedHeight(uiFont(12.0f), tr(Str::AppNormalTrackNote), w, 6.0f);
    note_ = { 0.0f, y, w, nh };
    y += nh + 6.0f;
    return juce::roundToInt(y);
}

void AppAudioEditor::paintContent(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    backdrop_.paint(g, content_.getLocalBounds(), p, settings_->glassAlpha(), lnf_.isDark());
    drawWordmark(g, wordmark_, "APP AUDIO", 14.0f, p.ink);
    auto t = title_;
    g.setColour(p.ink);
    g.setFont(uiFont(16.0f, Weight::SemiBold));
    g.drawText(tr(Str::SourcesTitle), t.removeFromTop(19.0f), juce::Justification::centredLeft, true);
    t.removeFromTop(3.0f);
    g.setColour(p.graphite);
    g.setFont(uiFont(12.5f));
    g.drawText(tr(Str::AppSubtitle), t, juce::Justification::centredLeft, true);
}

void AppAudioEditor::paintBody(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    g.setColour(p.hairline2);
    g.fillRect(divider_);
    g.setColour(p.ink);
    g.setFont(uiFont(14.0f, Weight::SemiBold));
    g.drawText(tr(Str::AppRecordTitle), recordTitle_, juce::Justification::centredLeft, true);

    drawInset(g, onlyBox_, 12.0f, p);
    {
        auto r = onlyBox_.reduced(12.0f).withTrimmedRight(56.0f);
        const float capH = wrappedHeight(uiFont(12.0f), tr(Str::ProgramOnlyCap), r.getWidth(), 6.0f);
        auto block = r.withSizeKeepingCentre(r.getWidth(), 18.0f + 2.0f + capH);
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawText(tr(Str::ProgramOnly), block.removeFromTop(18.0f), juce::Justification::centredLeft, true);
        block.removeFromTop(2.0f);
        drawWrapped(g, tr(Str::ProgramOnlyCap), uiFont(12.0f), p.graphite, block, 6.0f);
    }

    g.setColour(p.hairline2);
    g.drawRoundedRectangle(stepsBox_.reduced(0.5f), 12.0f, 1.0f);
    const auto daw = currentDaw();
    {
        auto tr2 = stepsTitle_;
        if (daw != Daw::Other) {
            g.setColour(p.graphite);
            g.setFont(uiFont(11.0f));
            g.drawText(tr(Str::Detected), tr2.removeFromRight(textWidth(uiFont(11.0f), tr(Str::Detected)) + 2.0f), juce::Justification::centredRight, false);
        }
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::SemiBold));
        g.drawText(trf(Str::SetUpIn, { dawName(daw) }), tr2, juce::Justification::centredLeft, true);
    }

    if (!others_.isEmpty()) {
        float y = others_.getY();
        auto line = [&](const juce::String& name, Str rest) {
            g.setColour(p.ink);
            g.setFont(uiFont(12.5f, Weight::SemiBold));
            const float nw = textWidth(uiFont(12.5f, Weight::SemiBold), name);
            g.drawText(name, juce::Rectangle<float>(0.0f, y, nw + 2.0f, 20.0f), juce::Justification::centredLeft, false);
            g.setColour(p.ink2);
            g.setFont(uiFont(12.5f));
            g.drawText(" " + tr(rest), juce::Rectangle<float>(nw + 2.0f, y, others_.getWidth() - nw, 20.0f), juce::Justification::centredLeft, true);
            y += 22.0f;
        };
        if (daw != Daw::StudioOne) line("Studio One", Str::StudioOneOther);
        if (daw != Daw::Cubase) line("Cubase", Str::CubaseOther);
        if (daw != Daw::Reaper) line("Reaper", Str::ReaperOther);
    }
    drawWrapped(g, tr(Str::AppNormalTrackNote), uiFont(12.0f), p.graphite, note_, 6.0f);
}

juce::AudioProcessorEditor* AppAudioProcessor::createEditor() { return new AppAudioEditor(*this); }

} // namespace hearaside
