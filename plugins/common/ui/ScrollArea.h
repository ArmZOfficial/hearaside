// The one scrolling area of every screen (prompt 3.2 "ScrollBar"): no rail, a 5 px pill in the
// card's padding (~9 px from the card edge), darker on hover, 7 px while dragged, at least 36 px
// long; the top / bottom edge fades out only while there is more content that way; open menus
// close when it scrolls.
#pragma once

#include "Components.h"

namespace hearaside {

class ScrollArea : public juce::Viewport {
public:
    // gutter: room on the right for the bar (the area reaches into the card's padding by this much)
    explicit ScrollArea(int gutter = 14);
    ~ScrollArea() override;

    // The content: sized by the owner (width = getWidth() - gutter()).
    void setContent(juce::Component& c);
    int gutter() const noexcept { return gutter_; }
    int contentWidth() const noexcept { return juce::jmax(0, getWidth() - gutter_); }
    // Colour the content fades into at the edges (the card's surface).
    void setFadeColour(juce::Colour c) { fade_ = c; fades_.repaint(); }
    void setFades(int top, int bottom) { fadeTop_ = top; fadeBottom_ = bottom; fades_.repaint(); }
    void scrollToShow(juce::Rectangle<int> areaInContent);   // smallest scroll that shows it

    std::function<void()> onScroll;

    void resized() override;
    void visibleAreaChanged(const juce::Rectangle<int>&) override;

private:
    struct Fades : juce::Component {
        explicit Fades(ScrollArea& a) : area(a) { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics&) override;
        ScrollArea& area;
    };
    struct Thumb : juce::Component {
        explicit Thumb(ScrollArea& a) : area(a) { setRepaintsOnMouseActivity(true); }
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override { dragging = false; repaint(); }
        void setHot(bool h) { if (h != hot) { hot = h; repaint(); } }
        ScrollArea& area;
        bool hot = false, dragging = false;
        int startY = 0;
    };
    void layoutThumb();
    // hover anywhere in the area darkens the bar (a plain listener: no second wheel event)
    struct Hover : juce::MouseListener {
        explicit Hover(ScrollArea& a) : area(a) {}
        void mouseEnter(const juce::MouseEvent&) override { area.thumb_.setHot(true); }
        void mouseExit(const juce::MouseEvent&) override { area.thumb_.setHot(area.getLocalBounds().contains(area.getMouseXYRelative())); }
        ScrollArea& area;
    } hover_ { *this };

    int gutter_;
    juce::Colour fade_;
    int fadeTop_ = 20, fadeBottom_ = 28;
    Fades fades_ { *this };
    Thumb thumb_ { *this };
    juce::Rectangle<int> track_;
};

} // namespace hearaside
