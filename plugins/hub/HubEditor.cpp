#include "HubEditor.h"
#include "MasteringPanel.h"
#include "ui/SettingsPanel.h"

namespace hearaside {

using ssbus::ParamId;

namespace {

constexpr float kRowH = 68.0f, kPillW = 118.0f, kLevelW = 190.0f, kGap = 12.0f;

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
        for (auto* r : { &nameRow_, &panRow_, &delayRow_, &stemRow_, &soloRow_ }) addAndMakeVisible(r);
        setSize(360, 5 * 42 + 16);
        timerCallback();
        startTimerHz(15);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        for (auto* row : { &nameRow_, &panRow_, &delayRow_, &stemRow_, &soloRow_ }) row->setBounds(r.removeFromTop(42));
    }

private:
    static juce::uint32 now() { return juce::Time::getMillisecondCounter(); }

    void timerCallback() override {
        const auto views = ed_.proc().tracks();
        const auto it = std::find_if(views.begin(), views.end(), [this](const TrackView& v) { return v.slot == slot_; });
        if (it == views.end()) return;
        const juce::ScopedValueSetter<bool> svs(updating_, true);
        if (!name_.hasKeyboardFocus(true) && it->name != lastName_) { lastName_ = it->name; name_.setText(it->name, false); }
        const bool idle = now() - touched_ > 400 && !juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
        if (idle) {
            pan_.setValue(it->pan * 100.0, juce::dontSendNotification);
            delay_.setValue(it->delayMs, juce::dontSendNotification);
            stem_.setSelectedId(it->stem + 2, juce::dontSendNotification);
        }
        solo_.setOn(it->solo, true);
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
    FormRow nameRow_ { Str::RenameDisplay, name_, 170 }, panRow_ { Str::Pan, pan_, 170 }, delayRow_ { Str::ViewersDelay, delay_, 170 },
            stemRow_ { Str::Stem, stem_, 170 }, soloRow_ { Str::StreamSolo, solo_, 50 };
    bool updating_ = false;
    juce::uint32 touched_ = 0;
};

// ---- Hub settings (gear): bus, sync, ceiling, stem names + UI preferences --------------------
class HubSettingsPanel : public juce::Component {
public:
    explicit HubSettingsPanel(HubProcessor& p) : proc_(p) {
        bus_.setText(proc_.busName(), false);
        bus_.onReturnKey = bus_.onFocusLost = [this] { proc_.setBusName(bus_.getText()); };
        sync_.addItemList({ "0", "1 " + tr(Str::Blocks), "2 " + tr(Str::Blocks) }, 1);
        syncAttachment_ = std::make_unique<juce::ComboBoxParameterAttachment>(*proc_.params().getParameter(hubparam::SyncSafety), sync_);
        syncRow_.setCaption(tr(Str::SyncSafetyHint));
        ceiling_.setRange(-12.0, 0.0, 0.1);
        ceilingAttachment_ = std::make_unique<juce::SliderParameterAttachment>(*proc_.params().getParameter(hubparam::Ceiling), ceiling_);
        ceiling_.onValueChange = [this] { ceilingRow_.setCaption(minusText(juce::String(ceiling_.getValue(), 1)) + " dBFS"); };
        ceiling_.onValueChange();
        for (auto* e : { &bus_ }) { e->setFont(uiFont(13.0f)); e->setIndents(10, 6); }
        for (auto* r : { &busRow_, &syncRow_, &ceilingRow_ }) inner_.addAndMakeVisible(r);
        for (int i = 0; i < ssbus::kMaxStems; ++i) {
            auto* e = stems_.add(new juce::TextEditor());
            e->setFont(uiFont(13.0f));
            e->setIndents(10, 6);
            e->setText(proc_.stemName(i), false);
            e->setTextToShowWhenEmpty("Stem " + juce::String(i + 1), paletteOf(*this).muted);
            e->onReturnKey = e->onFocusLost = [this, i, e] { proc_.setStemName(i, e->getText()); };
            inner_.addAndMakeVisible(e);
        }
        inner_.addAndMakeVisible(ui_);
        const int h = 3 * 48 + 30 + 4 * 40 + 20 + ui_.idealHeight() + 16;
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
        for (auto* row : { &busRow_, &syncRow_, &ceilingRow_ }) row->setBounds(r.removeFromTop(48));
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
    FormRow busRow_ { Str::BusName, bus_, 170 }, syncRow_ { Str::SyncSafety, sync_, 170 }, ceilingRow_ { Str::Ceiling, ceiling_, 170 };
    UiSettingsPanel ui_;
};

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
    for (juce::Component* c : std::initializer_list<juce::Component*> { &mon_, &str_, &hp_, &vw_, &in_, &more_, &warn_, &state_ })
        addAndMakeVisible(c);
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

void TrackRow::update(const TrackView& v, int shadeIndex) {
    const bool nameChanged = v.name != view_.name || v.colourARGB != view_.colourARGB || shadeIndex != shade_;
    view_ = v;
    shade_ = shadeIndex;
    mon_.setOn(v.mon);
    str_.setOn(v.str);
    mon_.setTitle(tr(Str::MonLabel) + " " + v.name + ": " + (v.mon ? tr(Str::StateOn) : tr(Str::StateOff)));
    str_.setTitle(tr(Str::StrLabel) + " " + v.name + ": " + (v.str ? tr(Str::StateOn) : tr(Str::StateOff)));
    hp_.setTitle(tr(Str::HeadphoneLevel) + " " + v.name);
    vw_.setTitle(tr(Str::ViewersLevel) + " " + v.name);
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
    repaint(hpValue_.getUnion(vwValue_).toNearestInt().expanded(2));
    if (nameChanged) repaint();
}

void TrackRow::resized() {
    auto r = getLocalBounds().toFloat().reduced(0.0f, 10.0f).withTrimmedLeft(16.0f).withTrimmedRight(12.0f);
    auto level = r.removeFromRight(kLevelW);
    r.removeFromRight(kGap);
    str_.setBounds(r.removeFromRight(kPillW).withSizeKeepingCentre(kPillW, 44.0f).toNearestInt());
    r.removeFromRight(kGap);
    mon_.setBounds(r.removeFromRight(kPillW).withSizeKeepingCentre(kPillW, 44.0f).toNearestInt());
    r.removeFromRight(kGap);

    // name column: [dot name  badges ...] [more]
    more_.setBounds(r.removeFromRight(26.0f).withSizeKeepingCentre(26.0f, 26.0f).toNearestInt());
    r.removeFromRight(6.0f);
    auto top = r.removeFromTop(r.getHeight() * 0.55f);
    in_.setBounds(r.withTrimmedLeft(18.0f).withWidth(72.0f).withSizeKeepingCentre(72.0f, 6.0f).toNearestInt());
    float right = top.getRight();
    if (state_.isVisible()) {
        const float w = float(state_.idealWidth());
        state_.setBounds(juce::Rectangle<float>(right - w, top.getCentreY() - 9.0f, w, 18.0f).toNearestInt());
        right -= w + 6.0f;
    }
    if (warn_.isVisible()) {
        warn_.setBounds(juce::Rectangle<float>(right - 18.0f, top.getCentreY() - 9.0f, 18.0f, 18.0f).toNearestInt());
        right -= 24.0f;
    }
    nameArea_ = top.withRight(right);

    // level column: two compact lines (headphones, viewers)
    auto line = [&](juce::Rectangle<float> l, juce::Rectangle<float>& icon, juce::Rectangle<float>& value, juce::Slider& s) {
        icon = l.removeFromLeft(14.0f);
        l.removeFromLeft(6.0f);
        value = l.removeFromRight(50.0f);
        l.removeFromRight(4.0f);
        s.setBounds(l.withSizeKeepingCentre(l.getWidth(), 20.0f).toNearestInt());
    };
    auto l1 = level.removeFromTop(level.getHeight() * 0.5f);
    line(l1, hpIcon_, hpValue_, hp_);
    line(level, vwIcon_, vwValue_, vw_);
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
// HubEditor

HubEditor::HubEditor(HubProcessor& p) : EditorShell(p, 1040, 790, 880, 750), proc_(p) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &scenes_, &obsChip_, &mute_, &settingsButton_, &previewBanner_,
                                                                        &panicBanner_, &viewport_, &meterL_, &meterR_, &masterSlider_,
                                                                        &limiter_, &preview_, &headphoneSlider_, &masteringButton_ })
        content_.addAndMakeVisible(c);
    viewport_.setViewedComponent(&list_, false);
    viewport_.setScrollBarsShown(true, false);
    previewBanner_.setVisible(false);
    panicBanner_.setVisible(false);

    scenes_.onPick = [this](int pos) { if (pos >= 0 && pos < int(sceneIndex_.size())) proc_.recallScene(sceneIndex_[size_t(pos)]); };
    scenes_.onContextMenu = [this](int pos) { if (pos >= 0 && pos < int(sceneIndex_.size())) sceneMenu(sceneIndex_[size_t(pos)]); };
    mute_.onClick = [this] { toggleParam(hubparam::Panic); };
    preview_.onClick = [this] { toggleParam(hubparam::Preview); };
    limiter_.onClick = [this] { toggleParam(hubparam::LimiterOn); };
    settingsButton_.onClick = [this] { showSettings(); };
    masteringButton_.onClick = [this] {
        auto panel = std::make_unique<MasteringPanel>(proc_.mastering());
        panel->setLookAndFeel(&lnf_);
        auto& box = juce::CallOutBox::launchAsynchronously(std::move(panel), masteringButton_.getScreenBounds(), nullptr);
        box.setLookAndFeel(&lnf_);
    };
    masteringListener_.fn = [this] { content_.repaint(masteringText_.toNearestInt().expanded(2)); content_.repaint(summaryCard_.toNearestInt()); };
    proc_.mastering().changed.addChangeListener(&masteringListener_);
    masterLink_ = std::make_unique<DbSliderLink>(masterSlider_, *proc_.params().getParameter(hubparam::Master));
    headphoneLink_ = std::make_unique<DbSliderLink>(headphoneSlider_, *proc_.params().getParameter(hubparam::Headphones));

    procListener_.fn = [this] { refreshScenes(); content_.repaint(); };
    proc_.stateChanged.addChangeListener(&procListener_);
    refreshTexts();
    refreshScenes();
    syncTracks();
    setContent(content_);
    startTimerHz(30);
}

HubEditor::~HubEditor() {
    proc_.stateChanged.removeChangeListener(&procListener_);
    proc_.mastering().changed.removeChangeListener(&masteringListener_);
}

bool HubEditor::paramOn(const char* id) const { return proc_.params().getRawParameterValue(id)->load() > 0.5f; }

void HubEditor::toggleParam(const char* id) { proc_.setParam(id, paramOn(id) ? 0.0f : 1.0f); timerCallback(); }

void HubEditor::lookChanged() {
    backdrop_.invalidate();
    refreshTexts();
    refreshScenes();
    for (auto* r : rows_) r->refreshTexts();
    layout();
    content_.repaint();
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
    settingsButton_.setTitle(tr(Str::Settings));
    masteringButton_.setButtonText(tr(Str::Manage));
    masteringButton_.setTitle(tr(Str::Mastering));
    masteringButton_.setTooltip(tr(Str::MasteringHint));
    previewBanner_.set(Banner::Style::Dark, icons::Icon::Headphones, tr(Str::BannerPreview));
    panicBanner_.set(Banner::Style::Outline, icons::Icon::SpeakerOff, tr(Str::BannerPanic));
}

void HubEditor::refreshScenes() {
    juce::StringArray names;
    sceneIndex_.clear();
    for (int i = 0; i < kNumScenes; ++i) {
        if (i < 3 || proc_.scene(i).saved() || proc_.scene(i).name.isNotEmpty()) {
            names.add(proc_.sceneName(i));
            sceneIndex_.push_back(i);
        }
    }
    scenes_.setScenes(names);
    int pos = -1;
    for (size_t k = 0; k < sceneIndex_.size(); ++k) if (sceneIndex_[k] == proc_.activeScene()) pos = int(k);
    scenes_.setSelected(pos);
    for (size_t k = 0; k < sceneIndex_.size(); ++k) {
        // tooltips: empty scenes explain how to fill them
        if (auto* c = scenes_.getChildComponent(int(k))) {
            if (auto* b = dynamic_cast<juce::Button*>(c))
                b->setTooltip(proc_.scene(sceneIndex_[k]).saved() ? juce::String() : tr(Str::SceneHint));
        }
    }
    layout();
}

void HubEditor::sceneMenu(int i) {
    juce::PopupMenu m;
    m.setLookAndFeel(&lnf_);
    m.addItem(1, tr(Str::SaveToScene));
    m.addItem(2, tr(Str::RenameScene));
    m.addItem(3, tr(Str::ClearScene), proc_.scene(i).saved());
    int freeSlot = -1;
    for (int k = 3; k < kNumScenes && freeSlot < 0; ++k)
        if (!proc_.scene(k).saved() && proc_.scene(k).name.isEmpty()) freeSlot = k;
    m.addSeparator();
    m.addItem(4, tr(Str::SaveAsNewScene), freeSlot >= 0);
    juce::Component::SafePointer<HubEditor> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&scenes_), [self, i, freeSlot](int r) {
        if (self == nullptr) return;
        auto& proc = self->proc_;
        if (r == 1) proc.saveScene(i);
        if (r == 2) self->askRename(tr(Str::RenameScene), proc.sceneName(i), [self, i](juce::String n) { if (self) self->proc_.renameScene(i, n); });
        if (r == 3) proc.clearScene(i);
        if (r == 4 && freeSlot >= 0) proc.saveScene(freeSlot);
    });
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

void HubEditor::showTrackPanel(int slot, juce::Component& anchor) {
    auto panel = std::make_unique<HubTrackPanel>(*this, slot);
    panel->setLookAndFeel(&lnf_);
    auto& box = juce::CallOutBox::launchAsynchronously(std::move(panel), anchor.getScreenBounds(), nullptr);
    box.setLookAndFeel(&lnf_);
}

void HubEditor::showSettings() {
    auto panel = std::make_unique<HubSettingsPanel>(proc_);
    panel->setLookAndFeel(&lnf_);
    auto& box = juce::CallOutBox::launchAsynchronously(std::move(panel), settingsButton_.getScreenBounds(), nullptr);
    box.setLookAndFeel(&lnf_);
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
    if (structure) { layoutList(); content_.repaint(); }
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
        layout();
        content_.repaint();
    }
    if (lim != lastLimiter_ || limiter_.isOn() != lim) { lastLimiter_ = lim; limiter_.setOn(lim, content_.isShowing()); }
    masterLink_->update();
    headphoneLink_->update();

    syncTracks();
    if (auto* bus = proc_.engine().bus()) {
        const auto& sh = bus->streamHeader;
        meterL_.setLevel(meterPosition(ssbus::bitsFloat(sh.peakBits[0][0].load(std::memory_order_relaxed))));
        meterR_.setLevel(meterPosition(ssbus::bitsFloat(sh.peakBits[0][1].load(std::memory_order_relaxed))));
    }

    if (++slowTick_ % 6 == 0) {   // ~5 Hz: chip, LUFS, summary text
        const bool obs = proc_.obsConnected();
        obsChip_.set(obs ? tr(Str::ObsConnected) : tr(Str::ObsNotConnected), obs ? StatusChip::Dot::Solid : StatusChip::Dot::None);
        content_.repaint(lufsBox_.toNearestInt().expanded(2));
        content_.repaint(summaryCard_.toNearestInt());
        content_.repaint(masterLabel_.toNearestInt());
        const auto w = obsChip_.idealWidth();
        if (w != obsChip_.getWidth()) layout();
    }
}

// ---------------------------------------------------------------------------------------------

void HubEditor::layout() {
    const auto b = content_.getLocalBounds().toFloat().reduced(20.0f);
    auto r = b;

    // header
    header_ = r.removeFromTop(60.0f);
    r.removeFromTop(16.0f);
    {
        auto h = header_.withTrimmedLeft(24.0f).withTrimmedRight(10.0f);
        wordmark_ = h.removeFromLeft(170.0f).withSizeKeepingCentre(170.0f, 30.0f);
        settingsButton_.setBounds(h.removeFromRight(36.0f).withSizeKeepingCentre(36.0f, 36.0f).toNearestInt());
        h.removeFromRight(8.0f);
        const float mw = float(mute_.idealWidth());
        mute_.setBounds(h.removeFromRight(mw).withSizeKeepingCentre(mw, 40.0f).toNearestInt());
        h.removeFromRight(8.0f);
        const float cw = float(obsChip_.idealWidth());
        obsChip_.setBounds(h.removeFromRight(cw).withSizeKeepingCentre(cw, 36.0f).toNearestInt());
        h.removeFromRight(12.0f);
        const float sw = juce::jmin(float(scenes_.idealWidth()), h.getWidth());
        scenes_.setBounds(h.withSizeKeepingCentre(sw, 44.0f).toNearestInt());
    }

    const bool showMain = proc_.connected() && proc_.engine().role() != ssengine::HubEngine::Role::Secondary;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &viewport_, &meterL_, &meterR_, &masterSlider_, &limiter_,
                                                                        &preview_, &headphoneSlider_, &masteringButton_ })
        c->setVisible(showMain);
    if (!showMain) {
        previewBanner_.setVisible(false);
        panicBanner_.setVisible(false);
        messageCard_ = r.withSizeKeepingCentre(juce::jmin(560.0f, r.getWidth()), 180.0f);
        backdrop_.setCards({ { header_, theme::radius::header }, { messageCard_, theme::radius::card } });
        return;
    }
    previewBanner_.setVisible(lastPreview_);
    panicBanner_.setVisible(lastPanic_);

    const float rightW = b.getWidth() > 1180.0f ? 340.0f : 300.0f;
    auto right = r.removeFromRight(rightW);
    r.removeFromRight(16.0f);
    tracksCard_ = r;

    // left card
    {
        auto c = tracksCard_.withTrimmedTop(22.0f).withTrimmedLeft(20.0f).withTrimmedRight(20.0f).withTrimmedBottom(20.0f);
        tracksTitle_ = c.removeFromTop(48.0f).withTrimmedLeft(4.0f);
        c.removeFromTop(4.0f);
        for (auto* bn : { &previewBanner_, &panicBanner_ }) {
            if (!bn->isVisible()) continue;
            const int h = bn->idealHeight(int(c.getWidth()));
            bn->setBounds(c.removeFromTop(float(h)).toNearestInt());
            c.removeFromTop(12.0f);
        }
        columns_ = c.removeFromTop(20.0f);
        c.removeFromTop(8.0f);
        viewport_.setBounds(c.toNearestInt());
        emptyArea_ = c;
        layoutList();
    }

    // stream card
    {
        streamCard_ = right.removeFromTop(352.0f);
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
        auto mRow = c.removeFromTop(38.0f);
        masteringButton_.setBounds(mRow.removeFromRight(84.0f).withSizeKeepingCentre(84.0f, 32.0f).toNearestInt());
        masteringText_ = mRow;
        c.removeFromTop(14.0f);
        preview_.setBounds(c.removeFromTop(48.0f).toNearestInt());
    }
    right.removeFromTop(16.0f);

    // summary card
    {
        summaryCard_ = right;
        auto c = summaryCard_.reduced(18.0f);
        summaryTitle_ = c.removeFromTop(24.0f);
        c.removeFromTop(12.0f);
        youBox_ = c.removeFromTop(96.0f);
        {
            auto in = youBox_.reduced(14.0f, 10.0f);
            youText_ = in.removeFromTop(38.0f);
            in.removeFromTop(4.0f);
            hpLabel_ = in.removeFromTop(16.0f);
            in.removeFromTop(2.0f);
            headphoneSlider_.setBounds(in.withSizeKeepingCentre(in.getWidth(), 20.0f).toNearestInt());
        }
        c.removeFromTop(8.0f);
        viewersBox_ = c.removeFromTop(62.0f);
        c.removeFromTop(8.0f);
        latencyBox_ = c.removeFromTop(juce::jmin(c.getHeight(), 58.0f));
    }

    backdrop_.setCards({ { header_, theme::radius::header }, { tracksCard_, theme::radius::card },
                         { streamCard_, theme::radius::card }, { summaryCard_, theme::radius::card } });
}

void HubEditor::layoutList() {
    const int w = viewport_.getWidth() - (rows_.size() * int(kRowH + 8.0f) > viewport_.getHeight() ? viewport_.getScrollBarThickness() + 4 : 0);
    int y = 0;
    for (auto* row : rows_) {
        row->setBounds(0, y, w, int(kRowH));
        y += int(kRowH) + 8;
    }
    list_.setSize(juce::jmax(1, w), juce::jmax(1, y - 8));
}

void HubEditor::paintContent(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    backdrop_.paint(g, content_.getLocalBounds(), p, settings_->glassAlpha(), lnf_.isDark());

    g.setColour(p.ink);
    drawWordmark(g, wordmark_, "HUB", 17.0f, p.ink);

    const bool secondary = proc_.engine().role() == ssengine::HubEngine::Role::Secondary;
    if (!proc_.connected() || secondary) {
        auto c = messageCard_.reduced(28.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(20.0f, Weight::SemiBold));
        g.drawText(secondary ? tr(Str::SecondHubTitle) : tr(Str::HubBusError).upToFirstOccurrenceOf(" ", false, false),
                   c.removeFromTop(30.0f), juce::Justification::centredLeft, true);
        c.removeFromTop(10.0f);
        drawTextBlock(g, secondary ? tr(Str::SecondHubBody) : tr(Str::HubBusError), uiFont(14.0f), p.graphite, c, 4.0f);
        return;
    }

    // ---- tracks card ------------------------------------------------------------------------
    {
        auto t = tracksTitle_;
        g.setColour(p.ink);
        g.setFont(uiFont(20.0f, Weight::SemiBold));
        g.drawText(tr(Str::TracksTitle), t.removeFromTop(26.0f), juce::Justification::centredLeft, true);
        g.setColour(p.graphite);
        g.setFont(uiFont(13.0f));
        g.drawText(tr(Str::TracksSubtitle), t.withTrimmedTop(4.0f), juce::Justification::topLeft, true);

        // column header (aligned with TrackRow)
        auto c = columns_.withTrimmedLeft(16.0f).withTrimmedRight(12.0f);
        auto level = c.removeFromRight(kLevelW);
        c.removeFromRight(kGap);
        auto strCol = c.removeFromRight(kPillW);
        c.removeFromRight(kGap);
        auto monCol = c.removeFromRight(kPillW);
        g.setFont(uiFont(12.0f));
        g.setColour(p.graphite);
        g.drawText(tr(Str::ColTrack), c, juce::Justification::centredLeft, true);
        auto head = [&](juce::Rectangle<float> col, icons::Icon icon, const juce::String& text) {
            const float tw = textWidth(uiFont(12.0f), text);
            const float x = col.getCentreX() - (14.0f + 6.0f + tw) * 0.5f;
            icons::draw(g, icon, { x, col.getCentreY() - 7.0f, 14.0f, 14.0f }, p.graphite);
            g.drawText(text, juce::Rectangle<float>(x + 20.0f, col.getY(), tw + 2.0f, col.getHeight()), juce::Justification::centredLeft, false);
        };
        head(monCol, icons::Icon::Headphones, tr(Str::MonLabel));
        head(strCol, icons::Icon::Broadcast, tr(Str::StrLabel));
        g.drawText(tr(Str::HeadphoneLevel) + " / " + tr(Str::ViewersLevel), level, juce::Justification::centredLeft, true);

        if (rows_.isEmpty()) {
            auto e = emptyArea_.withHeight(juce::jmin(emptyArea_.getHeight(), 110.0f));
            g.setColour(p.inset);
            g.fillRoundedRectangle(e, theme::radius::row);
            g.setColour(p.hairline2);
            g.drawRoundedRectangle(e.reduced(0.5f), theme::radius::row, 1.0f);
            drawTextBlock(g, tr(Str::NoTracks), uiFont(14.0f), p.graphite, e.reduced(24.0f, 30.0f), 4.0f);
        }
    }

    // ---- stream card -------------------------------------------------------------------------
    {
        auto t = streamTitle_;
        g.setColour(p.ink);
        g.setFont(uiFont(17.0f, Weight::SemiBold));
        g.drawFittedText(tr(Str::StreamTitle), t.removeFromTop(24.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::StreamSubtitle), t.withTrimmedTop(4.0f), juce::Justification::topLeft, true);

        drawInset(g, lufsBox_, theme::radius::small, p);
        auto lb = lufsBox_.reduced(12.0f, 8.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(11.0f));
        g.drawText(tr(Str::Loudness), lb.removeFromTop(14.0f), juce::Justification::centredRight, false);
        juce::String lufs;
        if (lastPanic_) lufs = tr(Str::LoudMuted);
        else if (auto* bus = proc_.engine().bus()) {
            const float v = ssbus::bitsFloat(bus->streamHeader.loudnessSBits.load(std::memory_order_relaxed));
            lufs = v < -70.0f ? tr(Str::LoudSilent) : minusText(juce::String(v, 1));
        }
        g.setColour(p.ink);
        g.setFont(uiFont(22.0f, Weight::SemiBold));
        g.drawText(lufs, lb, juce::Justification::centredRight, false);

        g.setColour(p.graphite);
        g.setFont(uiFont(10.0f));
        g.drawText("L", meterLabels_.removeFromTop(6.0f).expanded(0, 3), juce::Justification::centredLeft, false);
        meterLabels_.removeFromTop(6.0f);
        g.drawText("R", meterLabels_.expanded(0, 3), juce::Justification::centredLeft, false);

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

        auto mt = masteringText_;
        auto& chain = proc_.mastering();
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f));
        g.drawText(tr(Str::Mastering), mt.removeFromTop(18.0f), juce::Justification::centredLeft, true);
        g.setColour(p.graphite);
        g.setFont(uiFont(11.0f));
        juce::String cap = tr(Str::MasteringNone);
        if (chain.size() > 0) {
            const double ms = chain.totalLatencyFrames() * 1000.0 / juce::jmax(8000.0, proc_.getSampleRate() > 0 ? proc_.getSampleRate() : 48000.0);
            cap = tr(Str::MasteringCount).replaceFirstOccurrenceOf("%d", juce::String(chain.size()))
                                         .replaceFirstOccurrenceOf("%d", juce::String(juce::roundToInt(ms)));
        }
        g.drawText(cap, mt, juce::Justification::centredLeft, true);
    }

    // ---- summary card ------------------------------------------------------------------------
    {
        g.setColour(p.ink);
        g.setFont(uiFont(17.0f, Weight::SemiBold));
        g.drawText(tr(Str::SummaryTitle), summaryTitle_, juce::Justification::centredLeft, true);

        bool solo = false;
        for (const auto& v : views_) solo = solo || (v.solo && v.str);
        juce::StringArray you, viewers;
        for (const auto& v : views_) {
            if (!v.active) continue;
            if (v.mon) you.add(v.name);
            if (v.str && (!solo || v.solo)) viewers.add(v.name);
        }
        const auto youText = lastPreview_ ? tr(Str::SummaryPreviewing) : (you.isEmpty() ? tr(Str::None) : you.joinIntoString(", "));
        const auto viewersText = lastPanic_ ? tr(Str::SummaryPanic) : (viewers.isEmpty() ? tr(Str::None) : viewers.joinIntoString(", "));

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

        if (latencyBox_.getHeight() > 30.0f) {
            drawInset(g, latencyBox_, theme::radius::small, p);
            auto lt = latencyBox_.reduced(14.0f, 8.0f);
            if (proc_.obsConnected()) {
                g.setColour(p.graphite);
                g.setFont(uiFont(12.0f));
                g.drawText(tr(Str::LatencyToObs), lt, juce::Justification::centredLeft, true);
                g.setColour(p.ink);
                g.setFont(uiFont(13.0f, Weight::SemiBold));
                g.drawText(juce::String(juce::roundToInt(proc_.latencyToObsMs())) + " ms", lt, juce::Justification::centredRight, false);
            } else {
                drawTextBlock(g, tr(Str::ObsHint), uiFont(12.0f), p.graphite, lt, 1.0f);
            }
        }
    }
}

} // namespace hearaside
