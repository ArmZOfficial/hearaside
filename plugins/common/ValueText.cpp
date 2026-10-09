#include "ValueText.h"

namespace hearaside::valuetext {

namespace {

const juce::String& minus() {
    static const juce::String m = juce::String(juce::CharPointer_UTF8("\xe2\x88\x92"));   // U+2212
    return m;
}

// minus signs and decimal commas as plain ASCII, no spaces
juce::String normalise(const juce::String& text) {
    return text.trim()
        .replace(minus(), "-")
        .replace(juce::CharPointer_UTF8("\xe2\x80\x93"), "-")   // en dash
        .replaceCharacter(',', '.')
        .removeCharacters(" \t");
}

std::optional<float> number(const juce::String& t) {
    // optional sign, digits, optional fraction; nothing else may be left but a unit
    int i = 0;
    const int n = t.length();
    if (i < n && (t[i] == '-' || t[i] == '+')) ++i;
    const int digitsFrom = i;
    while (i < n && (juce::CharacterFunctions::isDigit(t[i]) || t[i] == '.')) ++i;
    const auto body = t.substring(digitsFrom, i);
    if (body.isEmpty() || body.retainCharacters("0123456789").isEmpty() || body.indexOfChar('.') != body.lastIndexOfChar('.'))
        return std::nullopt;
    return t.substring(0, i).getFloatValue();
}

} // namespace

juce::String formatDb(float db, float floorDb, bool unit) {
    juce::String s;
    if (db <= floorDb + 1.0e-4f) s = minus() + juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e"));   // −∞
    else {
        const float r = std::round(db * 10.0f) / 10.0f;
        s = juce::String(std::abs(r), 1);
        if (r > 0.04f) s = "+" + s;
        else if (r < -0.04f) s = minus() + s;
    }
    return unit ? s + " dB" : s;
}

juce::String editDb(float db, float floorDb) {
    if (db <= floorDb + 1.0e-4f) return "-inf";
    const float r = std::round(db * 10.0f) / 10.0f;
    if (r > 0.04f) return "+" + juce::String(r, 1);
    if (r < -0.04f) return juce::String(r, 1);
    return "0.0";
}

std::optional<float> parseDb(const juce::String& text, float lo, float hi) {
    auto t = normalise(text);
    if (t.endsWithIgnoreCase("db")) t = t.dropLastCharacters(2);
    const auto inf = juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e"));
    if (t.equalsIgnoreCase("-inf") || t.equalsIgnoreCase("inf") || t == inf || t == "-" + inf || t.equalsIgnoreCase("-infinity"))
        return lo;
    const auto v = number(t);
    if (!v) return std::nullopt;
    return juce::jlimit(lo, hi, std::round(*v * 10.0f) / 10.0f);
}

juce::String formatMs(float ms) { return juce::String(juce::roundToInt(ms)) + " ms"; }

std::optional<float> parseMs(const juce::String& text, float lo, float hi) {
    auto t = normalise(text);
    if (t.endsWithIgnoreCase("ms")) t = t.dropLastCharacters(2);
    const auto v = number(t);
    if (!v) return std::nullopt;
    return juce::jlimit(lo, hi, float(juce::roundToInt(*v)));
}

juce::String formatPan(float pan) {
    const int p = juce::roundToInt(juce::jlimit(-100.0f, 100.0f, pan));
    if (p == 0) return "C";
    return (p < 0 ? "L" : "R") + juce::String(std::abs(p));
}

std::optional<float> parsePan(const juce::String& text) {
    const auto t = normalise(text).toUpperCase();
    if (t == "C" || t == "CENTER" || t == "CENTRE" || t == "0" || t == "L0" || t == "R0") return 0.0f;
    if (t.startsWithChar('L') || t.startsWithChar('R')) {
        const auto v = number(t.substring(1));
        if (!v || *v < 0.0f) return std::nullopt;
        return juce::jlimit(-100.0f, 100.0f, float(juce::roundToInt(*v)) * (t.startsWithChar('L') ? -1.0f : 1.0f));
    }
    const auto v = number(t);
    if (!v) return std::nullopt;
    return juce::jlimit(-100.0f, 100.0f, float(juce::roundToInt(*v)));
}

juce::String formatPercent(float f) { return juce::String(juce::roundToInt(f * 100.0f)) + "%"; }

std::optional<float> parsePercent(const juce::String& text, float lo, float hi) {
    auto t = normalise(text);
    if (t.endsWithChar('%')) t = t.dropLastCharacters(1);
    const auto v = number(t);
    if (!v) return std::nullopt;
    return juce::jlimit(lo, hi, *v / 100.0f);
}

juce::String truncateUtf8(const juce::String& s, int maxBytes) {
    if (int(s.getNumBytesAsUTF8()) <= maxBytes) return s;
    juce::String out;
    int bytes = 0;
    for (auto p = s.getCharPointer(); !p.isEmpty(); ++p) {
        const auto c = *p;
        const int len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (bytes + len > maxBytes) break;
        out << juce::String::charToString(c);
        bytes += len;
    }
    return out;
}

juce::String initialOf(const juce::String& name) {
    const auto t = name.trim();
    for (auto p = t.getCharPointer(); !p.isEmpty(); ++p)
        if (*p < 0x0E40 || *p > 0x0E44)   // a Thai name's leading vowel (เ แ โ ใ ไ) isn't its initial
            return juce::String::charToString(*p).toUpperCase();
    return t.isEmpty() ? juce::String("?") : juce::String::charToString(t[0]);
}

} // namespace hearaside::valuetext
