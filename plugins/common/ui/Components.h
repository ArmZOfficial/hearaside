// Design-system components (docs/plan.md 7.5). All colours come from the palette of the
// attached hearaside::LookAndFeel so light / dark switch live.
#pragma once

#include "Icons.h"
#include "LookAndFeel.h"

#include <functional>

namespace hearaside {

// Rounded "inset" well used for rows, info boxes and summaries.
void drawInset(juce::Graphics&, juce::Rectangle<float>, float radius, const theme::Palette&, bool strongEdge = false);
void drawWordmark(juce::Graphics&, juce::Rectangle<float> area, const juce::String& sub, float size, juce::Colour colour);
float textWidth(const juce::Font&, const juce::String&);

// Word wrap that only breaks at spaces. JUCE's TextLayout mis-breaks Thai (no spaces inside
// phrases) and can drop the last line; UI copy separates Thai phrases with spaces instead.
juce::StringArray wrapText(const juce::Font&, const juce::String&, float width);
float wrappedHeight(const juce::Font&, const juce::String&, float width, float lineGap = 3.0f);
void drawWrapped(juce::Graphics&, const juce::String&, const juce::Font&, juce::Colour, juce::Rectangle<float>,
                 float lineGap = 3.0f, juce::Justification just = juce::Justification::left);
void drawRoundShadow(juce::Graphics&, juce::Rectangle<float> ellipse, float alpha, int radius = 3);

// Animated 0..1 value (knob slides, crossfades). Respects "reduce motion".
class Anim {
public:
    void setTarget(float t, bool instant);
    bool tick(double dtSec, double durationSec);   // true while moving
    float value() const noexcept { return v_; }
private:
    float v_ = 0.0f, target_ = 0.0f;
};

// ---- AudiblePill: "ได้ยิน" / "ไม่ได้ยิน" status button (118 x 44) ----------------------------
class AudiblePill : public juce::Button {
public:
    explicit AudiblePill(icons::Icon icon);
    void setOn(bool on);
    bool isOn() const noexcept { return on_; }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    icons::Icon icon_;
    bool on_ = false;
};

// ---- BigToggleRow (Track): icon tile + title + caption + switch, the whole row is one button --
class ToggleRow : public juce::Button, private juce::Timer {
public:
    ToggleRow(icons::Icon icon);
    void setTexts(const juce::String& title, const juce::String& caption);
    void setOn(bool on, bool animate);
    bool isOn() const noexcept { return on_; }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    void timerCallback() override;
    icons::Icon icon_;
    juce::String title_, caption_;
    bool on_ = false;
    Anim knob_;
    double lastTick_ = 0;
};

// ---- Switch (46 x 28) ----------------------------------------------------------------------
class Switch : public juce::Button, private juce::Timer {
public:
    Switch();
    void setOn(bool on, bool animate);
    bool isOn() const noexcept { return on_; }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    void timerCallback() override;
    bool on_ = false;
    Anim knob_;
    double lastTick_ = 0;
};

// ---- LevelSlider: -30 .. +6 dB, -30 shown as −∞, wheel 0.5 dB, double-click = 0 dB ---------
class LevelSlider : public juce::Slider {
public:
    LevelSlider();
    static float sliderToDb(double v) { return v <= -30.0 + 1.0e-6 ? -60.0f : float(v); }
    static double dbToSlider(float db) { return juce::jlimit(-30.0, 6.0, double(db)); }
    void mouseDown(const juce::MouseEvent&) override;
};

// ---- Meters --------------------------------------------------------------------------------
class MeterBar : public juce::Component {
public:
    explicit MeterBar(float thickness = 4.0f) : thickness_(thickness) { setInterceptsMouseClicks(false, false); }
    void setLevel(float linear01);
    void paint(juce::Graphics&) override;
private:
    float level_ = 0.0f, thickness_;
};

// peak (linear) -> 0..1 bar position, -60 dBFS .. 0 dBFS
float meterPosition(float peakLinear);

// ---- StatusChip: dot + text ----------------------------------------------------------------
class StatusChip : public juce::Component {
public:
    enum class Dot { Solid, Ring, None };
    explicit StatusChip(float height = 36.0f) : height_(height) {}
    void set(const juce::String& text, Dot dot);
    int idealWidth() const;
    void paint(juce::Graphics&) override;
private:
    juce::String text_;
    Dot dot_ = Dot::None;
    float height_;
};

// ---- Banner: preview (dark) / panic (outline) / warning ------------------------------------
class Banner : public juce::Component {
public:
    enum class Style { Dark, Outline, Warning };
    void set(Style, icons::Icon, const juce::String& text);
    int idealHeight(int width) const;
    void paint(juce::Graphics&) override;
private:
    Style style_ = Style::Warning;
    icons::Icon icon_ = icons::Icon::Warning;
    juce::String text_;
};

// ---- PrimaryButton ("ฟังแบบคนดู", 48 high) and MuteButton ("ตัดเสียงคนดู", 40 high) -------
class PrimaryButton : public juce::Button {
public:
    PrimaryButton(icons::Icon icon, float height = 48.0f) : juce::Button({}), icon_(icon), height_(height) {}
    void setActive(bool a) { if (a != active_) { active_ = a; repaint(); } }
    bool isActive() const noexcept { return active_; }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    icons::Icon icon_;
    float height_;
    bool active_ = false;
};

class MuteButton : public juce::Button {
public:
    MuteButton() : juce::Button({}) {}
    void setActive(bool a) { if (a != active_) { active_ = a; repaint(); } }
    bool isActive() const noexcept { return active_; }
    int idealWidth() const;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    bool active_ = false;
};

class IconButton : public juce::Button {
public:
    explicit IconButton(icons::Icon icon) : juce::Button({}), icon_(icon) {}
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    icons::Icon icon_;
};

// ---- SceneBar: segmented pill buttons, right-click = menu ----------------------------------
class SceneBar : public juce::Component {
public:
    std::function<void(int)> onPick;
    std::function<void(int)> onContextMenu;
    void setScenes(const juce::StringArray& names);
    void setSelected(int index);   // -1 = "custom" (nothing selected)
    int idealWidth() const;
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    class Segment : public juce::Button {
    public:
        Segment(SceneBar& o, int i) : juce::Button({}), owner(o), index(i) {}
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;
        void mouseUp(const juce::MouseEvent&) override;
        SceneBar& owner;
        int index;
    };
    juce::OwnedArray<Segment> segs_;
    int selected_ = -1;
};

} // namespace hearaside
