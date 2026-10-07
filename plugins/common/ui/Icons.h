// Stroke icons from the design (24 x 24 viewBox, stroke 1.8, round caps).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside::icons {

enum class Icon { Headphones, Broadcast, SpeakerOff, Sliders, Warning, More, Solo };

const juce::Path& path(Icon);
void draw(juce::Graphics&, Icon, juce::Rectangle<float> bounds, juce::Colour, float strokeWidth = 1.8f);

} // namespace hearaside::icons
