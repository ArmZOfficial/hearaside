// Design-system components (prompt 3.2, design export). All colours come from the palette of the
// attached hearaside::LookAndFeel so light / dark switch live. Inputs (values, names, dropdowns)
// are in Controls.h, menus / toasts in Overlay.h, scrolling in ScrollArea.h.
#pragma once

#include "Icons.h"
#include "LookAndFeel.h"
#include "../Strings.h"

#include <functional>

namespace hearaside {

// Rounded "inset" well used for rows, info boxes and summaries.
void drawInset(juce::Graphics&, juce::Rectangle<float>, float radius, const theme::Palette&, bool strongEdge = false);
void drawWordmark(juce::Graphics&, juce::Rectangle<float> area, const juce::String& sub, float size, juce::Colour colour);
float textWidth(const juce::Font&, const juce::String&);
// "Some long name…" that fits `width` (a single line)
juce::String ellipsize(const juce::Font&, const juce::String&, float width);

// Word wrap that only breaks at spaces. JUCE's TextLayout mis-breaks Thai (no spaces inside
// phrases) and can drop the last line; UI copy separates Thai phrases with spaces instead.
juce::StringArray wrapText(const juce::Font&, const juce::String&, float width);
float wrappedHeight(const juce::Font&, const juce::String&, float width, float lineGap = 3.0f);
// maxLines > 0: the last line ends in "…" when the text is longer (CSS line-clamp)
void drawWrapped(juce::Graphics&, const juce::String&, const juce::Font&, juce::Colour, juce::Rectangle<float>,
                 float lineGap = 3.0f, juce::Justification just = juce::Justification::left, int maxLines = 0);
void drawRoundShadow(juce::Graphics&, juce::Rectangle<float> ellipse, float alpha, int radius = 3);

// ---- small painted pieces ------------------------------------------------------------------
enum class Dot { None, Muted, Ok, Warn, Rec, RecRing };
void drawStatusDot(juce::Graphics&, juce::Point<float> centre, Dot, const theme::Palette&, float size = 7.0f);
juce::Colour dotColour(Dot, const theme::Palette&);

enum class BadgeStyle { Chip, Solid, Rec, Ok };
float badgeWidth(const juce::String& text, BadgeStyle = BadgeStyle::Chip);
void drawBadge(juce::Graphics&, juce::Rectangle<float>, const juce::String& text, BadgeStyle, const theme::Palette&);

// circle with an initial ("M"), solid (live / signed in) or hollow
void drawAvatar(juce::Graphics&, juce::Rectangle<float>, const juce::String& name, bool solid, const theme::Palette&,
                const juce::Image& photo = {});

// Animated 0..1 value (knob slides, crossfades). Respects "reduce motion".
class Anim {
public:
    void setTarget(float t, bool instant);
    bool tick(double dtSec, double durationSec);   // true while moving
    float value() const noexcept { return v_; }
private:
    float v_ = 0.0f, target_ = 0.0f;
};

// ---- AudibleToggle: "you hear" / "viewers hear" icon button (48 x 38; 40 x 32 in tables) ----
// Off = light tile + the icon struck through. With a label ("You hear") it is the 44-high
// button of Track fine settings.
class AudibleToggle : public juce::Button, private juce::Timer {
public:
    enum class Side { You, Viewers };
    explicit AudibleToggle(Side);
    void setOn(bool on, bool animate = true);
    bool isOn() const noexcept { return on_; }
    void setSubject(const juce::String& name);       // "You hear <name>" for screen readers
    void setLabelShown(bool shown) { labelShown_ = shown; repaint(); }
    int idealWidth() const;                          // with the label
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    void timerCallback() override;
    void updateTexts();
    Side side_;
    bool on_ = false, labelShown_ = false;
    juce::String subject_;
    Anim fade_;
    double lastTick_ = 0;
};

// ---- ToggleRow (Track): icon tile + title + caption + switch, the whole row is one button ----
class ToggleRow : public juce::Button, private juce::Timer {
public:
    explicit ToggleRow(icons::Icon icon);
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

// ---- Switch (36 x 20; 44-46 x 26 when the component is taller) -----------------------------
class Switch : public juce::Button, private juce::Timer {
public:
    Switch();
    void setOn(bool on, bool animate);
    bool isOn() const noexcept { return on_; }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    static void paintAt(juce::Graphics&, juce::Rectangle<float> track, float knob01, const theme::Palette&, float alpha = 1.0f);
private:
    void timerCallback() override;
    bool on_ = false;
    Anim knob_;
    double lastTick_ = 0;
};

// Switch with "On" / "Off" written next to it (App Audio header, Manage program audio)
class LabelledSwitch : public juce::Component, public juce::SettableTooltipClient {
public:
    LabelledSwitch();
    void setOn(bool on, bool animate) { sw_.setOn(on, animate); repaint(); }
    bool isOn() const noexcept { return sw_.isOn(); }
    std::function<void()> onClick;
    void setPill(bool p) { pill_ = p; resized(); repaint(); }   // the 36-high chip of the App Audio header
    Switch& switchButton() noexcept { return sw_; }
    int idealWidth() const;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
private:
    Switch sw_;
    bool pill_ = false;
};

// ---- LevelSlider: -30 .. +6 dB (-30 shown as −∞), wheel 0.5 dB, double-click = 0 dB ---------
class LevelSlider : public juce::Slider {
public:
    explicit LevelSlider(double maxDb = 6.0);
    static float sliderToDb(double v) { return v <= -30.0 + 1.0e-6 ? -60.0f : float(v); }
    static double dbToSlider(float db) { return juce::jlimit(-30.0, 6.0, double(db)); }
    void setDim(bool d);   // that side is off: drawn at 36 %
    void mouseDown(const juce::MouseEvent&) override;
};

// ---- PanSlider: -100 (L) .. +100 (R), fills from the middle, double-click = centre -----------
// Full: "L · C · double-click to center · R" under the bar. Compact: the bar only.
class PanSlider : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit PanSlider(bool full);
    juce::Slider& slider() noexcept { return slider_; }
    void setPan(float p) { slider_.setValue(p, juce::dontSendNotification); }
    float pan() const { return float(slider_.getValue()); }
    std::function<void(float)> onChange;
    std::function<void()> onDragStart, onDragEnd;
    static constexpr int kFullHeight = 40, kCompactHeight = 20;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    bool full_;
    juce::Slider slider_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
};

// ---- Meters --------------------------------------------------------------------------------
class MeterBar : public juce::Component {
public:
    explicit MeterBar(float thickness = 4.0f, bool soft = false) : thickness_(thickness), soft_(soft) { setInterceptsMouseClicks(false, false); }
    void setLevel(float linear01);
    void paint(juce::Graphics&) override;
private:
    float level_ = 0.0f, thickness_;
    bool soft_;   // ink2 fill (row meters), else ink
};

// peak (linear) -> 0..1 bar position, -60 dBFS .. 0 dBFS
float meterPosition(float peakLinear);

// ---- StatusChip: dot + text (+ value + chevron when it opens a popover) ----------------------
class StatusChip : public juce::Button {
public:
    explicit StatusChip(float height = 36.0f);
    void set(const juce::String& text, Dot dot, const juce::String& value = {}, bool chevron = false);
    int idealWidth() const;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    juce::String text_, value_;
    Dot dot_ = Dot::None;
    bool chevron_ = false;
    float height_;
};

// ---- Banner: warning (warnBg) / info / dark --------------------------------------------------
class Banner : public juce::Component {
public:
    enum class Style { Warning, Info, Dark, Outline };
    void set(Style, icons::Icon, const juce::String& text);
    int idealHeight(int width) const;
    void paint(juce::Graphics&) override;
private:
    Style style_ = Style::Warning;
    icons::Icon icon_ = icons::Icon::Warning;
    juce::String text_;
};

// ---- Buttons --------------------------------------------------------------------------------
// "Hear viewers' mix": 48 high ink; active = outlined
class PrimaryButton : public juce::Button {
public:
    PrimaryButton(icons::Icon icon, float height = 48.0f);
    void setActive(bool a) { if (a != active_) { active_ = a; repaint(); } }
    bool isActive() const noexcept { return active_; }
    void setIcon(icons::Icon i) { icon_ = i; hasIcon_ = true; repaint(); }
    void setNoIcon() { hasIcon_ = false; repaint(); }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    icons::Icon icon_;
    bool hasIcon_ = true;
    float height_;
    bool active_ = false;
};

// "Mute stream": outlined ink; muted = solid danger "Unmute stream". iconOnly for narrow headers.
class MuteButton : public juce::Button {
public:
    MuteButton();
    void setActive(bool a);
    bool isActive() const noexcept { return active_; }
    void setIconOnly(bool b) { iconOnly_ = b; repaint(); }
    int idealWidth() const;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    bool active_ = false, iconOnly_ = false;
};

// Square icon button (40 x 40, radius 12; 36 / 11 in the plug-ins). on = solid ink.
class IconButton : public juce::Button {
public:
    explicit IconButton(icons::Icon icon, const juce::String& tip = {});
    void setIcon(icons::Icon i) { icon_ = i; repaint(); }
    void setOn(bool on) { if (on != on_) { on_ = on; repaint(); } }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    icons::Icon icon_;
    bool on_ = false;
};

// "⋯" (32 x 32, no frame until hovered)
class MoreButton : public juce::Button {
public:
    MoreButton();
    void setOpen(bool o) { open_ = o; repaint(); }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    bool open_ = false;
};

// Text button with an optional icon: ghost (frame), solid (ink), small (30 high), danger (red frame)
class GhostButton : public juce::Button {
public:
    enum class Style { Ghost, Solid, Danger, DangerSolid };
    GhostButton(const juce::String& text = {}, Style = Style::Ghost);
    void setIcon(icons::Icon i) { icon_ = i; hasIcon_ = true; repaint(); }
    void setStyle(Style s) { style_ = s; repaint(); }
    void setSmall(bool s) { small_ = s; repaint(); }
    int idealWidth() const;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    Style style_;
    icons::Icon icon_ = icons::Icon::Check;
    bool hasIcon_ = false, small_ = false;
};

// Underlined text link ("Manage", "Create an account")
class LinkButton : public juce::Button {
public:
    explicit LinkButton(const juce::String& text = {}, float size = 12.0f);
    int idealWidth() const;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    float size_;
};

// "‹ Back" (header of a sub-page / a sheet)
class BackButton : public juce::Button {
public:
    explicit BackButton(bool withText = true);
    int idealWidth() const;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    bool withText_;
};

} // namespace hearaside
