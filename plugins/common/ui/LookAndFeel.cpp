#include "LookAndFeel.h"
#include "Components.h"

#include <BinaryData.h>

namespace hearaside {

namespace {

struct Fonts {
    Fonts() {
        regular  = juce::Typeface::createSystemTypefaceFor(HearasideAssets::Anuphan400_ttf, size_t(HearasideAssets::Anuphan400_ttfSize));
        medium   = juce::Typeface::createSystemTypefaceFor(HearasideAssets::Anuphan500_ttf, size_t(HearasideAssets::Anuphan500_ttfSize));
        semiBold = juce::Typeface::createSystemTypefaceFor(HearasideAssets::Anuphan600_ttf, size_t(HearasideAssets::Anuphan600_ttfSize));
    }
    juce::Typeface::Ptr get(Weight w) const {
        switch (w) {
            case Weight::Medium:   return medium;
            case Weight::SemiBold: return semiBold;
            case Weight::Regular:  break;
        }
        return regular;
    }
    juce::Typeface::Ptr regular, medium, semiBold;
};

const Fonts& fonts() {
    static juce::SharedResourcePointer<Fonts> f;   // lives while any plug-in in this process uses it
    return *f;
}

} // namespace

juce::Font uiFont(float px, Weight w) {
    return juce::Font(juce::FontOptions(fonts().get(w)).withPointHeight(px));
}

void drawFocusRing(juce::Graphics& g, juce::Rectangle<float> b, float radius, const theme::Palette& p) {
    g.setColour(p.ink);
    g.drawRoundedRectangle(b.expanded(3.0f), radius + 3.0f, 2.0f);
}

const theme::Palette& paletteOf(const juce::Component& c) {
    if (auto* lf = dynamic_cast<LookAndFeel*>(&c.getLookAndFeel())) return lf->pal();
    return theme::light();
}

LookAndFeel::LookAndFeel() {
    setDefaultSansSerifTypeface(fonts().regular);
    applyColours();
}

void LookAndFeel::setDark(bool dark) {
    dark_ = dark;
    pal_ = dark ? &theme::dark() : &theme::light();
    applyColours();
}

void LookAndFeel::applyColours() {
    const auto& p = *pal_;
    const auto opaqueInset = p.paper.overlaidWith(p.inset);
    setColour(juce::ResizableWindow::backgroundColourId, p.paper);
    setColour(juce::Label::textColourId, p.ink);
    setColour(juce::TextEditor::textColourId, p.ink);
    setColour(juce::TextEditor::backgroundColourId, opaqueInset);
    setColour(juce::TextEditor::highlightColourId, p.ink.withAlpha(0.18f));
    setColour(juce::TextEditor::highlightedTextColourId, p.ink);
    setColour(juce::TextEditor::outlineColourId, p.hairline3);
    setColour(juce::TextEditor::focusedOutlineColourId, p.ink);
    setColour(juce::CaretComponent::caretColourId, p.ink);
    setColour(juce::ComboBox::textColourId, p.ink);
    setColour(juce::ComboBox::backgroundColourId, opaqueInset);
    setColour(juce::ComboBox::arrowColourId, p.graphite);
    setColour(juce::ComboBox::outlineColourId, p.hairline3);
    setColour(juce::PopupMenu::backgroundColourId, p.paper);
    setColour(juce::PopupMenu::textColourId, p.ink);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, p.ink);
    setColour(juce::PopupMenu::highlightedTextColourId, p.onInk);
    setColour(juce::TooltipWindow::backgroundColourId, p.ink);
    setColour(juce::TooltipWindow::textColourId, p.onInk);
    setColour(juce::ScrollBar::thumbColourId, p.hairline3);
    setColour(juce::TextButton::buttonColourId, opaqueInset);
    setColour(juce::TextButton::buttonOnColourId, p.ink);
    setColour(juce::TextButton::textColourOffId, p.ink);
    setColour(juce::TextButton::textColourOnId, p.onInk);
    setColour(juce::ToggleButton::textColourId, p.ink);
    setColour(juce::Slider::thumbColourId, p.ink);
    setColour(juce::AlertWindow::backgroundColourId, p.paper);
    setColour(juce::AlertWindow::textColourId, p.ink);
    setColour(juce::AlertWindow::outlineColourId, p.hairline3);
    setColour(juce::DocumentWindow::backgroundColourId, p.paper);
}

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont(const juce::Font& f) {
    const auto style = f.getTypefaceStyle();
    if (f.isBold() || style.containsIgnoreCase("bold") || style.containsIgnoreCase("semi")) return fonts().semiBold;
    if (style.containsIgnoreCase("medium")) return fonts().medium;
    return fonts().regular;
}

void LookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float,
                                   float, juce::Slider::SliderStyle style, juce::Slider& s) {
    if (style != juce::Slider::LinearHorizontal) {
        LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, sliderPos, 0, 0, style, s);
        return;
    }
    const auto& p = *pal_;
    const float alpha = s.isEnabled() ? 1.0f : 0.35f;
    const float cy = float(y) + float(h) * 0.5f;
    const float left = float(x), right = float(x + w);
    const juce::Rectangle<float> rail(left, cy - 2.0f, right - left, 4.0f);

    g.setColour(p.sliderRail.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(rail, 2.0f);
    g.setColour(p.ink.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(rail.withRight(juce::jlimit(left, right, sliderPos)), 2.0f);

    const juce::Rectangle<float> thumb(sliderPos - 9.0f, cy - 9.0f, 18.0f, 18.0f);
    {
        juce::Path shadow;
        shadow.addEllipse(thumb);
        juce::DropShadow(juce::Colours::black.withAlpha(0.20f * alpha), 3, { 0, 1 }).drawForPath(g, shadow);
    }
    g.setColour((dark_ ? juce::Colour(0xffe9e9e5) : juce::Colours::white).withMultipliedAlpha(alpha));
    g.fillEllipse(thumb);
    g.setColour(juce::Colour(0x33181818).withMultipliedAlpha(alpha));
    g.drawEllipse(thumb.reduced(0.5f), 1.0f);

    if (s.hasKeyboardFocus(false)) {
        g.setColour(p.ink);
        g.drawEllipse(thumb.expanded(3.0f), 2.0f);
    }
}

void LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    const auto& p = *pal_;
    g.fillAll(p.paper);
    g.setColour(p.hairline3);
    g.drawRect(0, 0, width, height, 1);
}

void LookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                    bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                    const juce::String& text, const juce::String&, const juce::Drawable*,
                                    const juce::Colour*) {
    const auto& p = *pal_;
    if (isSeparator) {
        g.setColour(p.hairline2);
        g.fillRect(area.reduced(10, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }
    auto r = area.reduced(4, 1);
    if (isHighlighted && isActive) {
        g.setColour(p.ink);
        g.fillRoundedRectangle(r.toFloat(), 8.0f);
    }
    const auto fg = !isActive ? p.muted : (isHighlighted ? p.onInk : p.ink);
    g.setColour(fg);
    g.setFont(uiFont(13.0f));
    auto textArea = r.reduced(12, 0);
    if (isTicked) {
        auto tick = textArea.removeFromLeft(14).toFloat();
        juce::Path pth;
        pth.startNewSubPath(tick.getX() + 1, tick.getCentreY());
        pth.lineTo(tick.getX() + 5, tick.getCentreY() + 4);
        pth.lineTo(tick.getRight() - 1, tick.getCentreY() - 4);
        g.strokePath(pth, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        textArea.removeFromLeft(6);
    }
    if (hasSubMenu) {
        auto arrow = textArea.removeFromRight(10).toFloat();
        juce::Path pth;
        pth.startNewSubPath(arrow.getX() + 2, arrow.getCentreY() - 4);
        pth.lineTo(arrow.getRight() - 2, arrow.getCentreY());
        pth.lineTo(arrow.getX() + 2, arrow.getCentreY() + 4);
        g.strokePath(pth, juce::PathStrokeType(1.4f));
    }
    g.drawFittedText(text, textArea, juce::Justification::centredLeft, 1);
}

void LookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int, int& idealWidth,
                                            int& idealHeight) {
    if (isSeparator) { idealWidth = 50; idealHeight = 9; return; }
    idealHeight = 32;
    idealWidth = juce::GlyphArrangement::getStringWidthInt(uiFont(13.0f), text) + 52;
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                                   juce::Rectangle<int> parentArea) {
    const int w = juce::jmin(320, juce::GlyphArrangement::getStringWidthInt(uiFont(12.0f), tipText) + 24);
    const int h = juce::roundToInt(wrappedHeight(uiFont(12.0f), tipText, float(w - 20), 2.0f)) + 14;
    return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin(parentArea);
}

void LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) {
    const auto& p = *pal_;
    g.setColour(p.ink);
    g.fillRoundedRectangle(0, 0, float(width), float(height), 8.0f);
    drawWrapped(g, text, uiFont(12.0f), p.onInk, juce::Rectangle<float>(10, 7, float(width - 20), float(height - 14)), 2.0f);
}

void LookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                                bool isVertical, int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown) {
    const auto& p = *pal_;
    juce::Rectangle<int> thumb = isVertical ? juce::Rectangle<int>(x, thumbStart, width, thumbSize)
                                            : juce::Rectangle<int>(thumbStart, y, thumbSize, height);
    g.setColour(isMouseDown ? p.graphite : isMouseOver ? p.muted : p.hairline3);
    g.fillRoundedRectangle(thumb.reduced(2).toFloat(), 3.0f);
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& e) {
    g.setColour(e.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(0, 0, float(width), float(height), 10.0f);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& e) {
    const auto& p = *pal_;
    g.setColour(e.hasKeyboardFocus(true) ? p.ink : p.hairline3);
    g.drawRoundedRectangle(0.5f, 0.5f, float(width) - 1.0f, float(height) - 1.0f, 10.0f, e.hasKeyboardFocus(true) ? 2.0f : 1.0f);
}

void LookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) {
    const auto& p = *pal_;
    const auto r = juce::Rectangle<float>(0, 0, float(width), float(height));
    g.setColour(p.paper.overlaidWith(p.inset));
    g.fillRoundedRectangle(r, 10.0f);
    g.setColour(box.hasKeyboardFocus(true) ? p.ink : p.hairline3);
    g.drawRoundedRectangle(r.reduced(0.5f), 10.0f, 1.0f);
    juce::Path arrow;
    const float cx = float(width) - 16.0f, cy = float(height) * 0.5f;
    arrow.startNewSubPath(cx - 4, cy - 2);
    arrow.lineTo(cx, cy + 2);
    arrow.lineTo(cx + 4, cy - 2);
    g.setColour(p.graphite);
    g.strokePath(arrow, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void LookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label) {
    label.setBounds(10, 1, box.getWidth() - 34, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

void LookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool) {
    // rendered as the design-system switch with the label on the left
    const auto& p = *pal_;
    auto r = b.getLocalBounds().toFloat();
    auto sw = r.removeFromRight(46.0f).withSizeKeepingCentre(46.0f, 28.0f);
    const bool on = b.getToggleState();
    g.setColour(on ? p.ink : p.switchOff);
    g.fillRoundedRectangle(sw, 14.0f);
    const float kx = on ? sw.getRight() - 25.0f : sw.getX() + 3.0f;
    g.setColour(dark_ && on ? p.onInk : juce::Colours::white);
    g.fillEllipse(kx, sw.getY() + 3.0f, 22.0f, 22.0f);
    g.setColour(highlighted ? p.ink : p.ink.withAlpha(0.92f));
    g.setFont(uiFont(13.0f));
    g.drawFittedText(b.getButtonText(), r.toNearestInt().withTrimmedRight(10), juce::Justification::centredLeft, 2);
    if (b.hasKeyboardFocus(false)) drawFocusRing(g, sw, 14.0f, p);
}

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down) {
    const auto& p = *pal_;
    const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    const bool on = b.getToggleState();
    auto fill = on ? p.ink : p.paper.overlaidWith(p.inset);
    if (down) fill = fill.overlaidWith(p.ink.withAlpha(0.10f));
    else if (highlighted) fill = fill.overlaidWith(p.ink.withAlpha(0.05f));
    g.setColour(fill);
    g.fillRoundedRectangle(r, 10.0f);
    g.setColour(on ? p.ink : p.hairline3);
    g.drawRoundedRectangle(r, 10.0f, 1.0f);
    if (b.hasKeyboardFocus(false)) drawFocusRing(g, r, 10.0f, p);
}

void LookAndFeel::drawLabel(juce::Graphics& g, juce::Label& l) {
    if (l.isBeingEdited()) return;
    g.setColour(l.findColour(juce::Label::textColourId).withMultipliedAlpha(l.isEnabled() ? 1.0f : 0.5f));
    g.setFont(l.getFont());
    g.drawFittedText(l.getText(), l.getBorderSize().subtractedFrom(l.getLocalBounds()), l.getJustificationType(),
                     juce::jmax(1, int(float(l.getHeight()) / l.getFont().getHeight())), l.getMinimumHorizontalScale());
}

} // namespace hearaside
