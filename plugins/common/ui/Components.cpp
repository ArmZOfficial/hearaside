#include "Components.h"
#include "../Settings.h"
#include "../Strings.h"

namespace hearaside {

namespace {

bool reduceMotion() {
    static SharedSettings settings;
    return settings->reduceMotion();
}

juce::Colour withPress(juce::Colour base, const theme::Palette& p, bool highlighted, bool down) {
    if (down) return base.overlaidWith(p.ink.withAlpha(base == p.ink ? 0.0f : 0.10f)).brighter(base == p.ink ? 0.25f : 0.0f);
    if (highlighted) return base == p.ink ? base.brighter(0.18f) : base.overlaidWith(p.ink.withAlpha(0.05f));
    return base;
}

} // namespace

void drawInset(juce::Graphics& g, juce::Rectangle<float> r, float radius, const theme::Palette& p, bool strongEdge) {
    g.setColour(p.inset);
    g.fillRoundedRectangle(r, radius);
    g.setColour(strongEdge ? p.rowOnEdge : p.hairline1);
    g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
}

float textWidth(const juce::Font& f, const juce::String& s) {
    return juce::GlyphArrangement::getStringWidth(f, s);
}

juce::StringArray wrapText(const juce::Font& f, const juce::String& text, float width) {
    juce::StringArray lines;
    for (const auto& para : juce::StringArray::fromLines(text)) {
        const auto words = juce::StringArray::fromTokens(para, " ", "");
        juce::String line;
        for (const auto& w : words) {
            if (w.isEmpty()) continue;
            const auto candidate = line.isEmpty() ? w : line + " " + w;
            if (line.isNotEmpty() && textWidth(f, candidate) > width) {
                lines.add(line);
                line = w;
            } else {
                line = candidate;
            }
        }
        lines.add(line);
    }
    return lines;
}

float wrappedHeight(const juce::Font& f, const juce::String& text, float width, float lineGap) {
    const int n = wrapText(f, text, width).size();
    return float(n) * f.getHeight() + float(juce::jmax(0, n - 1)) * lineGap;
}

void drawWrapped(juce::Graphics& g, const juce::String& text, const juce::Font& f, juce::Colour c,
                 juce::Rectangle<float> r, float lineGap, juce::Justification just) {
    g.setFont(f);
    g.setColour(c);
    float y = r.getY();
    for (const auto& line : wrapText(f, text, r.getWidth())) {
        g.drawFittedText(line, juce::Rectangle<float>(r.getX(), y, r.getWidth(), f.getHeight()).toNearestInt(),
                         just.getOnlyHorizontalFlags() | juce::Justification::verticallyCentred, 1, 0.85f);
        y += f.getHeight() + lineGap;
    }
}

void drawRoundShadow(juce::Graphics& g, juce::Rectangle<float> e, float alpha, int radius) {
    juce::Path p;
    p.addEllipse(e);
    juce::DropShadow(juce::Colours::black.withAlpha(alpha), radius, { 0, 1 }).drawForPath(g, p);
}

void drawWordmark(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& sub, float size, juce::Colour colour) {
    // letter-spaced wordmark: draw glyph by glyph (tracking 0.28em / 0.46em)
    auto spaced = [&](const juce::String& text, juce::Font f, float tracking, float y, juce::Colour c) {
        float x = area.getX();
        g.setFont(f);
        g.setColour(c);
        for (auto ch : text) {
            const juce::String one = juce::String::charToString(ch);
            g.drawSingleLineText(one, juce::roundToInt(x), juce::roundToInt(y));
            x += textWidth(f, one) + tracking * f.getHeightInPoints();
        }
    };
    const auto big = uiFont(size, Weight::SemiBold);
    const auto small = uiFont(size * 0.53f, Weight::Medium);
    const float baseline1 = area.getY() + big.getAscent();
    spaced("HEARASIDE", big, 0.28f, baseline1, colour);
    spaced(sub, small, 0.46f, baseline1 + 3.0f + small.getAscent(), colour.withMultipliedAlpha(0.72f));
}

// ---------------------------------------------------------------------------------------------
void Anim::setTarget(float t, bool instant) {
    target_ = t;
    if (instant || reduceMotion()) v_ = t;
}

bool Anim::tick(double dt, double duration) {
    if (v_ == target_) return false;
    const float step = float(dt / juce::jmax(0.001, duration));
    v_ = target_ > v_ ? juce::jmin(target_, v_ + step) : juce::jmax(target_, v_ - step);
    return v_ != target_;
}

// ---------------------------------------------------------------------------------------------
AudiblePill::AudiblePill(icons::Icon icon, Str onLabel, Str offLabel)
    : juce::Button({}), icon_(icon), onLabel_(onLabel), offLabel_(offLabel) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void AudiblePill::setOn(bool on) {
    if (on == on_) return;
    on_ = on;
    setToggleState(on, juce::dontSendNotification);
    repaint();
}

void AudiblePill::setLabelOverride(const juce::String& label) {
    if (label == override_) return;
    override_ = label;
    repaint();
}

void AudiblePill::setPrefix(const juce::String& p) {
    if (p == prefix_) return;
    prefix_ = p;
    repaint();
}

int AudiblePill::idealWidth() const {
    const auto f = uiFont(13.0f, Weight::Medium);
    const float words = override_.isNotEmpty() ? textWidth(f, override_) : juce::jmax(textWidth(f, tr(onLabel_)), textWidth(f, tr(offLabel_)));
    return juce::roundToInt(textWidth(f, prefix_) + words + 16.0f + 8.0f + 2.0f * 12.0f);
}

void AudiblePill::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    const auto bg = on_ ? p.ink : p.offBg;
    g.setColour(withPress(bg, p, highlighted && isEnabled(), down));
    g.fillRoundedRectangle(r, theme::radius::small);
    g.setColour(on_ ? p.ink : (highlighted ? p.rowOnEdge : p.hairline3));
    g.drawRoundedRectangle(r, theme::radius::small, 1.0f);

    const auto fg = on_ ? p.onInk : p.offFg;
    const auto label = prefix_ + (override_.isNotEmpty() ? override_ : tr(on_ ? onLabel_ : offLabel_));
    const auto f = uiFont(13.0f, Weight::Medium);
    const float tw = textWidth(f, label);
    const float total = 16.0f + 8.0f + tw;
    float x = r.getCentreX() - total * 0.5f;
    icons::draw(g, icon_, { x, r.getCentreY() - 8.0f, 16.0f, 16.0f }, fg);
    g.setColour(fg);
    g.setFont(f);
    g.drawText(label, juce::Rectangle<float>(x + 24.0f, r.getY(), tw + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);
    if (!isEnabled()) { g.setColour(p.paper.withAlpha(0.4f)); g.fillRoundedRectangle(r, theme::radius::small); }
    if (hasKeyboardFocus(false)) drawFocusRing(g, r, theme::radius::small, p);
}

// ---------------------------------------------------------------------------------------------
ToggleRow::ToggleRow(icons::Icon icon) : juce::Button({}), icon_(icon) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void ToggleRow::setTexts(const juce::String& title, const juce::String& caption) {
    title_ = title;
    caption_ = caption;
    setTitle(title);
    setDescription(caption);
    repaint();
}

void ToggleRow::setOn(bool on, bool animate) {
    if (on == on_ && knob_.value() == (on ? 1.0f : 0.0f)) return;
    on_ = on;
    setToggleState(on, juce::dontSendNotification);
    knob_.setTarget(on ? 1.0f : 0.0f, !animate);
    lastTick_ = juce::Time::getMillisecondCounterHiRes();
    if (knob_.value() != (on ? 1.0f : 0.0f)) startTimerHz(60);
    repaint();
}

void ToggleRow::timerCallback() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (!knob_.tick((now - lastTick_) * 0.001, 0.160)) stopTimer();
    lastTick_ = now;
    repaint();
}

void ToggleRow::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(p.inset.overlaidWith(p.ink.withAlpha(down ? 0.06f : highlighted ? 0.03f : 0.0f)));
    g.fillRoundedRectangle(r, theme::radius::row);
    g.setColour(on_ ? p.rowOnEdge : p.hairline1);
    g.drawRoundedRectangle(r, theme::radius::row, 1.0f);

    auto inner = r.reduced(12.0f, 0.0f).withTrimmedRight(2.0f);
    auto tile = inner.removeFromLeft(38.0f).withSizeKeepingCentre(38.0f, 38.0f);
    g.setColour(on_ ? p.ink : p.offBg.withAlpha(1.0f).overlaidWith(p.offBg));
    g.fillRoundedRectangle(tile, 12.0f);
    g.setColour(p.hairline3);
    g.drawRoundedRectangle(tile.reduced(0.5f), 12.0f, 1.0f);
    icons::draw(g, icon_, tile.withSizeKeepingCentre(18.0f, 18.0f), on_ ? p.onInk : p.offFg);

    auto sw = inner.removeFromRight(46.0f).withSizeKeepingCentre(46.0f, 28.0f);
    const float k = knob_.value();
    g.setColour(p.switchOff.interpolatedWith(p.ink, k));
    g.fillRoundedRectangle(sw, 14.0f);
    const float kx = sw.getX() + 3.0f + 18.0f * k;
    drawRoundShadow(g, { kx, sw.getY() + 3.0f, 22.0f, 22.0f }, 0.25f);
    g.setColour(juce::Colours::white.interpolatedWith(p.onInk, paletteOf(*this).paper.getBrightness() < 0.5f ? k : 0.0f));
    g.fillEllipse(kx, sw.getY() + 3.0f, 22.0f, 22.0f);

    inner.removeFromLeft(12.0f);
    inner.removeFromRight(12.0f);
    const auto tf = uiFont(15.0f, Weight::Medium);
    const auto cf = uiFont(12.0f);
    const float block = tf.getHeight() + 2.0f + cf.getHeight();
    auto label = inner.withSizeKeepingCentre(inner.getWidth(), block);
    g.setColour(p.ink);
    g.setFont(tf);
    g.drawText(title_, label.removeFromTop(tf.getHeight()), juce::Justification::centredLeft, true);
    label.removeFromTop(2.0f);
    g.setColour(p.graphite);
    g.setFont(cf);
    g.drawText(caption_, label, juce::Justification::centredLeft, true);

    if (hasKeyboardFocus(false)) drawFocusRing(g, r, theme::radius::row, p);
}

// ---------------------------------------------------------------------------------------------
Switch::Switch() : juce::Button({}) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Switch::setOn(bool on, bool animate) {
    if (on == on_ && knob_.value() == (on ? 1.0f : 0.0f)) return;
    on_ = on;
    setToggleState(on, juce::dontSendNotification);
    knob_.setTarget(on ? 1.0f : 0.0f, !animate);
    lastTick_ = juce::Time::getMillisecondCounterHiRes();
    if (knob_.value() != (on ? 1.0f : 0.0f)) startTimerHz(60);
    repaint();
}

void Switch::timerCallback() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (!knob_.tick((now - lastTick_) * 0.001, 0.160)) stopTimer();
    lastTick_ = now;
    repaint();
}

void Switch::paintButton(juce::Graphics& g, bool highlighted, bool) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat();
    auto sw = r.withSizeKeepingCentre(juce::jmin(r.getWidth(), 50.0f), juce::jmin(r.getHeight(), 30.0f));
    const float k = knob_.value();
    g.setColour(p.switchOff.interpolatedWith(p.ink, k).brighter(highlighted ? 0.06f : 0.0f));
    g.fillRoundedRectangle(sw, sw.getHeight() * 0.5f);
    const float d = sw.getHeight() - 8.0f;
    const float kx = sw.getX() + 4.0f + (sw.getWidth() - 8.0f - d) * k;
    drawRoundShadow(g, { kx, sw.getY() + 4.0f, d, d }, 0.25f);
    g.setColour(p.paper.getBrightness() < 0.5f ? juce::Colours::white.interpolatedWith(p.onInk, k) : juce::Colours::white);
    g.fillEllipse(kx, sw.getY() + 4.0f, d, d);
    if (!isEnabled()) { g.setColour(p.paper.withAlpha(0.6f)); g.fillRoundedRectangle(sw, sw.getHeight() * 0.5f); }
    if (hasKeyboardFocus(false)) drawFocusRing(g, sw, sw.getHeight() * 0.5f, p);
}

// ---------------------------------------------------------------------------------------------
LevelSlider::LevelSlider() : juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox) {
    setRange(-30.0, 6.0, 0.5);
    setDoubleClickReturnValue(true, 0.0);
    setScrollWheelEnabled(true);
    // Shift + drag = fine adjustment (velocity mode with low sensitivity)
    setVelocityModeParameters(0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void LevelSlider::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isAltDown() && isEnabled()) {   // Alt/Option + click = 0 dB
        setValue(0.0, juce::sendNotificationSync);
        return;
    }
    juce::Slider::mouseDown(e);
}

// ---------------------------------------------------------------------------------------------
float meterPosition(float peak) {
    if (peak <= 1.0e-6f) return 0.0f;
    const float db = 20.0f * std::log10(peak);
    return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
}

void MeterBar::setLevel(float v) {
    v = juce::jlimit(0.0f, 1.0f, v);
    if (std::abs(v - level_) < 0.002f) return;
    level_ = v;
    repaint();
}

void MeterBar::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().withSizeKeepingCentre(float(getWidth()), thickness_);
    g.setColour(p.hairline2);
    g.fillRoundedRectangle(r, thickness_ * 0.5f);
    if (level_ > 0.0f) {
        g.setColour(p.ink);
        g.fillRoundedRectangle(r.withWidth(juce::jmax(thickness_, r.getWidth() * level_)), thickness_ * 0.5f);
    }
}

// ---------------------------------------------------------------------------------------------
void StatusChip::set(const juce::String& text, Dot dot) {
    if (text == text_ && dot == dot_) return;
    text_ = text;
    dot_ = dot;
    setTitle(text);
    repaint();
}

int StatusChip::idealWidth() const {
    const auto f = uiFont(height_ > 32.0f ? 12.0f : 11.0f);
    return juce::roundToInt(textWidth(f, text_) + (dot_ == Dot::None ? 36.0f : 50.0f));
}

void StatusChip::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(p.chipBg);
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour(p.hairline2);
    g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
    auto inner = r.reduced(height_ > 32.0f ? 14.0f : 12.0f, 0.0f);
    if (dot_ != Dot::None) {
        const float d = height_ > 32.0f ? 7.0f : 6.0f;
        auto dot = inner.removeFromLeft(d).withSizeKeepingCentre(d, d);
        g.setColour(p.ink.withAlpha(0.12f));
        g.fillEllipse(dot.expanded(3.0f));
        g.setColour(p.ink);
        if (dot_ == Dot::Solid) g.fillEllipse(dot);
        else g.drawEllipse(dot.reduced(0.6f), 1.2f);
        inner.removeFromLeft(8.0f);
    }
    g.setColour(p.ink2);
    g.setFont(uiFont(height_ > 32.0f ? 12.0f : 11.0f));
    g.drawText(text_, inner, juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------
void Banner::set(Style s, icons::Icon i, const juce::String& text) {
    if (s == style_ && i == icon_ && text == text_) return;
    style_ = s;
    icon_ = i;
    text_ = text;
    setTitle(text);
    repaint();
}

int Banner::idealHeight(int width) const {
    const float textW = juce::jmax(10.0f, float(width) - 14.0f * 2.0f - 26.0f);
    return juce::jmax(40, juce::roundToInt(wrappedHeight(uiFont(13.0f), text_, textW)) + 22);
}

void Banner::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    juce::Colour bg = p.ink, fg = p.onInk, edge = p.ink;
    if (style_ == Style::Outline) { bg = p.paper.overlaidWith(p.offBg).withAlpha(1.0f); fg = p.ink; edge = p.ink; }
    if (style_ == Style::Warning) { bg = p.paper.overlaidWith(p.offBg).withAlpha(1.0f); fg = p.ink; edge = p.hairline3; }
    g.setColour(bg);
    g.fillRoundedRectangle(r, theme::radius::small);
    g.setColour(edge);
    g.drawRoundedRectangle(r, theme::radius::small, 1.0f);
    auto inner = r.reduced(14.0f, 0.0f);
    icons::draw(g, icon_, inner.removeFromLeft(16.0f).withSizeKeepingCentre(16.0f, 16.0f), fg);
    inner.removeFromLeft(10.0f);
    const float h = wrappedHeight(uiFont(13.0f), text_, inner.getWidth());
    drawWrapped(g, text_, uiFont(13.0f), fg, inner.withSizeKeepingCentre(inner.getWidth(), h));
}

// ---------------------------------------------------------------------------------------------
void PrimaryButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const auto bg = active_ ? p.paper.overlaidWith(p.offBg).withAlpha(1.0f) : p.ink;
    const auto fg = active_ ? p.ink : p.onInk;
    g.setColour(withPress(bg, p, highlighted, down));
    g.fillRoundedRectangle(r, theme::radius::primary);
    g.setColour(p.ink);
    g.drawRoundedRectangle(r, theme::radius::primary, 1.0f);
    const auto f = uiFont(14.0f, Weight::SemiBold);
    const auto label = getButtonText();
    const float tw = textWidth(f, label);
    const float x = r.getCentreX() - (17.0f + 10.0f + tw) * 0.5f;
    icons::draw(g, icon_, { x, r.getCentreY() - 8.5f, 17.0f, 17.0f }, fg);
    g.setColour(fg);
    g.setFont(f);
    g.drawText(label, juce::Rectangle<float>(x + 27.0f, r.getY(), tw + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);
    if (hasKeyboardFocus(false)) drawFocusRing(g, r, theme::radius::primary, p);
}

int MuteButton::idealWidth() const {
    return juce::roundToInt(textWidth(uiFont(13.0f, Weight::SemiBold), getButtonText()) + 16.0f + 8.0f + 34.0f);
}

void MuteButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const auto bg = active_ ? p.ink : p.paper.overlaidWith(p.chipBg).withAlpha(1.0f);
    const auto fg = active_ ? p.onInk : p.ink;
    g.setColour(withPress(bg, p, highlighted, down));
    g.fillRoundedRectangle(r, theme::radius::small);
    g.setColour(p.ink);
    g.drawRoundedRectangle(r, theme::radius::small, 1.0f);
    auto inner = r.reduced(16.0f, 0.0f);
    icons::draw(g, icons::Icon::SpeakerOff, inner.removeFromLeft(16.0f).withSizeKeepingCentre(16.0f, 16.0f), fg);
    inner.removeFromLeft(8.0f);
    g.setColour(fg);
    g.setFont(uiFont(13.0f, Weight::SemiBold));
    g.drawText(getButtonText(), inner, juce::Justification::centredLeft, false);
    if (hasKeyboardFocus(false)) drawFocusRing(g, r, theme::radius::small, p);
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(p.chipBg.overlaidWith(p.ink.withAlpha(down ? 0.10f : highlighted ? 0.05f : 0.0f)));
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour(p.hairline2);
    g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
    icons::draw(g, icon_, r.withSizeKeepingCentre(17.0f, 17.0f), p.ink2);
    if (hasKeyboardFocus(false)) drawFocusRing(g, r, r.getHeight() * 0.5f, p);
}

} // namespace hearaside
