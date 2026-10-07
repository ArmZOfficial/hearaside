#include "TrackEditor.h"
#include "ui/SettingsPanel.h"

namespace hearaside {

using ssengine::TrackPublisher;

namespace {

juce::Colour shadeFor(const theme::Palette& p, int index) {
    const juce::Colour shades[] = { p.trackShade1, p.trackShade2, p.trackShade3, p.trackShade4, p.trackShade5 };
    return shades[juce::jmax(0, index) % 5];
}

juce::String msText(double ms) { return juce::String(juce::roundToInt(ms)) + " ms"; }

// ---- "ตั้งค่าละเอียด": per-track options + shared UI preferences --------------------------------
class TrackFinePanel : public juce::Component {
public:
    explicit TrackFinePanel(TrackProcessor& p) : proc_(p) {
        name_.setText(proc_.displayName(), false);
        name_.setTextToShowWhenEmpty(tr(Str::NewName), paletteOf(*this).muted);
        name_.onReturnKey = name_.onFocusLost = [this] { proc_.setDisplayNameOverride(name_.getText()); };
        bus_.setText(proc_.busName(), false);
        bus_.onReturnKey = bus_.onFocusLost = [this] { proc_.setBusName(bus_.getText()); };
        for (auto* e : { &name_, &bus_ }) { e->setFont(uiFont(13.0f)); e->setIndents(10, 6); }

        pan_.setRange(-100.0, 100.0, 1.0);
        pan_.setDoubleClickReturnValue(true, 0.0);
        panAttachment_ = std::make_unique<juce::SliderParameterAttachment>(*proc_.params().getParameter(trackparam::StrPan), pan_);
        pan_.onValueChange = [this] { panRow_.setCaption(proc_.params().getParameter(trackparam::StrPan)->getCurrentValueAsText()); };
        pan_.onValueChange();

        stem_.addItem(tr(Str::StemNone), 1);
        auto* bus = proc_.publisher().bus();
        for (int i = 0; i < ssbus::kMaxStems; ++i) {
            juce::String n;
            if (bus) n = juce::String::fromUTF8(bus->streamHeader.names[1 + i], ssbus::kNameBytes).upToFirstOccurrenceOf(juce::String::charToString(0), false, false);
            stem_.addItem(n.isNotEmpty() ? n : "Stem " + juce::String(i + 1), i + 2);
        }
        stem_.setSelectedId(proc_.stemIndex() + 2, juce::dontSendNotification);
        stem_.onChange = [this] { proc_.setStemIndex(stem_.getSelectedId() - 2); };

        solo_.setOn(proc_.params().getRawParameterValue(trackparam::StrSolo)->load() > 0.5f, false);
        solo_.onClick = [this] {
            auto* p = proc_.params().getParameter(trackparam::StrSolo);
            const bool on = p->getValue() < 0.5f;
            p->beginChangeGesture(); p->setValueNotifyingHost(on ? 1.0f : 0.0f); p->endChangeGesture();
            solo_.setOn(on, true);
        };

        for (auto* r : { &nameRow_, &panRow_, &stemRow_, &soloRow_, &busRow_ }) addAndMakeVisible(r);
        addAndMakeVisible(ui_);
        setSize(340, 5 * 40 + 24 + ui_.idealHeight() + 16);
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        g.setColour(p.hairline2);
        g.fillRect(juce::Rectangle<int>(0, 5 * 40 + 11, getWidth(), 1));
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        for (auto* row : { &nameRow_, &panRow_, &stemRow_, &soloRow_, &busRow_ }) row->setBounds(r.removeFromTop(40));
        r.removeFromTop(24);
        ui_.setBounds(r);
    }

private:
    TrackProcessor& proc_;
    juce::TextEditor name_, bus_;
    juce::Slider pan_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::ComboBox stem_;
    Switch solo_;
    std::unique_ptr<juce::SliderParameterAttachment> panAttachment_;
    FormRow nameRow_ { Str::RenameDisplay, name_ }, panRow_ { Str::Pan, pan_ }, stemRow_ { Str::Stem, stem_ },
            soloRow_ { Str::StreamSolo, solo_, 50 }, busRow_ { Str::BusName, bus_ };
    UiSettingsPanel ui_;
};

} // namespace

TrackEditor::TrackEditor(TrackProcessor& p) : EditorShell(p, kWidth, kHeight), proc_(p) {
    backdrop_.setBlobs({ { { -60.0f, -110.0f, 380.0f, 240.0f }, true },
                         { { 180.0f, float(kHeight) - 240.0f, 300.0f, 300.0f }, false } });

    for (juce::Component* c : std::initializer_list<juce::Component*> { &chip_, &fineButton_, &banner_, &inputMeter_, &monRow_, &strRow_,
                                                                        &headphoneSlider_, &viewersSlider_, &delaySlider_, &delayValue_ })
        content_.addAndMakeVisible(c);
    banner_.setVisible(false);

    monRow_.onClick = [this] { toggle(trackparam::Mon); };
    strRow_.onClick = [this] { toggle(trackparam::Str); };
    fineButton_.onClick = [this] { showFineSettings(); };

    headphoneLink_ = std::make_unique<DbSliderLink>(headphoneSlider_, *proc_.params().getParameter(trackparam::MonTrim));
    viewersLink_ = std::make_unique<DbSliderLink>(viewersSlider_, *proc_.params().getParameter(trackparam::StrGain));

    delaySlider_.setRange(0.0, 500.0, 1.0);
    delaySlider_.setSkewFactorFromMidPoint(100.0);   // fine control in the usual 0-100 ms region
    delaySlider_.setDoubleClickReturnValue(true, 0.0);
    delaySlider_.setVelocityModeParameters(0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    delaySlider_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    delayAttachment_ = std::make_unique<juce::SliderParameterAttachment>(*proc_.params().getParameter(trackparam::StrDelay), delaySlider_);
    delayValue_.setEditable(true, true, false);
    delayValue_.setJustificationType(juce::Justification::centredRight);
    delayValue_.setFont(uiFont(12.0f));
    delayValue_.onTextChange = [this] {
        const double ms = juce::jlimit(0.0, 500.0, delayValue_.getText().retainCharacters("0123456789.").getDoubleValue());
        delaySlider_.setValue(ms, juce::sendNotificationSync);
        delayValue_.setText(msText(delaySlider_.getValue()), juce::dontSendNotification);
    };

    procListener_.fn = [this] { refreshStatus(); content_.repaint(); };
    proc_.stateChanged.addChangeListener(&procListener_);
    refreshTexts();
    refreshStatus();
    setContent(content_);
    startTimerHz(30);
}

TrackEditor::~TrackEditor() {
    proc_.stateChanged.removeChangeListener(&procListener_);
}

bool TrackEditor::paramOn(const char* id) const {
    return proc_.params().getRawParameterValue(id)->load() > 0.5f;
}

void TrackEditor::toggle(const char* id) {
    auto* p = proc_.params().getParameter(id);
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->getValue() > 0.5f ? 0.0f : 1.0f);
    p->endChangeGesture();
    timerCallback();
}

void TrackEditor::lookChanged() {
    backdrop_.invalidate();
    refreshTexts();
    refreshStatus();
    layout();
    content_.repaint();
}

void TrackEditor::refreshTexts() {
    monRow_.setTexts(tr(Str::MonRowTitle), tr(Str::MonRowCaption));
    strRow_.setTexts(tr(Str::StrRowTitle), tr(Str::StrRowCaption));
    fineButton_.setTooltip(tr(Str::AdvancedSettings));
    fineButton_.setTitle(tr(Str::AdvancedSettings));
    headphoneSlider_.setTitle(tr(Str::HeadphoneLevel));
    headphoneSlider_.setTooltip(tr(Str::HeadphoneTip));
    viewersSlider_.setTitle(tr(Str::ViewersLevel));
    delaySlider_.setTitle(tr(Str::ViewersDelay));
    delaySlider_.setTooltip(tr(Str::DelayTip));
    delayValue_.setTooltip(tr(Str::DelayTip));
}

void TrackEditor::refreshStatus() {
    const auto st = proc_.publisher().status();
    auto* bus = proc_.publisher().bus();
    auto* slot = proc_.publisher().slot();
    const bool hub = proc_.publisher().hubPresent();
    const uint32_t hubFlags = bus ? bus->header.hubFlags.load(std::memory_order_relaxed) : 0u;
    const uint32_t hubStatus = slot ? slot->hubStatus.load(std::memory_order_relaxed) : 0u;

    if (hub && (hubFlags & ssbus::kHubPanic)) chip_.set(tr(Str::PanicActiveChip), StatusChip::Dot::Ring);
    else if (hub && (hubFlags & ssbus::kHubBypassed)) chip_.set(tr(Str::HubBypassedChip), StatusChip::Dot::Ring);
    else if (hub) chip_.set(tr(Str::HubConnected), StatusChip::Dot::Solid);
    else chip_.set(tr(Str::HubMissingChip), StatusChip::Dot::None);

    bool show = true;
    if (st == TrackPublisher::Status::SlotsFull) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::SlotsFull));
    else if (st == TrackPublisher::Status::ShmError) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::BusError));
    else if (proc_.isBypassedNow()) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::BypassWarning));
    else if (hub && (hubStatus & ssbus::kHubStatusRateMismatch)) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::RateMismatchTrack));
    else if (!hub) banner_.set(Banner::Style::Warning, icons::Icon::Broadcast, tr(Str::HubMissingBanner));
    else show = false;
    if (show != banner_.isVisible()) {
        banner_.setVisible(show);
        layout();
        content_.repaint();
    }
}

void TrackEditor::timerCallback() {
    const bool mon = paramOn(trackparam::Mon), str = paramOn(trackparam::Str);
    if (mon != lastMon_ || str != lastStr_) {
        const bool animate = content_.isShowing();
        monRow_.setOn(mon, animate);
        strRow_.setOn(str, animate);
        headphoneSlider_.setEnabled(mon);
        viewersSlider_.setEnabled(str);
        delaySlider_.setEnabled(str);
        lastMon_ = mon;
        lastStr_ = str;
        content_.repaint();
    }
    headphoneLink_->update();
    viewersLink_->update();
    if (!delayValue_.isBeingEdited()) delayValue_.setText(msText(delaySlider_.getValue()), juce::dontSendNotification);
    delayValue_.setColour(juce::Label::textColourId, str ? lnf_.pal().ink : lnf_.pal().muted);
    inputMeter_.setLevel(meterPosition(juce::jmax(proc_.inputPeak(0), proc_.inputPeak(1))));

    const auto name = proc_.displayName();
    if (name != lastName_) { lastName_ = name; content_.repaint(nameRow_.toNearestInt().expanded(4)); }

    if (++slowTick_ % 10 == 0) refreshStatus();
    content_.repaint(headLabel_.getUnion(viewLabel_).getUnion(delayLabel_).toNearestInt().expanded(2));
}

void TrackEditor::showFineSettings() {
    auto panel = std::make_unique<TrackFinePanel>(proc_);
    panel->setLookAndFeel(&lnf_);
    auto& box = juce::CallOutBox::launchAsynchronously(std::move(panel), fineButton_.getScreenBounds(), nullptr);
    box.setLookAndFeel(&lnf_);
}

void TrackEditor::layout() {
    const auto b = content_.getLocalBounds().toFloat();
    card_ = b.reduced(16.0f);
    backdrop_.setCards({ { card_, theme::radius::card } });
    auto r = card_.reduced(20.0f);

    auto header = r.removeFromTop(30.0f);
    wordmark_ = header.removeFromLeft(130.0f);
    fineButton_.setBounds(header.removeFromRight(30.0f).toNearestInt());
    header.removeFromRight(8.0f);
    const float chipW = float(juce::jmin(chip_.idealWidth(), int(header.getWidth())));
    chip_.setBounds(header.removeFromRight(chipW).toNearestInt());
    r.removeFromTop(16.0f);

    if (banner_.isVisible()) {
        const int h = banner_.idealHeight(int(r.getWidth()));
        banner_.setBounds(r.removeFromTop(float(h)).toNearestInt());
        r.removeFromTop(14.0f);
    }

    nameRow_ = r.removeFromTop(28.0f);
    r.removeFromTop(8.0f);
    auto meterRow = r.removeFromTop(14.0f);
    const float lw = textWidth(uiFont(11.0f), tr(Str::InputSignal)) + 10.0f;
    inputLabel_ = meterRow.removeFromLeft(lw);
    inputMeter_.setBounds(meterRow.toNearestInt());
    r.removeFromTop(16.0f);

    monRow_.setBounds(r.removeFromTop(64.0f).toNearestInt());
    r.removeFromTop(10.0f);
    strRow_.setBounds(r.removeFromTop(64.0f).toNearestInt());
    r.removeFromTop(16.0f);

    // bottom group (anchored to the bottom of the card)
    auto bottom = r.removeFromBottom(46.0f * 3.0f + 28.0f);
    auto place = [&](juce::Rectangle<float>& label, juce::Component& slider) {
        auto blk = bottom.removeFromTop(46.0f);
        label = blk.removeFromTop(18.0f);
        blk.removeFromTop(6.0f);
        slider.setBounds(blk.toNearestInt());
        bottom.removeFromTop(14.0f);
    };
    place(headLabel_, headphoneSlider_);
    place(viewLabel_, viewersSlider_);
    place(delayLabel_, delaySlider_);
    delayValue_.setBounds(delayLabel_.removeFromRight(70.0f).toNearestInt().translated(4, 0));

    summary_ = r.withHeight(juce::jmin(r.getHeight(), 84.0f));
}

void TrackEditor::paintContent(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    backdrop_.paint(g, content_.getLocalBounds(), p, settings_->glassAlpha(), lnf_.isDark());

    g.setColour(p.ink);
    drawWordmark(g, wordmark_.withTrimmedTop(4.0f), "TRACK", 14.0f, p.ink);

    // name + colour dot
    auto nr = nameRow_;
    auto dot = nr.removeFromLeft(10.0f).withSizeKeepingCentre(10.0f, 10.0f);
    const auto hostCol = proc_.trackColour();
    g.setColour(settings_->hostColours() && !hostCol.isTransparent() ? hostCol.withAlpha(1.0f) : shadeFor(p, proc_.publisher().slotIndex()));
    g.fillEllipse(dot);
    nr.removeFromLeft(10.0f);
    g.setColour(p.ink);
    g.setFont(uiFont(22.0f, Weight::SemiBold));
    g.drawText(proc_.displayName(), nr, juce::Justification::centredLeft, true);

    g.setColour(p.graphite);
    g.setFont(uiFont(11.0f));
    g.drawText(tr(Str::InputSignal), inputLabel_, juce::Justification::centredLeft, false);

    // summary sentence
    const bool mon = lastMon_, str = lastStr_;
    const Str key = mon && str ? Str::SumBoth : (!mon && str ? Str::SumViewersOnly : (mon ? Str::SumYouOnly : Str::SumSilent));
    const auto sf = uiFont(13.0f);
    const float th = wrappedHeight(sf, tr(key), summary_.getWidth() - 28.0f);
    auto box = summary_.withHeight(th + 24.0f);
    if (!mon && !str) {
        g.setColour(p.paper.getBrightness() < 0.5f ? p.paper.brighter(0.25f) : juce::Colours::white);
        g.fillRoundedRectangle(box, theme::radius::small);
    } else {
        g.setColour(p.inset);
        g.fillRoundedRectangle(box, theme::radius::small);
    }
    drawWrapped(g, tr(key), sf, p.ink, box.reduced(14.0f, 12.0f));

    // slider labels + values
    auto label = [&](juce::Rectangle<float> r, Str text, const juce::String& value, bool enabled, bool drawValue = true) {
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f));
        g.drawText(tr(text), r, juce::Justification::centredLeft, true);
        if (drawValue) {
            g.setColour(enabled ? p.ink : p.muted);
            g.setFont(uiFont(12.0f));
            g.drawText(value, r, juce::Justification::centredRight, false);
        }
    };
    label(headLabel_, Str::HeadphoneLevel, formatDb(float(headphoneSlider_.getValue())), mon);
    label(viewLabel_, Str::ViewersLevel, formatDb(float(viewersSlider_.getValue())), str);
    label(delayLabel_, Str::ViewersDelay, {}, str, false);
}

} // namespace hearaside
