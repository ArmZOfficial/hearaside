// HAS Karaoke's window frame, Discord-style: the title bar is the window's own colour, the HEARASIDE
// mark + name sit in the middle, flat line buttons on the right (close turns red when hovered).
#pragma once

#include "Components.h"
#include "LookAndFeel.h"

namespace hearaside::karaoke {

class KaraokeLookAndFeel : public hearaside::LookAndFeel {
public:
    static constexpr int kTitleBarH = 32;

    // The HEARASIDE mark from web/site/public/favicon.svg (512 x 512 units)
    static juce::Path mark() {
        juce::Path p;
        auto poly = [&p](std::initializer_list<float> xy) {
            const float* v = xy.begin();
            p.startNewSubPath(v[0], v[1]);
            for (size_t i = 2; i + 1 < xy.size(); i += 2) p.lineTo(v[i], v[i + 1]);
            p.closeSubPath();
        };
        poly({ 112, 72, 172, 72, 172, 153.37f, 208, 189.37f, 141.37f, 256, 208, 322.63f, 172, 358.63f, 172, 440, 112, 440 });
        poly({ 400, 72, 340, 72, 340, 153.37f, 304, 189.37f, 370.63f, 256, 304, 322.63f, 340, 358.63f, 340, 440, 400, 440 });
        poly({ 256, 164, 348, 256, 256, 348, 164, 256 });
        return p;
    }

    void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g, int w, int h,
                                    int, int, const juce::Image*, bool) override {
        const auto& p = pal();
        g.fillAll(p.paper);
        const auto f = uiFont(12.5f, Weight::Medium);
        const juce::String title = window.getName();
        const float iconS = 14.0f, gap = 8.0f;
        const float tw = juce::jmin(textWidth(f, title), float(w) - 3.0f * 46.0f * 2.0f);
        const float x = (float(w) - (iconS + gap + tw)) * 0.5f;
        auto m = mark();
        m.applyTransform(m.getTransformToScaleToFit({ x, (float(h) - iconS) * 0.5f, iconS, iconS }, true));
        g.setColour(p.ink);
        g.fillPath(m);
        g.setColour(p.ink2);
        g.setFont(f);
        g.drawText(title, juce::Rectangle<float>(x + iconS + gap, 0.0f, tw + 2.0f, float(h)), juce::Justification::centredLeft, true);
    }

    void drawResizableWindowBorder(juce::Graphics& g, int w, int h, const juce::BorderSize<int>&, juce::ResizableWindow&) override {
        g.setColour(pal().hairline2);
        g.drawRect(0, 0, w, h, 1);
    }

    juce::Button* createDocumentWindowButton(int type) override { return new TitleButton(type); }

    void positionDocumentWindowButtons(juce::DocumentWindow&, int x, int y, int w, int h,
                                       juce::Button* minimise, juce::Button* maximise, juce::Button* close, bool) override {
        int right = x + w;
        for (auto* b : { close, maximise, minimise }) {
            if (b == nullptr) continue;
            right -= 46;
            b->setBounds(right, y, 46, h);
        }
    }

private:
    class TitleButton : public juce::Button {
    public:
        explicit TitleButton(int type) : juce::Button({}), type_(type) {
            setWantsKeyboardFocus(false);
            setTitle(type == juce::DocumentWindow::closeButton ? "Close" : type == juce::DocumentWindow::maximiseButton ? "Maximise" : "Minimise");
        }

        void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
            const auto& p = paletteOf(*this);
            const auto r = getLocalBounds().toFloat();
            const bool close = type_ == juce::DocumentWindow::closeButton;
            const bool hot = highlighted || down;
            if (hot) {
                g.setColour(close ? juce::Colour(0xffc42b1c).withAlpha(down ? 0.85f : 1.0f) : p.ink.withAlpha(down ? 0.14f : 0.08f));
                g.fillRect(r);
            }
            const auto c = r.getCentre();
            const float s = 5.0f;
            juce::Path path;
            if (type_ == juce::DocumentWindow::minimiseButton) {
                path.startNewSubPath(c.x - s, c.y);
                path.lineTo(c.x + s, c.y);
            } else if (type_ == juce::DocumentWindow::maximiseButton) {
                const auto* dw = findParentComponentOfClass<juce::DocumentWindow>();
                if (dw != nullptr && dw->isFullScreen()) {   // restore: two overlapping squares
                    path.addRectangle(c.x - s, c.y - s + 2.0f, 2.0f * s - 2.0f, 2.0f * s - 2.0f);
                    path.startNewSubPath(c.x - s + 2.0f, c.y - s);
                    path.lineTo(c.x + s, c.y - s);
                    path.lineTo(c.x + s, c.y + s - 2.0f);
                } else {
                    path.addRectangle(c.x - s, c.y - s, 2.0f * s, 2.0f * s);
                }
            } else {
                path.startNewSubPath(c.x - s, c.y - s);
                path.lineTo(c.x + s, c.y + s);
                path.startNewSubPath(c.x + s, c.y - s);
                path.lineTo(c.x - s, c.y + s);
            }
            g.setColour(close && hot ? juce::Colours::white : p.ink2);
            g.strokePath(path, juce::PathStrokeType(1.1f));
        }

    private:
        int type_;
    };
};

} // namespace hearaside::karaoke
