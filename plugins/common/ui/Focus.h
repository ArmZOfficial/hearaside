// Keyboard focus, quiet (prompt 3.2 "Focus"): no black boxes. A mouse click shows nothing; Tab
// shows a soft 3 px ring around the control's own shape; a text field being typed in always shows
// it. One overlay per editor draws the ring (outside the control, so nothing has to leave room).
#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside {

// true while focus moved by keyboard (Tab / arrows) since the last mouse press
bool keyboardFocusMode();
// the control has focus and it should show (keyboard mode, or a text field)
bool focusVisible(const juce::Component&);

// How the ring hugs a control: corner radius (< 0 = a pill / circle), or none (it draws its own).
void setFocusShape(juce::Component&, float radius);
void setFocusDrawnBySelf(juce::Component&);
void setFocusAlways(juce::Component&);   // text fields: ring whenever focused

class FocusRingOverlay : public juce::Component, private juce::FocusChangeListener, private juce::Timer {
public:
    explicit FocusRingOverlay(juce::Component& root);
    ~FocusRingOverlay() override;
    void paint(juce::Graphics&) override;
    bool hitTest(int, int) override { return false; }

private:
    void globalFocusChanged(juce::Component*) override;
    void timerCallback() override;
    std::optional<std::pair<juce::Rectangle<float>, float>> ring() const;   // area in my coordinates + radius

    juce::Component& root_;
    juce::Component::SafePointer<juce::Component> focused_;
    juce::Rectangle<float> last_;
};

} // namespace hearaside
