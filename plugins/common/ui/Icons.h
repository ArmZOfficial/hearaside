// Stroke icons from the design export (24 x 24 viewBox, stroke 1.8, round caps).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside::icons {

enum class Icon {
    Headphones, Broadcast, SpeakerOff, SpeakerOn, Sliders, Warning, More, Solo, Power, Record, Drag, Folder, Link, Check,
    Info, Close, Window, Monitor, Person, Plus, Waveform, Grip, Appearance, AudioBars, Connection, Spread, Play,
    ChevronLeft, ChevronRight, ChevronDown, Refresh, Count
};

const juce::Path& path(Icon);
// strike: the diagonal line drawn over "off" icons (AudibleToggle)
void draw(juce::Graphics&, Icon, juce::Rectangle<float> bounds, juce::Colour, float strokeWidth = 1.8f, bool strike = false);

} // namespace hearaside::icons
