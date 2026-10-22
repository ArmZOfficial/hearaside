#pragma once

#include "qrcodegen.hpp"
#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside {

// Clean vector QR Code component rendered with quiet zone on high-contrast background card.
class QrView : public juce::Component {
public:
    QrView() = default;

    void setContent(const juce::String& text) {
        if (text == text_) return;
        text_ = text;
        try {
            if (text.isNotEmpty()) {
                qr_ = std::make_unique<qrcodegen::QrCode>(
                    qrcodegen::QrCode::encodeText(text.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM));
            } else {
                qr_.reset();
            }
        } catch (...) {
            qr_.reset();
        }
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        // White rounded background card for crisp optical contrast with phone cameras
        g.setColour(juce::Colours::white);
        g.fillRoundedRectangle(r, 12.0f);

        if (!qr_) return;

        const int size = qr_->getSize();
        constexpr int border = 2; // quiet zone modules
        const int total = size + border * 2;
        const float box = std::min(r.getWidth(), r.getHeight()) - 16.0f;
        const float moduleSize = std::floor(box / float(total));
        if (moduleSize < 1.0f) return;

        const float qrPixelSize = moduleSize * float(total);
        const float startX = r.getX() + std::floor((r.getWidth() - qrPixelSize) * 0.5f) + moduleSize * float(border);
        const float startY = r.getY() + std::floor((r.getHeight() - qrPixelSize) * 0.5f) + moduleSize * float(border);

        g.setColour(juce::Colour(0xff111418));
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (qr_->getModule(x, y)) {
                    g.fillRect(startX + float(x) * moduleSize,
                               startY + float(y) * moduleSize,
                               moduleSize, moduleSize);
                }
            }
        }
    }

private:
    juce::String text_;
    std::unique_ptr<qrcodegen::QrCode> qr_;
};

} // namespace hearaside
