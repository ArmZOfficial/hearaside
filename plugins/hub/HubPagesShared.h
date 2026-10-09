// Pieces shared by the Hub's pages (part 2 of the pages is in HubPages2.cpp).
#pragma once

#include "HubPages.h"
#include "Links.h"

namespace hearaside {

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
