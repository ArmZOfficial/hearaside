#include "ScrollArea.h"
#include "Overlay.h"

namespace hearaside {

ScrollArea::ScrollArea(int gutter) : gutter_(gutter) {
    setScrollBarsShown(false, false, true, false);   // our own bar; the wheel and keys still scroll
    setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    setSingleStepSizes(24, 24);
    addAndMakeVisible(fades_);
    addAndMakeVisible(thumb_);
    addMouseListener(&hover_, true);
}

ScrollArea::~ScrollArea() { removeMouseListener(&hover_); }

void ScrollArea::setContent(juce::Component& c) { setViewedComponent(&c, false); }

void ScrollArea::resized() {
    juce::Viewport::resized();
    fades_.setBounds(getLocalBounds().withTrimmedRight(gutter_));
    fades_.toFront(false);
    thumb_.toFront(false);
    layoutThumb();
}

void ScrollArea::visibleAreaChanged(const juce::Rectangle<int>&) {
    layoutThumb();
    fades_.repaint();
    if (auto* o = Overlay::find(*this)) if (o->isOpen()) {
        // a menu opened from inside this area no longer sits next to its button
        if (auto* a = o->anchor(); a != nullptr && isParentOf(a)) o->close();
    }
    if (onScroll) onScroll();
}

void ScrollArea::scrollToShow(juce::Rectangle<int> r) {
    const int top = getViewPositionY(), h = getViewHeight();
    if (r.getY() < top) setViewPosition(0, r.getY());
    else if (r.getBottom() > top + h) setViewPosition(0, r.getBottom() - h);
}

void ScrollArea::layoutThumb() {
    auto* c = getViewedComponent();
    const int ch = getHeight(), sh = c != nullptr ? c->getHeight() : 0;
    if (c == nullptr || sh <= ch + 1) { thumb_.setVisible(false); return; }
    track_ = juce::Rectangle<int>(getWidth() - 12, 8, 12, juce::jmax(0, ch - 16));
    const int len = juce::jmax(36, juce::roundToInt(float(track_.getHeight()) * float(ch) / float(sh)));
    const int top = juce::roundToInt(float(track_.getHeight() - len) * float(getViewPositionY()) / float(sh - ch));
    thumb_.setBounds(track_.getX(), track_.getY() + top, 12, len);
    thumb_.setVisible(true);
}

void ScrollArea::Fades::paint(juce::Graphics& g) {
    auto* c = area.getViewedComponent();
    if (c == nullptr) return;
    const int y = area.getViewPositionY(), h = area.getViewHeight();
    const auto col = area.fade_;
    if (y > 2 && area.fadeTop_ > 0) {
        juce::ColourGradient gr(col, 0, 0, col.withAlpha(0.0f), 0, float(area.fadeTop_), false);
        g.setGradientFill(gr);
        g.fillRect(0, 0, getWidth(), area.fadeTop_);
    }
    if (y + h < c->getHeight() - 2 && area.fadeBottom_ > 0) {
        const float b = float(getHeight());
        juce::ColourGradient gr(col.withAlpha(0.0f), 0, b - float(area.fadeBottom_), col, 0, b, false);
        g.setGradientFill(gr);
        g.fillRect(0, getHeight() - area.fadeBottom_, getWidth(), area.fadeBottom_);
    }
}

void ScrollArea::Thumb::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const bool over = isMouseOverOrDragging();
    const float w = over || dragging ? 7.0f : 5.0f;
    const float right = over || dragging ? 2.0f : 3.0f;
    const auto r = juce::Rectangle<float>(float(getWidth()) - right - w, 0.0f, w, float(getHeight()));
    g.setColour(dragging || over ? p.scrollThumbActive : hot ? p.scrollThumbHover : p.scrollThumb);
    g.fillRoundedRectangle(r, w * 0.5f);
}

void ScrollArea::Thumb::mouseDown(const juce::MouseEvent&) {
    dragging = true;
    startY = area.getViewPositionY();
    repaint();
}

void ScrollArea::Thumb::mouseDrag(const juce::MouseEvent& e) {
    auto* c = area.getViewedComponent();
    if (c == nullptr) return;
    const int room = juce::jmax(1, area.track_.getHeight() - getHeight());
    const float perPixel = float(c->getHeight() - area.getHeight()) / float(room);
    area.setViewPosition(area.getViewPositionX(), juce::roundToInt(float(startY) + float(e.getDistanceFromDragStartY()) * perPixel));
}

} // namespace hearaside
