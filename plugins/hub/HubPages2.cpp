// Hub sub-pages, part 2: Share and friends, Track fine settings, Manage tracks, Manage program audio.
#include "HubPagesShared.h"
#include "Host.h"
#include "app/AppCapture.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <map>

namespace hearaside {

using ssbus::ParamId;

namespace {

constexpr const char* kSystemAudio = "*system*";
constexpr const char* kLinkIn = "*link*";

juce::String clock(double seconds) {
    const int s = juce::roundToInt(seconds);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

juce::Font monoFont(float px) { return juce::Font(juce::FontOptions("Consolas", px, juce::Font::plain)); }

// A read-only link field (mono font) with the text selectable for copying.
struct LinkField : TextField {
    LinkField() {
        setReadOnly(true);
        setCaretVisible(false);
        setFont(monoFont(12.0f));
    }
    void setLink(const juce::String& l) { if (l != getText()) setText(l, false); }
};

// Track that a value was changed from the UI recently, so polling doesn't fight the user.
struct Touch {
    juce::uint32 at = 0;
    void now() { at = juce::Time::getMillisecondCounter(); }
    bool idle() const { return juce::Time::getMillisecondCounter() - at > 450 && !juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown(); }
};

const TrackView* findTrack(const std::vector<TrackView>& v, int slot) {
    for (const auto& t : v) if (t.slot == slot) return &t;
    return nullptr;
}

void setupMsSlider(juce::Slider& s) {
    s.setRange(0.0, 200.0, 1.0);
    s.setDoubleClickReturnValue(true, 0.0);
    s.setVelocityModeParameters(0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    s.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    s.setWantsKeyboardFocus(true);
    setFocusDrawnBySelf(s);
}

// =============================================================================================
// Share and friends (prompt 3.4 "Share"): the friends room on the left (one card per friend, Line up,
// Spread out, Add friend), the listen link / connection / receive cards on the right.

// One friend in the room: avatar + name + status, delay + microphone level, Copy link, remove;
// below: Volume (you and viewers) and Pan (viewers).
class FriendCard : public juce::Component {
public:
    static constexpr int kHeight = 112;

    FriendCard(HubEditor& e, uint32_t id) : ed_(e), id_(id) {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &name_, &mic_, &copy_, &remove_, &vol_, &volValue_, &pan_, &panValue_ })
            addAndMakeVisible(c);
        copy_.setSmall(true);
        remove_.setSize(32, 32);
        name_.onRename = [this](const juce::String& n) { ed_.renameFriend(id_, n); };
        copy_.onClick = [this] {
            const auto link = ed_.proc().friendLink(id_);
            if (link.isEmpty()) return;
            juce::SystemClipboard::copyTextToClipboard(link);
            copy_.setButtonText(tr(Str::Copied));
            juce::Component::SafePointer<FriendCard> sp(this);
            juce::Timer::callAfterDelay(1400, [sp] { if (sp) sp->copy_.setButtonText(tr(Str::CopyLink)); });
            ed_.toast(trf(Str::CopiedFriendLink, { v_.name }));
        };
        remove_.onClick = [this] {
            const auto name = v_.name;
            ed_.proc().removeFriend(id_);
            ed_.toast(trf(Str::RemovedFriendToast, { name }));
        };
        vol_.onValueChange = [this] { if (!updating_) send(LevelSlider::sliderToDb(vol_.getValue())); };
        volValue_.onCommit = [this](float db) { send(db <= valuetext::kFloorDb + 1.0e-3f ? -60.0f : db); };
        pan_.onChange = [this](float v) { if (!updating_) { touch_.now(); ed_.proc().setFriendPan(id_, v / 100.0f); panValue_.setValue(v); } };
        panValue_.onCommit = [this](float v) { touch_.now(); ed_.proc().setFriendPan(id_, v / 100.0f); pan_.setPan(v); };
        refreshTexts();
    }

    uint32_t id() const noexcept { return id_; }
    void startRename() { name_.startEditing(); }
    void refreshTexts() {
        copy_.setButtonText(tr(Str::CopyLink));
        name_.setTooltip(tr(Str::DoubleClickToRename));
        vol_.setTooltip(tr(Str::DoubleClickToReset));
        volValue_.setTooltip(tr(Str::DoubleClickToType));
        panValue_.setTooltip(tr(Str::PanTypeTip));
        pan_.setTooltip(tr(Str::PanTypeTip));
        remove_.setTitle(trf(Str::RemoveNamed, { v_.name }));
        remove_.setTooltip(trf(Str::RemoveNamed, { v_.name }));
        vol_.setTitle(tr(Str::Volume) + " " + v_.name);
        repaint();
    }

    void update(const FriendView& v) {
        const bool nameChanged = v.name != v_.name, dawChanged = v.inDaw != v_.inDaw;
        v_ = v;
        name_.setName(v.name, {});
        mic_.setLevel(v.state == FriendView::State::Live ? meterPosition(v.peak) : 0.0f);
        const juce::ScopedValueSetter<bool> svs(updating_, true);
        if (touch_.idle() && !vol_.isMouseButtonDown()) {
            vol_.setValue(LevelSlider::dbToSlider(v.volumeDb), juce::dontSendNotification);
            if (!volValue_.isEditing()) volValue_.setValue(juce::jmax(valuetext::kFloorDb, v.volumeDb));
            pan_.setPan(v.pan * 100.0f);
            if (!panValue_.isEditing()) panValue_.setValue(v.pan * 100.0f);
        }
        const bool dim = v.inDaw;   // a DAW track carries them: level and pan are set there
        vol_.setDim(dim);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &vol_, &volValue_, &pan_, &panValue_ }) c->setEnabled(!dim);
        if (nameChanged) refreshTexts();
        if (dawChanged) resized();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto r = getLocalBounds().toFloat();
        drawInset(g, r, 16.0f, p);
        drawAvatar(g, avatar_, v_.name, v_.live(), p);
        // status line
        Dot dot = Dot::Warn;
        juce::String text;
        if (v_.state == FriendView::State::Live) {
            dot = v_.overLimit ? Dot::Warn : Dot::Ok;
            if (v_.inDaw) text = v_.outSlot >= 0 ? trf(Str::SingingInDaw, { ed_.proc().feederTrackName(v_.feeder) }) : tr(Str::InDawNotHeadphones);
            else if (v_.overLimit) text = trf(Str::FriendOverLimit, { juce::String(ed_.proc().lineUpLimitMs()) });
            else text = tr(Str::FriendSinging);
        } else if (v_.state == FriendView::State::Offline) {
            dot = Dot::Muted;
            text = tr(Str::FriendOffline);
        } else {
            text = tr(Str::FriendNotOpened);
        }
        drawStatusDot(g, { statusLine_.getX() + 4.0f, statusLine_.getCentreY() }, dot, p);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(ellipsize(uiFont(12.0f), text, statusLine_.getWidth() - 14.0f), statusLine_.withTrimmedLeft(14.0f), juce::Justification::centredLeft, false);
        // delay
        g.setColour(p.ink2);
        g.setFont(uiFont(12.0f, Weight::Medium));
        g.drawText(v_.live() && v_.delayMs >= 0.0f ? trf(Str::DelayMsLabel, { juce::String(juce::roundToInt(v_.delayMs)) })
                                                   : juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")),
                   delay_, juce::Justification::centredRight, false);
        // the second row
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        if (v_.inDaw) {
            drawWrapped(g, trf(Str::SetLevelOnTrack, { v_.name, ed_.proc().feederTrackName(v_.feeder) }), uiFont(12.0f), p.graphite, row2_, 3.0f, juce::Justification::centredLeft, 2);
        } else {
            g.drawText(tr(Str::Volume), volLabel_, juce::Justification::centredLeft, false);
            g.drawText(tr(Str::PanWord), panLabel_, juce::Justification::centredLeft, false);
        }
    }

    void resized() override {
        auto r = getLocalBounds().toFloat().reduced(16.0f, 14.0f);
        auto top = r.removeFromTop(40.0f);
        remove_.setBounds(top.removeFromRight(32.0f).withSizeKeepingCentre(32.0f, 32.0f).toNearestInt());
        top.removeFromRight(6.0f);
        copy_.setBounds(top.removeFromRight(juce::jmax(92.0f, float(copy_.idealWidth()))).withSizeKeepingCentre(juce::jmax(92.0f, float(copy_.idealWidth())), 30.0f).toNearestInt());
        top.removeFromRight(12.0f);
        auto meterCol = top.removeFromRight(juce::jmin(130.0f, top.getWidth() * 0.4f));
        delay_ = meterCol.removeFromTop(20.0f);
        meterCol.removeFromTop(4.0f);
        mic_.setBounds(meterCol.removeFromTop(6.0f).toNearestInt());
        top.removeFromRight(12.0f);
        avatar_ = top.removeFromLeft(32.0f).withSizeKeepingCentre(32.0f, 32.0f);
        top.removeFromLeft(12.0f);
        auto col = top.withSizeKeepingCentre(top.getWidth(), 20.0f + 3.0f + 16.0f);
        name_.setBounds(col.removeFromTop(20.0f).toNearestInt());
        col.removeFromTop(3.0f);
        statusLine_ = col;
        r.removeFromTop(14.0f);
        row2_ = r.removeFromTop(28.0f).withTrimmedLeft(44.0f);
        auto cols = row2_;
        const float half = (cols.getWidth() - 24.0f) * 0.5f;
        auto left = cols.removeFromLeft(half);
        cols.removeFromLeft(24.0f);
        auto right = cols;
        const bool dawRow = v_.inDaw;
        for (juce::Component* c : std::initializer_list<juce::Component*> { &vol_, &volValue_, &pan_, &panValue_ }) c->setVisible(!dawRow);
        if (dawRow) return;
        volLabel_ = left.removeFromLeft(54.0f);
        volValue_.setBounds(left.removeFromRight(62.0f).withSizeKeepingCentre(62.0f, 26.0f).toNearestInt());
        left.removeFromRight(6.0f);
        vol_.setBounds(left.withSizeKeepingCentre(left.getWidth(), 20.0f).toNearestInt());
        panLabel_ = right.removeFromLeft(34.0f);
        panValue_.setBounds(right.removeFromRight(56.0f).withSizeKeepingCentre(56.0f, 26.0f).toNearestInt());
        right.removeFromRight(6.0f);
        pan_.setBounds(right.withSizeKeepingCentre(right.getWidth(), 20.0f).toNearestInt());
    }

private:
    void send(float db) {
        touch_.now();
        ed_.proc().setFriendVolumeDb(id_, db);
        volValue_.setValue(juce::jmax(valuetext::kFloorDb, db));
    }

    HubEditor& ed_;
    uint32_t id_;
    FriendView v_;
    InlineName name_ { 14.5f, Weight::SemiBold };
    MeterBar mic_ { 6.0f, true };
    GhostButton copy_ { {}, GhostButton::Style::Ghost };
    IconButton remove_ { icons::Icon::Close };
    LevelSlider vol_;
    EditableValue volValue_ { EditableValue::Kind::Db, -30.0f, 6.0f };
    PanSlider pan_ { false };
    EditableValue panValue_ { EditableValue::Kind::Pan, -100.0f, 100.0f };
    juce::Rectangle<float> avatar_, statusLine_, delay_, row2_, volLabel_, panLabel_;
    Touch touch_;
    bool updating_ = false;
};

class SharePage : public HubPage {
public:
    explicit SharePage(HubEditor& e) : HubPage(e) {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &spread_, &add_, &lineSwitch_, &measure_, &scroll_, &listenSwitch_, &listenField_, &open_, &copy_,
                                                                            &install_, &backupField_, &backupCopy_, &receiveField_, &connect_ })
            addAndMakeVisible(c);
        scroll_.setContent(body_);
        spread_.setIcon(icons::Icon::Spread);
        add_.setIcon(icons::Icon::Plus);
        for (auto* b : { &open_, &copy_, &install_, &backupCopy_, &measure_ }) b->setSmall(true);
        add_.onClick = [this] {
            if (ed.proc().roomFull()) { ed.toast(tr(Str::FriendFullToast)); return; }
            const auto id = ed.proc().addFriend(tr(Str::FriendDefaultName).replace("%d", juce::String(ed.proc().friendCount() + 1)));
            if (id == 0) return;
            tick();
            // a new friend is in the name box at once: type who it is for
            juce::Component::SafePointer<SharePage> sp(this);
            juce::Timer::callAfterDelay(60, [sp, id] { if (sp) sp->startRename(id); });
        };
        spread_.onClick = [this] { ed.proc().spreadFriends(); tick(); };
        lineSwitch_.onClick = [this] { ed.proc().setLineUp(!ed.proc().lineUp()); tick(); };
        measure_.onClick = [this] {
            for (const auto& f : ed.proc().friends()) ed.proc().remeasureFriend(f.id);
            measuringUntil_ = juce::Time::getMillisecondCounter() + 4000;
            tick();
        };
        listenSwitch_.onClick = [this] { ed.proc().setSharing(!ed.proc().sharing()); tick(); };
        copy_.onClick = [this] { copyText(listenField_.getText(), copy_); };
        backupCopy_.onClick = [this] { copyText(backupField_.getText(), backupCopy_); };
        install_.onClick = [this] { copyText("winget install --id Cloudflare.cloudflared", install_); };
        // on this computer: localhost (no tunnel round trip, and a secure context for the player)
        open_.onClick = [this] { if (ed.proc().share().running()) juce::URL(ed.proc().share().localListenUrl()).launchInDefaultBrowser(); };
        receiveField_.setPlaceholder(juce::String(juce::CharPointer_UTF8("https://\xe2\x80\xa6/l/\xe2\x80\xa6")));
        receiveField_.setFont(monoFont(12.0f));
        receiveField_.onReturnKey = [this] { receive(); };
        connect_.onClick = [this] { receive(); };
        refreshTexts();
        tick();
    }

    std::vector<GlassCard> cards() const override {
        std::vector<GlassCard> c;
        c.push_back({ left_, theme::radius::card });
        c.push_back({ listenCard_, theme::radius::card });
        if (!connCard_.isEmpty()) c.push_back({ connCard_, theme::radius::card });
        if (!recvCard_.isEmpty()) c.push_back({ recvCard_, theme::radius::card });
        return c;
    }

    void refreshTexts() override {
        spread_.setButtonText(tr(Str::SpreadOut));
        spread_.setTooltip(tr(Str::SpreadOutTip));
        add_.setButtonText(tr(Str::AddFriend));
        measure_.setButtonText(tr(Str::MeasureAgain));
        open_.setButtonText(tr(Str::Open));
        copy_.setButtonText(tr(Str::Copy));
        install_.setButtonText(tr(Str::CopyInstall));
        backupCopy_.setButtonText(tr(Str::Copy));
        connect_.setButtonText(tr(Str::Connect));
        listenSwitch_.setTitle(tr(Str::ShareListen));
        listenField_.setTitle(tr(Str::ShareListen));
        lineSwitch_.setTitle(tr(Str::LineUpFriends));
        receiveField_.setTitle(tr(Str::LinkToReceive));
        for (auto* c : cardViews_) c->refreshTexts();
        repaint();
    }

    void tick() override {
        auto& proc = ed.proc();
        const auto& sh = proc.share();
        const bool on = proc.sharing();
        const bool live = on && sh.running();
        const bool permanent = proc.permanentLinksSet();
        listenSwitch_.setOn(on, isShowing());
        // the permanent link can be copied any time (send it once); the others only exist while sharing
        listenField_.setLink(permanent ? proc.permanentUrl(true) : live ? sh.listenUrl() : juce::String());
        backupField_.setLink(permanent && live ? sh.listenUrl() : juce::String());
        copy_.setEnabled(listenField_.getText().isNotEmpty());
        open_.setEnabled(live);
        const bool backup = backupField_.getText().isNotEmpty() && proc.directory().state() != ShareDirectory::State::Online;
        const bool missing = (on || proc.friendCount() > 0) && (sh.tunnel() == ShareServer::Tunnel::Missing || sh.tunnel() == ShareServer::Tunnel::Failed);
        bool relayout = false;
        if (backup != backupField_.isVisible() || missing != install_.isVisible()) {
            backupField_.setVisible(backup);
            backupCopy_.setVisible(backup);
            install_.setVisible(missing);
            relayout = true;
        }
        // the friends: one card each, in the room's order
        const auto views = proc.friends();
        bool structure = views.size() != cardViews_.size();
        for (size_t i = 0; !structure && i < views.size(); ++i) structure = cardViews_[i]->id() != views[i].id;
        if (structure) {
            cardViews_.clear();
            body_.removeAllChildren();
            cards_.clear();
            for (const auto& v : views) {
                auto* c = cards_.add(new FriendCard(ed, v.id));
                body_.addAndMakeVisible(c);
                cardViews_.push_back(c);
            }
            relayout = true;
        }
        for (size_t i = 0; i < views.size(); ++i) cardViews_[i]->update(views[i]);
        add_.setEnabled(!proc.roomFull());
        spread_.setEnabled(views.size() >= 2);
        lineSwitch_.setOn(proc.lineUp(), isShowing());
        const bool measuring = juce::Time::getMillisecondCounter() < measuringUntil_;
        measure_.setButtonText(measuring ? tr(Str::Measuring) : tr(Str::MeasureAgain));
        measure_.setEnabled(!views.empty() && !measuring);
        if (relayout) resized();
        repaint();
    }

    void resized() override {
        auto r = getLocalBounds().toFloat();
        const bool narrow = r.getWidth() < 760.0f;
        juce::Rectangle<float> aside;
        if (narrow) {   // Compact: the listen link on top, the friends below
            aside = r.removeFromTop(170.0f);
            r.removeFromTop(16.0f);
            left_ = r;
        } else {
            aside = r.removeFromRight(320.0f);
            r.removeFromRight(16.0f);
            left_ = r;
        }
        // listen link card
        listenCard_ = aside.removeFromTop(170.0f);
        {
            auto c = listenCard_.reduced(20.0f);
            auto head = c.removeFromTop(36.0f);
            listenSwitch_.setBounds(head.removeFromRight(46.0f).withSizeKeepingCentre(46.0f, 26.0f).toNearestInt());
            listenHead_ = head;
            c.removeFromTop(12.0f);
            listenField_.setBounds(c.removeFromTop(38.0f).toNearestInt());
            c.removeFromTop(10.0f);
            auto row = c.removeFromTop(32.0f);
            copy_.setBounds(row.removeFromRight(86.0f).toNearestInt());
            row.removeFromRight(8.0f);
            open_.setBounds(row.removeFromRight(float(open_.idealWidth())).toNearestInt());
            listeners_ = row;
        }
        connCard_ = recvCard_ = {};
        for (juce::Component* comp : std::initializer_list<juce::Component*> { &install_, &backupField_, &backupCopy_, &receiveField_, &connect_ }) comp->setVisible(false);
        if (!narrow) {
            aside.removeFromTop(16.0f);
            // connection card (grows with the backup link / install command)
            install_.setVisible(installWanted());
            backupField_.setVisible(backupWanted());
            backupCopy_.setVisible(backupWanted());
            const float connH = 20.0f + 20.0f + 10.0f + 76.0f + (install_.isVisible() ? 40.0f : 0.0f) + (backupField_.isVisible() ? 18.0f + 6.0f + 38.0f + 10.0f : 0.0f) + 20.0f;
            connCard_ = aside.removeFromTop(connH);
            {
                auto c = connCard_.reduced(20.0f);
                c.removeFromTop(20.0f + 10.0f);
                connBox_ = c.removeFromTop(76.0f);
                if (install_.isVisible()) { c.removeFromTop(8.0f); install_.setBounds(c.removeFromTop(32.0f).withWidth(float(install_.idealWidth())).toNearestInt()); }
                if (backupField_.isVisible()) {
                    c.removeFromTop(10.0f);
                    backupLabel_ = c.removeFromTop(18.0f);
                    c.removeFromTop(6.0f);
                    auto row = c.removeFromTop(38.0f);
                    backupCopy_.setBounds(row.removeFromRight(70.0f).withSizeKeepingCentre(70.0f, 32.0f).toNearestInt());
                    row.removeFromRight(8.0f);
                    backupField_.setBounds(row.toNearestInt());
                }
            }
            aside.removeFromTop(16.0f);
            recvCard_ = aside;
            {
                auto c = recvCard_.reduced(20.0f);
                recvText_ = c.removeFromTop(20.0f + 10.0f + 36.0f);
                c.removeFromTop(10.0f);
                receiveField_.setBounds(c.removeFromTop(38.0f).toNearestInt());
                receiveField_.setVisible(true);
                c.removeFromTop(10.0f);
                connect_.setBounds(c.removeFromTop(38.0f).toNearestInt());
                connect_.setVisible(true);
                recvError_ = c.removeFromTop(24.0f);
            }
        }
        // left card: Friends
        auto c = left_.withTrimmedLeft(24.0f).withTrimmedRight(24.0f).withTrimmedTop(22.0f).withTrimmedBottom(16.0f);
        auto head = c.removeFromTop(38.0f);
        add_.setBounds(head.removeFromRight(float(add_.idealWidth())).toNearestInt());
        head.removeFromRight(12.0f);
        spread_.setBounds(head.removeFromRight(float(spread_.idealWidth())).toNearestInt());
        friendsHead_ = head;
        c.removeFromTop(14.0f);
        if (!narrow) {
            footer_ = c.removeFromBottom(12.0f + wrappedHeight(uiFont(12.0f), tr(Str::FriendsFooter), c.getWidth() - 26.0f, 6.0f));
            c.removeFromBottom(12.0f);
        } else {
            footer_ = {};
        }
        // Line up: icon + title + switch + Measure again, and the caption
        const float capH = wrappedHeight(uiFont(12.0f), lineCaption(), c.getWidth() - 32.0f, 4.0f);
        lineBox_ = c.removeFromTop(16.0f + 34.0f + 6.0f + capH + 14.0f);
        {
            auto b = lineBox_.reduced(16.0f, 16.0f);
            auto row = b.removeFromTop(34.0f);
            lineSwitch_.setBounds(row.removeFromRight(46.0f).withSizeKeepingCentre(46.0f, 26.0f).toNearestInt());
            row.removeFromRight(10.0f);
            const float mw = juce::jmax(96.0f, float(measure_.idealWidth()));
            measure_.setBounds(row.removeFromRight(mw).withSizeKeepingCentre(mw, 30.0f).toNearestInt());
            row.removeFromRight(10.0f);
            lineTitle_ = row;
            b.removeFromTop(6.0f);
            lineCap_ = b;
        }
        c.removeFromTop(12.0f);
        // the friends (or the empty state)
        list_ = c;
        const bool any = !cardViews_.empty();
        scroll_.setVisible(any);
        if (any) {
            scroll_.setBounds(list_.withTrimmedRight(-14.0f).toNearestInt());
            const int w = juce::jmax(1, scroll_.contentWidth());
            int y = 0;
            for (auto* card : cardViews_) { card->setBounds(0, y, w, FriendCard::kHeight); y += FriendCard::kHeight + 8; }
            body_.setSize(w, juce::jmax(1, y - 8));
        }
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto& proc = ed.proc();
        const auto& sh = proc.share();
        const bool on = proc.sharing(), live = on && sh.running();
        if (!left_.isEmpty()) {
            auto h = friendsHead_;
            g.setColour(p.ink);
            g.setFont(uiFont(20.0f, Weight::SemiBold));
            const float tw = textWidth(uiFont(20.0f, Weight::SemiBold), tr(Str::FriendsTitle)) + 2.0f;
            g.drawText(tr(Str::FriendsTitle), h.removeFromLeft(tw), juce::Justification::centredLeft, false);
            h.removeFromLeft(12.0f);
            const auto count = trf(Str::FriendsCount, { juce::String(proc.friendCount()) });
            const float cw = textWidth(uiFont(12.0f, Weight::Medium), count) + 20.0f;
            drawBadge(g, h.removeFromLeft(cw).withSizeKeepingCentre(cw, 24.0f), count, BadgeStyle::Chip, p);

            // Line up
            drawInset(g, lineBox_, 16.0f, p);
            auto t = lineTitle_;
            icons::draw(g, icons::Icon::Refresh, t.removeFromLeft(20.0f).withSizeKeepingCentre(18.0f, 18.0f), p.ink);
            t.removeFromLeft(10.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(14.5f, Weight::SemiBold));
            g.drawText(ellipsize(uiFont(14.5f, Weight::SemiBold), tr(Str::LineUpFriends), t.getWidth()), t, juce::Justification::centredLeft, false);
            drawWrapped(g, lineCaption(), uiFont(12.0f), p.graphite, lineCap_, 4.0f);

            if (cardViews_.empty()) {   // empty state of the room
                auto e = list_.withSizeKeepingCentre(juce::jmin(list_.getWidth(), 420.0f), 70.0f);
                if (list_.getHeight() > 90.0f) {
                    g.setColour(p.ink);
                    g.setFont(uiFont(15.0f, Weight::SemiBold));
                    g.drawText(tr(Str::NoFriends), e.removeFromTop(22.0f), juce::Justification::centred, true);
                    e.removeFromTop(6.0f);
                    drawWrapped(g, tr(Str::NoFriendsCaption), uiFont(12.5f), p.graphite, e, 5.0f, juce::Justification::centred);
                }
            }
            // footer
            if (!footer_.isEmpty()) {
                g.setColour(p.hairline2);
                g.fillRect(footer_.withHeight(1.0f));
                auto f = footer_.withTrimmedTop(12.0f);
                icons::draw(g, icons::Icon::Info, f.removeFromLeft(16.0f).withHeight(16.0f).withY(f.getY() + 1.0f), p.graphite);
                f.removeFromLeft(10.0f);
                drawWrapped(g, tr(Str::FriendsFooter), uiFont(12.0f), p.graphite, f, 6.0f);
            }
        }
        // listen link
        {
            auto h = listenHead_;
            g.setColour(p.ink);
            g.setFont(uiFont(15.0f, Weight::SemiBold));
            g.drawText(tr(Str::ShareListen), h.removeFromTop(19.0f), juce::Justification::centredLeft, true);
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f));
            g.drawText(tr(Str::ListenLinkCap), h.withTrimmedTop(2.0f), juce::Justification::centredLeft, true);
            if (live) {
                drawStatusDot(g, { listeners_.getX() + 4.0f, listeners_.getCentreY() }, sh.listeners() > 0 ? Dot::Ok : Dot::Muted, p);
                g.setColour(p.ink2);
                g.setFont(uiFont(12.0f));
                g.drawText(trf(Str::ListeningCount, { juce::String(sh.listeners()) }), listeners_.withTrimmedLeft(14.0f), juce::Justification::centredLeft, true);
            }
        }
        // connection
        if (!connCard_.isEmpty()) {
            auto c = connCard_.reduced(20.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(15.0f, Weight::SemiBold));
            g.drawText(tr(Str::ConnectionTitle), c.removeFromTop(20.0f), juce::Justification::centredLeft, true);
            Dot dot = Dot::Muted;
            juce::String title = tr(Str::SharingOff), cap = tr(Str::SharingOffCap);
            if (on || proc.friendCount() > 0) {   // friends use the same tunnel as the listen link
                switch (sh.tunnel()) {
                    case ShareServer::Tunnel::Ready:
                        dot = Dot::Ok; title = tr(Str::LinksAnywhere);
                        cap = proc.permanentLinksSet() ? tr(Str::LinksAnywhereCap) : tr(Str::LinksChangeCap);
                        if (proc.permanentLinksSet() && proc.directory().state() == ShareDirectory::State::Unreachable) { dot = Dot::Warn; cap = tr(Str::ShareDirOffline); }
                        break;
                    case ShareServer::Tunnel::Starting: dot = Dot::Muted; title = tr(Str::LinksStarting); cap = {}; break;
                    case ShareServer::Tunnel::Missing:
                    case ShareServer::Tunnel::Failed:   dot = Dot::Warn; title = tr(Str::WifiOnly); cap = tr(Str::WifiOnlyCap); break;
                    case ShareServer::Tunnel::Off:      break;
                }
            }
            g.setColour(p.inset);
            g.fillRoundedRectangle(connBox_, 12.0f);
            auto in = connBox_.reduced(12.0f);
            drawStatusDot(g, { in.getX() + 4.0f, in.getY() + 9.0f }, dot, p, 8.0f);
            in.removeFromLeft(18.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(13.0f, Weight::Medium));
            g.drawText(title, in.removeFromTop(18.0f), juce::Justification::centredLeft, true);
            in.removeFromTop(2.0f);
            drawWrapped(g, cap, uiFont(12.0f), p.graphite, in, 6.0f, juce::Justification::left, 2);
            if (backupField_.isVisible()) {
                g.setColour(p.graphite);
                g.setFont(uiFont(12.0f));
                g.drawText(tr(Str::ShareBackup), backupLabel_, juce::Justification::centredLeft, true);
            }
        }
        // receive
        if (!recvCard_.isEmpty()) {
            auto t = recvText_;
            g.setColour(p.ink);
            g.setFont(uiFont(15.0f, Weight::SemiBold));
            g.drawText(tr(Str::ReceiveTitle), t.removeFromTop(20.0f), juce::Justification::centredLeft, true);
            t.removeFromTop(10.0f);
            drawWrapped(g, tr(Str::ReceiveCap), uiFont(12.0f), p.graphite, t, 6.0f);
            if (recvError_.getHeight() > 4.0f && error_.isNotEmpty()) {
                g.setColour(p.danger);
                g.setFont(uiFont(12.0f));
                g.drawText(error_, recvError_, juce::Justification::centredLeft, true);
            }
        }
    }

    // ui-snapshot / the Add friend button: put a friend's name box in edit mode
    void startRename(uint32_t id) {
        for (auto* c : cardViews_) if (c->id() == id) c->startRename();
    }

private:
    bool installWanted() const {
        const auto& sh = ed.proc().share();
        return (ed.proc().sharing() || ed.proc().friendCount() > 0) && (sh.tunnel() == ShareServer::Tunnel::Missing || sh.tunnel() == ShareServer::Tunnel::Failed);
    }
    bool backupWanted() const {
        auto& proc = ed.proc();
        return backupField_.getText().isNotEmpty() && proc.directory().state() != ShareDirectory::State::Online;
    }
    juce::String lineCaption() const {
        auto& proc = ed.proc();
        if (!proc.lineUp()) return tr(Str::LineUpOff);
        const int d = juce::roundToInt(proc.lineUpMs());
        if (d <= 0) return tr(Str::LineUpWaiting);
        juce::String slowest = "?";
        const int id = proc.lineUpSlowestFriend();
        for (const auto& f : proc.friends()) if (int(f.id) == id) slowest = f.name;
        return trf(Str::LineUpCaption, { juce::String(d), slowest });
    }
    void copyText(const juce::String& text, GhostButton& b) {
        if (text.isEmpty()) return;
        juce::SystemClipboard::copyTextToClipboard(text);
        b.setButtonText(tr(Str::Copied));
        juce::Component::SafePointer<GhostButton> sp(&b);
        juce::Timer::callAfterDelay(1400, [sp, label = (&b == &install_ ? tr(Str::CopyInstall) : tr(Str::Copy))] { if (sp) sp->setButtonText(label); });
    }
    void receive() {
        const auto url = receiveField_.getText().trim();
        const auto& sources = ed.sourceViews();
        if (!links::isListen(url)) error_ = tr(Str::AppLinkBad);
        else if (sources.empty()) error_ = tr(Str::ReceiveNoApp);
        else {
            ed.proc().chooseSourceApp(sources.front().index, url);
            error_ = {};
            ed.toast(tr(Str::AppLinkReceiving));
        }
        repaint();
    }

    GhostButton spread_ { {}, GhostButton::Style::Ghost }, add_ { {}, GhostButton::Style::Solid };
    LinkField listenField_, backupField_;
    GhostButton open_, copy_ { {}, GhostButton::Style::Solid }, install_, backupCopy_, measure_ { {}, GhostButton::Style::Ghost };
    Switch listenSwitch_, lineSwitch_;
    ScrollArea scroll_ { 14 };
    juce::Component body_;
    juce::OwnedArray<FriendCard> cards_;
    std::vector<FriendCard*> cardViews_;
    TextField receiveField_;
    GhostButton connect_ { {}, GhostButton::Style::Solid };
    juce::String error_;
    juce::uint32 measuringUntil_ = 0;
    juce::Rectangle<float> left_, listenCard_, connCard_, recvCard_, friendsHead_, lineBox_, lineTitle_, lineCap_, list_, footer_;
    juce::Rectangle<float> listenHead_, listeners_, connBox_, backupLabel_, recvText_, recvError_;
};

// =============================================================================================
// Track fine settings (prompt 3.4 "Track")

class TrackPage : public HubPage {
public:
    TrackPage(HubEditor& e, int slot) : HubPage(e), slot_(slot) {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &prev_, &next_, &picker_, &you_, &viewers_, &scroll_, &hpField_, &name_, &status_ })
            addAndMakeVisible(c);
        scroll_.setContent(body_);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &vwField_, &panField_, &delayField_, &measure_, &solo_, &stem_ })
            body_.addAndMakeVisible(c);
        you_.setLabelShown(true);
        viewers_.setLabelShown(true);
        if (slot_ < 0 && !ed.trackViews().empty()) slot_ = ed.trackViews().front().slot;
        prev_.onClick = [this] { step(-1); };
        next_.onClick = [this] { step(1); };
        picker_.onChange = [this](int i) { if (i >= 0 && i < int(ed.trackViews().size())) { slot_ = ed.trackViews()[size_t(i)].slot; tick(); } };
        you_.onClick = [this] { if (auto* v = view()) send(ParamId::Mon, v->mon ? 0.0f : 1.0f); };
        viewers_.onClick = [this] { if (auto* v = view()) send(ParamId::Str, v->str ? 0.0f : 1.0f); };
        vw_.onValueChange = [this] { if (!updating_) send(ParamId::StrGainDb, LevelSlider::sliderToDb(vw_.getValue())); };
        hp_.onValueChange = [this] { if (!updating_) send(ParamId::MonTrimDb, LevelSlider::sliderToDb(hp_.getValue())); };
        vwValue_.onCommit = [this](float db) { send(ParamId::StrGainDb, db <= valuetext::kFloorDb + 1.0e-3f ? -60.0f : db); };
        hpValue_.onCommit = [this](float db) { send(ParamId::MonTrimDb, db <= valuetext::kFloorDb + 1.0e-3f ? -60.0f : db); };
        pan_.onChange = [this](float v) { if (!updating_) { send(ParamId::StrPan, v / 100.0f); panValue_.setValue(v); } };
        panValue_.onCommit = [this](float v) { send(ParamId::StrPan, v / 100.0f); };
        setupMsSlider(delay_);
        delay_.onValueChange = [this] { if (!updating_) send(ParamId::StrDelayMs, float(delay_.getValue())); };
        delayValue_.onCommit = [this](float v) { send(ParamId::StrDelayMs, v); };
        solo_.sw.onClick = [this] { if (auto* v = view()) send(ParamId::StrSolo, v->solo ? 0.0f : 1.0f); };
        stem_.dd.onChange = [this](int i) { send(ParamId::StemIndex, float(i - 1)); };
        measure_.onClick = [this] { ed.showPage(HubEditor::Page::Sync); };
        name_.onReturnKey = [this] { commitName(); };
        name_.onFocusLost = [this] { commitName(); };
        name_.setMaxUtf8Bytes(63);
        refreshTexts();
        tick();
    }

    std::vector<GlassCard> cards() const override {
        return { { bar_, theme::radius::card }, { left_, theme::radius::card }, { hpCard_, theme::radius::card }, { infoCard_, theme::radius::card } };
    }

    void refreshTexts() override {
        prev_.setTitle(tr(Str::PrevTrack));
        prev_.setTooltip(tr(Str::PrevTrack));
        next_.setTitle(tr(Str::NextTrack));
        next_.setTooltip(tr(Str::NextTrack));
        picker_.setTitle(tr(Str::ChooseTrack));
        vwField_.setTexts(tr(Str::ViewersLevel), {});
        panField_.setTexts(tr(Str::PanWord), tr(Str::PanCap));
        delayField_.setTexts(tr(Str::ColDelay), tr(Str::DelayCapFine));
        hpField_.setTexts(tr(Str::HeadphoneLevel), tr(Str::HeadphoneLevelCap));
        measure_.setButtonText(tr(Str::MeasureAuto));
        solo_.title = tr(Str::StreamSolo);
        solo_.caption = tr(Str::SoloCap);
        solo_.sw.setTitle(tr(Str::StreamSolo));
        stem_.title = tr(Str::Stem);
        name_.setTitle(tr(Str::NameShown));
        repaint();
    }

    void tick() override {
        const auto& views = ed.trackViews();
        juce::StringArray names;
        int index = -1;
        for (size_t i = 0; i < views.size(); ++i) { names.add(views[i].name); if (views[i].slot == slot_) index = int(i); }
        picker_.setItems(names);
        if (index >= 0) picker_.setSelected(index);
        const auto* v = view();
        for (juce::Component* c : std::initializer_list<juce::Component*> { &you_, &viewers_, &vwField_, &panField_, &delayField_, &solo_, &stem_, &hpField_, &name_ })
            c->setEnabled(v != nullptr);
        prev_.setEnabled(index > 0);
        next_.setEnabled(index >= 0 && index < int(views.size()) - 1);
        if (v == nullptr) { repaint(); return; }
        RowData rd;
        rd.colour = v->colourARGB;
        rd.shade = index;
        picker_.setDot(ed.shadeColour(rd));
        you_.setOn(v->mon, isShowing());
        viewers_.setOn(v->str, isShowing());
        you_.setSubject(v->name);
        viewers_.setSubject(v->name);
        vw_.setDim(!v->str);
        hp_.setDim(!v->mon);
        if (touch_.idle()) {
            const juce::ScopedValueSetter<bool> svs(updating_, true);
            vw_.setValue(LevelSlider::dbToSlider(v->gainDb), juce::dontSendNotification);
            hp_.setValue(LevelSlider::dbToSlider(v->trimDb), juce::dontSendNotification);
            pan_.setPan(v->pan * 100.0f);
            delay_.setValue(v->delayMs, juce::dontSendNotification);
            if (!vwValue_.isEditing()) vwValue_.setValue(juce::jmax(valuetext::kFloorDb, v->gainDb));
            if (!hpValue_.isEditing()) hpValue_.setValue(juce::jmax(valuetext::kFloorDb, v->trimDb));
            if (!panValue_.isEditing()) panValue_.setValue(v->pan * 100.0f);
            if (!delayValue_.isEditing()) delayValue_.setValue(v->delayMs);
        }
        solo_.sw.setOn(v->solo, isShowing());
        juce::StringArray stems { tr(Str::StemNone) };
        for (int i = 0; i < ssbus::kMaxStems; ++i)
            stems.add(ed.proc().stemName(i).isNotEmpty() ? ed.proc().stemName(i) : trf(Str::StemN, { juce::String(i + 1) }));
        stem_.dd.setItems(stems);
        stem_.dd.setSelected(v->stem + 1);
        if (!name_.hasKeyboardFocus(true) && name_.getText() != v->name) name_.setText(v->name, false);
        const bool bypassed = v->bypassed, rate = (v->hubStatus & ssbus::kHubStatusRateMismatch) != 0;
        status_.setText(!v->active ? tr(Str::NotRunning) : bypassed ? tr(Str::BypassedWord) : tr(Str::Running));
        status_.setDot(!v->active ? Dot::Muted : bypassed ? Dot::Warn : Dot::Ok);
        status_.setJustification(juce::Justification::centredRight);
        rateText_ = rate ? tr(Str::DiffersHub) : tr(Str::MatchesHub);
        chainText_ = valuetext::formatMs(v->chainMs);
        repaint();
    }

    void resized() override {
        auto r = getLocalBounds().toFloat();
        const bool narrow = r.getWidth() < 760.0f;
        bar_ = r.removeFromTop(narrow ? 120.0f : 72.0f);
        r.removeFromTop(16.0f);
        {
            auto b = bar_.reduced(16.0f, 16.0f);
            auto toggles = narrow ? b.removeFromBottom(42.0f) : b.removeFromRight(float(you_.idealWidth() + viewers_.idealWidth()) + 10.0f);
            if (narrow) b.removeFromBottom(10.0f);
            viewers_.setBounds(toggles.removeFromRight(float(viewers_.idealWidth())).withSizeKeepingCentre(float(viewers_.idealWidth()), 42.0f).toNearestInt());
            toggles.removeFromRight(10.0f);
            you_.setBounds(toggles.removeFromRight(float(you_.idealWidth())).withSizeKeepingCentre(float(you_.idealWidth()), 42.0f).toNearestInt());
            prev_.setBounds(b.removeFromLeft(40.0f).withSizeKeepingCentre(40.0f, 40.0f).toNearestInt());
            b.removeFromLeft(10.0f);
            const float pw = juce::jmin(300.0f, b.getWidth() - 50.0f - (narrow ? 0.0f : 110.0f));
            picker_.setBounds(b.removeFromLeft(pw).withSizeKeepingCentre(pw, 44.0f).toNearestInt());
            b.removeFromLeft(10.0f);
            next_.setBounds(b.removeFromLeft(40.0f).withSizeKeepingCentre(40.0f, 40.0f).toNearestInt());
            b.removeFromLeft(14.0f);
            countText_ = b;
        }
        juce::Rectangle<float> aside;
        if (!narrow) {
            aside = r.removeFromRight(340.0f);
            r.removeFromRight(16.0f);
        }
        left_ = r;
        if (narrow) {   // headphones + info go below in the same scroll
            hpCard_ = infoCard_ = {};
        } else {
            hpCard_ = aside.removeFromTop(22.0f + 26.0f + 16.0f + float(hpField_.idealHeight(292)) + 22.0f);
            aside.removeFromTop(16.0f);
            infoCard_ = aside;
            auto h = hpCard_.reduced(24.0f, 22.0f);
            hpHead_ = h.removeFromTop(26.0f);
            h.removeFromTop(16.0f);
            hpField_.setBounds(h.toNearestInt());
            auto i = infoCard_.reduced(24.0f, 22.0f);
            infoHead_ = i.removeFromTop(22.0f);
            i.removeFromTop(12.0f);
            nameLabel_ = i.removeFromTop(18.0f);
            i.removeFromTop(6.0f);
            name_.setBounds(i.removeFromTop(40.0f).toNearestInt());
            i.removeFromTop(16.0f);
            kv_ = i.removeFromTop(3.0f * 36.0f);
            status_.setBounds(kv_.removeFromTop(36.0f).withTrimmedTop(1.0f).removeFromRight(140.0f).toNearestInt());
            kv_ = kv_.withY(float(status_.getY()) - 1.0f).withHeight(3.0f * 36.0f);
        }
        hpField_.setVisible(!narrow);
        name_.setVisible(!narrow);
        status_.setVisible(!narrow);

        auto c = left_.reduced(24.0f).withTrimmedBottom(-4.0f);
        leftHead_ = c.removeFromTop(24.0f);
        c.removeFromTop(22.0f);
        scroll_.setBounds(c.withTrimmedRight(-18.0f).toNearestInt());
        scroll_.setFadeColour(paletteOf(*this).paper.overlaidWith(paletteOf(*this).glass));
        const int w = scroll_.contentWidth();
        int y = 0;
        auto place = [&](juce::Component& comp, int hh) { comp.setBounds(0, y, w, hh); y += hh + 22; };
        place(vwField_, vwField_.idealHeight(w));
        place(panField_, panField_.idealHeight(w));
        place(delayField_, delayField_.idealHeight(w));
        // "Measure automatically" sits at the end of the delay caption (inside the field)
        const auto capF = uiFont(12.0f);
        const auto lines = wrapText(capF, tr(Str::DelayCapFine), float(w));
        const float lastW = textWidth(capF, lines[lines.size() - 1]) + 6.0f;
        const int mw = measure_.idealWidth();
        delayField_.addAndMakeVisible(measure_);
        if (lastW + float(mw) <= float(w)) {
            measure_.setBounds(juce::roundToInt(lastW), delayField_.getHeight() - juce::roundToInt(capF.getHeight()) - 2, mw, juce::roundToInt(capF.getHeight()) + 2);
        } else {
            delayField_.setSize(w, delayField_.getHeight() + 22);
            measure_.setBounds(0, delayField_.getHeight() - 20, mw, 18);
            y += 22;
        }
        soloLine_ = juce::Rectangle<float>(0.0f, float(y - 4), float(w), 1.0f);
        y += 14;
        const int half = (w - 16) / 2;
        solo_.setBounds(0, y, half, 64);
        stem_.setBounds(half + 16, y, w - half - 16, 64);
        y += 64 + 8;
        body_.setSize(w, y);
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        const auto& views = ed.trackViews();
        int index = -1;
        for (size_t i = 0; i < views.size(); ++i) if (views[i].slot == slot_) index = int(i);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f));
        if (index >= 0) g.drawText(trf(Str::TrackNOfM, { juce::String(index + 1), juce::String(int(views.size())) }), countText_, juce::Justification::centredLeft, true);
        else g.drawText(tr(Str::NoTracks), countText_, juce::Justification::centredLeft, true);
        auto head = [&](juce::Rectangle<float> r, icons::Icon icon, Str title, Str cap) {
            icons::draw(g, icon, r.removeFromLeft(20.0f).withSizeKeepingCentre(20.0f, 20.0f), p.ink);
            r.removeFromLeft(10.0f);
            const auto f = uiFont(17.0f, Weight::SemiBold);
            const float tw = textWidth(f, tr(title)) + 2.0f;
            g.setColour(p.ink);
            g.setFont(f);
            g.drawText(tr(title), r.removeFromLeft(tw), juce::Justification::centredLeft, false);
            if (cap != Str::None) {
                r.removeFromLeft(10.0f);
                g.setColour(p.graphite);
                g.setFont(uiFont(12.0f));
                g.drawText(tr(cap), r, juce::Justification::centredLeft, true);
            }
        };
        head(leftHead_, icons::Icon::Broadcast, Str::ViewersCard, Str::ViewersCardCap);
        if (!hpCard_.isEmpty()) {
            head(hpHead_, icons::Icon::Headphones, Str::HeadphonesCard, Str::None);
            g.setColour(p.ink);
            g.setFont(uiFont(17.0f, Weight::SemiBold));
            g.drawText(tr(Str::TrackInfo), infoHead_, juce::Justification::centredLeft, true);
            g.setColour(p.graphite);
            g.setFont(uiFont(12.5f));
            g.drawText(tr(Str::NameShown), nameLabel_, juce::Justification::centredLeft, true);
            auto kv = kv_;
            auto row = [&](Str label, const juce::String& value) {
                auto r = kv.removeFromTop(36.0f);
                g.setColour(p.hairline2);
                g.fillRect(r.removeFromTop(1.0f));
                g.setColour(p.graphite);
                g.setFont(uiFont(12.5f));
                g.drawText(tr(label), r, juce::Justification::centredLeft, true);
                if (value.isNotEmpty()) {
                    g.setColour(p.ink);
                    g.drawText(value, r, juce::Justification::centredRight, false);
                }
            };
            row(Str::StatusWord, {});
            row(Str::ChainLatency, chainText_);
            row(Str::SampleRate, rateText_);
        }
    }

    void paintOverChildren(juce::Graphics& g) override {
        // the line above Solo / Send to stem, inside the scrolling body
        juce::ignoreUnused(g);
    }

private:
    struct SoloBox : juce::Component {
        Switch sw;
        juce::String title, caption;
        SoloBox() { addAndMakeVisible(sw); }
        void resized() override { sw.setBounds(getWidth() - 44, (getHeight() - 26) / 2 + 6, 44, 26); }
        void paint(juce::Graphics& g) override {
            const auto& p = paletteOf(*this);
            g.setColour(p.hairline2);
            g.fillRect(0.0f, 0.0f, float(getWidth()) * 2.0f + 16.0f, 1.0f);
            auto r = getLocalBounds().toFloat().withTrimmedTop(12.0f).withTrimmedRight(56.0f);
            auto block = r.withSizeKeepingCentre(r.getWidth(), 18.0f + 2.0f + 16.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(13.5f, Weight::Medium));
            g.drawText(title, block.removeFromTop(18.0f), juce::Justification::centredLeft, true);
            block.removeFromTop(2.0f);
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f));
            g.drawText(caption, block, juce::Justification::centredLeft, true);
        }
    };
    struct StemBox : juce::Component {
        Dropdown dd;
        juce::String title;
        StemBox() { addAndMakeVisible(dd); }
        void resized() override { dd.setBounds(0, getHeight() - 38, getWidth(), 38); }
        void paint(juce::Graphics& g) override {
            const auto& p = paletteOf(*this);
            g.setColour(p.hairline2);
            g.fillRect(0.0f, 0.0f, float(getWidth()), 1.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(13.5f, Weight::Medium));
            g.drawText(title, juce::Rectangle<float>(0.0f, 0.0f, float(getWidth()), float(getHeight() - 44)).withTrimmedTop(8.0f), juce::Justification::bottomLeft, true);
        }
    };

    const TrackView* view() const { return findTrack(ed.trackViews(), slot_); }
    void send(ParamId id, float v) { if (slot_ >= 0) { touch_.now(); ed.proc().send(slot_, id, v); } }
    void step(int d) {
        const auto& views = ed.trackViews();
        for (size_t i = 0; i < views.size(); ++i)
            if (views[i].slot == slot_) {
                const int j = juce::jlimit(0, int(views.size()) - 1, int(i) + d);
                slot_ = views[size_t(j)].slot;
                break;
            }
        tick();
    }
    void commitName() {
        auto* v = view();
        if (v == nullptr) return;
        const auto n = name_.getText().trim();
        if (n == v->name) return;
        ed.renameRow(slot_, n);
    }

    int slot_;
    IconButton prev_ { icons::Icon::ChevronLeft }, next_ { icons::Icon::ChevronRight };
    Dropdown picker_;
    AudibleToggle you_ { AudibleToggle::Side::You }, viewers_ { AudibleToggle::Side::Viewers };
    ScrollArea scroll_ { 18 };
    juce::Component body_;
    LevelSlider vw_, hp_;
    EditableValue vwValue_ { EditableValue::Kind::Db, -30.0f, 6.0f }, hpValue_ { EditableValue::Kind::Db, -30.0f, 6.0f };
    PanSlider pan_ { true };
    EditableValue panValue_ { EditableValue::Kind::Pan, -100.0f, 100.0f };
    juce::Slider delay_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    EditableValue delayValue_ { EditableValue::Kind::Ms, 0.0f, 500.0f };
    Field vwField_ { {}, &vw_, 20, &vwValue_, 76 }, panField_ { {}, &pan_, PanSlider::kFullHeight, &panValue_, 76 },
          delayField_ { {}, &delay_, 20, &delayValue_, 76 }, hpField_ { {}, &hp_, 20, &hpValue_, 76 };
    LinkButton measure_ { {}, 12.0f };
    SoloBox solo_;
    StemBox stem_;
    TextField name_;
    TextLine status_ { 12.5f, Weight::Regular, TextLine::Tone::Ink };
    Touch touch_;
    bool updating_ = false;
    juce::String rateText_, chainText_;
    juce::Rectangle<float> bar_, left_, hpCard_, infoCard_, countText_, leftHead_, hpHead_, infoHead_, nameLabel_, kv_, soloLine_;
};

// =============================================================================================
// Manage tracks (prompt 3.4 "Tracks"): every track and setting in one table

class TracksPage : public HubPage {
public:
    explicit TracksPage(HubEditor& e) : HubPage(e) {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &filter_, &clearSolo_, &reset_, &scroll_ }) addAndMakeVisible(c);
        scroll_.setContent(list_);
        filter_.setTall(true);
        filter_.onChange = [this](int) { rebuild(); };
        clearSolo_.onClick = [this] {
            for (const auto& v : ed.trackViews()) if (v.solo) ed.proc().send(v.slot, ParamId::StrSolo, 0.0f);
        };
        reset_.onClick = [this] {
            for (const auto& v : ed.trackViews()) {
                ed.proc().send(v.slot, ParamId::StrGainDb, 0.0f);
                ed.proc().send(v.slot, ParamId::MonTrimDb, 0.0f);
                ed.proc().send(v.slot, ParamId::StrPan, 0.0f);
            }
            ed.toast(tr(Str::ResetLevelsToast));
        };
        refreshTexts();
        tick();
    }

    std::vector<GlassCard> cards() const override { return { { bar_, theme::radius::card }, { table_, theme::radius::card } }; }

    void refreshTexts() override {
        clearSolo_.setButtonText(tr(Str::ClearSolo));
        reset_.setButtonText(tr(Str::ResetLevelsPan));
        filter_.setTitle(tr(Str::FilterTip));
        for (auto* r : rows_) r->refreshTexts();
        repaint();
    }

    void tick() override {
        const auto& views = ed.trackViews();
        int you = 0, viewers = 0, silent = 0;
        bool solo = false;
        for (const auto& v : views) { you += v.mon; viewers += v.str; silent += (!v.mon && !v.str); solo = solo || v.solo; }
        filter_.setSegments({ { trf(Str::FilterAll, { juce::String(int(views.size())) }) }, { trf(Str::FilterYou, { juce::String(you) }) },
                              { trf(Str::FilterViewers, { juce::String(viewers) }) }, { trf(Str::FilterSilent, { juce::String(silent) }) } });
        clearSolo_.setEnabled(solo);
        rebuild();
    }

    void resized() override {
        auto r = getLocalBounds().toFloat();
        bar_ = r.removeFromTop(64.0f);
        r.removeFromTop(16.0f);
        table_ = r;
        auto b = bar_.reduced(16.0f, 13.0f);
        reset_.setBounds(b.removeFromRight(float(reset_.idealWidth())).toNearestInt());
        b.removeFromRight(10.0f);
        clearSolo_.setBounds(b.removeFromRight(float(clearSolo_.idealWidth())).toNearestInt());
        b.removeFromRight(16.0f);
        filter_.setBounds(b.withWidth(juce::jmin(b.getWidth(), float(filter_.idealWidth()) + 20.0f)).toNearestInt());
        auto t = table_.reduced(20.0f, 16.0f);
        footer_ = t.removeFromBottom(30.0f);
        t.removeFromBottom(6.0f);
        header_ = t.removeFromTop(28.0f);
        scroll_.setBounds(t.withTrimmedRight(-14.0f).toNearestInt());
        scroll_.setFadeColour(paletteOf(*this).paper.overlaidWith(paletteOf(*this).glass));
        layoutRows();
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        const auto cols = columns(header_.getWidth());
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        const Str heads[] = { Str::ColTrack, Str::ColYou, Str::ColViewers, Str::ColHpLevel, Str::ColVwLevel, Str::ColPan, Str::ColDelay, Str::ColStem, Str::ColSolo };
        for (size_t i = 0; i < std::size(heads); ++i) {
            if (cols[i].isEmpty()) continue;
            const auto just = (i == 1 || i == 2 || i == 8) ? juce::Justification::centred : (i == 6 ? juce::Justification::centredRight : juce::Justification::centredLeft);
            const auto cell = cols[i].withY(header_.getY()).withHeight(header_.getHeight()).translated(header_.getX(), 0.0f);
            g.drawFittedText(tr(heads[i]), cell.toNearestInt(), just, 1, 0.8f);
        }
        g.setColour(p.hairline2);
        g.fillRect(footer_.withHeight(1.0f));
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(ellipsize(uiFont(12.0f), tr(Str::TracksFooter), footer_.getWidth()), footer_.withTrimmedTop(10.0f), juce::Justification::centredLeft, false);
        if (rows_.isEmpty()) {
            g.setColour(p.graphite);
            g.setFont(uiFont(13.0f));
            g.drawText(tr(Str::NoTracksFilter), scroll_.getBounds().toFloat().withHeight(60.0f), juce::Justification::centred, false);
        }
    }

    // grid: Track | You | Viewers | Headphone level | Viewers level | Pan | Delay | Stem | Solo | ›
    // (narrower windows drop the headphone level, then pan and stem)
    static std::vector<juce::Rectangle<float>> columns(float w) {
        const float widths[] = { 0, 40, 40, 156, 156, 112, 66, 100, 40, 28 };
        bool show[10] = { true, true, true, true, true, true, true, true, true, true };
        auto fixed = [&] { float f = 0; for (int i = 1; i < 10; ++i) if (show[i]) f += widths[i] + 8.0f; return f; };
        if (w - fixed() < 150.0f) show[3] = false;
        if (w - fixed() < 150.0f) { show[5] = false; show[7] = false; }
        if (w - fixed() < 120.0f) { show[6] = false; show[8] = false; }
        std::vector<juce::Rectangle<float>> out(10);
        float x = 14.0f;
        const float nameW = w - 14.0f - 10.0f - fixed();
        out[0] = { x, 0.0f, nameW, 52.0f };
        x += nameW + 8.0f;
        for (int i = 1; i < 10; ++i) {
            if (!show[i]) continue;
            out[size_t(i)] = { x, 0.0f, widths[i], 52.0f };
            x += widths[i] + 8.0f;
        }
        return out;
    }

private:
    class TableRow : public juce::Component {
    public:
        explicit TableRow(HubEditor& e) : ed_(e) {
            for (juce::Component* c : std::initializer_list<juce::Component*> { &name_, &you_, &viewers_, &hp_, &hpValue_, &vw_, &vwValue_, &pan_, &panValue_, &delay_, &stem_, &solo_, &open_ })
                addAndMakeVisible(c);
            stem_.setSmall(true);
            name_.onRename = [this](const juce::String& n) { ed_.renameRow(v_.slot, n); };
            you_.onClick = [this] { send(ParamId::Mon, v_.mon ? 0.0f : 1.0f); };
            viewers_.onClick = [this] { send(ParamId::Str, v_.str ? 0.0f : 1.0f); };
            hp_.onValueChange = [this] { if (!updating_) send(ParamId::MonTrimDb, LevelSlider::sliderToDb(hp_.getValue())); };
            vw_.onValueChange = [this] { if (!updating_) send(ParamId::StrGainDb, LevelSlider::sliderToDb(vw_.getValue())); };
            hpValue_.onCommit = [this](float db) { send(ParamId::MonTrimDb, db <= valuetext::kFloorDb + 1.0e-3f ? -60.0f : db); };
            vwValue_.onCommit = [this](float db) { send(ParamId::StrGainDb, db <= valuetext::kFloorDb + 1.0e-3f ? -60.0f : db); };
            pan_.onChange = [this](float p) { if (!updating_) { send(ParamId::StrPan, p / 100.0f); panValue_.setValue(p); } };
            panValue_.onCommit = [this](float p) { send(ParamId::StrPan, p / 100.0f); };
            delay_.onCommit = [this](float ms) { send(ParamId::StrDelayMs, ms); };
            stem_.onChange = [this](int i) { send(ParamId::StemIndex, float(i - 1)); };
            solo_.onClick = [this] { send(ParamId::StrSolo, v_.solo ? 0.0f : 1.0f); };
            open_.onClick = [this] { ed_.showPage(HubEditor::Page::Track, v_.slot); };
            for (auto* ev : { &hpValue_, &vwValue_ }) ev->setFontSize(12.0f);
            panValue_.setFontSize(12.0f);
            delay_.setFontSize(12.0f);
            refreshTexts();
        }
        void refreshTexts() {
            open_.setTitle(tr(Str::FineSettings));
            open_.setTooltip(tr(Str::FineSettings));
            solo_.setTitle(tr(Str::StreamSolo));
        }
        void update(const TrackView& v, int shade) {
            v_ = v;
            shade_ = shade;
            name_.setName(v.name, {});
            you_.setOn(v.mon, isShowing());
            viewers_.setOn(v.str, isShowing());
            you_.setSubject(v.name);
            viewers_.setSubject(v.name);
            hp_.setDim(!v.mon);
            vw_.setDim(!v.str);
            hpValue_.setDim(!v.mon);
            vwValue_.setDim(!v.str);
            if (touch_.idle()) {
                const juce::ScopedValueSetter<bool> svs(updating_, true);
                hp_.setValue(LevelSlider::dbToSlider(v.trimDb), juce::dontSendNotification);
                vw_.setValue(LevelSlider::dbToSlider(v.gainDb), juce::dontSendNotification);
                pan_.setPan(v.pan * 100.0f);
                if (!hpValue_.isEditing()) hpValue_.setValue(juce::jmax(valuetext::kFloorDb, v.trimDb));
                if (!vwValue_.isEditing()) vwValue_.setValue(juce::jmax(valuetext::kFloorDb, v.gainDb));
                if (!panValue_.isEditing()) panValue_.setValue(v.pan * 100.0f);
                if (!delay_.isEditing()) delay_.setValue(v.delayMs);
            }
            juce::StringArray stems { tr(Str::StemNone) };
            for (int i = 0; i < ssbus::kMaxStems; ++i)
                stems.add(ed_.proc().stemName(i).isNotEmpty() ? ed_.proc().stemName(i) : trf(Str::StemN, { juce::String(i + 1) }));
            stem_.setItems(stems);
            stem_.setSelected(v.stem + 1);
            solo_.setOn(v.solo, isShowing());
            setAlpha(v.active ? 1.0f : 0.5f);
            repaint();
        }
        int slot() const noexcept { return v_.slot; }
        void resized() override {
            const auto cols = columns(float(getWidth()));
            auto at = [&](size_t i) { return cols[i]; };
            auto name = at(0);
            dot_ = name.removeFromLeft(8.0f).withSizeKeepingCentre(8.0f, 8.0f);
            name.removeFromLeft(8.0f);
            const float soloW = v_.solo ? badgeWidth(tr(Str::SoloBadge), BadgeStyle::Solid) + 6.0f : 0.0f;
            badge_ = name.removeFromRight(soloW);
            name_.setBounds(name.withTrimmedLeft(-7.0f).withSizeKeepingCentre(name.getWidth() + 7.0f, 26.0f).toNearestInt());
            auto place = [&](juce::Component& c, juce::Rectangle<float> r, float h) { c.setVisible(!r.isEmpty()); if (!r.isEmpty()) c.setBounds(r.withSizeKeepingCentre(r.getWidth(), h).toNearestInt()); };
            place(you_, at(1), 32.0f);
            place(viewers_, at(2), 32.0f);
            auto lvl = [&](juce::Rectangle<float> r, LevelSlider& s, EditableValue& v) {
                s.setVisible(!r.isEmpty());
                v.setVisible(!r.isEmpty());
                if (r.isEmpty()) return;
                v.setBounds(r.removeFromRight(68.0f).withSizeKeepingCentre(68.0f, 26.0f).toNearestInt());
                s.setBounds(r.withTrimmedRight(4.0f).withSizeKeepingCentre(r.getWidth() - 4.0f, 20.0f).toNearestInt());
            };
            lvl(at(3), hp_, hpValue_);
            lvl(at(4), vw_, vwValue_);
            {
                auto r = at(5);
                pan_.setVisible(!r.isEmpty());
                panValue_.setVisible(!r.isEmpty());
                if (!r.isEmpty()) {
                    panValue_.setBounds(r.removeFromRight(44.0f).withSizeKeepingCentre(44.0f, 26.0f).toNearestInt());
                    pan_.setBounds(r.withTrimmedRight(4.0f).withSizeKeepingCentre(r.getWidth() - 4.0f, 20.0f).toNearestInt());
                }
            }
            place(delay_, at(6), 26.0f);
            place(stem_, at(7), 30.0f);
            place(solo_, at(8), 20.0f);
            place(open_, at(9), 28.0f);
        }
        void paint(juce::Graphics& g) override {
            const auto& p = paletteOf(*this);
            const auto r = getLocalBounds().toFloat().reduced(0.5f);
            g.setColour(p.inset);
            g.fillRoundedRectangle(r, 13.0f);
            g.setColour(p.hairline1);
            g.drawRoundedRectangle(r, 13.0f, 1.0f);
            RowData rd;
            rd.colour = v_.colourARGB;
            rd.shade = shade_;
            g.setColour(ed_.shadeColour(rd));
            g.fillEllipse(dot_);
            if (!badge_.isEmpty()) drawBadge(g, badge_.withTrimmedLeft(6.0f).withSizeKeepingCentre(badge_.getWidth() - 6.0f, 20.0f), tr(Str::SoloBadge), BadgeStyle::Solid, p);
        }
    private:
        void send(ParamId id, float v) { touch_.now(); ed_.proc().send(v_.slot, id, v); }
        HubEditor& ed_;
        TrackView v_;
        int shade_ = 0;
        InlineName name_;
        AudibleToggle you_ { AudibleToggle::Side::You }, viewers_ { AudibleToggle::Side::Viewers };
        LevelSlider hp_, vw_;
        EditableValue hpValue_ { EditableValue::Kind::Db, -30.0f, 6.0f }, vwValue_ { EditableValue::Kind::Db, -30.0f, 6.0f };
        PanSlider pan_ { false };
        EditableValue panValue_ { EditableValue::Kind::Pan, -100.0f, 100.0f };
        EditableValue delay_ { EditableValue::Kind::Ms, 0.0f, 500.0f };
        Dropdown stem_;
        Switch solo_;
        IconButton open_ { icons::Icon::ChevronRight };
        Touch touch_;
        bool updating_ = false;
        juce::Rectangle<float> dot_, badge_;
    };

    void rebuild() {
        const auto& views = ed.trackViews();
        std::vector<std::pair<const TrackView*, int>> shown;
        for (size_t i = 0; i < views.size(); ++i) {
            const auto& v = views[i];
            const int f = filter_.selected();
            if (f == 1 && !v.mon) continue;
            if (f == 2 && !v.str) continue;
            if (f == 3 && (v.mon || v.str)) continue;
            shown.push_back({ &v, int(i) });
        }
        bool structure = shown.size() != size_t(rows_.size());
        for (size_t i = 0; !structure && i < shown.size(); ++i) structure = rows_[int(i)]->slot() != shown[i].first->slot;
        if (structure) {
            rows_.clear();
            for (size_t i = 0; i < shown.size(); ++i) list_.addAndMakeVisible(rows_.add(new TableRow(ed)));
            layoutRows();
            repaint();
        }
        for (size_t i = 0; i < shown.size(); ++i) rows_[int(i)]->update(*shown[i].first, shown[i].second);
    }

    void layoutRows() {
        const int w = scroll_.contentWidth();
        int y = 0;
        for (auto* r : rows_) { r->setBounds(0, y, w, 52); y += 52 + 6; }
        list_.setSize(w, juce::jmax(1, y - 6));
    }

    SegmentedControl filter_;
    GhostButton clearSolo_, reset_;
    ScrollArea scroll_;
    juce::Component list_;
    juce::OwnedArray<TableRow> rows_;
    juce::Rectangle<float> bar_, table_, header_, footer_;
};

// =============================================================================================
// Manage program audio (prompt 3.4 "Programs")

struct TakeInfo {
    juce::File file;
    juce::String label;
    int number = 1;
    double seconds = 0;
    juce::Time time;
};

class ProgramsPage : public HubPage {
public:
    explicit ProgramsPage(HubEditor& e) : HubPage(e) {
        for (juce::Component* c : std::initializer_list<juce::Component*> { &scroll_, &takesScroll_, &openFolder_, &howTo_ }) addAndMakeVisible(c);
        scroll_.setContent(list_);
        takesScroll_.setContent(takes_);
        openFolder_.onClick = [] {
            const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HEARASIDE").getChildFile("Recordings");
            dir.createDirectory();
            dir.revealToUser();
        };
        howTo_.onClick = [this] { showHowTo(); };
        refreshTexts();
        tick();
    }

    std::vector<GlassCard> cards() const override { return { { left_, theme::radius::card }, { right_, theme::radius::card } }; }

    void refreshTexts() override {
        openFolder_.setButtonText(tr(Str::OpenFolder));
        howTo_.setButtonText(tr(Str::HowToSetUp));
        for (auto* c : cards_) c->refreshTexts();
        repaint();
    }

    void tick() override {
        const auto& srcs = ed.sourceViews();
        bool structure = srcs.size() != size_t(cards_.size());
        for (size_t i = 0; !structure && i < srcs.size(); ++i) structure = cards_[int(i)]->index() != srcs[i].index;
        if (structure) {
            cards_.clear();
            for (size_t i = 0; i < srcs.size(); ++i) list_.addAndMakeVisible(cards_.add(new AppCard(ed)));
            layoutList();
        }
        for (size_t i = 0; i < srcs.size(); ++i) cards_[int(i)]->update(srcs[i]);
        if (++ticks_ % 20 == 1) scanTakes();   // every ~2 s
        recording_.clear();
        for (const auto& s : srcs) if (s.recording()) recording_.push_back({ s.app == kSystemAudio ? tr(Str::WholeComputerShort) : HubEditor::programLabel(s.app), s.recordSec });
        if (recording_.size() != lastRecording_) { lastRecording_ = recording_.size(); layoutTakes(); }
        takes_.repaint();
        repaint();
    }

    void resized() override {
        auto r = getLocalBounds().toFloat();
        const bool narrow = r.getWidth() < 760.0f;
        if (!narrow) {
            right_ = r.removeFromRight(330.0f);
            r.removeFromRight(16.0f);
        } else {
            right_ = r.removeFromBottom(juce::jmin(260.0f, r.getHeight() * 0.4f));
            r.removeFromBottom(10.0f);
        }
        left_ = r;
        auto c = left_.reduced(24.0f, 20.0f);
        head_ = c.removeFromTop(26.0f);
        c.removeFromTop(12.0f);
        scroll_.setBounds(c.withTrimmedRight(-14.0f).toNearestInt());
        scroll_.setFadeColour(paletteOf(*this).paper.overlaidWith(paletteOf(*this).glass));
        layoutList();
        auto t = right_.reduced(20.0f);
        takesHead_ = t.removeFromTop(19.0f + 3.0f + 16.0f);
        t.removeFromTop(12.0f);
        openFolder_.setBounds(t.removeFromBottom(36.0f).toNearestInt());
        t.removeFromBottom(14.0f);
        const float fh = wrappedHeight(uiFont(12.0f), tr(Str::TakesFolderHint), t.getWidth(), 6.0f);
        takesFooter_ = t.removeFromBottom(fh + 12.0f);
        t.removeFromBottom(8.0f);
        takesScroll_.setBounds(t.withTrimmedRight(-14.0f).toNearestInt());
        takesScroll_.setFadeColour(paletteOf(*this).paper.overlaidWith(paletteOf(*this).glass));
        layoutTakes();
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto h = head_;
        const auto f = uiFont(20.0f, Weight::SemiBold);
        const float tw = textWidth(f, tr(Str::AppAudioWord)) + 2.0f;
        g.setColour(p.ink);
        g.setFont(f);
        g.drawText(tr(Str::AppAudioWord), h.removeFromLeft(tw), juce::Justification::centredLeft, false);
        h.removeFromLeft(12.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f));
        g.drawText(trf(Str::InThisProject, { juce::String(int(ed.sourceViews().size())) }), h, juce::Justification::centredLeft, true);

        auto th = takesHead_;
        g.setColour(p.ink);
        g.setFont(uiFont(16.0f, Weight::SemiBold));
        g.drawText(tr(Str::RecordedTakes), th.removeFromTop(19.0f), juce::Justification::centredLeft, true);
        th.removeFromTop(3.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f));
        g.drawText(tr(Str::RecordedTakesCap), th, juce::Justification::centredLeft, true);
        g.setColour(p.hairline2);
        g.fillRect(takesFooter_.withHeight(1.0f));
        drawWrapped(g, tr(Str::TakesFolderHint), uiFont(12.0f), p.graphite, takesFooter_.withTrimmedTop(12.0f), 6.0f);
    }

private:
    // One App Audio: source, on/off, status, level, delay, you / viewers, recording
    class AppCard : public juce::Component {
    public:
        explicit AppCard(HubEditor& e) : ed_(e) {
            for (juce::Component* c : std::initializer_list<juce::Component*> { &picker_, &power_, &status_, &meter_, &level_, &levelValue_, &delay_, &delayValue_,
                                                                                &you_, &viewers_, &print_, &follow_ })
                addAndMakeVisible(c);
            power_.setPill(true);
            power_.onClick = [this] { ed_.proc().sendSource(v_.index, ssbus::SourceParam::On, v_.on() ? 0.0f : 1.0f); };
            picker_.buildMenu = [this] { return sourceMenu(); };
            level_.setRange(-30.0, 12.0, 0.5);
            level_.onValueChange = [this] { if (!updating_) { touch_.now(); ed_.proc().sendSource(v_.index, ssbus::SourceParam::LevelDb, LevelSlider::sliderToDb(level_.getValue())); } };
            levelValue_.onCommit = [this](float db) { touch_.now(); ed_.proc().sendSource(v_.index, ssbus::SourceParam::LevelDb, db <= valuetext::kFloorDb + 1.0e-3f ? -60.0f : db); };
            setupMsSlider(delay_);
            delay_.onValueChange = [this] { if (!updating_) { touch_.now(); ed_.proc().sendSource(v_.index, ssbus::SourceParam::DelayMs, float(delay_.getValue())); } };
            delayValue_.onCommit = [this](float ms) { touch_.now(); ed_.proc().sendSource(v_.index, ssbus::SourceParam::DelayMs, ms); };
            you_.onClick = [this] { if (v_.slot >= 0) ed_.proc().send(v_.slot, ParamId::Mon, v_.mon ? 0.0f : 1.0f); };
            viewers_.onClick = [this] { if (v_.slot >= 0) ed_.proc().send(v_.slot, ParamId::Str, v_.str ? 0.0f : 1.0f); };
            print_.onClick = [this] { ed_.proc().sendSource(v_.index, ssbus::SourceParam::Record, v_.recording() ? 0.0f : 1.0f); };
            follow_.sw.onClick = [this] { ed_.proc().sendSource(v_.index, ssbus::SourceParam::FollowRecord, (v_.flags & ssbus::kSrcFollowRec) ? 0.0f : 1.0f); };
            refreshTexts();
        }
        int index() const noexcept { return v_.index; }
        void refreshTexts() {
            power_.setTitle(tr(Str::PowerTip));
            follow_.text = tr(Str::RecordWithDaw);
            follow_.sw.setTitle(tr(Str::RecordWithDaw));
            level_.setTitle(tr(Str::AppLevel));
            delay_.setTitle(tr(Str::DelayViewers));
            repaint();
        }
        void update(const SourceView& v) {
            const bool changed = v.app != v_.app || v.flags != v_.flags || v.capture != v_.capture || int(v.recordSec) != int(v_.recordSec) || v.name != v_.name;
            v_ = v;
            picker_.set(HubEditor::programIcon(v.app), HubEditor::programLabel(v.app));
            power_.setOn(v.on(), isShowing());
            you_.setOn(v.mon, isShowing());
            viewers_.setOn(v.str, isShowing());
            you_.setSubject(HubEditor::programLabel(v.app));
            viewers_.setSubject(HubEditor::programLabel(v.app));
            you_.setEnabled(v.slot >= 0);
            viewers_.setEnabled(v.slot >= 0);
            meter_.setLevel(v.on() ? meterPosition(v.peak) : 0.0f);
            if (touch_.idle()) {
                const juce::ScopedValueSetter<bool> svs(updating_, true);
                level_.setValue(juce::jlimit(-30.0, 12.0, double(v.levelDb)), juce::dontSendNotification);
                delay_.setValue(v.delayMs, juce::dontSendNotification);
                if (!levelValue_.isEditing()) levelValue_.setValue(juce::jmax(valuetext::kFloorDb, v.levelDb));
                if (!delayValue_.isEditing()) delayValue_.setValue(v.delayMs);
            }
            const bool rec = v.recording();
            print_.setButtonText(rec ? trf(Str::StopTime, { clock(v.recordSec) }) : tr(Str::PrintToFile));
            print_.setStyle(rec ? GhostButton::Style::Danger : GhostButton::Style::Ghost);
            print_.setIcon(rec ? icons::Icon::Record : icons::Icon::Record);
            follow_.sw.setOn((v.flags & ssbus::kSrcFollowRec) != 0, isShowing());
            Dot dot = Dot::Muted;
            juce::String st;
            if (!v.on()) st = tr(Str::OffChannel);
            else switch (v.capture) {
                case 2: dot = Dot::Ok; st = v.app == kLinkIn ? tr(Str::SenderOn) : v.app.startsWith("http") ? tr(Str::AppLinkReceiving) : tr(Str::AppRunning); break;
                case 1: st = tr(Str::AppStarting); break;
                case 3: dot = Dot::Warn; st = v.app == kLinkIn ? tr(Str::SenderOff) : v.app.startsWith("http") ? tr(Str::AppLinkOffline) : tr(Str::AppNotRunning); break;
                case 4: dot = Dot::Rec; st = tr(Str::AppFailed); break;
                default: st = tr(Str::AppNone); break;
            }
            status_.setText(st);
            status_.setDot(dot);
            for (juce::Component* c : std::initializer_list<juce::Component*> { &level_, &levelValue_, &delay_, &delayValue_, &you_, &viewers_, &print_, &follow_, &status_, &meter_ })
                c->setAlpha(v.on() ? 1.0f : 0.4f);
            if (changed) { resized(); repaint(); }
        }
        static int idealHeight() { return 16 + 48 + 10 + 20 + 14 + 18 + 4 + 20 + 14 + 1 + 14 + 38 + 16; }
        void resized() override {
            auto r = getLocalBounds().toFloat().reduced(16.0f);
            auto top = r.removeFromTop(48.0f);
            const float pw = float(power_.idealWidth());
            power_.setBounds(top.removeFromRight(pw).withSizeKeepingCentre(pw, 36.0f).toNearestInt());
            top.removeFromRight(10.0f);
            const auto onText = trf(Str::OnChannel, { v_.name });
            const float ow = juce::jmin(top.getWidth() * 0.4f, textWidth(uiFont(12.0f), onText) + 4.0f);
            channel_ = top.removeFromRight(ow);
            top.removeFromRight(10.0f);
            picker_.setBounds(top.toNearestInt());
            r.removeFromTop(10.0f);
            auto st = r.removeFromTop(20.0f);
            meter_.setBounds(st.removeFromRight(100.0f).withSizeKeepingCentre(100.0f, 8.0f).toNearestInt());
            st.removeFromRight(10.0f);
            status_.setBounds(st.toNearestInt());
            r.removeFromTop(14.0f);
            auto grid = r.removeFromTop(18.0f + 4.0f + 20.0f);
            auto toggles = grid.removeFromRight(40.0f * 2.0f + 8.0f);
            viewers_.setBounds(toggles.removeFromRight(40.0f).removeFromBottom(32.0f).toNearestInt());
            toggles.removeFromRight(8.0f);
            you_.setBounds(toggles.removeFromRight(40.0f).removeFromBottom(32.0f).toNearestInt());
            grid.removeFromRight(18.0f);
            const float colW = (grid.getWidth() - 18.0f) * 0.5f;
            auto a = grid.removeFromLeft(colW);
            grid.removeFromLeft(18.0f);
            auto b = grid;
            levelLabel_ = a.removeFromTop(18.0f);
            levelValue_.setBounds(levelLabel_.withLeft(levelLabel_.getRight() - 62.0f).withSizeKeepingCentre(62.0f, 24.0f).toNearestInt());
            a.removeFromTop(4.0f);
            level_.setBounds(a.toNearestInt());
            delayLabel_ = b.removeFromTop(18.0f);
            delayValue_.setBounds(delayLabel_.withLeft(delayLabel_.getRight() - 62.0f).withSizeKeepingCentre(62.0f, 24.0f).toNearestInt());
            b.removeFromTop(4.0f);
            delay_.setBounds(b.toNearestInt());
            r.removeFromTop(14.0f);
            sep_ = r.removeFromTop(1.0f);
            r.removeFromTop(14.0f);
            auto rec = r.removeFromTop(38.0f);
            const float bw = juce::jmax(136.0f, float(print_.idealWidth()));
            print_.setBounds(rec.removeFromLeft(bw).toNearestInt());
            rec.removeFromLeft(18.0f);
            follow_.setBounds(rec.removeFromLeft(juce::jmin(rec.getWidth(), 44.0f + 10.0f + textWidth(uiFont(13.0f), tr(Str::RecordWithDaw)) + 4.0f)).toNearestInt());
        }
        void paint(juce::Graphics& g) override {
            const auto& p = paletteOf(*this);
            const auto r = getLocalBounds().toFloat().reduced(0.5f);
            g.setColour(p.inset);
            g.fillRoundedRectangle(r, 16.0f);
            g.setColour(p.hairline1);
            g.drawRoundedRectangle(r, 16.0f, 1.0f);
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f));
            g.drawText(trf(Str::OnChannel, { v_.name }), channel_, juce::Justification::centredRight, true);
            g.setColour(p.ink.withMultipliedAlpha(v_.on() ? 1.0f : 0.4f));
            g.setFont(uiFont(12.5f, Weight::Medium));
            g.drawText(tr(Str::AppLevel), levelLabel_, juce::Justification::centredLeft, true);
            g.drawText(tr(Str::DelayViewers), delayLabel_, juce::Justification::centredLeft, true);
            g.setColour(p.hairline2);
            g.fillRect(sep_);
        }
    private:
        struct FollowBox : juce::Component {
            Switch sw;
            juce::String text;
            FollowBox() { addAndMakeVisible(sw); }
            void resized() override { sw.setBounds(0, (getHeight() - 26) / 2, 44, 26); }
            void paint(juce::Graphics& g) override {
                g.setColour(paletteOf(*this).ink);
                g.setFont(uiFont(13.0f));
                g.drawText(text, getLocalBounds().withTrimmedLeft(54), juce::Justification::centredLeft, true);
            }
        };
        Menu sourceMenu() {
            Menu m(300);
            m.header(tr(Str::ChooseSource));
            const auto cur = v_.app;
            const int index = v_.index;
            auto& proc = ed_.proc();
            bool listed = false;
            for (const auto& a : AppCapture::listAudioApps()) {
                const juce::String exe(a.exe);
                listed = listed || exe.equalsIgnoreCase(cur);
                m.check(HubEditor::programLabel(exe), exe.equalsIgnoreCase(cur), [&proc, index, exe] { proc.chooseSourceApp(index, exe); });
                m.last().icon = icons::Icon::Window;
            }
            if (!listed && cur.isNotEmpty() && cur != kSystemAudio && cur != kLinkIn && !cur.startsWith("http")) {
                m.check(HubEditor::programLabel(cur), true, [] {});
                m.last().icon = icons::Icon::Window;
            }
            m.check(tr(Str::AppSystem), cur == kSystemAudio, [&proc, index] { proc.chooseSourceApp(index, kSystemAudio); });
            m.last().icon = icons::Icon::Monitor;
            m.check(tr(Str::SentInLegacy), cur == kLinkIn, [&proc, index] { proc.chooseSourceApp(index, kLinkIn); });
            m.last().icon = icons::Icon::Link;
            m.separator();
            juce::Component::SafePointer<AppCard> self(this);
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

        HubEditor& ed_;
        SourceView v_;
        SourcePicker picker_;
        LabelledSwitch power_;
        TextLine status_ { 12.5f, Weight::Regular, TextLine::Tone::Ink2 };
        MeterBar meter_ { 4.0f, true };
        LevelSlider level_;
        EditableValue levelValue_ { EditableValue::Kind::Db, -30.0f, 12.0f };
        juce::Slider delay_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        EditableValue delayValue_ { EditableValue::Kind::Ms, 0.0f, 500.0f };
        AudibleToggle you_ { AudibleToggle::Side::You }, viewers_ { AudibleToggle::Side::Viewers };
        GhostButton print_;
        FollowBox follow_;
        Touch touch_;
        bool updating_ = false;
        juce::Rectangle<float> channel_, levelLabel_, delayLabel_, sep_;
    };

    // the list of WAV takes in Documents\HEARASIDE\Recordings, newest first
    struct Takes : juce::Component {
        ProgramsPage* page = nullptr;
        void paint(juce::Graphics& g) override { page->paintTakes(g); }
        void mouseDrag(const juce::MouseEvent& e) override {
            if (dragging_ || e.getDistanceFromDragStart() < 6) return;
            const int i = page->takeAt(e.getMouseDownPosition().toFloat());
            if (i < 0) return;
            dragging_ = true;
            juce::DragAndDropContainer::performExternalDragDropOfFiles({ page->takes[size_t(i)].file.getFullPathName() }, false, this,
                                                                       [this] { dragging_ = false; });
        }
        void mouseUp(const juce::MouseEvent&) override { dragging_ = false; }
        void mouseMove(const juce::MouseEvent& e) override {
            setMouseCursor(page->takeAt(e.position) >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        }
        bool dragging_ = false;
    };

    void layoutList() {
        const int w = scroll_.contentWidth();
        int y = 0;
        for (auto* c : cards_) { c->setBounds(0, y, w, AppCard::idealHeight()); y += AppCard::idealHeight() + 12; }
        addBox_ = { 0.0f, float(y), float(w), 70.0f };
        howTo_.setBounds(juce::Rectangle<float>(addBox_.getRight() - 16.0f - float(howTo_.idealWidth()), addBox_.getCentreY() - 18.0f, float(howTo_.idealWidth()), 36.0f).toNearestInt());
        list_.removeChildComponent(&howTo_);
        list_.addAndMakeVisible(howTo_);
        list_.page = this;
        list_.setSize(w, y + 70 + 4);
        list_.repaint();
    }
    struct List : juce::Component {
        ProgramsPage* page = nullptr;
        void paint(juce::Graphics& g) override {
            if (page == nullptr) return;
            const auto& p = paletteOf(*this);
            const auto b = page->addBox_;
            juce::Path box;
            box.addRoundedRectangle(b.reduced(0.75f), 16.0f);
            juce::Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType(1.5f).createDashedStroke(dashed, box, dashes, 2);
            g.setColour(p.hairline3);
            g.fillPath(dashed);
            auto in = b.reduced(16.0f, 0.0f).withRight(float(page->howTo_.getX()) - 12.0f);
            icons::draw(g, icons::Icon::Plus, in.removeFromLeft(18.0f).withSizeKeepingCentre(18.0f, 18.0f), p.graphite);
            in.removeFromLeft(12.0f);
            const float th = wrappedHeight(uiFont(12.5f), tr(Str::AddSourceHint), in.getWidth(), 6.0f);
            drawWrapped(g, tr(Str::AddSourceHint), uiFont(12.5f), p.ink2, in.withSizeKeepingCentre(in.getWidth(), th), 6.0f, juce::Justification::left, 2);
        }
    };

    void scanTakes() {
        const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HEARASIDE").getChildFile("Recordings");
        auto files = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
        std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        if (files.size() > 40) files.removeRange(40, files.size());
        std::vector<TakeInfo> out;
        std::map<juce::String, int> counts;
        for (int i = files.size() - 1; i >= 0; --i) {   // oldest first, to number the takes
            TakeInfo t;
            t.file = files[i];
            const auto base = files[i].getFileNameWithoutExtension();
            t.label = base.length() > 20 ? base.dropLastCharacters(20).trim() : base;   // "<label> YYYY-MM-DD HH-MM-SS"
            t.number = ++counts[t.label];
            t.time = files[i].getLastModificationTime();
            const auto key = files[i].getFullPathName() + juce::String(files[i].getSize());
            if (auto it = durations_.find(key); it != durations_.end()) t.seconds = it->second;
            else {
                juce::WavAudioFormat wav;
                if (auto in = files[i].createInputStream())
                    if (auto r = std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(in.release(), true)))
                        t.seconds = r->sampleRate > 0 ? double(r->lengthInSamples) / r->sampleRate : 0.0;
                durations_[key] = t.seconds;
            }
            out.insert(out.begin(), t);
        }
        takes = std::move(out);
        layoutTakes();
    }
    void layoutTakes() {
        const int w = takesScroll_.contentWidth();
        const int n = int(recording_.size() + takes.size());
        takes_.page = this;
        takes_.setSize(w, juce::jmax(60, n * (52 + 8)));
        takes_.repaint();
    }
    int takeAt(juce::Point<float> pos) const {
        const int i = int(pos.y) / 60 - int(recording_.size());
        return i >= 0 && i < int(takes.size()) ? i : -1;
    }
    void paintTakes(juce::Graphics& g) {
        const auto& p = paletteOf(*this);
        const float w = float(takes_.getWidth());
        float y = 0.0f;
        if (recording_.empty() && takes.empty()) {
            g.setColour(p.graphite);
            g.setFont(uiFont(13.0f));
            g.drawText(tr(Str::TakeNone), juce::Rectangle<float>(0.0f, 0.0f, w, 40.0f), juce::Justification::centredLeft, false);
            return;
        }
        for (const auto& [name, secs] : recording_) {
            const juce::Rectangle<float> box(0.0f, y, w, 52.0f);
            y += 60.0f;
            g.setColour(p.paper.overlaidWith(p.offBg));
            g.fillRoundedRectangle(box, 12.0f);
            g.setColour(p.danger);
            g.drawRoundedRectangle(box.reduced(0.5f), 12.0f, 1.0f);
            auto in = box.reduced(14.0f, 8.0f);
            drawStatusDot(g, { in.getX() + 5.0f, in.getCentreY() }, Dot::Rec, p, 10.0f);
            in.removeFromLeft(24.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(13.0f, Weight::Medium));
            g.drawText(name, in.removeFromTop(18.0f), juce::Justification::centredLeft, true);
            g.setColour(p.danger);
            g.setFont(uiFont(12.0f));
            g.drawText(trf(Str::RecordingTime, { clock(secs) }), in, juce::Justification::centredLeft, true);
        }
        const auto today = juce::Time::getCurrentTime();
        for (const auto& t : takes) {
            const juce::Rectangle<float> box(0.0f, y, w, 52.0f);
            y += 60.0f;
            drawInset(g, box, 12.0f, p);
            auto in = box.reduced(12.0f, 8.0f);
            icons::draw(g, icons::Icon::Waveform, in.removeFromLeft(18.0f).withSizeKeepingCentre(18.0f, 18.0f), p.graphite);
            in.removeFromLeft(10.0f);
            auto drag = in.removeFromRight(66.0f).withSizeKeepingCentre(66.0f, 30.0f);
            juce::Path dbox;
            dbox.addRoundedRectangle(drag.reduced(0.5f), 9.0f);
            juce::Path dashed;
            const float dashes[] = { 3.0f, 3.0f };
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, dbox, dashes, 2);
            g.setColour(p.hairline3);
            g.fillPath(dashed);
            auto dl = drag.reduced(8.0f, 0.0f);
            icons::draw(g, icons::Icon::Grip, dl.removeFromLeft(14.0f).withSizeKeepingCentre(14.0f, 14.0f), p.graphite);
            g.setColour(p.ink2);
            g.setFont(uiFont(12.0f));
            g.drawText(tr(Str::DragWord), dl.withTrimmedLeft(4.0f), juce::Justification::centredLeft, false);
            in.removeFromRight(8.0f);
            g.setColour(p.ink);
            g.setFont(uiFont(13.0f, Weight::Medium));
            g.drawText(ellipsize(uiFont(13.0f, Weight::Medium), trf(Str::TakeName, { t.label, juce::String(t.number) }), in.getWidth()), in.removeFromTop(18.0f),
                       juce::Justification::centredLeft, false);
            const bool isToday = t.time.getYear() == today.getYear() && t.time.getDayOfYear() == today.getDayOfYear();
            const auto when = isToday ? trf(Str::TakeMetaToday, { clock(t.seconds), t.time.formatted("%H:%M") })
                                      : trf(Str::TakeMetaDay, { clock(t.seconds), t.time.formatted("%d %b %H:%M") });
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f));
            g.drawText(when, in, juce::Justification::centredLeft, true);
        }
    }

    void showHowTo() {
        struct HowTo : juce::Component {
            NumberedSteps steps;
            HowTo() {
                const auto daw = currentDaw();
                juce::StringArray s;
                if (daw == Daw::Cubase) s = { tr(Str::CubaseStep1), tr(Str::StudioOneStep3), tr(Str::CubaseStep2) };
                else if (daw == Daw::Reaper) s = { tr(Str::ReaperStep1), tr(Str::StudioOneStep3), tr(Str::ReaperStep2) };
                else if (daw == Daw::StudioOne) s = { tr(Str::StudioOneStep1), tr(Str::StudioOneStep2), tr(Str::StudioOneStep3), tr(Str::StudioOneStep4) };
                else s = { tr(Str::OtherDawStep1), tr(Str::StudioOneStep3), tr(Str::OtherDawStep2) };
                steps.setSteps(s);
                addAndMakeVisible(steps);
                setSize(340, 30 + steps.idealHeight(340));
            }
            void paint(juce::Graphics& g) override {
                g.setColour(paletteOf(*this).ink);
                g.setFont(uiFont(14.0f, Weight::SemiBold));
                g.drawText(tr(Str::HowToSetUp), juce::Rectangle<float>(0, 0, float(getWidth()), 20), juce::Justification::centredLeft, true);
            }
            void resized() override { steps.setBounds(0, 30, getWidth(), getHeight() - 30); }
        };
        ed.overlay().showPopover(std::make_unique<HowTo>(), howTo_, false);
    }

public:
    std::vector<TakeInfo> takes;

private:
    ScrollArea scroll_, takesScroll_;
    List list_;
    Takes takes_;
    juce::OwnedArray<AppCard> cards_;
    GhostButton openFolder_, howTo_;
    std::map<juce::String, double> durations_;
    std::vector<std::pair<juce::String, double>> recording_;
    size_t lastRecording_ = 0;
    int ticks_ = 0;
    juce::Rectangle<float> left_, right_, head_, takesHead_, takesFooter_, addBox_;
};

} // namespace

std::unique_ptr<HubPage> makeSharePage(HubEditor& e) { return std::make_unique<SharePage>(e); }
std::unique_ptr<HubPage> makeTrackPage(HubEditor& e, int slot) { return std::make_unique<TrackPage>(e, slot); }
std::unique_ptr<HubPage> makeTracksPage(HubEditor& e) { return std::make_unique<TracksPage>(e); }
std::unique_ptr<HubPage> makeProgramsPage(HubEditor& e) { return std::make_unique<ProgramsPage>(e); }

} // namespace hearaside
