#include "TrackEditor.h"
#include "FriendDirectory.h"
#include "Host.h"
#include "Links.h"

namespace hearaside {

using ssengine::TrackPublisher;

namespace {

juce::Colour shadeFor(const theme::Palette& p, int index) {
    const juce::Colour shades[] = { p.trackShade1, p.trackShade2, p.trackShade3, p.trackShade4, p.trackShade5 };
    return shades[juce::jmax(0, index) % 5];
}

constexpr float kPad = 12.0f, kGap = 10.0f, kHeaderH = 54.0f;

void setupMsSlider(juce::Slider& s) {
    s.setRange(0.0, 200.0, 1.0);
    s.setDoubleClickReturnValue(true, 0.0);
    s.setVelocityModeParameters(0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    s.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    s.setScrollWheelEnabled(true);
    s.setWantsKeyboardFocus(true);
    setFocusDrawnBySelf(s);
}

} // namespace

// ---------------------------------------------------------------------------------------------
void TrackEditor::Body::resized() {
    int y = 2;
    for (auto* f : fields) {
        const int h = f->idealHeight(getWidth());
        f->setBounds(0, y, getWidth(), h);
        y += h + 12;
    }
}

TrackEditor::Sheet::AccountRow::AccountRow(TrackEditor& e) : ed(e) {
    AccountManager::get().addListener(this);
    addAndMakeVisible(actionBtn);
    refreshTexts();
}

TrackEditor::Sheet::AccountRow::~AccountRow() {
    AccountManager::get().removeListener(this);
}

void TrackEditor::Sheet::AccountRow::accountStateChanged(AccountManager::State) {
    refreshTexts();
    repaint();
}

void TrackEditor::Sheet::AccountRow::accountProfileChanged(const AccountManager::Profile&) {
    refreshTexts();
    repaint();
}

void TrackEditor::Sheet::AccountRow::refreshTexts() {
    const bool signedIn = AccountManager::get().isSignedIn();
    actionBtn.setButtonText(signedIn ? tr(Str::EditArrow) : tr(Str::SignInEllipsis));
    actionBtn.setTitle(signedIn ? tr(Str::EditArrow) : tr(Str::SignInEllipsis));
    actionBtn.setTooltip(signedIn ? tr(Str::EditArrow) : tr(Str::SignInEllipsis));
    repaint();
}

void TrackEditor::Sheet::AccountRow::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat();
    const bool signedIn = AccountManager::get().isSignedIn();
    const auto& prof = AccountManager::get().profile();

    auto avR = r.removeFromLeft(28.0f).withSizeKeepingCentre(28.0f, 28.0f);
    r.removeFromLeft(10.0f);
    r.removeFromRight(float(actionBtn.getWidth() + 8));

    if (signedIn) {
        drawAvatar(g, avR, prof.displayName.isNotEmpty() ? prof.displayName : prof.handle, true, p, prof.avatarImage);
        auto top = r.removeFromTop(18.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawText(prof.displayName.isNotEmpty() ? prof.displayName : prof.handle, top, juce::Justification::centredLeft, true);
        g.setColour(p.graphite);
        g.setFont(uiFont(11.5f, Weight::Regular));
        g.drawText("@" + prof.handle, r, juce::Justification::centredLeft, true);
    } else {
        drawAvatar(g, avR, "?", false, p);
        g.setColour(p.graphite);
        g.setFont(uiFont(13.0f, Weight::Regular));
        g.drawText(tr(Str::NotSignedIn), r, juce::Justification::centredLeft, true);
    }
}

void TrackEditor::Sheet::AccountRow::resized() {
    const int btnW = actionBtn.idealWidth() > 0 ? actionBtn.idealWidth() : 74;
    actionBtn.setBounds(getWidth() - btnW, (getHeight() - 30) / 2, btnW, 30);
}

TrackEditor::Sheet::Sheet(TrackEditor& e)
    : ed(e), sourceField({}, &source, 48), nameField({}, &name, 38), panField({}, &pan, PanSlider::kFullHeight, &panValue),
      stemField({}, &stem, 38), busField({}, &bus, 38), soloField({}, &soloRow, 44), appearanceField({}, &appearance, 400),
      accountRow(e), accountField({}, &accountRow, 46) {
    source.buildMenu = [this] { return buildSourceMenu(); };
    soloRow.sw = &solo;
    soloRow.addAndMakeVisible(solo);
    addAndMakeVisible(back);
    addAndMakeVisible(scroll);
    scroll.setContent(body);
    body.fields = { &sourceField, &nameField, &panField, &stemField, &soloField, &busField, &appearanceField, &accountField };
    for (auto* f : { &sourceField, &nameField, &panField, &stemField, &soloField, &busField, &appearanceField, &accountField })
        body.addAndMakeVisible(f);

    accountRow.actionBtn.onClick = [this] { ed.openAccount(true); };
    back.onClick = [this] { ed.openFineSettings(false); };
    name.setMaxUtf8Bytes(63);
    name.onReturnKey = [this] { ed.proc_.setDisplayNameOverride(name.getText()); ed.grabKeyboardFocus(); };
    name.onFocusLost = [this] { ed.proc_.setDisplayNameOverride(name.getText()); };
    bus.onReturnKey = [this] { ed.proc_.setBusName(bus.getText()); };
    bus.onFocusLost = [this] { ed.proc_.setBusName(bus.getText()); };
    auto& panParam = *ed.proc_.params().getParameter(trackparam::StrPan);
    pan.onDragStart = [&panParam] { panParam.beginChangeGesture(); };
    pan.onDragEnd = [&panParam] { panParam.endChangeGesture(); };
    pan.onChange = [&panParam, this](float v) {
        panParam.setValueNotifyingHost(panParam.convertTo0to1(v));
        panValue.setValue(v);
    };
    panValue.onCommit = [&panParam](float v) { setParamWithGesture(panParam, v); };
    stem.onChange = [this](int i) { ed.proc_.setStemIndex(i - 1); };
    solo.onClick = [this] { ed.toggle(trackparam::StrSolo); };
    setFocusDrawnBySelf(*this);
    getProperties().set("hsCovers", true);   // a sheet over the card (ui-snapshot's layout check)
    refreshTexts();
    sync();
}

Menu TrackEditor::Sheet::buildSourceMenu() {
    Menu m(260);
    m.header(tr(Str::ChooseSource));

    const bool isTrack = (ed.proc_.role() == TrackProcessor::Role::Track);
    m.check(tr(Str::SourceThisTrack), isTrack, [this] {
        if (ed.proc_.role() != TrackProcessor::Role::Track) {
            ed.proc_.setRole(TrackProcessor::Role::Track);
            sync();
            ed.refreshStatus();
            ed.layout();
            ed.content_.repaint();
        }
    });
    m.last().icon = icons::Icon::Waveform;

    m.separator();

    const auto friends = FriendDirectory::all();
    const uint32_t currentFriendId = ed.proc_.isFriendInput() ? ed.proc_.friendId() : 0;
    bool currentFound = false;

    for (const auto& f : friends) {
        const bool isSelected = (ed.proc_.isFriendInput() && currentFriendId == f.id);
        if (isSelected) currentFound = true;

        juce::String label;
        if (f.inDaw && f.feeder != ed.proc_.feederIndex()) {
            label = trf(Str::FriendInTrack, { f.name, f.feeder >= 0 ? juce::String(f.feeder + 1) : juce::String() });
        } else {
            label = trf(Str::FriendItem, { f.name });
        }

        m.check(label, isSelected, [this, fid = f.id] {
            ed.proc_.setRole(TrackProcessor::Role::FriendInput);
            ed.proc_.setFriendId(fid);
            sync();
            ed.refreshStatus();
            ed.layout();
            ed.content_.repaint();
        });
        m.last().icon = icons::Icon::Person;
        m.last().dot = f.live() ? Dot::Ok : f.state == ssbus::kFriendWaiting ? Dot::Warn : Dot::Muted;
    }

    if (friends.empty() && !ed.proc_.publisher().hubPresent()) {
        m.item(tr(Str::SourceNeedsHub), [] {});
        m.last().enabled = false;
    }

    if (ed.proc_.isFriendInput() && currentFriendId != 0 && !currentFound) {
        const auto fn = FriendDirectory::nameOf(currentFriendId);
        m.check(trf(Str::FriendItem, { fn.isNotEmpty() ? fn : tr(Str::FriendWord) }), true, [] {});
        m.last().icon = icons::Icon::Person;
        m.last().dot = Dot::Warn;
    }

    m.separator();

    m.item(tr(Str::SourcePasteLink), [this] {
        const auto clip = juce::SystemClipboard::getTextFromClipboard().trim();
        if (clip.isNotEmpty()) {
            const auto parsed = links::parse(clip);
            if (parsed.kind == links::Kind::Listen) {
                ed.overlay().toast(tr(Str::LinkListenNotLinedUp));
                return;
            }
            if (parsed.kind == links::Kind::Send || clip.contains("/s/")) {
                ed.proc_.setRole(TrackProcessor::Role::FriendInput);
                ed.proc_.requestResolveToken(parsed.token.isNotEmpty() ? parsed.token : clip);
                sync();
                ed.refreshStatus();
                ed.layout();
                ed.content_.repaint();
                return;
            }
        }
        ed.overlay().toast(tr(Str::LinkNotHearaside));
    });
    m.last().icon = icons::Icon::Link;

    m.item(tr(Str::SourceInviteFriend), [this] {
        if (!ed.proc_.publisher().hubPresent() && ed.proc_.bus() == nullptr) {
            ed.overlay().toast(tr(Str::SourceNeedsHub));
            return;
        }
        const int nextNum = int(FriendDirectory::all().size()) + 1;
        ed.proc_.setRole(TrackProcessor::Role::FriendInput);
        ed.proc_.requestCreateFriend(trf(Str::FriendDefaultName, { juce::String(nextNum) }));
        sync();
        ed.refreshStatus();
        ed.layout();
        ed.content_.repaint();
    });
    m.last().icon = icons::Icon::Plus;

    if (ed.proc_.isFriendInput()) {
        const uint32_t fid = ed.proc_.friendId();
        FriendDirectory::Info fi;
        const auto fn = (fid != 0 && FriendDirectory::find(fid, fi)) ? fi.name : tr(Str::FriendWord);

        m.separator();
        if (fid != 0) {
            m.item(trf(Str::LinkCopiedSendTo, { fn }), [this, fid, fn] {
                ed.proc_.requestCopyLink(fid);
                ed.overlay().toast(trf(Str::LinkCopiedSendTo, { fn }));
            });
            m.last().icon = icons::Icon::Link;
        }
        m.item(trf(Str::StopBringing, { fn }), [this] {
            ed.proc_.requestReleaseFriend();
            ed.proc_.setRole(TrackProcessor::Role::Track);
            sync();
            ed.refreshStatus();
            ed.layout();
            ed.content_.repaint();
        });
        m.last().danger = true;
    }

    return m;
}

void TrackEditor::Sheet::refreshTexts() {
    back.setTitle(tr(Str::BackToTrack));
    back.setTooltip(tr(Str::BackToTrack));
    sourceField.setTexts(tr(Str::ChooseSource), {});
    nameField.setTexts(tr(Str::RenameDisplay), tr(Str::RenameCap));
    panField.setTexts(tr(Str::Pan), {});
    stemField.setTexts(tr(Str::Stem), {});
    busField.setTexts(tr(Str::BusName), tr(Str::BusNameTrackCap));
    soloField.setTexts({}, {});
    soloRow.title = tr(Str::StreamSolo);
    soloRow.caption = tr(Str::SoloCap);
    solo.setTitle(tr(Str::StreamSolo));
    appearanceField.setTexts(tr(Str::SecAppearance), {});
    name.setTitle(tr(Str::RenameTrack));
    accountField.setTexts(tr(Str::AccountTitle), {});
    accountRow.refreshTexts();
    soloRow.repaint();
    sync();
}

void TrackEditor::Sheet::sync() {
    auto& p = ed.proc_;
    if (p.isFriendInput()) {
        const uint32_t fid = p.friendId();
        FriendDirectory::Info fi;
        const auto fn = (fid != 0 && FriendDirectory::find(fid, fi)) ? fi.name : tr(Str::FriendWord);
        source.set(icons::Icon::Person, trf(Str::FriendItem, { fn }));
        body.fields = { &sourceField, &nameField, &busField, &appearanceField, &accountField };
    } else {
        source.set(icons::Icon::Waveform, tr(Str::SourceThisTrack));
        body.fields = { &sourceField, &nameField, &panField, &stemField, &soloField, &busField, &appearanceField, &accountField };
    }
    for (auto* f : { &sourceField, &nameField, &panField, &stemField, &soloField, &busField, &appearanceField, &accountField })
        f->setVisible(false);
    for (auto* f : body.fields)
        f->setVisible(true);

    if (!name.hasKeyboardFocus(true)) {
        name.setText(p.displayNameOverride(), false);
        name.setPlaceholder(p.hostTrackName().isNotEmpty() ? p.hostTrackName() : p.displayName());
    }
    if (!bus.hasKeyboardFocus(true)) bus.setText(p.busName(), false);
    juce::StringArray stems { tr(Str::StemNone) };
    auto* b = p.bus();
    for (int i = 0; i < ssbus::kMaxStems; ++i) {
        juce::String n;
        if (b) n = juce::String::fromUTF8(b->streamHeader.names[1 + i], ssbus::kNameBytes).upToFirstOccurrenceOf(juce::String::charToString(0), false, false);
        stems.add(n.isNotEmpty() ? n : trf(Str::StemN, { juce::String(i + 1) }));
    }
    stem.setItems(stems);
    stem.setSelected(p.stemIndex() + 1);
    const float panNow = paramPlain(*p.params().getParameter(trackparam::StrPan));
    if (!pan.slider().isMouseButtonDown()) pan.setPan(panNow);
    if (!panValue.isEditing()) panValue.setValue(panNow);
    solo.setOn(p.params().getRawParameterValue(trackparam::StrSolo)->load() > 0.5f, isShowing());
    resized();
}

void TrackEditor::Sheet::SoloRow::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().withTrimmedRight(56.0f);
    const auto tf = uiFont(13.0f, Weight::Medium), cf = uiFont(12.0f);
    auto block = r.withSizeKeepingCentre(r.getWidth(), tf.getHeight() + 2.0f + cf.getHeight());
    g.setColour(p.ink);
    g.setFont(tf);
    g.drawText(title, block.removeFromTop(tf.getHeight()), juce::Justification::centredLeft, true);
    block.removeFromTop(2.0f);
    g.setColour(p.graphite);
    g.setFont(cf);
    g.drawText(caption, block, juce::Justification::centredLeft, true);
}

void TrackEditor::Sheet::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    g.setColour(p.paper.overlaidWith(p.sheet));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 20.0f);
    g.setColour(p.ink);
    g.setFont(uiFont(15.0f, Weight::SemiBold));
    g.drawText(tr(Str::FineSettings), juce::Rectangle<float>(float(back.getRight() + 10), 14.0f, float(getWidth() - back.getRight() - 28), 34.0f),
               juce::Justification::centredLeft, true);
}

void TrackEditor::Sheet::resized() {
    auto r = getLocalBounds().reduced(18, 0).withTrimmedTop(14).withTrimmedBottom(16);
    auto top = r.removeFromTop(34);
    back.setBounds(top.removeFromLeft(back.idealWidth()));
    r.removeFromTop(14);
    scroll.setBounds(r.withTrimmedRight(-14));
    scroll.setFadeColour(paletteOf(*this).paper.overlaidWith(paletteOf(*this).sheet));
    const int w = scroll.contentWidth();
    appearanceField.setControlHeight(appearance.idealHeight(w));
    int h = 2;
    for (auto* f : body.fields) h += f->idealHeight(w) + 16;
    body.setSize(w, h);
    int y = 2;
    for (auto* f : body.fields) {
        const int fh = f->idealHeight(w);
        f->setBounds(0, y, w, fh);
        y += fh + 16;
    }
}

// ---------------------------------------------------------------------------------------------
TrackEditor::TrackEditor(TrackProcessor& p)
    : EditorShell(p, kWidth, kHeight, int(theme::layout::trackMinW), int(theme::layout::trackMinH), "track", 640, 1400), proc_(p) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &chip_, &fineButton_, &banner_, &name_, &inputMeter_, &monRow_, &strRow_, &scroll_, &footer_ })
        content_.addAndMakeVisible(c);
    content_.addChildComponent(sheet_);
    content_.addChildComponent(accountPanel_);
    accountPanel_.onBack = [this] { openAccount(false); };
    scroll_.setContent(body_);
    body_.fields = { &headField_, &viewField_, &delayField_ };
    for (auto* f : body_.fields) body_.addAndMakeVisible(f);
    banner_.setVisible(false);

    monRow_.onClick = [this] { toggle(trackparam::Mon); };
    strRow_.onClick = [this] { toggle(trackparam::Str); };
    fineButton_.onClick = [this] { openFineSettings(!sheet_.isVisible()); };
    name_.onRename = [this](const juce::String& n) { proc_.setDisplayNameOverride(n); };

    auto& pr = proc_.params();
    headphoneLink_ = std::make_unique<DbSliderLink>(headphoneSlider_, *pr.getParameter(trackparam::MonTrim));
    viewersLink_ = std::make_unique<DbSliderLink>(viewersSlider_, *pr.getParameter(trackparam::StrGain));
    setupMsSlider(delaySlider_);
    delayLink_ = std::make_unique<DbSliderLink>(delaySlider_, *pr.getParameter(trackparam::StrDelay));
    headValueLink_ = std::make_unique<ParamValueLink>(headValue_, *pr.getParameter(trackparam::MonTrim));
    viewValueLink_ = std::make_unique<ParamValueLink>(viewValue_, *pr.getParameter(trackparam::StrGain));
    delayValueLink_ = std::make_unique<ParamValueLink>(delayValue_, *pr.getParameter(trackparam::StrDelay));

    procListener_.fn = [this] {
        const uint32_t rep = proc_.lastReply();
        if (rep == ssbus::kReplyNotInRoom) {
            overlay().toast(tr(Str::LinkOtherRoom));
            proc_.clearReply();
        } else if (rep == ssbus::kReplyRoomFull) {
            overlay().toast(tr(Str::FriendFullToast));
            proc_.clearReply();
        } else if (rep > 0 && rep < 0xFFFF0000u) {
            proc_.clearReply();
        }
        refreshStatus();
        sheet_.sync();
        content_.repaint();
    };
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
    if (proc_.isFriendInput()) return;
    auto* p = proc_.params().getParameter(id);
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->getValue() > 0.5f ? 0.0f : 1.0f);
    p->endChangeGesture();
    timerCallback();
}

void TrackEditor::openFineSettings(bool open) {
    if (!open) accountPanel_.setVisible(false);
    if (open == sheet_.isVisible()) return;
    overlay().close();
    sheet_.sync();
    sheet_.setVisible(open);
    if (open) sheet_.toFront(false);
    fineButton_.setOn(open);
    (open ? static_cast<juce::Component&>(sheet_.back) : static_cast<juce::Component&>(fineButton_)).grabKeyboardFocus();
}

void TrackEditor::openAccount(bool open) {
    if (open) {
        sheet_.setVisible(false);
        accountPanel_.setVisible(true);
        accountPanel_.setBounds(card_.toNearestInt());
        accountPanel_.toFront(true);
    } else {
        accountPanel_.setVisible(false);
        sheet_.setVisible(true);
        sheet_.toFront(true);
    }
    content_.repaint();
}

void TrackEditor::lookChanged() {
    backdrop_.invalidate();
    refreshTexts();
    refreshStatus();
    sheet_.refreshTexts();
    accountPanel_.refreshTexts();
    layout();
    content_.repaint();
}

void TrackEditor::refreshTexts() {
    monRow_.setTexts(tr(Str::MonRowTitle), tr(Str::MonRowCaption));
    strRow_.setTexts(tr(Str::StrRowTitle), tr(Str::StrRowCaption));
    fineButton_.setTooltip(tr(Str::FineSettings));
    fineButton_.setTitle(tr(Str::FineSettings));
    headField_.setTexts(tr(Str::HeadphoneLevel), {});
    viewField_.setTexts(tr(Str::ViewersLevel), {});
    delayField_.setTexts(tr(Str::ViewersDelay), tr(Str::TrackDelayCap));
    headphoneSlider_.setTitle(tr(Str::HeadphoneLevel));
    viewersSlider_.setTitle(tr(Str::ViewersLevel));
    delaySlider_.setTitle(tr(Str::ViewersDelayMsAria));
    delaySlider_.setTooltip(tr(Str::DoubleClickToResetMs));
    headphoneSlider_.setTooltip(tr(Str::DoubleClickToReset));
    viewersSlider_.setTooltip(tr(Str::DoubleClickToReset));
    footer_.setTooltip(tr(Str::DawBufferFooterTip));
}

void TrackEditor::refreshStatus() {
    const auto st = proc_.publisher().status();
    auto* bus = proc_.bus();
    auto* slot = proc_.publisher().slot();
    const bool hub = proc_.publisher().hubPresent() || (bus && bus->header.hubFlags.load(std::memory_order_relaxed) != 0);
    const uint32_t hubFlags = bus ? bus->header.hubFlags.load(std::memory_order_relaxed) : 0u;
    const uint32_t hubStatus = slot ? slot->hubStatus.load(std::memory_order_relaxed) : 0u;

    if (hub && (hubFlags & ssbus::kHubPanic)) chip_.set(tr(Str::PanicActiveChip), Dot::RecRing);
    else if (hub && (hubFlags & ssbus::kHubBypassed)) chip_.set(tr(Str::HubBypassedChip), Dot::Warn);
    else if (hub) chip_.set(tr(Str::HubConnected), Dot::Ok);
    else chip_.set(tr(Str::HubMissingChip), Dot::Muted);

    bool show = true;
    if (proc_.isFriendInput()) {
        const uint32_t fid = proc_.friendId();
        FriendDirectory::Info fi;
        const bool hasFriend = (fid != 0 && FriendDirectory::find(fid, fi));
        const auto friendName = hasFriend ? fi.name : tr(Str::FriendWord);

        if (!hub) {
            banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::HubMissingBanner));
        } else if (proc_.isBypassedNow()) {
            banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::BypassWarning));
        } else if (auto* f = proc_.feederRecord(); f != nullptr && lastFeederBlockCount_ > 0 && (juce::Time::getMillisecondCounter() - lastFeederBlockTime_ > 1000)) {
            banner_.set(Banner::Style::Warning, icons::Icon::Warning, currentDaw() == Daw::Cubase ? tr(Str::FriendDawPausedCubase) : tr(Str::FriendDawPaused));
        } else if (hasFriend && fi.outSlot >= 0 && bus != nullptr) {
            const float fxMs = ssbus::bitsFloat(bus->slots[fi.outSlot].fxLatencyBits.load(std::memory_order_relaxed));
            banner_.set(Banner::Style::Info, icons::Icon::Check, trf(Str::FriendPaired, { friendName, juce::String(juce::roundToInt(fxMs)) }));
        } else {
            banner_.set(Banner::Style::Warning, icons::Icon::Warning, trf(Str::FriendNotPaired, { friendName }));
        }
    }
    else if (st == TrackPublisher::Status::SlotsFull) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::SlotsFull));
    else if (st == TrackPublisher::Status::ShmError) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::BusError));
    else if (proc_.isBypassedNow()) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::BypassWarning));
    else if (hub && (hubStatus & ssbus::kHubStatusRateMismatch)) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::RateMismatchTrack));
    else if (!hub) banner_.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::HubMissingBanner));
    else if (const uint32_t pairedId = proc_.pairedFriendId(); pairedId != 0) {
        FriendDirectory::Info fi;
        const auto friendName = FriendDirectory::find(pairedId, fi) ? fi.name : tr(Str::FriendWord);
        banner_.set(Banner::Style::Info, icons::Icon::Check, trf(Str::FriendPaired, { friendName, juce::String(juce::roundToInt(proc_.fxLatencyMs())) }));
    }
    else show = false;

    if (show != banner_.isVisible()) {
        banner_.setVisible(show);
        layout();
        content_.repaint();
    }
}

Str TrackEditor::summaryKey() const {
    return lastMon_ && lastStr_ ? Str::SumBoth : (!lastMon_ && lastStr_ ? Str::SumViewersOnly : (lastMon_ ? Str::SumYouOnly : Str::SumSilent));
}

juce::String TrackEditor::summaryText() const {
    if (proc_.isFriendInput()) {
        const uint32_t fid = proc_.friendId();
        FriendDirectory::Info fi;
        const auto fn = (fid != 0 && FriendDirectory::find(fid, fi)) ? fi.name : tr(Str::FriendWord);
        return trf(Str::FriendHearSummary, { fn });
    }
    return tr(summaryKey());
}

void TrackEditor::timerCallback() {
    const bool isFriend = proc_.isFriendInput();
    if (isFriend) {
        if (auto* f = proc_.feederRecord()) {
            const uint64_t bc = f->blockCount.load(std::memory_order_relaxed);
            if (bc != lastFeederBlockCount_) {
                lastFeederBlockCount_ = bc;
                lastFeederBlockTime_ = juce::Time::getMillisecondCounter();
            }
        }
    }
    const bool mon = !isFriend && paramOn(trackparam::Mon), str = !isFriend && paramOn(trackparam::Str);
    if (mon != lastMon_ || str != lastStr_ || slowTick_ == 0) {
        const bool animate = content_.isShowing();
        monRow_.setOn(mon, animate);
        strRow_.setOn(str, animate);
        headphoneSlider_.setDim(!mon || isFriend);
        viewersSlider_.setDim(!str || isFriend);
        delaySlider_.getProperties().set(sliderlook::dim, !str || isFriend);
        delaySlider_.repaint();
        headValue_.setDim(!mon || isFriend);
        viewValue_.setDim(!str || isFriend);
        delayValue_.setDim(!str || isFriend);
        lastMon_ = mon;
        lastStr_ = str;
        layout();   // the summary sentence changes length
        content_.repaint();
    }
    headphoneLink_->update();
    viewersLink_->update();
    delayLink_->update();
    headValueLink_->update();
    viewValueLink_->update();
    delayValueLink_->update();
    inputMeter_.setLevel(meterPosition(juce::jmax(proc_.inputPeak(0), proc_.inputPeak(1))));
    {
        const int blk = proc_.lastBlockSize();
        const double sr = proc_.currentSampleRate();
        footer_.setText(blk > 0 ? trf(Str::DawBufferFooter, { juce::String(blk), juce::String(blk * 1000.0 / juce::jmax(8000.0, sr), 1), juce::String(sr / 1000.0, 1) })
                                : juce::String());
    }
    const auto host = proc_.hostTrackName();
    name_.setName(proc_.displayName(), host);
    if (sheet_.isVisible() && ++slowTick_ % 6 == 0) sheet_.sync();
    if (++slowTick_ % 10 == 0) refreshStatus();
}

void TrackEditor::layout() {
    const auto b = content_.getLocalBounds().toFloat();
    auto r = b.reduced(kPad);
    header_ = r.removeFromTop(kHeaderH);
    r.removeFromTop(kGap);
    card_ = r;
    backdrop_.setBlobs({ { { -60.0f, -110.0f, 380.0f, 240.0f }, true },
                         { { b.getWidth() - 200.0f, b.getHeight() - 240.0f, 300.0f, 300.0f }, false } });
    backdrop_.setCards({ { header_, 18.0f }, { card_, 20.0f } });

    // header: wordmark, status chip, fine settings
    auto h = header_.withTrimmedLeft(16.0f).withTrimmedRight(9.0f);
    fineButton_.setBounds(h.removeFromRight(36.0f).withSizeKeepingCentre(36.0f, 36.0f).toNearestInt());
    h.removeFromRight(8.0f);
    wordmark_ = h.removeFromLeft(118.0f).withSizeKeepingCentre(118.0f, 26.0f);
    const float chipW = float(juce::jmin(chip_.idealWidth(), int(h.getWidth())));
    chip_.setBounds(h.removeFromRight(chipW).withSizeKeepingCentre(chipW, 30.0f).toNearestInt());

    // card
    auto c = card_.withTrimmedLeft(18.0f).withTrimmedRight(18.0f).withTrimmedTop(16.0f).withTrimmedBottom(12.0f);
    if (banner_.isVisible()) {
        const int bh = banner_.idealHeight(int(c.getWidth()));
        banner_.setBounds(c.removeFromTop(float(bh)).toNearestInt());
        c.removeFromTop(12.0f);
    }
    auto nameRow = c.removeFromTop(26.0f);
    dot_ = nameRow.removeFromLeft(10.0f).withSizeKeepingCentre(10.0f, 10.0f);
    nameRow.removeFromLeft(10.0f);
    inputMeter_.setBounds(nameRow.removeFromRight(80.0f).withSizeKeepingCentre(80.0f, 8.0f).toNearestInt());
    nameRow.removeFromRight(10.0f);
    inputLabel_ = nameRow.removeFromRight(textWidth(uiFont(11.0f), tr(Str::InputSignal)) + 2.0f);
    nameRow.removeFromRight(10.0f);
    name_.setBounds(nameRow.withTrimmedLeft(-7.0f).toNearestInt());
    c.removeFromTop(12.0f);

    monRow_.setBounds(c.removeFromTop(60.0f).toNearestInt());
    c.removeFromTop(8.0f);
    strRow_.setBounds(c.removeFromTop(60.0f).toNearestInt());
    c.removeFromTop(12.0f);

    const float th = wrappedHeight(uiFont(12.5f), summaryText(), c.getWidth() - 24.0f, 6.0f);
    summary_ = c.removeFromTop(th + 20.0f);
    c.removeFromTop(12.0f);

    footerLine_ = c.removeFromBottom(27.0f);
    footer_.setBounds(footerLine_.withTrimmedTop(11.0f).toNearestInt());
    c.removeFromBottom(4.0f);
    scroll_.setBounds(c.withTrimmedRight(-14.0f).toNearestInt());
    scroll_.setFadeColour(lnf_.pal().paper.overlaidWith(lnf_.pal().glass.withMultipliedAlpha(settings_->glassAlpha() / theme::glass::alphaDefault)));
    const int w = scroll_.contentWidth();
    int bh = 2;
    for (auto* f : body_.fields) bh += f->idealHeight(w) + 12;
    body_.setSize(w, bh);

    sheet_.setBounds(card_.toNearestInt());
    accountPanel_.setBounds(card_.toNearestInt());
}

void TrackEditor::paintContent(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    backdrop_.paint(g, content_.getLocalBounds(), p, settings_->glassAlpha(), lnf_.isDark());
    drawWordmark(g, wordmark_, "TRACK", 14.0f, p.ink);

    const auto hostCol = proc_.trackColour();
    g.setColour(settings_->hostColours() && !hostCol.isTransparent() ? hostCol.withAlpha(1.0f) : shadeFor(p, proc_.publisher().slotIndex() + 1));
    g.fillEllipse(dot_);
    g.setColour(p.graphite);
    g.setFont(uiFont(11.0f));
    g.drawText(tr(Str::InputSignal), inputLabel_, juce::Justification::centredLeft, false);

    // summary sentence in a dashed box
    {
        juce::Path box;
        box.addRoundedRectangle(summary_.reduced(0.5f), 12.0f);
        juce::Path dashed;
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, box, dashes, 2);
        g.setColour(p.hairline3);
        g.fillPath(dashed);
        drawWrapped(g, summaryText(), uiFont(12.5f), p.ink2, summary_.reduced(12.0f, 10.0f), 6.0f);
    }
    g.setColour(p.hairline2);
    g.fillRect(footerLine_.withHeight(1.0f));
}

} // namespace hearaside
