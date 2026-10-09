// Text <-> value for every number the user can see or type (EditableValue, PanSlider, names).
// Pure functions (no GUI), checked by `ui-snapshot --test-ui`.
#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace hearaside::valuetext {

inline constexpr float kFloorDb = -30.0f;   // a level slider's bottom, shown as −∞

// "0.0 dB", "−3.0 dB" (U+2212), "+2.0 dB", "−∞ dB" (at or below floorDb)
juce::String formatDb(float db, float floorDb = kFloorDb, bool unit = true);
// what the field holds when it opens for typing: "-inf", "+2.0", "-3.0", "0.0"
juce::String editDb(float db, float floorDb = kFloorDb);
// "-3", "−3", "-3 dB", "+2", "2,5", "-inf", "∞", "-∞" -> value clamped to [lo, hi]; "-inf" = lo.
// Nothing usable -> nullopt (the field keeps the old value).
std::optional<float> parseDb(const juce::String& text, float lo, float hi);

juce::String formatMs(float ms);                                  // "40 ms"
std::optional<float> parseMs(const juce::String& text, float lo, float hi);   // "40", "40 ms", "40,5"

// pan -100 (left) .. 100 (right): "C", "L40", "R25"
juce::String formatPan(float pan);
// "L40", "l 40", "C", "c", "R25", "-40", "40", "center" -> -100..100; nullopt when unreadable
std::optional<float> parsePan(const juce::String& text);

juce::String formatPercent(float fraction01);                     // "62%"
std::optional<float> parsePercent(const juce::String& text, float lo, float hi);   // "62", "62%" -> 0.62

// The longest prefix of `s` whose UTF-8 form fits in maxBytes, cut at a code point boundary.
juce::String truncateUtf8(const juce::String& s, int maxBytes = 63);

// "3 of 8", initials ("Mint" -> "M"), and the like
juce::String initialOf(const juce::String& name);

} // namespace hearaside::valuetext
