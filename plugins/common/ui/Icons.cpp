#include "Icons.h"

#include <array>

namespace hearaside::icons {

namespace {

juce::Path svg(std::initializer_list<const char*> parts) {
    juce::Path p;
    for (const char* d : parts) p.addPath(juce::Drawable::parseSVGPath(d));
    return p;
}

juce::Path build(Icon i) {
    juce::Path p;
    switch (i) {
        case Icon::Headphones:
            p = svg({ "M4 15v-3a8 8 0 0 1 16 0v3" });
            p.addRoundedRectangle(3.0f, 14.0f, 4.0f, 6.0f, 1.5f);
            p.addRoundedRectangle(17.0f, 14.0f, 4.0f, 6.0f, 1.5f);
            break;
        case Icon::Broadcast:
            p = svg({ "M8.5 8.5a5 5 0 0 0 0 7", "M15.5 8.5a5 5 0 0 1 0 7", "M5.6 5.6a9 9 0 0 0 0 12.8", "M18.4 5.6a9 9 0 0 1 0 12.8" });
            p.addEllipse(10.0f, 10.0f, 4.0f, 4.0f);
            break;
        case Icon::SpeakerOff:
            p = svg({ "M11 5 6 9H3v6h3l5 4z", "M16 9 21 15", "M21 9 16 15" });
            break;
        case Icon::Sliders:
            p = svg({ "M20 7h-9", "M14 17H5" });
            p.addEllipse(14.0f, 14.0f, 6.0f, 6.0f);
            p.addEllipse(4.0f, 4.0f, 6.0f, 6.0f);
            break;
        case Icon::Warning:
            p = svg({ "M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z", "M12 9v4", "M12 17h.01" });
            break;
        case Icon::More:
            p.addEllipse(4.5f, 11.0f, 2.0f, 2.0f);
            p.addEllipse(11.0f, 11.0f, 2.0f, 2.0f);
            p.addEllipse(17.5f, 11.0f, 2.0f, 2.0f);
            break;
        case Icon::Solo:
            p = svg({ "M12 3v18", "M7 8h10", "M5 16h14" });
            break;
    }
    return p;
}

} // namespace

const juce::Path& path(Icon i) {
    static const std::array<juce::Path, 7> cache { build(Icon::Headphones), build(Icon::Broadcast), build(Icon::SpeakerOff),
                                                   build(Icon::Sliders), build(Icon::Warning), build(Icon::More), build(Icon::Solo) };
    return cache[size_t(i)];
}

void draw(juce::Graphics& g, Icon i, juce::Rectangle<float> b, juce::Colour c, float strokeWidth) {
    const float s = juce::jmin(b.getWidth(), b.getHeight()) / 24.0f;
    const auto t = juce::AffineTransform::scale(s).translated(b.getCentreX() - 12.0f * s, b.getCentreY() - 12.0f * s);
    g.setColour(c);
    g.strokePath(path(i), juce::PathStrokeType(strokeWidth * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);
    if (i == Icon::More) g.fillPath(path(i), t);
}

} // namespace hearaside::icons
