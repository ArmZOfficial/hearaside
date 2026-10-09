// Pieces shared by the Hub's pages (part 2 of the pages is in HubPages2.cpp).
#pragma once

#include "HubPages.h"
#include "Links.h"
#include "FriendDirectory.h"

namespace hearaside {

// Friends of the room in a program picker (Program audio row, Manage program audio, App Audio): "Mint · friend"
// with a status dot. The old single send-in link is only listed while an old project still uses it.
inline void addFriendItems(Menu& m, const juce::String& cur, const std::function<void(const juce::String&)>& choose) {
    const auto friends = FriendDirectory::all();
    bool curListed = false;
    for (const auto& f : friends) {
        const auto app = FriendDirectory::appFor(f.id);
        curListed = curListed || app == cur;
        m.check(trf(Str::FriendItem, { f.name }), app == cur, [choose, app] { choose(app); });
        m.last().icon = icons::Icon::Person;
        m.last().dot = f.live() ? Dot::Ok : f.state == ssbus::kFriendWaiting ? Dot::Warn : Dot::Muted;
    }
    if (FriendDirectory::isFriendApp(cur) && !curListed) {   // chosen, but not in the room any more
        m.check(trf(Str::FriendItem, { FriendDirectory::nameOf(0).isNotEmpty() ? juce::String() : tr(Str::FriendWord) }), true, [] {});
        m.last().icon = icons::Icon::Person;
    }
    if (cur == "*link*") {
        m.check(tr(Str::SentInLegacy), true, [] {});
        m.last().icon = icons::Icon::Link;
    }
}

// What a program row says about a friend source (capture: AppCapture::State as a number); dot out
inline juce::String friendProgramStatus(const juce::String& app, uint32_t capture, double takeShiftMs, Dot& dot) {
    uint32_t id = 0;
    FriendDirectory::isFriendApp(app, &id);
    FriendDirectory::Info f;
    const bool known = FriendDirectory::find(id, f);
    const auto name = known ? f.name : tr(Str::FriendWord);
    switch (capture) {
        case 2:
            dot = Dot::Ok;
            return takeShiftMs >= 1.0 ? trf(Str::FriendTakeShift, { name, juce::String(juce::roundToInt(takeShiftMs)) }) : trf(Str::ReceivingFriend, { name });
        case 4:
            dot = Dot::Warn;
            return trf(Str::FriendGone, { name });
        case 0:
            dot = Dot::Muted;
            return tr(Str::AppNone);
        default:
            dot = Dot::Warn;
            return trf(Str::FriendWaiting, { name });
    }
}

std::unique_ptr<HubPage> makeSharePage(HubEditor&);
std::unique_ptr<HubPage> makeTrackPage(HubEditor&, int slot);
std::unique_ptr<HubPage> makeTracksPage(HubEditor&);
std::unique_ptr<HubPage> makeProgramsPage(HubEditor&);

// "Receive someone's link…" in a popover (Program audio row, Share page): the link goes to App Audio
class ReceiveLinkPanel : public juce::Component {
public:
    explicit ReceiveLinkPanel(std::function<bool(const juce::String&)> connect) : connect_(std::move(connect)) {
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
        const auto url = field_.getText().trim();
        if (links::isListen(url) && connect_(url)) return;
        error_ = links::isListen(url) ? tr(Str::ReceiveNoApp) : tr(Str::AppLinkBad);
        repaint();
    }
    std::function<bool(const juce::String&)> connect_;
    TextField field_;
    GhostButton button_ { tr(Str::Connect), GhostButton::Style::Solid };
    juce::String error_;
};


} // namespace hearaside
