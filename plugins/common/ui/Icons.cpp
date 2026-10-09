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
        case Icon::Power:
            p = svg({ "M12 3v8", "M6.3 6.8a8 8 0 1 0 11.4 0" });
            break;
        case Icon::Record:
            p.addEllipse(6.0f, 6.0f, 12.0f, 12.0f);
            break;
        case Icon::Drag:   // arrow into a tray: "drag this into the DAW"
            p = svg({ "M12 3v12", "M7 10l5 5 5-5", "M4 17v3h16v-3" });
            break;
        case Icon::Folder:
            p = svg({ "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" });
            break;
        case Icon::Link:
            p = svg({ "M10 14a4 4 0 0 0 5.66 0l3-3a4 4 0 0 0-5.66-5.66l-1 1", "M14 10a4 4 0 0 0-5.66 0l-3 3a4 4 0 0 0 5.66 5.66l1-1" });
            break;
        case Icon::Check:
            p = svg({ "M5 12.5l4.5 4.5L19 7.5" });
            break;
        case Icon::Info:
            p = svg({ "M12 11v6", "M12 7.5h.01" });
            p.addEllipse(3.0f, 3.0f, 18.0f, 18.0f);
            break;
        case Icon::Close:
            p = svg({ "M6 6l12 12", "M18 6 6 18" });
            break;
        case Icon::Count:
            break;
    }
    return p;
}

} // namespace

const juce::Path& path(Icon i) {
    static const auto cache = [] {
        std::array<juce::Path, size_t(Icon::Count)> c;
        for (size_t k = 0; k < c.size(); ++k) c[k] = build(Icon(k));
        return c;
    }();
    return cache[size_t(i)];
}

void draw(juce::Graphics& g, Icon i, juce::Rectangle<float> b, juce::Colour c, float strokeWidth) {
    const float s = juce::jmin(b.getWidth(), b.getHeight()) / 24.0f;
    const auto t = juce::AffineTransform::scale(s).translated(b.getCentreX() - 12.0f * s, b.getCentreY() - 12.0f * s);
    g.setColour(c);
    g.strokePath(path(i), juce::PathStrokeType(strokeWidth * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);
    if (i == Icon::More || i == Icon::Record) g.fillPath(path(i), t);
}

} // namespace hearaside::icons
