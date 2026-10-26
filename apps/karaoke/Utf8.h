#pragma once

#include <juce_core/juce_core.h>

// The Thai text in this app is written inline. juce::String(const char*) reads the bytes as ASCII, so a plain
// "ไทย" shows up as "à¸à¸..."; write "ไทย"_th instead (decoded as UTF-8).
inline juce::String operator""_th(const char* s, size_t n) { return juce::String::fromUTF8(s, int(n)); }
