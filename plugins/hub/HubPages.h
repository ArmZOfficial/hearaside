// The Hub's sub-pages (prompt 3.4): each fills the window under the header, has Back + Mute in
// the header (drawn by HubEditor) and lays out its own glass cards.
#pragma once

#include "HubEditor.h"

namespace hearaside {

std::unique_ptr<HubPage> makeHubPage(HubEditor&, HubEditor::Page, int slot);

// Settings (prompt 3.4 "Settings"): a section menu on the left, the section on the right.
class HubSettingsPage : public HubPage {
public:
    explicit HubSettingsPage(HubEditor&);
    ~HubSettingsPage() override;
    void select(int section);
    int section() const noexcept { return section_; }
    std::vector<GlassCard> cards() const override;
    void tick() override;
    void refreshTexts() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Nav : juce::Button {
        Nav(icons::Icon i) : juce::Button({}), icon(i) { setWantsKeyboardFocus(true); setMouseCursor(juce::MouseCursor::PointingHandCursor); setFocusShape(*this, 12.0f); }
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;
        icons::Icon icon;
        bool on = false;
    };
    void build();
    void layoutSection();

    int section_ = 0;
    juce::OwnedArray<Nav> nav_;
    ScrollArea scroll_ { 22 };
    juce::Component holder_;
    std::unique_ptr<juce::Component> body_;   // the section's rows
    juce::Rectangle<float> navCard_, card_, note_, title_;
};

} // namespace hearaside
