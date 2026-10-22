#pragma once

#include "HubProcessor.h"
#include "HubEditor.h"
#include "ui/Components.h"
#include "ui/Controls.h"
#include "ui/QrView.h"
#include "ui/Overlay.h"

namespace hearaside {

// Modal / popover displayed when clicking "เริ่มห้องคาราโอเกะ"
// Shows room readiness, friend join URL, high-contrast QR code for instant mobile scan,
// and audio/OBS status checks.
class KaraokeRoomPanel : public juce::Component {
public:
    KaraokeRoomPanel(HubEditor& ed, uint32_t friendId) : ed_(ed), friendId_(friendId) {
        setSize(340, 480);
        addAndMakeVisible(qr_);
        addAndMakeVisible(linkField_);
        addAndMakeVisible(copyBtn_);
        addAndMakeVisible(closeBtn_);

        copyBtn_.setButtonText(tr(Str::Copy));
        copyBtn_.setIcon(icons::Icon::Link);
        copyBtn_.onClick = [this] {
            const auto link = linkField_.getText();
            if (link.isEmpty()) return;
            juce::SystemClipboard::copyTextToClipboard(link);
            copyBtn_.setButtonText(tr(Str::Copied));
            juce::Component::SafePointer<KaraokeRoomPanel> sp(this);
            juce::Timer::callAfterDelay(1400, [sp] {
                if (sp) sp->copyBtn_.setButtonText(tr(Str::Copy));
            });
            ed_.toast(tr(Str::Copied));
        };

        closeBtn_.setButtonText(tr(Str::Close));
        closeBtn_.onClick = [this] {
            if (auto* ov = Overlay::find(*this)) ov->close();
        };

        refreshLink();
    }

    void refreshLink() {
        const auto link = ed_.proc().friendLink(friendId_);
        linkField_.setLink(link);
        qr_.setContent(link);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        auto r = getLocalBounds().toFloat().reduced(16.0f, 14.0f);

        // Header Title
        g.setColour(p.ink);
        g.setFont(uiFont(16.0f, Weight::SemiBold));
        g.drawText(tr(Str::KaraokeRoomTitle), r.removeFromTop(24.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(8.0f);

        // Status 1: Buffer Check
        const int block = ed_.proc().latency().block;
        const bool bufferOk = block <= 256;
        const Dot bufDot = bufferOk ? Dot::Ok : Dot::Warn;
        const juce::String bufText = bufferOk ? trf(Str::KaraokeBufferOk, { juce::String(block) })
                                              : trf(Str::KaraokeBufferHigh, { juce::String(block) });
        auto row1 = r.removeFromTop(20.0f);
        drawStatusDot(g, { row1.getX() + 6.0f, row1.getCentreY() }, bufDot, p);
        g.setColour(bufferOk ? p.ink2 : p.warn);
        g.setFont(uiFont(12.0f));
        g.drawText(bufText, row1.withTrimmedLeft(18.0f), juce::Justification::centredLeft, false);
        r.removeFromTop(4.0f);

        // Status 2: OBS Check
        const bool obs = ed_.proc().obsConnected();
        const Dot obsDot = obs ? Dot::Ok : Dot::Muted;
        const juce::String obsText = obs ? tr(Str::KaraokeObsOk) : tr(Str::KaraokeObsNotConnected);
        auto row2 = r.removeFromTop(20.0f);
        drawStatusDot(g, { row2.getX() + 6.0f, row2.getCentreY() }, obsDot, p);
        g.setColour(p.ink2);
        g.setFont(uiFont(12.0f));
        g.drawText(obsText, row2.withTrimmedLeft(18.0f), juce::Justification::centredLeft, false);
        r.removeFromTop(12.0f);

        // QR Code area
        auto qrArea = r.removeFromTop(170.0f);
        qr_.setBounds(qrArea.withSizeKeepingCentre(160.0f, 160.0f).toNearestInt());
        r.removeFromTop(6.0f);

        // Scan QR caption
        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f));
        g.drawText(tr(Str::KaraokeScanQr), r.removeFromTop(18.0f), juce::Justification::centred, false);
        r.removeFromTop(10.0f);

        // Link section label
        g.setColour(p.ink2);
        g.setFont(uiFont(12.0f, Weight::Medium));
        g.drawText(tr(Str::KaraokeRoomLink), r.removeFromTop(18.0f), juce::Justification::centredLeft, false);
        r.removeFromTop(4.0f);

        // Link input + copy button row
        auto linkRow = r.removeFromTop(36.0f);
        copyBtn_.setBounds(linkRow.removeFromRight(76.0f).toNearestInt());
        linkRow.removeFromRight(8.0f);
        linkField_.setBounds(linkRow.toNearestInt());
        r.removeFromTop(14.0f);

        // Close button
        closeBtn_.setBounds(r.removeFromTop(32.0f).withSizeKeepingCentre(100.0f, 32.0f).toNearestInt());
    }

private:
    HubEditor& ed_;
    uint32_t friendId_ = 0;
    QrView qr_;
    struct ReadOnlyLink : TextField {
        ReadOnlyLink() {
            setReadOnly(true);
            setCaretVisible(false);
            setFont(juce::Font(juce::FontOptions("Consolas", 12.0f, juce::Font::plain)));
        }
        void setLink(const juce::String& l) { if (l != getText()) setText(l, false); }
    } linkField_;
    GhostButton copyBtn_ { {}, GhostButton::Style::Solid };
    GhostButton closeBtn_ { {}, GhostButton::Style::Ghost };
};

} // namespace hearaside
