#include "Icons.h"

#include <array>

namespace hearaside::icons {

namespace {

juce::Path svg(std::initializer_list<const char*> parts) {
    juce::Path p;
    for (const char* d : parts) p.addPath(juce::Drawable::parseSVGPath(d));
    return p;
}

void circle(juce::Path& p, float cx, float cy, float r) { p.addEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f); }

juce::Path build(Icon i) {
    juce::Path p;
    switch (i) {
        case Icon::Headphones:
            p = svg({ "M4 17v-5a8 8 0 0 1 16 0v5" });
            p.addRoundedRectangle(3.0f, 14.0f, 4.5f, 7.0f, 1.8f);
            p.addRoundedRectangle(16.5f, 14.0f, 4.5f, 7.0f, 1.8f);
            break;
        case Icon::Broadcast:
            p = svg({ "M15.5 8.5a5 5 0 0 1 0 7", "M8.5 15.5a5 5 0 0 1 0-7", "M18.4 5.6a9 9 0 0 1 0 12.8", "M5.6 18.4a9 9 0 0 1 0-12.8" });
            circle(p, 12.0f, 12.0f, 2.0f);
            break;
        case Icon::SpeakerOff:  p = svg({ "M11 5L6 9H3v6h3l5 4V5z", "M16 9l5 6", "M21 9l-5 6" }); break;
        case Icon::SpeakerOn:   p = svg({ "M11 5L6 9H3v6h3l5 4V5z", "M15.5 8.5a5 5 0 0 1 0 7" }); break;
        case Icon::Sliders:
            p = svg({ "M4 7h10", "M18 7h2", "M4 17h4", "M12 17h8" });
            circle(p, 16.0f, 7.0f, 2.0f);
            circle(p, 10.0f, 17.0f, 2.0f);
            break;
        case Icon::Warning:     p = svg({ "M12 3l9.5 17h-19z", "M12 10v4", "M12 17.5v.5" }); break;
        case Icon::More:        circle(p, 5.0f, 12.0f, 1.7f); circle(p, 12.0f, 12.0f, 1.7f); circle(p, 19.0f, 12.0f, 1.7f); break;
        case Icon::Solo:        p = svg({ "M12 3v18", "M7 8h10", "M5 16h14" }); break;
        case Icon::Power:       p = svg({ "M12 3v8", "M6.3 6.8a8 8 0 1 0 11.4 0" }); break;
        case Icon::Record:      circle(p, 12.0f, 12.0f, 6.0f); break;
        case Icon::Drag:        p = svg({ "M12 3v12", "M7 10l5 5 5-5", "M4 17v3h16v-3" }); break;
        case Icon::Folder:      p = svg({ "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" }); break;
        case Icon::Link:        p = svg({ "M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7", "M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7" }); break;
        case Icon::Check:       p = svg({ "M5 12.5l4.5 4.5L19 7.5" }); break;
        case Icon::Info:        circle(p, 12.0f, 12.0f, 9.0f); p.addPath(svg({ "M12 11v6", "M12 7.5v.5" })); break;
        case Icon::Close:       p = svg({ "M6 6l12 12", "M18 6L6 18" }); break;
        case Icon::Window:      p.addRoundedRectangle(3.0f, 4.0f, 18.0f, 16.0f, 3.0f); p.addPath(svg({ "M3 9h18" })); break;
        case Icon::Monitor:     p.addRoundedRectangle(3.0f, 4.0f, 18.0f, 12.0f, 2.0f); p.addPath(svg({ "M8 20h8", "M12 16v4" })); break;
        case Icon::Person:      circle(p, 12.0f, 8.0f, 4.0f); p.addPath(svg({ "M4 21a8 8 0 0 1 16 0" })); break;
        case Icon::Plus:        p = svg({ "M12 5v14", "M5 12h14" }); break;
        case Icon::Waveform:    p = svg({ "M3 12h2", "M7 8v8", "M11 5v14", "M15 9v6", "M19 11v2" }); break;
        case Icon::Grip:
            for (float y : { 6.0f, 12.0f, 18.0f }) { circle(p, 9.0f, y, 1.6f); circle(p, 15.0f, y, 1.6f); }
            break;
        case Icon::Appearance:  circle(p, 12.0f, 12.0f, 9.0f); break;   // + filled half in draw()
        case Icon::AudioBars:   p = svg({ "M4 10v4", "M8 6v12", "M12 3v18", "M16 7v10", "M20 10v4" }); break;
        case Icon::Connection:  p = svg({ "M9 7H6a5 5 0 0 0 0 10h3", "M15 7h3a5 5 0 0 1 0 10h-3", "M8 12h8" }); break;
        case Icon::Spread:      p = svg({ "M4 12h16", "M4 12l3-3", "M4 12l3 3", "M20 12l-3-3", "M20 12l-3 3" }); break;
        case Icon::Play:        p = svg({ "M8 5.5v13l10.5-6.5z" }); break;
        case Icon::ChevronLeft:  p = svg({ "M15 6l-6 6 6 6" }); break;
        case Icon::ChevronRight: p = svg({ "M9 6l6 6-6 6" }); break;
        case Icon::ChevronDown:  p = svg({ "M6 9l6 6 6-6" }); break;
        case Icon::Refresh:     p = svg({ "M3 12a9 9 0 0 1 15.5-6.2L21 8", "M21 3v5h-5", "M21 12a9 9 0 0 1-15.5 6.2L3 16", "M3 21v-5h5" }); break;
        case Icon::Count: break;
    }
    return p;
}

bool filled(Icon i) { return i == Icon::More || i == Icon::Record || i == Icon::Grip || i == Icon::Play; }

} // namespace

const juce::Path& path(Icon i) {
    static const auto cache = [] {
        std::array<juce::Path, size_t(Icon::Count)> c;
        for (size_t k = 0; k < c.size(); ++k) c[k] = build(Icon(k));
        return c;
    }();
    return cache[size_t(i)];
}

void draw(juce::Graphics& g, Icon i, juce::Rectangle<float> b, juce::Colour c, float strokeWidth, bool strike) {
    const float s = juce::jmin(b.getWidth(), b.getHeight()) / 24.0f;
    const auto t = juce::AffineTransform::scale(s).translated(b.getCentreX() - 12.0f * s, b.getCentreY() - 12.0f * s);
    const juce::PathStrokeType stroke(strokeWidth * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour(c);
    if (filled(i)) g.fillPath(path(i), t);
    else g.strokePath(path(i), stroke, t);
    if (i == Icon::Appearance) {
        juce::Path half;
        half.addPieSegment(3.0f, 3.0f, 18.0f, 18.0f, juce::MathConstants<float>::pi, juce::MathConstants<float>::twoPi, 0.0f);
        g.fillPath(half, t);
    }
    if (strike) {
        juce::Path line;
        line.startNewSubPath(3.0f, 3.0f);
        line.lineTo(21.0f, 21.0f);
        g.strokePath(line, stroke, t);
    }
}

} // namespace hearaside::icons
