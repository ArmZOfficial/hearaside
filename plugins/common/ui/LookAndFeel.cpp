#include "LookAndFeel.h"
#include "Components.h"
#include "Icons.h"

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
    g.setColour(p.focusRing);
    g.drawRoundedRectangle(b.expanded(1.5f), radius + 1.5f, 3.0f);
}

void drawCardShadow(juce::Graphics& g, juce::Rectangle<float> r, float radius, float strength) {
    juce::Path shape;
    shape.addRoundedRectangle(r, radius);
    juce::DropShadow(juce::Colours::black.withAlpha(0.06f * strength), 6, { 0, 2 }).drawForPath(g, shape);
    juce::DropShadow(juce::Colours::black.withAlpha(0.14f * strength), 28, { 0, 14 }).drawForPath(g, shape);
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
    const auto field = p.paper.overlaidWith(p.offBg);
    setColour(juce::ResizableWindow::backgroundColourId, p.paper);
    setColour(juce::Label::textColourId, p.ink);
    setColour(juce::TextEditor::textColourId, p.ink);
    setColour(juce::TextEditor::backgroundColourId, field);
    setColour(juce::TextEditor::highlightColourId, p.ink.withAlpha(0.16f));
    setColour(juce::TextEditor::highlightedTextColourId, p.ink);
    setColour(juce::TextEditor::outlineColourId, p.hairline3);
    setColour(juce::TextEditor::focusedOutlineColourId, p.graphite);
    setColour(juce::CaretComponent::caretColourId, p.ink);
    setColour(juce::ComboBox::textColourId, p.ink);
    setColour(juce::ComboBox::backgroundColourId, field);
    setColour(juce::ComboBox::arrowColourId, p.graphite);
    setColour(juce::ComboBox::outlineColourId, p.hairline3);
    // not quite opaque: the menu window is then transparent and gets rounded corners
    setColour(juce::PopupMenu::backgroundColourId, p.menuBg.withAlpha(0.995f));
    setColour(juce::PopupMenu::textColourId, p.ink);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, p.inset);
    setColour(juce::PopupMenu::highlightedTextColourId, p.ink);
    setColour(juce::PopupMenu::headerTextColourId, p.graphite);
    setColour(juce::TooltipWindow::backgroundColourId, p.ink);
    setColour(juce::TooltipWindow::textColourId, p.onInk);
    setColour(juce::ScrollBar::thumbColourId, p.scrollThumb);
    setColour(juce::TextButton::buttonColourId, field);
    setColour(juce::TextButton::buttonOnColourId, p.ink);
    setColour(juce::TextButton::textColourOffId, p.ink);
    setColour(juce::TextButton::textColourOnId, p.onInk);
    setColour(juce::ToggleButton::textColourId, p.ink);
    setColour(juce::Slider::thumbColourId, p.thumb);
    setColour(juce::AlertWindow::backgroundColourId, p.menuBg);
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
    const bool dim = bool(s.getProperties()[sliderlook::dim]) || !s.isEnabled();
    const float alpha = dim ? 0.36f : 1.0f;
    const float r = float(getSliderThumbRadius(s));
    const float cy = float(y) + float(h) * 0.5f;
    const float left = float(x) - r, right = float(x + w) + r;   // the rail runs under the whole thumb travel
    const juce::Rectangle<float> rail(left, cy - 2.0f, right - left, 4.0f);

    g.setColour(p.sliderRail.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(rail, 2.0f);
    g.setColour(p.ink.withMultipliedAlpha(alpha));
    if (bool(s.getProperties()[sliderlook::fromCentre])) {
        const float mid = float(x) + float(w) * 0.5f;
        const float a = juce::jmin(mid, sliderPos), b = juce::jmax(mid, sliderPos);
        if (b - a > 0.5f) g.fillRoundedRectangle(juce::Rectangle<float>(a, cy - 2.0f, b - a, 4.0f), 2.0f);
    } else {
        g.fillRoundedRectangle(rail.withRight(juce::jlimit(left, right, sliderPos)), 2.0f);
    }

    const juce::Rectangle<float> thumb(sliderPos - r, cy - r, r * 2.0f, r * 2.0f);
    if (s.hasKeyboardFocus(false) && keyboardFocusMode()) {
        g.setColour(p.focusRing);
        g.fillEllipse(thumb.expanded(4.0f));
    }
    drawRoundShadow(g, thumb, 0.22f * alpha, 3);
    g.setColour(p.thumb.withMultipliedAlpha(alpha));
    g.fillEllipse(thumb);
    g.setColour(juce::Colours::black.withAlpha(0.18f * alpha));
    g.drawEllipse(thumb.reduced(0.5f), 1.0f);
}

void LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    const auto& p = *pal_;
    const auto r = juce::Rectangle<float>(0, 0, float(width), float(height)).reduced(0.5f);
    g.setColour(p.menuBg);
    g.fillRoundedRectangle(r, 14.0f);
    g.setColour(p.hairline2);
    g.drawRoundedRectangle(r, 14.0f, 1.0f);
}

void LookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                    bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                    const juce::String& text, const juce::String& value, const juce::Drawable*,
                                    const juce::Colour* textColour) {
    const auto& p = *pal_;
    if (isSeparator) {
        g.setColour(p.hairline2);
        g.fillRect(area.reduced(6, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }
    auto r = area.toFloat();
    if (isHighlighted && isActive) {
        g.setColour(p.inset);
        g.fillRoundedRectangle(r, 9.0f);
    }
    const auto fg = !isActive ? p.muted : textColour != nullptr ? *textColour : p.ink;
    auto inner = r.reduced(10.0f, 0.0f);
    if (isTicked) {
        icons::draw(g, icons::Icon::Check, inner.removeFromRight(16.0f).withSizeKeepingCentre(16.0f, 16.0f), fg, 2.0f);
        inner.removeFromRight(8.0f);
    }
    if (hasSubMenu) {
        icons::draw(g, icons::Icon::ChevronRight, inner.removeFromRight(14.0f).withSizeKeepingCentre(14.0f, 14.0f), p.graphite, 2.0f);
        inner.removeFromRight(6.0f);
    }
    if (value.isNotEmpty()) {
        const auto vf = uiFont(12.0f);
        const float vw = textWidth(vf, value) + 2.0f;
        g.setColour(p.graphite);
        g.setFont(vf);
        g.drawText(value, inner.removeFromRight(vw), juce::Justification::centredRight, false);
        inner.removeFromRight(12.0f);
    }
    g.setColour(fg);
    g.setFont(uiFont(13.0f, isTicked ? Weight::SemiBold : Weight::Regular));
    g.drawText(text, inner, juce::Justification::centredLeft, true);
}

void LookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name) {
    g.setColour(pal_->graphite);
    g.setFont(uiFont(11.5f));
    g.drawText(name, area.toFloat().reduced(10.0f, 0.0f), juce::Justification::bottomLeft, true);
}

void LookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int, int& idealWidth,
                                            int& idealHeight) {
    if (isSeparator) { idealWidth = 50; idealHeight = 11; return; }
    idealHeight = 36;
    idealWidth = juce::GlyphArrangement::getStringWidthInt(uiFont(13.0f, Weight::SemiBold), text) + 64;
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                                   juce::Rectangle<int> parentArea) {
    float longest = 0.0f;
    for (const auto& line : juce::StringArray::fromLines(tipText)) longest = juce::jmax(longest, textWidth(uiFont(12.0f), line));
    const int w = juce::jmin(320, juce::roundToInt(longest) + 24);
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
    // no rail, no arrows: a 5 px pill that darkens on hover and widens to 7 px while dragged
    const auto& p = *pal_;
    if (thumbSize <= 0) return;
    const float t = isMouseDown ? 7.0f : 5.0f;
    const float edge = isMouseDown ? 2.0f : 3.0f;
    const auto thumb = isVertical ? juce::Rectangle<float>(float(x + width) - edge - t, float(thumbStart), t, float(thumbSize))
                                  : juce::Rectangle<float>(float(thumbStart), float(y + height) - edge - t, float(thumbSize), t);
    g.setColour(isMouseDown ? p.scrollThumbActive : isMouseOver ? p.scrollThumbHover : p.scrollThumb);
    g.fillRoundedRectangle(thumb, t * 0.5f);
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& e) {
    const auto& p = *pal_;
    const float radius = e.getProperties().getWithDefault("hsRadius", 10.0f);
    g.setColour(e.getProperties().contains("hsInline") ? p.menuBg : e.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(0, 0, float(width), float(height), radius);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& e) {
    const auto& p = *pal_;
    const float radius = e.getProperties().getWithDefault("hsRadius", 10.0f);
    g.setColour(e.hasKeyboardFocus(true) ? p.graphite : p.hairline3);
    g.drawRoundedRectangle(0.5f, 0.5f, float(width) - 1.0f, float(height) - 1.0f, radius, 1.0f);
}

void LookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) {
    const auto& p = *pal_;
    const auto r = juce::Rectangle<float>(0, 0, float(width), float(height)).reduced(0.5f);
    const bool open = box.isPopupActive();
    g.setColour(p.paper.overlaidWith(p.offBg));
    g.fillRoundedRectangle(r, 10.0f);
    g.setColour(open ? p.graphite : p.hairline3);
    g.drawRoundedRectangle(r, 10.0f, 1.0f);
    if (open) drawFocusRing(g, r.reduced(2.0f), 8.0f, p);
    const auto chev = juce::Rectangle<float>(float(width) - 26.0f, float(height) * 0.5f - 7.0f, 14.0f, 14.0f);
    if (open) {
        g.saveState();
        g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::pi, chev.getCentreX(), chev.getCentreY()));
        icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
        g.restoreState();
    } else {
        icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
    }
}

void LookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label) {
    label.setBounds(10, 1, box.getWidth() - 40, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

void LookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool) {
    // rendered as the design-system switch (36 x 20) with the label on the left
    const auto& p = *pal_;
    auto r = b.getLocalBounds().toFloat();
    auto sw = r.removeFromRight(36.0f).withSizeKeepingCentre(36.0f, 20.0f);
    const bool on = b.getToggleState();
    g.setColour(on ? p.ink : p.switchOff);
    g.fillRoundedRectangle(sw, 10.0f);
    g.setColour(on ? p.onInk : juce::Colours::white);
    g.fillEllipse(on ? sw.getRight() - 18.0f : sw.getX() + 2.0f, sw.getY() + 2.0f, 16.0f, 16.0f);
    g.setColour(highlighted ? p.ink : p.ink.withAlpha(0.92f));
    g.setFont(uiFont(13.0f));
    g.drawFittedText(b.getButtonText(), r.toNearestInt().withTrimmedRight(10), juce::Justification::centredLeft, 2);
}

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down) {
    // the "ghost" button of the design (36 high, radius 12); toggled = solid ink
    const auto& p = *pal_;
    const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    const bool on = b.getToggleState();
    auto fill = on ? p.ink : (highlighted ? p.paper.overlaidWith(p.offBg) : p.paper.overlaidWith(p.chipBg));
    if (down) fill = fill.overlaidWith(p.ink.withAlpha(0.08f));
    g.setColour(fill);
    g.fillRoundedRectangle(r, 12.0f);
    g.setColour(on ? p.ink : p.hairline3);
    g.drawRoundedRectangle(r, 12.0f, 1.0f);
}

void LookAndFeel::drawLabel(juce::Graphics& g, juce::Label& l) {
    if (l.isBeingEdited()) return;
    g.setColour(l.findColour(juce::Label::textColourId).withMultipliedAlpha(l.isEnabled() ? 1.0f : 0.5f));
    g.setFont(l.getFont());
    g.drawFittedText(l.getText(), l.getBorderSize().subtractedFrom(l.getLocalBounds()), l.getJustificationType(),
                     juce::jmax(1, int(float(l.getHeight()) / l.getFont().getHeight())), l.getMinimumHorizontalScale());
}

void LookAndFeel::drawCallOutBoxBackground(juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&) {
    const auto& p = *pal_;
    juce::DropShadow(juce::Colours::black.withAlpha(0.16f), 18, { 0, 8 }).drawForPath(g, path);
    g.setColour(p.menuBg);
    g.fillPath(path);
    g.setColour(p.hairline2);
    g.strokePath(path, juce::PathStrokeType(1.0f));
}

} // namespace hearaside
