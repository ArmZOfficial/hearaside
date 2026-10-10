#include "HubEditor.h"
#include "HubPages.h"
#include "HubPagesShared.h"
#include "Host.h"
#include "Links.h"
#include "app/AppCapture.h"

namespace hearaside {

using ssbus::ParamId;

namespace {

constexpr const char* kSystemAudio = "*system*";   // AppAudioProcessor::kSystemAudio
constexpr const char* kLinkIn = "*link*";          // AppAudioProcessor::kLinkIn
constexpr float kRightW = 300.0f, kRightWideW = 340.0f, kHeaderH = 64.0f, kCompactHeaderH = 56.0f;
constexpr float kToggleW = 48.0f, kLevelW = 206.0f, kMoreW = 32.0f, kColGap = 10.0f;
constexpr int kGroupGap = 14;

juce::String clock(double seconds) {
    const int s = juce::roundToInt(seconds);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

juce::String minusText(const juce::String& s) { return s.replace("-", juce::String(juce::CharPointer_UTF8("\xe2\x88\x92"))); }

juce::String joinDots(const juce::StringArray& a) { return a.joinIntoString(juce::String(juce::CharPointer_UTF8(" \xc2\xb7 "))); }

// The OBS chip's popover (prompt 3.3 header item 2): connection + the delay to OBS
class ObsPopover : public juce::Component {
public:
    explicit ObsPopover(HubProcessor& p) : proc_(p) { setSize(300, height()); }
    int height() const { return (proc_.obsConnected() ? 186 : 236) + (lineUpShown() ? 20 : 0); }
    bool lineUpShown() const { return proc_.lineUp() && proc_.lineUpMs() > 0.5f; }
    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto r = getLocalBounds().toFloat();
        const bool obs = proc_.obsConnected();
        auto head = r.removeFromTop(20.0f);
        drawStatusDot(g, { head.getX() + 4.0f, head.getCentreY() }, obs ? Dot::Ok : Dot::Warn, p, 7.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(14.0f, Weight::SemiBold));
        g.drawText(obs ? tr(Str::ObsConnected) : tr(Str::ObsNotConnected), head.withTrimmedLeft(16.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(12.0f);
        if (!obs) {
            NumberedText(g, r, { tr(Str::ObsOpen), tr(Str::ObsAddSource) }, p);
        } else {
            drawWrapped(g, tr(Str::ObsConnectedCap), uiFont(13.0f), p.ink2, r.removeFromTop(20.0f), 3.0f);
            r.removeFromTop(12.0f);
        }
        g.setColour(p.hairline2);
        g.fillRect(r.removeFromTop(1.0f));
        r.removeFromTop(12.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::LatencyToObs), r.removeFromTop(16.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(10.0f);
        const auto li = proc_.latency();
        auto ms = [](double v) { return juce::String(v, 1) + " ms"; };
        auto line = [&](const juce::String& a, const juce::String& b, bool total) {
            auto row = r.removeFromTop(total ? 26.0f : 20.0f);
            if (total) {
                g.setColour(p.hairline2);
                g.fillRect(row.removeFromTop(1.0f));
                row.removeFromTop(5.0f);
            }
            g.setColour(total ? p.ink : p.ink2);
            g.setFont(uiFont(12.5f, total ? Weight::SemiBold : Weight::Regular));
            g.drawText(a, row, juce::Justification::centredLeft, true);
            g.drawText(b, row, juce::Justification::centredRight, false);
            r.removeFromTop(total ? 0.0f : 6.0f);
        };
        line(tr(Str::DawBuffer), li.block > 0 ? juce::String(li.block) + " " + tr(Str::SampleWord) + juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")) + ms(li.dawMs)
                                              : juce::String(juce::CharPointer_UTF8("\xe2\x80\x93")), false);
        line(tr(Str::TrackFx), ms(li.trackFxMs), false);
        line(tr(Str::MasterFx), ms(li.masterFxMs), false);
        line(tr(Str::Total), trf(Str::AboutMs, { juce::String(li.total(), 1) }), true);
        if (lineUpShown()) { r.removeFromTop(4.0f); line(tr(Str::LineUpForFriends), ms(double(proc_.lineUpMs())), false); }
    }
private:
    static void NumberedText(juce::Graphics& g, juce::Rectangle<float>& r, const juce::StringArray& steps, const theme::Palette& p) {
        for (int i = 0; i < steps.size(); ++i) {
            const float h = wrappedHeight(uiFont(13.0f), steps[i], r.getWidth() - 18.0f, 4.0f);
            auto row = r.removeFromTop(h);
            g.setColour(p.ink2);
            g.setFont(uiFont(13.0f));
            g.drawText(juce::String(i + 1) + ".", row.removeFromLeft(18.0f).withHeight(18.0f), juce::Justification::centredLeft, false);
            drawWrapped(g, steps[i], uiFont(13.0f), p.ink2, row, 4.0f);
            r.removeFromTop(4.0f);
        }
        r.removeFromTop(8.0f);
    }
    HubProcessor& proc_;
};

} // namespace

// =============================================================================================
juce::String HubEditor::programLabel(const juce::String& exe) {
    if (exe.isEmpty()) return tr(Str::AppPick);
    if (exe == kSystemAudio) return tr(Str::AppSystem);
    if (exe == kLinkIn) return tr(Str::SentInLegacy);
    if (uint32_t fid = 0; FriendDirectory::isFriendApp(exe, &fid)) {
        const auto n = FriendDirectory::nameOf(fid);
        return trf(Str::FriendItem, { n.isNotEmpty() ? n : tr(Str::FriendWord) });
    }
    if (exe.startsWith("http")) return tr(Str::AppLinkFrom) + " " + juce::URL(exe).getDomain();
    const auto name = exe.endsWithIgnoreCase(".exe") ? exe.dropLastCharacters(4) : exe;
    return name.substring(0, 1).toUpperCase() + name.substring(1);   // "chrome.exe" -> "Chrome"
}

icons::Icon HubEditor::programIcon(const juce::String& exe) {
    if (exe == kSystemAudio) return icons::Icon::Monitor;
    if (FriendDirectory::isFriendApp(exe)) return icons::Icon::Person;
    if (exe == kLinkIn || exe.startsWith("http")) return icons::Icon::Link;
    return icons::Icon::Window;
}

// =============================================================================================
// ChannelRow

ChannelRow::ChannelRow(HubEditor& ed) : ed_(ed) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &name_, &picker_, &status_, &meter_, &you_, &viewers_, &level_, &value_, &more_ })
        addChildComponent(c);
    you_.onClick = [this] {
        if (d_.kind == RowData::Kind::Friend) ed_.proc().setFriendMon(uint32_t(d_.index), !d_.you);
        else if (d_.slot >= 0) ed_.proc().send(d_.slot, ParamId::Mon, d_.you ? 0.0f : 1.0f);
    };
    viewers_.onClick = [this] {
        if (d_.kind == RowData::Kind::Friend) ed_.proc().setFriendStr(uint32_t(d_.index), !d_.viewers);
        else if (d_.slot >= 0) ed_.proc().send(d_.slot, ParamId::Str, d_.viewers ? 0.0f : 1.0f);
    };
    level_.onValueChange = [this] { if (!updating_) sendLevel(LevelSlider::sliderToDb(level_.getValue())); };
    value_.onCommit = [this](float db) { sendLevel(db <= valuetext::kFloorDb + 1.0e-3f ? -60.0f : db); };
    name_.onRename = [this](const juce::String& n) { if (d_.kind == RowData::Kind::Friend) ed_.renameFriend(uint32_t(d_.index), n); else ed_.renameRow(d_.slot, n); };
    more_.onClick = [this] { showMenu(); };
    picker_.buildMenu = [this] { return programMenu(); };
    refreshTexts();
}

void ChannelRow::refreshTexts() {
    more_.setTitle(tr(Str::OptionsFor).replace("%s", d_.name));
    value_.setTooltip(tr(Str::DoubleClickToType));
    level_.setTooltip(tr(Str::DoubleClickToReset));
}

void ChannelRow::sendLevel(float db) {
    if (d_.kind == RowData::Kind::Friend) {   // one volume for you and the viewers
        lastUserMs_ = juce::Time::getMillisecondCounter();
        ed_.proc().setFriendVolumeDb(uint32_t(d_.index), db);
        value_.setValue(juce::jmax(valuetext::kFloorDb, db));
        return;
    }
    if (d_.slot < 0) return;
    lastUserMs_ = juce::Time::getMillisecondCounter();
    ed_.proc().send(d_.slot, ed_.levelsShowHeadphones() ? ParamId::MonTrimDb : ParamId::StrGainDb, db);
    value_.setValue(juce::jmax(valuetext::kFloorDb, db));
}

juce::String ChannelRow::statusText(Dot& dot) const {
    dot = Dot::None;
    if (d_.kind == RowData::Kind::Friend) {
        const int ms = juce::roundToInt(d_.friendDelayMs);
        if (d_.friendState == 1) {
            if (d_.inDaw) {
                if (!d_.paired) { dot = Dot::Warn; return tr(Str::InDawNotHeadphones); }
                dot = Dot::Ok;
                return d_.friendDelayMs >= 0.0f ? trf(Str::MixedInDaw, { juce::String(ms) }) : trf(Str::SingingInDaw, { d_.dawTrack });
            }
            dot = d_.overLimit ? Dot::Warn : Dot::Ok;
            if (d_.friendDelayMs < 0.0f) return tr(Str::FriendSinging);
            const int panI = juce::roundToInt(d_.pan * 100.0f);
            return trf(Str::FriendLive, { juce::String(ms), panI == 0 ? tr(Str::Center) : valuetext::formatPan(float(panI)) });
        }
        if (d_.friendState == 2) { dot = Dot::Muted; return tr(Str::FriendOffline); }
        dot = Dot::Warn;
        return tr(Str::FriendNotOpened);
    }
    if (d_.kind != RowData::Kind::Program) return {};
    if (!d_.on) { dot = Dot::Muted; return tr(Str::AppOffShort); }
    if (FriendDirectory::isFriendApp(d_.app)) return friendProgramStatus(d_.app, d_.capture, d_.takeShiftMs, dot);
    switch (d_.capture) {   // AppCapture::State: Idle, Starting, Running, NotRunning, Failed
        case 2: dot = Dot::Ok; return d_.app == kLinkIn ? tr(Str::SenderOn) : d_.app.startsWith("http") ? tr(Str::AppLinkReceiving) : tr(Str::AppRunning);
        case 1: dot = Dot::Muted; return tr(Str::AppStarting);
        case 3: dot = Dot::Warn; return d_.app == kLinkIn ? tr(Str::SenderOff) : d_.app.startsWith("http") ? tr(Str::AppLinkOffline) : tr(Str::AppNotRunning);
        case 4: dot = Dot::Rec; return tr(Str::AppFailed);
        default: break;
    }
    dot = Dot::Muted;
    return tr(Str::AppNone);
}

void ChannelRow::update(const RowData& d) {
    const bool kindChanged = first_ || d.kind != d_.kind;
    const bool layoutChanged = kindChanged || d.name != d_.name || d.delayMs != d_.delayMs || d.solo != d_.solo || d.active != d_.active
                            || d.recording != d_.recording || d.warnTip != d_.warnTip || d.app != d_.app;
    d_ = d;
    first_ = false;
    const bool program = d.kind == RowData::Kind::Program, friendRow = d.kind == RowData::Kind::Friend;
    name_.setVisible(!program);
    picker_.setVisible(program);
    status_.setVisible(program || friendRow);
    meter_.setVisible(!program && !friendRow);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &you_, &viewers_, &level_, &value_, &more_ }) c->setVisible(true);

    name_.setName(d.name, {});
    name_.setEnabled(d.kind == RowData::Kind::Track || d.kind == RowData::Kind::Friend);
    if (program) picker_.set(HubEditor::programIcon(d.app), HubEditor::programLabel(d.app));
    Dot dot;
    status_.setText(statusText(dot));
    status_.setDot(dot);
    you_.setOn(d.you, isShowing());
    viewers_.setOn(d.viewers, isShowing());
    you_.setSubject(d.name);
    viewers_.setSubject(d.name);
    you_.setEnabled(d.linked);
    viewers_.setEnabled(d.linked);
    const bool hp = ed_.levelsShowHeadphones();
    const bool sideOn = hp ? d.you : d.viewers;
    level_.setDim(!sideOn);
    value_.setDim(!sideOn);
    level_.setEnabled(d.linked);
    value_.setEnabled(d.linked);
    level_.setTitle((hp ? tr(Str::HeadphoneLevel) : tr(Str::ViewersLevel)) + " " + d.name);
    const float db = hp ? d.hpDb : d.vwDb;
    const auto now = juce::Time::getMillisecondCounter();
    if (!level_.isMouseButtonDown() && now - lastUserMs_ > 400) {
        const juce::ScopedValueSetter<bool> svs(updating_, true);
        level_.setValue(LevelSlider::dbToSlider(db), juce::dontSendNotification);
        if (!value_.isEditing()) value_.setValue(juce::jmax(valuetext::kFloorDb, db));
    }
    meter_.setLevel(meterPosition(d.meter));
    setAlpha(d.active ? 1.0f : 0.5f);
    more_.setTitle(tr(Str::OptionsFor).replace("%s", d.name));
    if (layoutChanged) { resized(); repaint(); }
}

void ChannelRow::resized() {
    auto r = getLocalBounds().toFloat();
    const bool friendRow = d_.kind == RowData::Kind::Friend, program = d_.kind == RowData::Kind::Program;
    const auto badgeFont = uiFont(11.0f, Weight::Medium);
    float badgesW = 0.0f;
    if (d_.kind == RowData::Kind::Track && d_.delayMs >= 0.5f) badgesW += badgeWidth(valuetext::formatMs(d_.delayMs)) + 6.0f;
    if (d_.solo) badgesW += badgeWidth(tr(Str::SoloBadge), BadgeStyle::Solid) + 6.0f;
    if (!d_.active) badgesW += badgeWidth(tr(Str::Inactive)) + 6.0f;
    if (d_.warnTip.isNotEmpty()) badgesW += 22.0f;
    juce::ignoreUnused(badgeFont);

    if (narrow_) {   // two lines: [dot name badges  you viewers] / [slider value ⋯]
        auto in = r.withTrimmedLeft(14.0f).withTrimmedRight(8.0f).withTrimmedTop(10.0f).withTrimmedBottom(8.0f);
        auto top = in.removeFromTop(36.0f);
        viewers_.setBounds(top.removeFromRight(44.0f).toNearestInt());
        top.removeFromRight(8.0f);
        you_.setBounds(top.removeFromRight(44.0f).toNearestInt());
        top.removeFromRight(8.0f);
        if (friendRow) { avatar_ = top.removeFromLeft(28.0f).withSizeKeepingCentre(28.0f, 28.0f); top.removeFromLeft(10.0f); }
        else if (!program) { dot_ = top.removeFromLeft(8.0f).withSizeKeepingCentre(8.0f, 8.0f); top.removeFromLeft(8.0f); }
        badges_ = top.removeFromRight(juce::jmin(badgesW, top.getWidth() * 0.5f));
        if (program) {
            auto nameCol = top.withSizeKeepingCentre(top.getWidth(), 42.0f);
            picker_.setBounds(nameCol.removeFromTop(24.0f).withTrimmedLeft(-6.0f).withWidth(juce::jmin(nameCol.getWidth() + 6.0f, float(picker_.idealWidth()))).toNearestInt());
            nameCol.removeFromTop(2.0f);
            status_.setBounds(nameCol.toNearestInt());
        } else if (friendRow) {
            auto nameCol = top.withSizeKeepingCentre(top.getWidth(), 38.0f);
            name_.setBounds(nameCol.removeFromTop(20.0f).toNearestInt());
            status_.setBounds(nameCol.withTrimmedTop(2.0f).toNearestInt());
        } else {
            name_.setBounds(top.withTrimmedLeft(-7.0f).withSizeKeepingCentre(top.getWidth() + 7.0f, 26.0f).toNearestInt());
            meter_.setBounds({});
        }
        in.removeFromTop(8.0f);
        auto bottom = in;
        more_.setBounds(bottom.removeFromRight(kMoreW).withSizeKeepingCentre(kMoreW, kMoreW).toNearestInt());
        bottom.removeFromRight(8.0f);
        value_.setBounds(bottom.removeFromRight(62.0f).withSizeKeepingCentre(62.0f, 26.0f).toNearestInt());
        bottom.removeFromRight(8.0f);
        level_.setBounds(bottom.withSizeKeepingCentre(bottom.getWidth(), 20.0f).toNearestInt());
        return;
    }

    // wide grid: name 1fr | 48 | 48 | 206 | 32, padding 0 10 0 14 (friends 12), gap 10
    auto in = r.withTrimmedLeft(friendRow ? 12.0f : 14.0f).withTrimmedRight(10.0f);
    more_.setBounds(in.removeFromRight(kMoreW).withSizeKeepingCentre(kMoreW, kMoreW).toNearestInt());
    in.removeFromRight(kColGap);
    auto lvl = in.removeFromRight(kLevelW);
    value_.setBounds(lvl.removeFromRight(62.0f).withSizeKeepingCentre(62.0f, 26.0f).toNearestInt());
    lvl.removeFromRight(8.0f);
    level_.setBounds(lvl.withSizeKeepingCentre(lvl.getWidth(), 20.0f).toNearestInt());
    in.removeFromRight(kColGap);
    viewers_.setBounds(in.removeFromRight(kToggleW).withSizeKeepingCentre(kToggleW, 38.0f).toNearestInt());
    in.removeFromRight(kColGap);
    you_.setBounds(in.removeFromRight(kToggleW).withSizeKeepingCentre(kToggleW, 38.0f).toNearestInt());
    in.removeFromRight(kColGap);
    if (program) {
        auto col = in.withSizeKeepingCentre(in.getWidth(), 26.0f + 3.0f + 16.0f);
        const float pw = juce::jmin(col.getWidth() + 6.0f, float(picker_.idealWidth()));
        picker_.setBounds(col.removeFromTop(26.0f).withTrimmedLeft(-6.0f).withWidth(pw).toNearestInt());
        col.removeFromTop(3.0f);
        badges_ = col.removeFromRight(juce::jmin(badgesW + (d_.recording ? badgeWidth(tr(Str::RecordingBadge), BadgeStyle::Rec) + 6.0f : 0.0f), col.getWidth() * 0.6f));
        status_.setBounds(col.toNearestInt());
        return;
    }
    if (friendRow) {
        avatar_ = in.removeFromLeft(28.0f).withSizeKeepingCentre(28.0f, 28.0f);
        in.removeFromLeft(10.0f);
        auto col = in.withSizeKeepingCentre(in.getWidth(), 20.0f + 3.0f + 16.0f);
        name_.setBounds(col.removeFromTop(20.0f).toNearestInt());
        col.removeFromTop(3.0f);
        status_.setBounds(col.toNearestInt());
        return;
    }
    auto col = in.withSizeKeepingCentre(in.getWidth(), 26.0f + 8.0f + 3.0f);
    auto line = col.removeFromTop(26.0f);
    dot_ = line.removeFromLeft(8.0f).withSizeKeepingCentre(8.0f, 8.0f);
    line.removeFromLeft(8.0f);
    const float nameW = juce::jmin(float(name_.idealWidth()), juce::jmax(60.0f, line.getWidth() - badgesW));
    name_.setBounds(line.removeFromLeft(nameW).withTrimmedLeft(-7.0f).toNearestInt());
    badges_ = line.removeFromLeft(juce::jmin(line.getWidth(), badgesW)).withTrimmedLeft(2.0f);
    col.removeFromTop(8.0f);
    meter_.setBounds(col.withWidth(juce::jmin(120.0f, col.getWidth())).toNearestInt());
}

void ChannelRow::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(p.inset);
    g.fillRoundedRectangle(r, theme::radius::row);
    g.setColour(p.hairline1);
    g.drawRoundedRectangle(r, theme::radius::row, 1.0f);

    if (d_.kind == RowData::Kind::Track && !dot_.isEmpty()) {
        g.setColour(ed_.shadeColour(d_));
        g.fillEllipse(dot_);
    }
    if (d_.kind == RowData::Kind::Friend) drawAvatar(g, avatar_, d_.name, d_.active, p);

    // badges left to right: delay, solo, recording, inactive, warning
    auto b = badges_;
    auto badge = [&](const juce::String& t, BadgeStyle s) {
        const float w = badgeWidth(t, s);
        if (b.getWidth() < w) return;
        drawBadge(g, b.removeFromLeft(w).withSizeKeepingCentre(w, d_.kind == RowData::Kind::Program ? 18.0f : 20.0f), t, s, p);
        b.removeFromLeft(6.0f);
    };
    if (d_.kind == RowData::Kind::Track && d_.delayMs >= 0.5f) badge(valuetext::formatMs(d_.delayMs), BadgeStyle::Chip);
    if (d_.solo) badge(tr(Str::SoloBadge), BadgeStyle::Solid);
    if (d_.recording) badge(d_.recordSec > 0.5 ? tr(Str::RecordingBadge) + " " + clock(d_.recordSec) : tr(Str::RecordingBadge), BadgeStyle::Rec);
    if (!d_.active) badge(tr(Str::Inactive), BadgeStyle::Chip);
    if (d_.warnTip.isNotEmpty() && b.getWidth() >= 16.0f) icons::draw(g, icons::Icon::Warning, b.removeFromLeft(16.0f).withSizeKeepingCentre(15.0f, 15.0f), p.warn);
}

void ChannelRow::paintOverChildren(juce::Graphics& g) {
    const auto now = juce::Time::getMillisecondCounter();
    if (now >= flashUntil_) return;
    const auto& p = paletteOf(*this);
    const float a = float(flashUntil_ - now) / 1200.0f;
    g.setColour(p.ink.withAlpha(0.5f * a));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), theme::radius::row, 2.0f);
    juce::Component::SafePointer<ChannelRow> self(this);
    juce::Timer::callAfterDelay(40, [self] { if (self) self->repaint(); });
}

void ChannelRow::flashDawTrack() {
    if (d_.outSlot >= 0) ed_.goToTrack(d_.outSlot);
}

void ChannelRow::flash() {
    flashUntil_ = juce::Time::getMillisecondCounter() + 1200;
    repaint();
}

void ChannelRow::mouseUp(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) showMenu();
}

Menu ChannelRow::programMenu() {
    Menu m(270);
    m.header(tr(Str::AppPick));
    const auto cur = d_.app;
    const int index = d_.index;
    auto& proc = ed_.proc();
    bool listed = false;
    for (const auto& a : AppCapture::listAudioApps()) {   // refreshed every time the list opens
        const juce::String exe(a.exe);
        listed = listed || exe.equalsIgnoreCase(cur);
        m.check(HubEditor::programLabel(exe), exe.equalsIgnoreCase(cur), [&proc, index, exe] { proc.chooseSourceApp(index, exe); });
        m.last().icon = icons::Icon::Window;
    }
    if (!listed && cur.isNotEmpty() && cur != kSystemAudio && cur != kLinkIn && !cur.startsWith("http") && !FriendDirectory::isFriendApp(cur)) {
        m.check(HubEditor::programLabel(cur), true, [] {});
        m.last().icon = icons::Icon::Window;
    }
    m.check(tr(Str::AppSystem), cur == kSystemAudio, [&proc, index] { proc.chooseSourceApp(index, kSystemAudio); });
    m.last().icon = icons::Icon::Monitor;
    addFriendItems(m, cur, [&proc, index](const juce::String& app) { proc.chooseSourceApp(index, app); });
    if (cur.startsWith("http")) {
        m.check(HubEditor::programLabel(cur), true, [] {});
        m.last().icon = icons::Icon::Link;
    }
    m.separator();
    juce::Component::SafePointer<ChannelRow> self(this);
    m.item(tr(Str::AppLinkAsk), [self, index] {
        if (self == nullptr) return;
        auto& ed = self->ed_;
        ed.overlay().showPopover(std::make_unique<ReceiveLinkPanel>([&ed, index](const juce::String& url) {
            ed.proc().chooseSourceApp(index, url);
            ed.overlay().close();
            ed.toast(tr(Str::AppLinkReceiving));
            return true;
        }), self->picker_, true);
    });
    return m;
}

void ChannelRow::showMenu() {
    auto* o = Overlay::find(*this);
    if (o == nullptr) return;
    auto& proc = ed_.proc();
    const int slot = d_.slot, index = d_.index;
    juce::Component::SafePointer<ChannelRow> self(this);
    if (d_.kind == RowData::Kind::Program) {
        const bool on = d_.on, rec = d_.recording, follow = d_.follow;
        Menu m(250);
        m.header(HubEditor::programLabel(d_.app));
        m.toggle(tr(Str::CaptureProgram), on, [&proc, index, on] { proc.sendSource(index, ssbus::SourceParam::On, on ? 0.0f : 1.0f); });
        m.item(rec ? tr(Str::StopRecording) : tr(Str::PrintToFile), [&proc, index, rec] { proc.sendSource(index, ssbus::SourceParam::Record, rec ? 0.0f : 1.0f); });
        m.last().dot = Dot::Rec;
        m.toggle(tr(Str::RecordWithDaw), follow, [&proc, index, follow] { proc.sendSource(index, ssbus::SourceParam::FollowRecord, follow ? 0.0f : 1.0f); });
        m.item(tr(Str::OpenRecordings), [] {
            const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HEARASIDE").getChildFile("Recordings");
            dir.createDirectory();
            dir.revealToUser();
        });
        m.separator();
        m.item(tr(Str::FineSettingsEllipsis), [self] { if (self) self->ed_.showPage(HubEditor::Page::Programs); }, {}, true);
        more_.setOpen(true);
        o->showMenu(std::move(m), more_, false, [self] { if (self) self->more_.setOpen(false); });
        return;
    }
    if (d_.kind == RowData::Kind::Friend) {
        const uint32_t id = uint32_t(d_.index);
        const auto name = d_.name;
        const int ms = juce::roundToInt(d_.friendDelayMs);
        const int panI = juce::roundToInt(d_.pan * 100.0f);
        const juce::String panText = panI == 0 ? tr(Str::Center) : valuetext::formatPan(float(panI));
        Dot headDot;
        const auto status = statusText(headDot);
        const bool solo = d_.solo, inDaw = d_.inDaw;
        Menu m(236);
        m.header(name + juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")) + status);
        m.item(tr(Str::CopyFriendLink), [&proc, id, name, self] {
            const auto link = proc.friendLink(id);
            if (link.isEmpty() || self == nullptr) return;
            juce::SystemClipboard::copyTextToClipboard(link);
            self->ed_.toast(trf(Str::CopiedFriendLink, { name }));
        });
        if (!inDaw) {
            m.item(tr(Str::Pan), [self] { if (self) self->ed_.showPage(HubEditor::Page::Share); }, panText + juce::String(juce::CharPointer_UTF8(" \xe2\x80\xba")));
            m.item(tr(Str::MeasureDelayAgain), [&proc, id, name, self] {
                proc.remeasureFriend(id);
                if (self) self->ed_.toast(tr(Str::Measuring));
            }, d_.friendDelayMs >= 0.0f ? valuetext::formatMs(float(ms)) : juce::String());
            m.toggle(tr(Str::StreamSolo), solo, [&proc, id, solo] { proc.setFriendSolo(id, !solo); });
        }
        m.separator();
        if (inDaw) {
            m.item(tr(Str::GoToTrack), [self] { if (self) self->flashDawTrack(); });
            m.item(tr(Str::BringBackToHub), [&proc, id] { proc.bringBackFriend(id); });
        } else {
            m.item(tr(Str::MixInDawTrack), [self, id] { if (self) self->ed_.showMixInDaw(id, self->more_); }, juce::String(juce::CharPointer_UTF8("\xe2\x80\xba")), true);
        }
        m.item(tr(Str::RecordInDaw), [self, id] { if (self) self->ed_.showMixInDaw(id, self->more_); }, juce::String(juce::CharPointer_UTF8("\xe2\x80\xba")), true);
        m.item(tr(Str::RemoveFriend), [&proc, id, name, self] {
            proc.removeFriend(id);
            if (self) self->ed_.toast(trf(Str::RemovedFriendToast, { name }));
        });
        more_.setOpen(true);
        o->showMenu(std::move(m), more_, false, [self] { if (self) self->more_.setOpen(false); });
        return;
    }
    const bool solo = d_.solo;
    Menu m(236);
    m.header(d_.name);
    m.toggle(tr(Str::StreamSolo), solo, [&proc, slot, solo] { proc.send(slot, ParamId::StrSolo, solo ? 0.0f : 1.0f); });
    m.item(tr(Str::RenameDisplay), [self] { if (self) self->startRename(); }, tr(Str::RenameHint));
    m.separator();
    const int panI = juce::roundToInt(d_.pan * 100.0f);
    m.item(tr(Str::Pan), [self, slot] { if (self) self->ed_.showPage(HubEditor::Page::Track, slot); },
           panI == 0 ? tr(Str::Center) : valuetext::formatPan(float(panI)));
    m.item(tr(Str::Delay), [self, slot] { if (self) self->ed_.showPage(HubEditor::Page::Track, slot); }, valuetext::formatMs(d_.delayMs));
    const auto stemName = d_.stem < 0 ? tr(Str::StemNone)
                                      : (proc.stemName(d_.stem).isNotEmpty() ? proc.stemName(d_.stem) : trf(Str::StemN, { juce::String(d_.stem + 1) }));
    m.item(tr(Str::Stem), [self, slot] {
        if (self == nullptr) return;
        auto& pr = self->ed_.proc();
        Menu stems(236);
        stems.header(tr(Str::Stem));
        stems.check(tr(Str::StemNone), self->d_.stem < 0, [&pr, slot] { pr.send(slot, ParamId::StemIndex, -1.0f); });
        for (int i = 0; i < ssbus::kMaxStems; ++i)
            stems.check(pr.stemName(i).isNotEmpty() ? pr.stemName(i) : trf(Str::StemN, { juce::String(i + 1) }), self->d_.stem == i,
                        [&pr, slot, i] { pr.send(slot, ParamId::StemIndex, float(i)); });
        if (auto* ov = Overlay::find(*self)) ov->showMenu(std::move(stems), self->more_);
    }, stemName + juce::String(juce::CharPointer_UTF8(" \xe2\x80\xba")));
    m.separator();
    m.item(tr(Str::FineSettingsEllipsis), [self, slot] { if (self) self->ed_.showPage(HubEditor::Page::Track, slot); }, {}, true);
    more_.setOpen(true);
    o->showMenu(std::move(m), more_, false, [self] { if (self) self->more_.setOpen(false); });
}

// =============================================================================================
// HubEditor

HubEditor::HubEditor(HubProcessor& p)
    : EditorShell(p, 1040, 790, int(theme::layout::hubMinW), int(theme::layout::hubMinH), "hub"), proc_(p) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &obs_, &share_, &settingsButton_, &accountButton_, &compactMore_, &mute_, &back_, &manage_, &autoSync_,
                                                                        &levelMode_, &tabs_, &silentBanner_, &syncBanner_, &scroll_, &meterL_, &meterR_,
                                                                        &masterSlider_, &headphoneSlider_, &masterValue_, &headphoneValue_, &limiter_, &preview_ })
        content_.addChildComponent(c);
    scroll_.setContent(list_);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &manageTracksLink_, &manageProgramsLink_, &manageFriendsLink_, &startHide_ })
        list_.addChildComponent(c);
    manage_.setIcon(icons::Icon::Sliders);
    autoSync_.setIcon(icons::Icon::Refresh);
    levelHeadphones_ = settings_->flag("levelModeHeadphones");

    mute_.onClick = [this] { toggleParam(hubparam::Panic); };
    preview_.onClick = [this] { toggleParam(hubparam::Preview); };
    limiter_.onClick = [this] { toggleParam(hubparam::LimiterOn); };
    share_.onClick = [this] { showPage(Page::Share); };
    settingsButton_.onClick = [this] { showPage(Page::Settings); };
    accountButton_.onClick = [this] { showSettings(SettingsSection::Account); };
    compactMore_.onClick = [this] { showCompactMore(); };
    back_.onClick = [this] { showPage(Page::Main); };
    obs_.onClick = [this] { showObsPopover(); };
    manage_.onClick = [this] { showPage(Page::Tracks); };
    autoSync_.onClick = [this] { showPage(Page::Sync); };
    manageTracksLink_.onClick = [this] { showPage(Page::Programs); };
    manageProgramsLink_.onClick = [this] { showPage(Page::Programs); };
    manageFriendsLink_.onClick = [this] { showPage(Page::Share); };
    startHide_.onClick = [this] { settings_->setFlag("startHidden", true); updateStart(); };
    levelMode_.onChange = [this](int i) {
        levelHeadphones_ = i == 0;
        settings_->setFlag("levelModeHeadphones", levelHeadphones_);
        syncRows();
    };
    tabs_.onChange = [this](int i) { tab_ = Tab(i); layout(); content_.repaint(); };
    silentBanner_.setInterceptsMouseClicks(true, false);
    silentBanner_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    silentClick_.fn = [this] { showPage(Page::Setup); };   // "viewers hear nothing": how to fix it
    silentBanner_.addMouseListener(&silentClick_, false);

    masterLink_ = std::make_unique<DbSliderLink>(masterSlider_, *proc_.params().getParameter(hubparam::Master));
    headphoneLink_ = std::make_unique<DbSliderLink>(headphoneSlider_, *proc_.params().getParameter(hubparam::Headphones));
    masterValueLink_ = std::make_unique<ParamValueLink>(masterValue_, *proc_.params().getParameter(hubparam::Master));
    headphoneValueLink_ = std::make_unique<ParamValueLink>(headphoneValue_, *proc_.params().getParameter(hubparam::Headphones));

    procListener_.fn = [this] { layout(); content_.repaint(); if (pageView_) pageView_->refreshTexts(); };
    proc_.stateChanged.addChangeListener(&procListener_);
    refreshTexts();
    syncRows();
    updateStart();
    slowTick_ = 5;   // the first timer tick fills the OBS chip
    setContent(content_);
    timerCallback();
    startTimerHz(30);
}

HubEditor::~HubEditor() {
    proc_.stateChanged.removeChangeListener(&procListener_);
    pageView_.reset();
}

bool HubEditor::paramOn(const char* id) const { return proc_.params().getRawParameterValue(id)->load() > 0.5f; }

void HubEditor::toggleParam(const char* id) { proc_.setParam(id, paramOn(id) ? 0.0f : 1.0f); timerCallback(); }

juce::Colour HubEditor::shadeColour(const RowData& d) const {
    const auto& p = lnf_.pal();
    if (settings_->hostColours() && (d.colour >> 24) != 0) return juce::Colour(d.colour).withAlpha(1.0f);
    const juce::Colour shades[] = { p.trackShade1, p.trackShade2, p.trackShade3, p.trackShade4, p.trackShade5 };
    return shades[juce::jmax(0, d.shade) % 5];
}

void HubEditor::lookChanged() {
    backdrop_.invalidate();
    refreshTexts();
    for (auto* r : rows_) { r->refreshTexts(); r->resized(); }
    if (pageView_) pageView_->refreshTexts();
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
    mute_.setActive(paramOn(hubparam::Panic));
    preview_.setButtonText(paramOn(hubparam::Preview) ? tr(Str::PreviewOn) : tr(Str::PreviewOff));
    preview_.setTitle(preview_.getButtonText());
    limiter_.setTitle(tr(Str::Limiter));
    masterSlider_.setTitle(tr(Str::Master));
    headphoneSlider_.setTitle(tr(Str::HeadphoneMaster));
    headphoneSlider_.setTooltip(tr(Str::HeadphoneMasterTip));
    share_.setTooltip(tr(Str::ShareAudioTip));
    share_.setTitle(tr(Str::ShareAudioTip));
    settingsButton_.setTooltip(tr(Str::SettingsTip));
    settingsButton_.setTitle(tr(Str::SettingsTip));
    accountButton_.setTooltip(tr(Str::AccountTitle));
    accountButton_.setTitle(tr(Str::AccountTitle));
    compactMore_.setTitle(tr(Str::MoreMenuAria));
    compactMore_.setTooltip(tr(Str::More));
    back_.setTitle(tr(Str::BackTip));
    back_.setTooltip(tr(Str::BackTip));
    obs_.setTooltip(tr(Str::ObsChipTip));
    manage_.setButtonText(tr(Str::Manage));
    manage_.setTooltip(tr(Str::ManageTracksTip));
    autoSync_.setButtonText(tr(Str::SyncButton));
    autoSync_.setTooltip(tr(Str::SyncTip));
    manageTracksLink_.setButtonText(tr(Str::Manage));
    manageProgramsLink_.setButtonText(tr(Str::Manage));
    manageFriendsLink_.setButtonText(tr(Str::Manage));
    startHide_.setButtonText(tr(Str::StartHide));
    startHide_.setSmall(true);
    levelMode_.setSegments({ { tr(Str::LevelModeHeadphones), icons::Icon::Headphones }, { tr(Str::LevelModeViewers), icons::Icon::Broadcast } });
    levelMode_.setSelected(levelHeadphones_ ? 0 : 1);
    levelMode_.setTitle(tr(Str::LevelModeTip));
    levelMode_.setTooltip(tr(Str::LevelModeTip));
    silentBanner_.set(Banner::Style::Warning, icons::Icon::SpeakerOff, tr(Str::ViewersSilentBanner));
    step1Detail_ = step1For(currentDaw());
}

void HubEditor::renameRow(int slot, const juce::String& name) {
    if (slot < 0) return;
    const auto it = std::find_if(views_.begin(), views_.end(), [slot](const TrackView& v) { return v.slot == slot; });
    const auto trimmed = valuetext::truncateUtf8(name.trim(), ssbus::kNameBytes - 1);
    proc_.rename(slot, trimmed);
    if (trimmed.isNotEmpty()) {
        toast(trf(Str::RenamedToast, { trimmed }), 2600);
    } else {   // the Track goes back to the DAW's name: say which once it arrives
        pendingRevertSlot_ = slot;
        pendingRevertUntil_ = juce::Time::getMillisecondCounter() + 2000;
        pendingRevertFrom_ = it != views_.end() ? it->name : juce::String();
    }
}

void HubEditor::renameFriend(uint32_t id, const juce::String& name) {
    const auto trimmed = valuetext::truncateUtf8(name.trim(), ssbus::kNameBytes - 1);
    proc_.renameFriend(id, trimmed);
    toast(trf(Str::RenamedToast, { trimmed.isNotEmpty() ? trimmed : FriendRoom::defaultName(1) }), 2600);
}

void HubEditor::goToFriend(uint32_t id) {
    showPage(Page::Main);
    for (auto* r : rows_)
        if (r->data().kind == RowData::Kind::Friend && uint32_t(r->data().index) == id) {
            scroll_.scrollToShow(r->getBounds());
            r->flash();
        }
}

void HubEditor::goToTrack(int slot) {
    showPage(Page::Main);
    for (auto* r : rows_)
        if (r->data().kind == RowData::Kind::Track && r->data().slot == slot) {
            scroll_.scrollToShow(r->getBounds());
            r->flash();
        }
}

// ---------------------------------------------------------------------------------------------
void HubEditor::syncRows() {
    views_ = proc_.tracks();
    srcViews_ = proc_.sources();
    std::vector<RowData> data;
    int shade = 0;
    for (const auto& v : views_) {
        RowData d;
        d.kind = RowData::Kind::Track;
        d.slot = v.slot;
        d.name = v.name;
        d.colour = v.colourARGB;
        d.shade = shade++;
        d.you = v.mon;
        d.viewers = v.str;
        d.solo = v.solo;
        d.active = v.active;
        d.hpDb = v.trimDb;
        d.vwDb = v.gainDb;
        d.pan = v.pan;
        d.delayMs = v.delayMs;
        d.stem = v.stem;
        d.meter = v.peakIn;
        if (v.hubStatus & ssbus::kHubStatusRateMismatch) d.warnTip = tr(Str::RateMismatch);
        else if (v.hubStatus & ssbus::kHubStatusAhead) d.warnTip = tr(Str::AheadWarning);
        else if (v.bypassed) d.warnTip = tr(Str::BypassedTip);
        data.push_back(d);
    }
    for (const auto& s : srcViews_) {
        RowData d;
        d.kind = RowData::Kind::Program;
        d.slot = s.slot;
        d.index = s.index;
        d.name = programLabel(s.app);
        d.colour = s.colourARGB;
        d.shade = shade++;
        d.you = s.mon;
        d.viewers = s.str;
        d.active = s.active;
        d.linked = s.slot >= 0;
        d.hpDb = s.trimDb;
        d.vwDb = s.gainDb;
        d.delayMs = s.delayMs;
        d.meter = s.peak;
        d.app = s.app;
        d.on = s.on();
        d.recording = s.recording();
        d.follow = (s.flags & ssbus::kSrcFollowRec) != 0;
        d.capture = s.capture;
        d.recordSec = s.recordSec;
        d.latencyMs = s.latencyMs;
        d.takeShiftMs = s.takeShiftMs;
        if (s.flags & ssbus::kSrcDropped) d.warnTip = tr(Str::TakeDropped);
        data.push_back(d);
    }
    for (const auto& f : proc_.friends()) {
        RowData d;
        d.kind = RowData::Kind::Friend;
        d.index = int(f.id);
        d.name = f.name;
        d.shade = shade++;
        d.you = f.mon;
        d.viewers = f.str;
        d.solo = f.solo;
        d.active = f.live();
        d.hpDb = d.vwDb = f.volumeDb;
        d.pan = f.pan;
        d.meter = f.peak;
        d.friendState = f.state == FriendView::State::Live ? 1 : f.state == FriendView::State::Offline ? 2 : 0;
        d.friendDelayMs = f.delayMs;
        d.inDaw = f.inDaw;
        d.paired = f.outSlot >= 0;
        d.outSlot = f.outSlot;
        d.overLimit = f.overLimit;
        if (f.inDaw && f.feeder >= 0) d.dawTrack = proc_.feederTrackName(f.feeder);
        data.push_back(d);
    }
    bool structure = data.size() != size_t(rows_.size());
    for (size_t i = 0; !structure && i < data.size(); ++i)
        structure = rows_[int(i)]->data().kind != data[i].kind || rows_[int(i)]->data().slot != data[i].slot || rows_[int(i)]->data().index != data[i].index;
    if (structure) {
        rows_.clear();
        for (size_t i = 0; i < data.size(); ++i) list_.addAndMakeVisible(rows_.add(new ChannelRow(*this)));
    }
    for (size_t i = 0; i < data.size(); ++i) rows_[int(i)]->update(data[i]);
    if (structure) { updateStart(); layoutList(); }

    // "Back to the DAW's name" once the Track has published it
    if (pendingRevertSlot_ >= 0) {
        const auto it = std::find_if(views_.begin(), views_.end(), [this](const TrackView& v) { return v.slot == pendingRevertSlot_; });
        if (it != views_.end() && it->name != pendingRevertFrom_) {
            toast(trf(Str::RenameRevertToast, { it->name }), 2600);
            pendingRevertSlot_ = -1;
        } else if (juce::Time::getMillisecondCounter() > pendingRevertUntil_) {
            pendingRevertSlot_ = -1;
        }
    }
}

// Who hears what (solo only applies to the viewers).
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
    for (const auto& s : srcViews_)   // App Audio: while it is on and has a slot; the short name ("Whole computer")
        if (s.active && s.on() && s.slot >= 0) add(s.app == kSystemAudio ? tr(Str::WholeComputerShort) : programLabel(s.app), s.mon, s.str && !solo);
    for (const auto& f : proc_.friends())   // friends the Hub plays itself (one a DAW track carries is that track)
        if (f.live() && !f.inDaw) add(f.name, f.mon, f.str && (!solo || f.solo));
    return l;
}

// Getting started: tracks are there, OBS listens, the viewers' mix was heard once.
void HubEditor::updateStart() {
    const bool s1 = !views_.empty() || !srcViews_.empty(), s2 = proc_.obsConnected(), s3 = settings_->flag("triedPreview");
    const bool show = !s1 || (!(s2 && s3) && !settings_->flag("startHidden"));
    if (show == showStart_ && s1 == steps_[0] && s2 == steps_[1] && s3 == steps_[2]) return;
    showStart_ = show;
    steps_[0] = s1;
    steps_[1] = s2;
    steps_[2] = s3;
    layoutList();
    list_.repaint();
}

juce::String HubEditor::lufsText() const {
    if (lastPanic_) return tr(Str::LoudMuted);
    if (auto* bus = proc_.engine().bus()) {
        const float v = ssbus::bitsFloat(bus->streamHeader.loudnessSBits.load(std::memory_order_relaxed));
        return v < -70.0f ? tr(Str::SilentLufs) : minusText(juce::String(v, 1));
    }
    return tr(Str::SilentLufs);
}

juce::String HubEditor::obsLatencyText() const {
    const auto li = proc_.latency();
    return juce::String(li.total(), 1) + " ms";
}

void HubEditor::timerCallback() {
    const bool preview = paramOn(hubparam::Preview), panic = paramOn(hubparam::Panic);
    if (preview != lastPreview_ || panic != lastPanic_) {
        lastPreview_ = preview;
        lastPanic_ = panic;
        preview_.setActive(preview);
        refreshTexts();
        if (preview) { settings_->setFlag("triedPreview", true); updateStart(); }
        content_.repaint();
    }
    limiter_.setOn(paramOn(hubparam::LimiterOn), content_.isShowing());
    masterLink_->update();
    headphoneLink_->update();
    masterValueLink_->update();
    headphoneValueLink_->update();
    syncRows();
    {   // Line up changed the delay of the live: say so (the viewers' side changed, never silently)
        const uint32_t changes = proc_.lineUpChanges();
        if (changes != lastLineChanges_) {
            const int ms = juce::roundToInt(proc_.lineUpMs());
            if (lastLineChanges_ != ~0u) toast(ms <= 0 ? tr(Str::LineUpOffToast) : lastLineMs_ <= 0 ? trf(Str::LineUpOnToast, { juce::String(ms) }) : trf(Str::LineUpChangedToast, { juce::String(ms) }), 3600);
            lastLineChanges_ = changes;
            lastLineMs_ = ms;
            content_.repaint();
        }
    }
    if (auto* bus = proc_.engine().bus()) {
        const auto& sh = bus->streamHeader;
        meterL_.setLevel(meterPosition(ssbus::bitsFloat(sh.peakBits[0][0].load(std::memory_order_relaxed))));
        meterR_.setLevel(meterPosition(ssbus::bitsFloat(sh.peakBits[0][1].load(std::memory_order_relaxed))));
    }

    if (++slowTick_ % 3 == 0 && pageView_ != nullptr) pageView_->tick();
    if (slowTick_ % 6 == 0) {   // ~5 Hz: OBS chip, LUFS, summary, banners
        const bool obs = proc_.obsConnected();
        const bool compact = mode_ == Mode::Compact;
        const auto ms = obsLatencyText();
        obs_.set(compact ? tr(Str::ObsShort) : (obs ? tr(Str::ObsConnected) : tr(Str::ObsNotConnected)), obs ? Dot::Ok : Dot::Warn, ms, !compact);
        obs_.setTitle(trf(Str::ObsAriaCompact, { obs ? tr(Str::ObsConnected) : tr(Str::ObsNotConnected), juce::String(proc_.latency().total(), 1) }));
        // auto sync running: viewers hear nothing, say so wherever you are
        using P = HubProcessor::SyncPhase;
        const auto ph = proc_.autoSync().phase;
        const bool busy = ph == P::Countdown || ph == P::Reference || ph == P::Microphone;
        const bool silent = proc_.viewersSilent() && !panic;
        bool relayout = false;
        if (busy != syncBanner_.isVisible()) {
            syncBanner_.set(Banner::Style::Dark, icons::Icon::Headphones, tr(Str::SyncViewersUntil));
            syncBanner_.setVisible(busy && page_ == Page::Main);
            relayout = true;
        }
        if (silent != silentBanner_.isVisible() && page_ == Page::Main) { silentBanner_.setVisible(silent); relayout = true; }
        updateStart();
        const auto summary = joinDots(summaryLists().you) + "|" + joinDots(summaryLists().viewers) + lufsText();
        if (relayout) layout();
        if (summary != lastSummary_ || relayout) { lastSummary_ = summary; content_.repaint(); }
        else {
            content_.repaint(lufs_.toNearestInt().expanded(4));
            content_.repaint(bottomBar_.toNearestInt());
        }
        if (obs_.idealWidth() != lastObsWidth_) { lastObsWidth_ = obs_.idealWidth(); layoutHeader(); }
    }
}

// ---------------------------------------------------------------------------------------------
void HubEditor::showPage(Page p, int slot) {
    overlay().close();
    if (p == page_ && p != Page::Track && pageView_ != nullptr) return;
    pageView_.reset();
    page_ = p;
    if (p != Page::Main) {
        pageView_ = makeHubPage(*this, p, slot);
        content_.addAndMakeVisible(*pageView_);
    }
    layout();
    content_.repaint();
    if (p == Page::Main) share_.grabKeyboardFocus();
    else back_.grabKeyboardFocus();
}

void HubEditor::showSettings(SettingsSection s) {
    showPage(Page::Settings);
    if (auto* sp = dynamic_cast<HubSettingsPage*>(pageView_.get())) sp->select(int(s));
}

// "Mix in a DAW track…" and "Record in your DAW…": what to do in the DAW (S7 / S5)
namespace {
class MixInDawPanel : public juce::Component {
public:
    explicit MixInDawPanel(const juce::String& name) : name_(name) { setSize(330, 20 + 12 + 3 * 52 + 12 + 70); }
    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto r = getLocalBounds().toFloat();
        g.setColour(p.ink);
        g.setFont(uiFont(14.5f, Weight::SemiBold));
        g.drawText(tr(Str::MixFriendTitle), r.removeFromTop(20.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(12.0f);
        const juce::String steps[3] = { tr(Str::MixFriendStep1), trf(Str::MixFriendStep2, { name_ }), tr(Str::MixFriendStep3) };
        for (int i = 0; i < 3; ++i) {
            auto row = r.removeFromTop(52.0f);
            const auto dot = row.removeFromLeft(24.0f).withHeight(24.0f).reduced(1.0f);
            g.setColour(p.ink);
            g.fillEllipse(dot);
            g.setColour(p.onInk);
            g.setFont(uiFont(12.0f, Weight::SemiBold));
            g.drawText(juce::String(i + 1), dot, juce::Justification::centred, false);
            row.removeFromLeft(10.0f);
            drawWrapped(g, steps[i], uiFont(13.0f), p.ink2, row.withTrimmedTop(2.0f), 3.0f);
        }
        r.removeFromTop(12.0f);
        g.setColour(p.hairline2);
        g.fillRect(r.removeFromTop(1.0f));
        r.removeFromTop(10.0f);
        drawWrapped(g, tr(Str::RecordFriendHint), uiFont(12.0f), p.graphite, r, 4.0f);
    }
private:
    juce::String name_;
};
} // namespace

void HubEditor::showMixInDaw(uint32_t id, juce::Component& anchor) {
    juce::String name = "?";
    for (const auto& f : proc_.friends()) if (f.id == id) name = f.name;
    overlay().showPopover(std::make_unique<MixInDawPanel>(name), anchor, true);
}

void HubEditor::showObsPopover() {
    overlay().showPopover(std::make_unique<ObsPopover>(proc_), obs_, false);
}

void HubEditor::showCompactMore() {
    Menu m(250);
    m.item(tr(Str::ShareAudioTip), [this] { showPage(Page::Share); }, {}, true);
    m.item(tr(Str::SyncButton), [this] { showPage(Page::Sync); }, {}, true);
    m.item(tr(Str::TracksPageTitle), [this] { showPage(Page::Tracks); }, {}, true);
    m.item(tr(Str::ProgramsPageTitle), [this] { showPage(Page::Programs); }, {}, true);
    m.separator();
    m.item(tr(Str::AccountTitle), [this] { showSettings(SettingsSection::Account); }, {}, true);
    m.item(tr(Str::SettingsTip), [this] { showPage(Page::Settings); }, {}, true);
    overlay().showMenu(std::move(m), compactMore_);
}

// ---------------------------------------------------------------------------------------------
// layout

void HubEditor::updateMode(float w) {
    const float cb = theme::layout::compactBelow, wa = theme::layout::wideAbove, hy = theme::layout::hysteresis * 0.5f;
    if (w < cb - hy) mode_ = Mode::Compact;
    else if (w > wa + hy) mode_ = Mode::Wide;
    else if (w >= cb + hy && w <= wa - hy) mode_ = Mode::Regular;
    else if (w < cb + hy) { if (mode_ == Mode::Wide) mode_ = Mode::Regular; }   // between Compact and Regular: keep
    else if (mode_ == Mode::Compact) mode_ = Mode::Regular;                       // between Regular and Wide: keep
}

void HubEditor::layoutHeader() {
    const bool compact = mode_ == Mode::Compact;
    const bool sub = page_ != Page::Main;
    auto h = header_.withTrimmedLeft(sub ? 12.0f : (compact ? 16.0f : 24.0f)).withTrimmedRight(compact ? 8.0f : 12.0f);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &obs_, &share_, &settingsButton_, &accountButton_, &compactMore_, &back_ })
        c->setVisible(false);
    mute_.setVisible(true);
    mute_.setIconOnly(compact);
    const float mw = float(mute_.idealWidth());
    mute_.setBounds(h.removeFromRight(mw).withSizeKeepingCentre(mw, 40.0f).toNearestInt());
    divider_ = {};
    if (sub) {
        back_.setVisible(true);
        back_.setBounds(h.removeFromLeft(40.0f).withWidth(100.0f).withSizeKeepingCentre(100.0f, 40.0f).toNearestInt());
        back_.setBounds(back_.getBounds().withWidth(back_.idealWidth()));
        headerTitle_ = h.withLeft(float(back_.getRight()) + 14.0f).withTrimmedRight(14.0f);
        wordmark_ = {};
        return;
    }
    headerTitle_ = {};
    if (compact) {
        h.removeFromRight(6.0f);
        compactMore_.setVisible(true);
        compactMore_.setBounds(h.removeFromRight(40.0f).withSizeKeepingCentre(40.0f, 40.0f).toNearestInt());
        h.removeFromRight(6.0f);
        const float cw = juce::jmin(float(obs_.idealWidth()), h.getWidth() - 140.0f);
        obs_.setVisible(cw > 60.0f);
        obs_.setBounds(h.removeFromRight(cw).withSizeKeepingCentre(cw, 32.0f).toNearestInt());
        wordmark_ = h.withSizeKeepingCentre(h.getWidth(), 24.0f);
        return;
    }
    divider_ = h.removeFromRight(9.0f).withSizeKeepingCentre(1.0f, 24.0f);
    h.removeFromRight(4.0f);
    settingsButton_.setVisible(true);
    settingsButton_.setBounds(h.removeFromRight(40.0f).withSizeKeepingCentre(40.0f, 40.0f).toNearestInt());
    h.removeFromRight(8.0f);
    accountButton_.setVisible(true);
    accountButton_.setBounds(h.removeFromRight(40.0f).withSizeKeepingCentre(40.0f, 40.0f).toNearestInt());
    h.removeFromRight(8.0f);
    share_.setVisible(true);
    share_.setBounds(h.removeFromRight(40.0f).withSizeKeepingCentre(40.0f, 40.0f).toNearestInt());
    h.removeFromRight(8.0f);
    const float cw = juce::jmin(float(obs_.idealWidth()), h.getWidth() - 190.0f);
    obs_.setVisible(cw > 80.0f);
    obs_.setBounds(h.removeFromRight(cw).withSizeKeepingCentre(cw, 36.0f).toNearestInt());
    wordmark_ = h.withSizeKeepingCentre(h.getWidth(), 26.0f);
}

void HubEditor::layout() {
    const auto all = content_.getLocalBounds().toFloat();
    if (all.isEmpty()) return;
    updateMode(all.getWidth());
    const bool compact = mode_ == Mode::Compact;
    const float pad = compact ? theme::layout::compactPad : theme::space::hubPad;
    const float gap = compact ? 10.0f : theme::space::cardGap;
    auto r = all.reduced(pad);
    header_ = r.removeFromTop(compact ? kCompactHeaderH : kHeaderH);
    r.removeFromTop(gap);
    layoutHeader();

    // hide everything of the main screen, then show what this layout uses
    for (juce::Component* c : std::initializer_list<juce::Component*> { &manage_, &autoSync_, &levelMode_, &tabs_, &scroll_, &meterL_, &meterR_, &masterSlider_,
                                                                        &headphoneSlider_, &masterValue_, &headphoneValue_, &limiter_, &preview_ })
        c->setVisible(false);
    if (page_ != Page::Main) silentBanner_.setVisible(false);
    if (page_ != Page::Main) syncBanner_.setVisible(false);
    tracksCard_ = viewersCard_ = rightNow_ = bottomBar_ = messageCard_ = columns_ = tracksHead_ = slidersSetLabel_ = {};

    std::vector<GlassCard> cards { { header_, theme::radius::header } };
    if (page_ != Page::Main && pageView_ != nullptr) {
        pageView_->setBounds(r.toNearestInt());
        for (auto c : pageView_->cards()) cards.push_back({ c.bounds + pageView_->getPosition().toFloat(), c.radius });
        backdrop_.setCards(std::move(cards));
        return;
    }

    const bool showMain = proc_.connected() && proc_.engine().role() != ssengine::HubEngine::Role::Secondary;
    if (!showMain) {
        messageCard_ = r.withSizeKeepingCentre(juce::jmin(600.0f, r.getWidth()), juce::jmin(250.0f, r.getHeight()));
        cards.push_back({ messageCard_, theme::radius::card });
        backdrop_.setCards(std::move(cards));
        return;
    }
    layoutMain(r);
    cards.push_back({ tracksCard_, theme::radius::card });
    for (auto* c : { &viewersCard_, &rightNow_, &bottomBar_ })
        if (!c->isEmpty()) cards.push_back({ *c, c == &bottomBar_ ? 20.0f : theme::radius::card });
    backdrop_.setCards(std::move(cards));
}

void HubEditor::layoutMain(juce::Rectangle<float> r) {
    const bool compact = mode_ == Mode::Compact;
    if (compact) {
        tabs_.setVisible(true);
        tabs_.setTall(true);
        tabs_.setSegments({ { trf(Str::TabTracks, { juce::String(rows_.size()) }) }, { tr(Str::TabLevels) }, { tr(Str::TabSummary) } });
        tabs_.setSelected(int(tab_));
        tabs_.setBounds(r.removeFromTop(38.0f).toNearestInt());
        r.removeFromTop(10.0f);
        bottomBar_ = r.removeFromBottom(64.0f);
        r.removeFromBottom(10.0f);
        {
            auto b = bottomBar_.reduced(16.0f, 10.0f);
            const float pw = juce::jmin(b.getWidth() * 0.5f, textWidth(uiFont(13.5f, Weight::SemiBold), preview_.getButtonText()) + 18.0f + 8.0f + 36.0f);
            preview_.setVisible(true);
            preview_.setBounds(b.removeFromRight(pw).withSizeKeepingCentre(pw, 44.0f).toNearestInt());
        }
        if (tab_ == Tab::Levels) {
            viewersCard_ = r.withHeight(juce::jmin(r.getHeight(), 380.0f));
            layoutViewersCard(viewersCard_, false);
            preview_.setVisible(true);
            return;
        }
        if (tab_ == Tab::Summary) {
            rightNow_ = r.withHeight(juce::jmin(r.getHeight(), 320.0f));
            return;
        }
        tracksCard_ = r;
        auto c = r.reduced(12.0f);
        auto line = c.removeFromTop(38.0f);
        slidersSetLabel_ = line.removeFromLeft(textWidth(uiFont(12.0f), tr(Str::SlidersSet)) + 14.0f);
        levelMode_.setVisible(true);
        levelMode_.setTall(true);
        levelMode_.setBounds(line.toNearestInt());
        c.removeFromTop(10.0f);
        for (auto* bn : { &silentBanner_, &syncBanner_ }) {
            if (!bn->isVisible()) continue;
            bn->setBounds(c.removeFromTop(float(bn->idealHeight(int(c.getWidth())))).toNearestInt());
            c.removeFromTop(10.0f);
        }
        narrowRows_ = true;
        scroll_.setVisible(true);
        scroll_.setBounds(c.withTrimmedRight(-10.0f).toNearestInt());
        scroll_.setFadeColour(lnf_.pal().paper.overlaidWith(lnf_.pal().glass));
        layoutList();
        return;
    }

    const float rightW = mode_ == Mode::Wide ? kRightWideW : kRightW;
    auto right = r.removeFromRight(rightW);
    r.removeFromRight(theme::space::cardGap);
    tracksCard_ = r;

    // right column: as much of the two cards as the height allows (prompt 3.3)
    const float fullViewers = 377.0f, condViewers = 300.0f, rightNowFull = 260.0f, rightNowCounts = 150.0f;
    const float h = right.getHeight();
    condensed_ = h < fullViewers + theme::space::cardGap + rightNowCounts;
    rightNowLists_ = h >= fullViewers + theme::space::cardGap + rightNowFull;
    rightNowShown_ = h >= (condensed_ ? condViewers : fullViewers) + theme::space::cardGap + 120.0f;
    viewersCard_ = right.removeFromTop(juce::jmin(h, condensed_ ? condViewers : fullViewers));
    layoutViewersCard(viewersCard_, condensed_);
    right.removeFromTop(theme::space::cardGap);
    if (rightNowShown_) rightNow_ = right;

    // tracks card: head, column header, the list
    auto c = tracksCard_.reduced(20.0f);
    tracksHead_ = c.removeFromTop(42.0f);
    {
        auto head = tracksHead_;
        manage_.setVisible(true);
        autoSync_.setVisible(true);
        const float sw = float(autoSync_.idealWidth()), mw = float(manage_.idealWidth());
        autoSync_.setBounds(head.removeFromRight(sw).withHeight(36.0f).toNearestInt());
        head.removeFromRight(12.0f);
        manage_.setBounds(head.removeFromRight(mw).withHeight(36.0f).toNearestInt());
    }
    c.removeFromTop(14.0f);
    for (auto* bn : { &silentBanner_, &syncBanner_ }) {
        if (!bn->isVisible()) continue;
        bn->setBounds(c.removeFromTop(float(bn->idealHeight(int(c.getWidth())))).toNearestInt());
        c.removeFromTop(12.0f);
    }
    const float rowW = c.getWidth();
    narrowRows_ = rowW < 520.0f;
    levelMode_.setVisible(true);
    levelMode_.setTall(false);
    if (narrowRows_) {   // two-line rows: the level switch gets a line of its own
        auto line = c.removeFromTop(38.0f);
        slidersSetLabel_ = line.removeFromLeft(textWidth(uiFont(12.0f), tr(Str::SlidersSet)) + 14.0f);
        levelMode_.setBounds(line.toNearestInt());
    } else {
        columns_ = c.removeFromTop(32.0f);
        auto col = columns_.withTrimmedLeft(14.0f).withTrimmedRight(10.0f);
        col.removeFromRight(kMoreW + kColGap);
        levelMode_.setBounds(col.removeFromRight(kLevelW).toNearestInt());
    }
    c.removeFromTop(14.0f);
    scroll_.setVisible(true);
    scroll_.setBounds(c.withTrimmedRight(-14.0f).toNearestInt());
    scroll_.setFadeColour(lnf_.pal().paper.overlaidWith(lnf_.pal().glass));
    layoutList();
}

void HubEditor::layoutViewersCard(juce::Rectangle<float> card, bool condensed) {
    for (juce::Component* c : std::initializer_list<juce::Component*> { &meterL_, &meterR_, &masterSlider_, &masterValue_, &limiter_, &preview_ })
        c->setVisible(true);
    headphoneSlider_.setVisible(!condensed);
    headphoneValue_.setVisible(!condensed);
    auto c = card.reduced(20.0f);
    auto top = c.removeFromTop(40.0f);
    lufs_ = top.removeFromRight(110.0f);
    lufsLabel_ = top;
    c.removeFromTop(14.0f);
    auto meters = c.removeFromTop(19.0f);
    meterLabels_ = meters.removeFromLeft(16.0f);
    meterL_.setBounds(meters.removeFromTop(7.0f).toNearestInt());
    meters.removeFromTop(5.0f);
    meterR_.setBounds(meters.removeFromTop(7.0f).toNearestInt());
    c.removeFromTop(14.0f);
    masterLabel_ = c.removeFromTop(26.0f);
    masterValue_.setBounds(masterLabel_.withLeft(masterLabel_.getRight() - 62.0f).withTrimmedRight(-6.0f).withRight(masterLabel_.getRight()).toNearestInt());
    c.removeFromTop(6.0f);
    masterSlider_.setBounds(c.removeFromTop(20.0f).toNearestInt());
    c.removeFromTop(14.0f);
    if (!condensed) {
        hpLabel_ = c.removeFromTop(26.0f);
        headphoneValue_.setBounds(hpLabel_.withLeft(hpLabel_.getRight() - 62.0f).toNearestInt());
        c.removeFromTop(6.0f);
        headphoneSlider_.setBounds(c.removeFromTop(20.0f).toNearestInt());
        c.removeFromTop(14.0f);
    } else {
        hpLabel_ = {};
    }
    sep_ = c.removeFromTop(1.0f);
    c.removeFromTop(14.0f);
    auto lim = c.removeFromTop(condensed ? 26.0f : 36.0f);
    limiter_.setBounds(lim.removeFromRight(46.0f).withSizeKeepingCentre(46.0f, 26.0f).toNearestInt());
    limiterText_ = lim;
    c.removeFromTop(14.0f);
    preview_.setBounds(c.removeFromTop(48.0f).toNearestInt());
}

void HubEditor::layoutList() {
    const int w = juce::jmax(1, scroll_.contentWidth());
    const int gap = int(theme::space::rowGap);
    int y = 0;
    startBox_ = programsHead_ = friendsHead_ = {};
    startHide_.setVisible(false);
    if (showStart_) {
        const float sw = float(w) - 36.0f - 34.0f;
        float h = 14.0f + 24.0f + 6.0f;
        for (int i = 0; i < 3; ++i) {
            auto t = tr(i == 0 ? Str::StartStep1 : i == 1 ? Str::StartStep2 : Str::StartStep3);
            if (i == 0 && !steps_[0] && step1Detail_.isNotEmpty()) t << "\n" << step1Detail_;
            h += juce::jmax(24.0f, wrappedHeight(uiFont(13.0f), t, sw, 2.0f)) + 8.0f;
        }
        h += 6.0f;
        startBox_ = { 0.0f, 0.0f, float(w), h };
        if (steps_[0]) {
            const int bw = startHide_.idealWidth();
            startHide_.setBounds(w - bw - 12, 12, bw, 30);
            startHide_.setVisible(true);
        }
        y = juce::roundToInt(h) + kGroupGap;
    }
    const int rh = ChannelRow::heightFor(narrowRows_);
    bool programsStarted = false, friendsStarted = false;
    for (auto* row : rows_) {
        row->setNarrow(narrowRows_);
        if (row->data().kind == RowData::Kind::Friend && !friendsStarted) {
            friendsStarted = true;
            y += kGroupGap - gap + 4;
            friendsHead_ = { 0.0f, float(y), float(w), 22.0f };
            y += 22 + kGroupGap;
        }
        if (row->data().kind == RowData::Kind::Program && !programsStarted) {
            programsStarted = true;
            y += kGroupGap - gap + 4;
            programsHead_ = { 0.0f, float(y), float(w), 22.0f };
            y += 22 + kGroupGap;
        }
        row->setBounds(0, y, w, rh);
        y += rh + gap;
    }
    if (!rows_.isEmpty()) y -= gap;
    // Friends: with no friend yet the head still shows, so people find it
    if (!friendsStarted) {
        y += kGroupGap + 4;
        friendsHead_ = { 0.0f, float(y), float(w), 22.0f };
        y += 22;
    }
    manageProgramsLink_.setVisible(programsStarted);
    manageTracksLink_.setVisible(false);
    manageFriendsLink_.setVisible(true);
    auto placeLink = [&](LinkButton& l, juce::Rectangle<float> head) {
        const int lw = l.idealWidth();
        l.setBounds(juce::Rectangle<float>(head.getRight() - float(lw) - 2.0f, head.getY(), float(lw) + 2.0f, head.getHeight()).toNearestInt());
    };
    if (programsStarted) placeLink(manageProgramsLink_, programsHead_);
    placeLink(manageFriendsLink_, friendsHead_);
    list_.setSize(w, juce::jmax(1, y + 4));
    list_.repaint();
}

// ---------------------------------------------------------------------------------------------
// painting

void HubEditor::paintHeader(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    if (!wordmark_.isEmpty()) {
        const bool compact = mode_ == Mode::Compact;
        drawWordmark(g, wordmark_, "HUB", compact ? 14.0f : 16.0f, p.ink);
    }
    if (!divider_.isEmpty()) {
        g.setColour(p.hairline3);
        g.fillRect(divider_);
    }
    if (!headerTitle_.isEmpty()) {
        Str title = Str::Settings, cap = Str::SettingsSubtitle;
        switch (page_) {
            case Page::Share:    title = Str::SharePageTitle; cap = Str::SharePageSubtitle; break;
            case Page::Settings: title = Str::Settings; cap = Str::SettingsSubtitle; break;
            case Page::Track:    title = Str::TrackFineTitle; cap = Str::TrackFineSubtitle; break;
            case Page::Sync:     title = Str::SyncButton; cap = Str::SyncSubtitle; break;
            case Page::Tracks:   title = Str::TracksPageTitle; cap = Str::TracksPageSubtitle; break;
            case Page::Programs: title = Str::ProgramsPageTitle; cap = Str::ProgramsPageSubtitle; break;
            case Page::Setup:    title = Str::SetupTitle; cap = Str::SetupCheckCap; break;
            case Page::Main:     break;
        }
        auto t = headerTitle_.withSizeKeepingCentre(headerTitle_.getWidth(), 20.0f + 2.0f + 16.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(17.0f, Weight::SemiBold));
        g.drawText(ellipsize(uiFont(17.0f, Weight::SemiBold), tr(title), t.getWidth()), t.removeFromTop(20.0f), juce::Justification::centredLeft, false);
        t.removeFromTop(2.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(ellipsize(uiFont(12.0f), tr(cap), t.getWidth()), t, juce::Justification::centredLeft, false);
    }
}

void HubEditor::paintViewersCard(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    auto t = lufsLabel_;
    g.setColour(p.ink);
    g.setFont(uiFont(17.0f, Weight::SemiBold));
    g.drawText(ellipsize(uiFont(17.0f, Weight::SemiBold), tr(Str::StreamTitle), t.getWidth()), t.removeFromTop(22.0f), juce::Justification::centredLeft, false);
    t.removeFromTop(3.0f);
    g.setColour(p.graphite);
    g.setFont(uiFont(12.0f));
    g.drawText(tr(Str::Loudness), t, juce::Justification::topLeft, true);
    g.setColour(lastPanic_ ? p.danger : p.ink);
    g.setFont(uiFont(lastPanic_ || lufsText().containsAnyOf("0123456789") ? 28.0f : 22.0f, Weight::SemiBold));
    g.drawFittedText(lufsText(), lufs_.withHeight(34.0f).toNearestInt(), juce::Justification::centredRight, 1, 0.7f);
    auto ml = meterLabels_;
    g.setColour(p.graphite);
    g.setFont(uiFont(10.0f));
    g.drawText("L", ml.removeFromTop(7.0f).expanded(0, 3), juce::Justification::centredLeft, false);
    ml.removeFromTop(5.0f);
    g.drawText("R", ml.expanded(0, 3), juce::Justification::centredLeft, false);
    g.setColour(p.ink);
    g.setFont(uiFont(12.5f));
    g.drawText(tr(Str::Master), masterLabel_, juce::Justification::centredLeft, true);
    if (!hpLabel_.isEmpty()) g.drawText(tr(Str::HeadphoneMaster), hpLabel_, juce::Justification::centredLeft, true);
    g.setColour(p.hairline2);
    g.fillRect(sep_);
    auto lt = limiterText_;
    const bool cond = lt.getHeight() < 30.0f;
    auto block = lt.withSizeKeepingCentre(lt.getWidth(), cond ? 20.0f : 36.0f);
    g.setColour(p.ink);
    g.setFont(uiFont(13.5f, Weight::Medium));
    g.drawText(tr(Str::Limiter), block.removeFromTop(19.0f), juce::Justification::centredLeft, true);
    if (!cond) {
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::LimiterCaption), block.withTrimmedTop(1.0f), juce::Justification::centredLeft, true);
    }
}

void HubEditor::paintRightNow(juce::Graphics& g, juce::Rectangle<float> area, bool lists) {
    const auto& p = lnf_.pal();
    const auto l = summaryLists();
    auto c = area;
    g.setColour(p.ink);
    g.setFont(uiFont(17.0f, Weight::SemiBold));
    g.drawText(tr(Str::SummaryTitle), c.removeFromTop(22.0f), juce::Justification::centredLeft, true);
    c.removeFromTop(12.0f);
    auto block = [&](icons::Icon icon, Str label, const juce::String& count, juce::Colour countColour, const juce::String& text) {
        auto head = c.removeFromTop(17.0f);
        icons::draw(g, icon, head.removeFromLeft(14.0f).withSizeKeepingCentre(14.0f, 14.0f), p.graphite);
        head.removeFromLeft(8.0f);
        g.setColour(countColour);
        g.setFont(uiFont(12.0f, Weight::SemiBold));
        const float cw = textWidth(uiFont(12.0f, Weight::SemiBold), count) + 2.0f;
        g.drawText(count, head.removeFromRight(cw), juce::Justification::centredRight, false);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(label), head, juce::Justification::centredLeft, true);
        if (lists) {
            c.removeFromTop(4.0f);
            const auto f = uiFont(13.0f);
            const float th = juce::jmin(2.0f * f.getHeight() + 6.5f, wrappedHeight(f, text, c.getWidth(), 6.5f));
            drawWrapped(g, text, f, p.ink2, c.removeFromTop(th + 1.0f), 6.5f, juce::Justification::left, 2);
        }
    };
    const auto none = tr(Str::None);
    block(icons::Icon::Headphones, Str::SummaryYou, lastPreview_ ? tr(Str::PreviewOff) : trf(Str::TracksCount, { juce::String(l.you.size()) }), p.ink,
          lastPreview_ ? tr(Str::SummaryPreviewing) : (l.you.isEmpty() ? none : joinDots(l.you)));
    c.removeFromTop(12.0f);
    g.setColour(p.hairline2);
    g.fillRect(c.removeFromTop(1.0f));
    c.removeFromTop(12.0f);
    block(icons::Icon::Broadcast, Str::SummaryViewers, lastPanic_ ? tr(Str::LoudMuted) : trf(Str::TracksCount, { juce::String(l.viewers.size()) }),
          lastPanic_ ? p.danger : p.ink, lastPanic_ ? tr(Str::SummaryPanic) : (l.viewers.isEmpty() ? none : joinDots(l.viewers)));
    if (!lastPanic_ && !lastPreview_ && !l.onlyViewers.isEmpty()) {
        const auto lf = uiFont(12.0f), bf = uiFont(12.0f, Weight::SemiBold);
        const auto label = tr(Str::OnlyViewersLabel) + " ", names = joinDots(l.onlyViewers);
        const float bh = 18.0f + 18.0f;
        auto box = juce::Rectangle<float>(area.getX(), juce::jmax(c.getY() + 10.0f, area.getBottom() - bh), area.getWidth(), bh);
        g.setColour(p.inset);
        g.fillRoundedRectangle(box, 12.0f);
        auto in = box.reduced(12.0f, 9.0f);
        g.setColour(p.ink2);
        g.setFont(lf);
        const float lw = textWidth(lf, label);
        g.drawText(label, in.removeFromLeft(juce::jmin(lw, in.getWidth() * 0.6f)), juce::Justification::centredLeft, true);
        g.setColour(p.ink);
        g.setFont(bf);
        g.drawText(ellipsize(bf, names, in.getWidth()), in, juce::Justification::centredLeft, false);
    }
}

void HubEditor::paintContent(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    backdrop_.paint(g, content_.getLocalBounds(), p, settings_->glassAlpha(), lnf_.isDark());
    paintHeader(g);
    if (page_ != Page::Main) return;

    const bool secondary = proc_.engine().role() == ssengine::HubEngine::Role::Secondary;
    if (!proc_.connected() || secondary) {
        auto c = messageCard_.reduced(juce::jmin(28.0f, messageCard_.getWidth() * 0.06f));
        g.setColour(p.ink);
        g.setFont(uiFont(20.0f, Weight::SemiBold));
        const juce::String title = secondary ? tr(Str::SecondHubTitle) : tr(Str::HubBusError).upToFirstOccurrenceOf(" ", false, false);
        const juce::String body = secondary ? tr(Str::SecondHubBody) : tr(Str::HubBusError);
        g.drawFittedText(title, c.removeFromTop(30.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        c.removeFromTop(10.0f);
        drawWrapped(g, body, uiFont(14.0f), p.graphite, c, 4.0f);
        return;
    }

    if (!tracksHead_.isEmpty()) {
        auto t = tracksHead_.withRight(float(manage_.getX()) - 12.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(20.0f, Weight::SemiBold));
        g.drawText(tr(Str::TracksTitle), t.removeFromTop(25.0f), juce::Justification::centredLeft, true);
        t.removeFromTop(4.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f));
        g.drawText(ellipsize(uiFont(12.5f), tr(Str::TracksSubtitle), t.getWidth()), t, juce::Justification::topLeft, false);
    }
    if (!columns_.isEmpty()) {
        auto col = columns_.withTrimmedLeft(14.0f).withTrimmedRight(10.0f);
        col.removeFromRight(kMoreW + kColGap + kLevelW + kColGap);
        auto viewersCol = col.removeFromRight(kToggleW);
        col.removeFromRight(kColGap);
        auto youCol = col.removeFromRight(kToggleW);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::ColTrack), col, juce::Justification::centredLeft, true);
        g.drawFittedText(tr(Str::ColYou), youCol.expanded(6.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
        g.drawFittedText(tr(Str::ColViewers), viewersCol.expanded(6.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }
    if (!slidersSetLabel_.isEmpty()) {
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::SlidersSet), slidersSetLabel_.withTrimmedLeft(2.0f), juce::Justification::centredLeft, true);
    }
    if (!viewersCard_.isEmpty()) paintViewersCard(g);
    if (!rightNow_.isEmpty()) paintRightNow(g, rightNow_.reduced(20.0f, 18.0f), rightNowLists_ || mode_ == Mode::Compact);
    if (!bottomBar_.isEmpty()) {
        auto b = bottomBar_.reduced(16.0f, 10.0f).withRight(float(preview_.getX()) - 10.0f);
        const auto l = summaryLists();
        auto block = b.withSizeKeepingCentre(b.getWidth(), 16.0f + 3.0f + 18.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f));
        g.drawText(tr(Str::SummaryTitle), block.removeFromTop(16.0f), juce::Justification::centredLeft, true);
        block.removeFromTop(3.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::Medium));
        const auto line = lastPanic_ ? trf(Str::CompactSummaryMuted, { juce::String(l.you.size()) })
                                     : trf(Str::CompactSummary, { juce::String(l.you.size()), juce::String(l.viewers.size()), lufsText() });
        g.drawText(ellipsize(uiFont(13.0f, Weight::Medium), line, block.getWidth()), block, juce::Justification::centredLeft, false);
    }
}

void HubEditor::paintList(juce::Graphics& g) {
    const auto& p = lnf_.pal();
    if (!startBox_.isEmpty()) {   // getting started: three steps that tick themselves
        drawInset(g, startBox_, theme::radius::row, p, true);
        auto r = startBox_.reduced(18.0f, 14.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText(tr(Str::StartTitle), r.removeFromTop(24.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(6.0f);
        for (int i = 0; i < 3; ++i) {
            auto text = tr(i == 0 ? Str::StartStep1 : i == 1 ? Str::StartStep2 : Str::StartStep3);
            if (i == 0 && !steps_[0] && step1Detail_.isNotEmpty()) text << "\n" << step1Detail_;
            const float h = juce::jmax(24.0f, wrappedHeight(uiFont(13.0f), text, r.getWidth() - 34.0f, 2.0f));
            auto line = r.removeFromTop(h);
            const auto dot = line.removeFromLeft(24.0f).withHeight(24.0f).reduced(1.0f);
            if (steps_[i]) {
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
            drawWrapped(g, text, uiFont(13.0f), steps_[i] ? p.graphite : p.ink, line.withTrimmedTop(juce::jmax(0.0f, (24.0f - uiFont(13.0f).getHeight()) * 0.5f)), 2.0f);
            r.removeFromTop(8.0f);
        }
    }
    auto groupHead = [&](juce::Rectangle<float> head, Str title, const juce::String& caption, juce::Component* link) {
        if (head.isEmpty()) return;
        auto h = head.withTrimmedLeft(2.0f);
        if (link != nullptr && link->isVisible()) h.setRight(float(link->getX()) - 10.0f);
        const auto tf = uiFont(15.0f, Weight::SemiBold);
        const float tw = textWidth(tf, tr(title)) + 2.0f;
        g.setColour(p.ink);
        g.setFont(tf);
        g.drawText(tr(title), h.removeFromLeft(tw), juce::Justification::centredLeft, false);
        h.removeFromLeft(10.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(ellipsize(uiFont(12.0f), caption, h.getWidth()), h, juce::Justification::centredLeft, false);
    };
    groupHead(programsHead_, Str::SourcesTitle, tr(Str::ProgramCaption), &manageProgramsLink_);
    const int nFriends = proc_.friendCount();
    juce::String friendsCaption = tr(Str::FriendsEmptyCaption);
    if (nFriends > 0) {
        const int d = juce::roundToInt(proc_.lineUpMs());
        friendsCaption = proc_.lineUp() && d > 0 ? trf(Str::FriendsCaption, { juce::String(nFriends), juce::String(d) })
                                                 : trf(Str::FriendsCaptionNoLineUp, { juce::String(nFriends) });
    }
    groupHead(friendsHead_, Str::FriendsTitle, friendsCaption, &manageFriendsLink_);
}

} // namespace hearaside
