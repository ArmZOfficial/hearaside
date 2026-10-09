#include "Components.h"
#include "../Settings.h"
#include "../Strings.h"
#include "../ValueText.h"

namespace hearaside {

namespace {

bool reduceMotion() {
    static SharedSettings settings;
    return settings->reduceMotion();
}

// hover / press tint of a filled shape
juce::Colour pressed(juce::Colour base, const theme::Palette& p, bool highlighted, bool down) {
    if (base == p.ink) return down ? base.interpolatedWith(p.paper, 0.22f) : highlighted ? base.interpolatedWith(p.paper, 0.12f) : base;
    if (down) return base.overlaidWith(p.ink.withAlpha(0.08f));
    if (highlighted) return base.overlaidWith(p.ink.withAlpha(0.04f));
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

juce::String ellipsize(const juce::Font& f, const juce::String& s, float width) {
    if (textWidth(f, s) <= width) return s;
    const juce::String dots = juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"));
    int lo = 0, hi = s.length();
    while (lo < hi) {   // the longest prefix that fits with the dots
        const int mid = (lo + hi + 1) / 2;
        if (textWidth(f, s.substring(0, mid).trimEnd() + dots) <= width) lo = mid;
        else hi = mid - 1;
    }
    return s.substring(0, lo).trimEnd() + dots;
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
                 juce::Rectangle<float> r, float lineGap, juce::Justification just, int maxLines) {
    g.setFont(f);
    g.setColour(c);
    auto lines = wrapText(f, text, r.getWidth());
    if (maxLines > 0 && lines.size() > maxLines) {
        juce::String rest;
        for (int i = maxLines - 1; i < lines.size(); ++i) rest << (rest.isEmpty() ? "" : " ") << lines[i];
        lines.removeRange(maxLines - 1, lines.size());
        lines.add(ellipsize(f, rest, r.getWidth()));
    }
    float y = r.getY();
    for (const auto& line : lines) {
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
    // letter-spaced wordmark (0.34em / 0.42em): drawn glyph by glyph
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
    const auto small = uiFont(size * 0.625f, Weight::Medium);
    const float baseline1 = area.getY() + big.getAscent() * 0.86f;
    spaced("HEARASIDE", big, 0.34f, baseline1, colour);
    spaced(sub, small, 0.42f, baseline1 + 3.0f + small.getAscent() * 0.9f, colour.interpolatedWith(colour.contrasting(), 0.35f));
}

// ---------------------------------------------------------------------------------------------
juce::Colour dotColour(Dot d, const theme::Palette& p) {
    switch (d) {
        case Dot::Ok:   return p.ok;
        case Dot::Warn: return p.warn;
        case Dot::Rec: case Dot::RecRing: return p.danger;
        case Dot::Muted: case Dot::None: break;
    }
    return p.graphite;
}

void drawStatusDot(juce::Graphics& g, juce::Point<float> c, Dot d, const theme::Palette& p, float size) {
    if (d == Dot::None) return;
    const auto r = juce::Rectangle<float>(size, size).withCentre(c);
    g.setColour(dotColour(d, p));
    if (d == Dot::RecRing) g.drawEllipse(r.reduced(0.75f), 1.5f);
    else g.fillEllipse(r);
}

float badgeWidth(const juce::String& text, BadgeStyle s) {
    return textWidth(uiFont(11.0f, Weight::Medium), text) + 14.0f + (s == BadgeStyle::Rec || s == BadgeStyle::Ok ? 11.0f : 0.0f) + 2.0f;
}

void drawBadge(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, BadgeStyle s, const theme::Palette& p) {
    r = r.reduced(0.5f);
    const float rad = r.getHeight() * 0.5f;
    juce::Colour bg = p.chipBg, edge = p.hairline2, fg = p.ink2;
    if (s == BadgeStyle::Solid) { bg = p.ink; edge = p.ink; fg = p.onInk; }
    if (s == BadgeStyle::Rec) { bg = juce::Colours::transparentBlack; edge = p.danger; fg = p.danger; }
    if (s == BadgeStyle::Ok) { bg = p.chipBg; edge = p.hairline2; fg = p.ink2; }
    g.setColour(bg);
    g.fillRoundedRectangle(r, rad);
    g.setColour(edge);
    g.drawRoundedRectangle(r, rad, 1.0f);
    auto inner = r.reduced(7.0f, 0.0f);
    if (s == BadgeStyle::Rec || s == BadgeStyle::Ok) {
        drawStatusDot(g, { inner.getX() + 3.5f, inner.getCentreY() }, s == BadgeStyle::Rec ? Dot::Rec : Dot::Ok, p);
        inner.removeFromLeft(11.0f);
    }
    g.setColour(fg);
    g.setFont(uiFont(11.0f, Weight::Medium));
    g.drawText(text, inner, juce::Justification::centredLeft, false);
}

void drawAvatar(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& name, bool solid, const theme::Palette& p,
                const juce::Image& photo) {
    if (photo.isValid()) {
        g.saveState();
        juce::Path clip;
        clip.addEllipse(r);
        g.reduceClipRegion(clip);
        g.drawImage(photo, r, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
        g.restoreState();
        return;
    }
    if (solid) {
        g.setColour(p.ink);
        g.fillEllipse(r);
    } else {
        g.setColour(p.hairline3.withMultipliedAlpha(2.0f));
        g.drawEllipse(r.reduced(0.75f), 1.5f);
    }
    g.setColour(solid ? p.onInk : p.graphite);
    g.setFont(uiFont(r.getHeight() * 0.43f, Weight::SemiBold));
    g.drawText(valuetext::initialOf(name), r, juce::Justification::centred, false);
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
AudibleToggle::AudibleToggle(Side s) : juce::Button({}), side_(s) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 12.0f);
    updateTexts();
}

void AudibleToggle::setOn(bool on, bool animate) {
    if (on == on_ && fade_.value() == (on ? 1.0f : 0.0f)) return;
    on_ = on;
    setToggleState(on, juce::dontSendNotification);
    fade_.setTarget(on ? 1.0f : 0.0f, !animate);
    lastTick_ = juce::Time::getMillisecondCounterHiRes();
    if (fade_.value() != (on ? 1.0f : 0.0f)) startTimerHz(60);
    updateTexts();
    repaint();
}

void AudibleToggle::setSubject(const juce::String& name) {
    if (name == subject_) return;
    subject_ = name;
    updateTexts();
}

void AudibleToggle::updateTexts() {
    const bool you = side_ == Side::You;
    setTooltip(tr(you ? (on_ ? Str::YouHearTip : Str::YouDontHearTip) : (on_ ? Str::ViewersHearTip : Str::ViewersDontHearTip)));
    setTitle(subject_.isNotEmpty() ? tr(you ? Str::YouHearAria : Str::ViewersHearAria).replace("%s", subject_)
                                   : tr(you ? Str::MonLabel : Str::StrLabel));
    setButtonText(tr(you ? Str::MonLabel : Str::StrLabel));
}

int AudibleToggle::idealWidth() const {
    return juce::roundToInt(textWidth(uiFont(13.0f, Weight::Medium), tr(side_ == Side::You ? Str::MonLabel : Str::StrLabel)) + 18.0f + 8.0f + 32.0f);
}

void AudibleToggle::timerCallback() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (!fade_.tick((now - lastTick_) * 0.001, theme::motion::switchMs * 0.001)) stopTimer();
    lastTick_ = now;
    repaint();
}

void AudibleToggle::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float k = fade_.value();
    const float radius = r.getHeight() >= 36.0f ? 12.0f : 10.0f;
    const auto bg = p.paper.overlaidWith(p.offBg).interpolatedWith(p.ink, k);
    g.setColour(highlighted || down ? pressed(k > 0.5f ? p.ink : bg, p, highlighted && isEnabled(), down) : bg);
    g.fillRoundedRectangle(r, radius);
    g.setColour(p.hairline3.interpolatedWith(p.ink, k));
    g.drawRoundedRectangle(r, radius, 1.0f);
    const auto fg = p.offFg.interpolatedWith(p.onInk, k);
    const auto icon = side_ == Side::You ? icons::Icon::Headphones : icons::Icon::Broadcast;
    const float is = r.getHeight() >= 36.0f ? 18.0f : 16.0f;
    if (labelShown_) {
        const auto f = uiFont(13.0f, Weight::Medium);
        const auto label = getButtonText();
        const float total = is + 8.0f + textWidth(f, label);
        const float x = r.getCentreX() - total * 0.5f;
        icons::draw(g, icon, { x, r.getCentreY() - is * 0.5f, is, is }, fg, 1.8f, !on_);
        g.setColour(fg);
        g.setFont(f);
        g.drawText(label, juce::Rectangle<float>(x + is + 8.0f, r.getY(), total, r.getHeight()), juce::Justification::centredLeft, false);
    } else {
        icons::draw(g, icon, r.withSizeKeepingCentre(is, is), fg, 1.8f, !on_);
    }
    if (!isEnabled()) { g.setColour(p.paper.withAlpha(0.45f)); g.fillRoundedRectangle(r, radius); }
}

// ---------------------------------------------------------------------------------------------
ToggleRow::ToggleRow(icons::Icon icon) : juce::Button({}), icon_(icon) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 14.0f);
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
    if (!knob_.tick((now - lastTick_) * 0.001, theme::motion::switchMs * 0.001)) stopTimer();
    lastTick_ = now;
    repaint();
}

void ToggleRow::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float k = knob_.value();
    g.setColour(p.inset.overlaidWith(p.ink.withAlpha(down ? 0.05f : 0.0f)));
    g.fillRoundedRectangle(r, theme::radius::row);
    g.setColour(highlighted ? p.hairline3 : p.hairline1);
    g.drawRoundedRectangle(r, theme::radius::row, 1.0f);

    auto inner = r.withTrimmedLeft(10.0f).withTrimmedRight(12.0f);
    auto tile = inner.removeFromLeft(38.0f).withSizeKeepingCentre(38.0f, 38.0f);
    g.setColour(p.paper.overlaidWith(p.offBg).interpolatedWith(p.ink, k));
    g.fillRoundedRectangle(tile, 11.0f);
    g.setColour(p.hairline3.interpolatedWith(p.ink, k));
    g.drawRoundedRectangle(tile.reduced(0.5f), 11.0f, 1.0f);
    icons::draw(g, icon_, tile.withSizeKeepingCentre(18.0f, 18.0f), p.offFg.interpolatedWith(p.onInk, k), 1.8f, !on_);

    auto sw = inner.removeFromRight(44.0f).withSizeKeepingCentre(44.0f, 26.0f);
    Switch::paintAt(g, sw, k, p);

    inner.removeFromLeft(12.0f);
    inner.removeFromRight(12.0f);
    const auto tf = uiFont(14.0f, Weight::SemiBold);
    const auto cf = uiFont(12.0f);
    const float block = tf.getHeight() + 1.0f + cf.getHeight();
    auto label = inner.withSizeKeepingCentre(inner.getWidth(), block);
    g.setColour(p.ink);
    g.setFont(tf);
    g.drawText(title_, label.removeFromTop(tf.getHeight()), juce::Justification::centredLeft, true);
    label.removeFromTop(1.0f);
    g.setColour(p.graphite);
    g.setFont(cf);
    g.drawText(caption_, label, juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------
Switch::Switch() : juce::Button({}) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, -1.0f);
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
    if (!knob_.tick((now - lastTick_) * 0.001, theme::motion::switchMs * 0.001)) stopTimer();
    lastTick_ = now;
    repaint();
}

void Switch::paintAt(juce::Graphics& g, juce::Rectangle<float> sw, float k, const theme::Palette& p, float alpha) {
    g.setColour(p.switchOff.interpolatedWith(p.ink, k).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(sw, sw.getHeight() * 0.5f);
    const float d = sw.getHeight() - 4.0f;
    const float kx = sw.getX() + 2.0f + (sw.getWidth() - 4.0f - d) * k;
    g.setColour(juce::Colours::white.interpolatedWith(p.onInk, k).withMultipliedAlpha(alpha));
    g.fillEllipse(kx, sw.getY() + 2.0f, d, d);
}

void Switch::paintButton(juce::Graphics& g, bool highlighted, bool) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat();
    const bool large = r.getHeight() >= 26.0f;
    const auto sw = r.withSizeKeepingCentre(large ? juce::jmin(r.getWidth(), 46.0f) : 36.0f, large ? 26.0f : 20.0f);
    setFocusShape(*this, -1.0f);
    Switch::paintAt(g, sw, knob_.value(), p, isEnabled() ? 1.0f : 0.45f);
    if (highlighted && isEnabled()) { g.setColour(p.ink.withAlpha(0.05f)); g.fillRoundedRectangle(sw, sw.getHeight() * 0.5f); }
}

LabelledSwitch::LabelledSwitch() {
    setRepaintsOnMouseActivity(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    addAndMakeVisible(sw_);
    sw_.onClick = [this] { if (onClick) onClick(); repaint(); };
}

int LabelledSwitch::idealWidth() const {
    const auto f = uiFont(12.5f, pill_ ? Weight::SemiBold : Weight::Medium);
    const float text = juce::jmax(textWidth(f, tr(Str::PowerOn)), textWidth(f, tr(Str::PowerOff)));
    return juce::roundToInt(text + 10.0f + 44.0f + (pill_ ? 12.0f + 6.0f : 0.0f)) + 2;
}

void LabelledSwitch::resized() {
    auto r = getLocalBounds();
    if (pill_) r.removeFromRight(6);
    sw_.setBounds(r.removeFromRight(44).withSizeKeepingCentre(44, 26));
}

void LabelledSwitch::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat();
    if (pill_) {
        const auto pill = r.reduced(0.5f);
        g.setColour(isMouseOver(true) ? p.paper.overlaidWith(p.offBg) : p.chipBg);
        g.fillRoundedRectangle(pill, pill.getHeight() * 0.5f);
        g.setColour(p.hairline2);
        g.drawRoundedRectangle(pill, pill.getHeight() * 0.5f, 1.0f);
        r.removeFromLeft(12.0f);
        r.removeFromRight(6.0f);
    }
    r.removeFromRight(44.0f + 10.0f);
    g.setColour(sw_.isOn() || pill_ ? p.ink : p.graphite);
    g.setFont(uiFont(12.5f, pill_ ? Weight::SemiBold : Weight::Medium));
    g.drawText(tr(sw_.isOn() ? Str::PowerOn : Str::PowerOff), r, pill_ ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
}

void LabelledSwitch::mouseUp(const juce::MouseEvent& e) {
    if (getLocalBounds().contains(e.getPosition()) && !e.mods.isPopupMenu()) sw_.triggerClick();
}

// ---------------------------------------------------------------------------------------------
LevelSlider::LevelSlider(double maxDb) : juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox) {
    setRange(-30.0, maxDb, 0.5);
    setDoubleClickReturnValue(true, 0.0);
    setScrollWheelEnabled(true);
    // Shift + drag = fine adjustment (velocity mode with low sensitivity)
    setVelocityModeParameters(0.25, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusDrawnBySelf(*this);
    setTooltip(tr(Str::DoubleClickToReset));
}

void LevelSlider::setDim(bool d) {
    if (bool(getProperties()[sliderlook::dim]) == d) return;
    getProperties().set(sliderlook::dim, d);
    repaint();
}

void LevelSlider::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isAltDown() && isEnabled()) {   // Alt/Option + click = 0 dB
        setValue(0.0, juce::sendNotificationSync);
        return;
    }
    juce::Slider::mouseDown(e);
}

// ---------------------------------------------------------------------------------------------
PanSlider::PanSlider(bool full) : full_(full) {
    slider_.setRange(-100.0, 100.0, 1.0);
    slider_.setDoubleClickReturnValue(true, 0.0);
    slider_.setScrollWheelEnabled(true);
    slider_.setWantsKeyboardFocus(true);
    slider_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    slider_.getProperties().set(sliderlook::fromCentre, true);
    slider_.setTooltip(tr(Str::PanTypeTip));
    setFocusDrawnBySelf(slider_);
    slider_.onValueChange = [this] { if (onChange) onChange(float(slider_.getValue())); };
    slider_.onDragStart = [this] { if (onDragStart) onDragStart(); };
    slider_.onDragEnd = [this] { if (onDragEnd) onDragEnd(); };
    addAndMakeVisible(slider_);
}

void PanSlider::resized() {
    slider_.setBounds(getLocalBounds().removeFromTop(kCompactHeight));
}

void PanSlider::paint(juce::Graphics& g) {
    if (!full_) return;
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().withTrimmedTop(float(kCompactHeight) + 4.0f);
    g.setFont(uiFont(11.0f));
    g.setColour(p.graphite);
    g.drawText("L", r, juce::Justification::centredLeft, false);
    g.drawText("R", r, juce::Justification::centredRight, false);
    g.drawText(tr(Str::PanCenterHint), r, juce::Justification::centred, false);
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
    g.setColour(p.sliderRail);
    g.fillRoundedRectangle(r, thickness_ * 0.5f);
    if (level_ > 0.0f) {
        g.setColour(soft_ ? p.ink2 : p.ink);
        g.fillRoundedRectangle(r.withWidth(juce::jmax(thickness_, r.getWidth() * level_)), thickness_ * 0.5f);
    }
}

// ---------------------------------------------------------------------------------------------
StatusChip::StatusChip(float height) : juce::Button({}), height_(height) {
    setFocusShape(*this, -1.0f);
}

void StatusChip::set(const juce::String& label, Dot dot, const juce::String& value, bool chevron) {
    if (label == text_ && dot == dot_ && value == value_ && chevron == chevron_) return;
    text_ = label;
    dot_ = dot;
    value_ = value;
    chevron_ = chevron;
    setTitle(label + (value.isNotEmpty() ? ", " + value : juce::String()));
    setWantsKeyboardFocus(chevron);
    setMouseCursor(chevron ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

int StatusChip::idealWidth() const {
    const auto f = uiFont(height_ > 32.0f ? 12.5f : 12.0f, Weight::Medium);
    float w = textWidth(f, text_) + (height_ > 32.0f ? 26.0f : 22.0f);
    if (dot_ != Dot::None) w += 7.0f + 8.0f;
    if (value_.isNotEmpty()) w += 8.0f + textWidth(f, value_);
    if (chevron_) w += 8.0f + 14.0f;
    return juce::roundToInt(w) + 2;
}

void StatusChip::paintButton(juce::Graphics& g, bool highlighted, bool) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const bool hover = highlighted && chevron_;
    g.setColour(hover ? p.paper.overlaidWith(p.offBg) : p.chipBg);
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour(hover ? p.hairline3 : p.hairline2);
    g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
    auto inner = r.withTrimmedLeft(height_ > 32.0f ? 14.0f : 11.0f).withTrimmedRight(height_ > 32.0f ? 12.0f : 11.0f);
    if (dot_ != Dot::None) {
        drawStatusDot(g, { inner.getX() + 3.5f, inner.getCentreY() }, dot_, p);
        inner.removeFromLeft(15.0f);
    }
    const auto f = uiFont(height_ > 32.0f ? 12.5f : 12.0f, Weight::Medium);
    if (chevron_) {
        icons::draw(g, icons::Icon::ChevronDown, inner.removeFromRight(14.0f).withSizeKeepingCentre(14.0f, 14.0f), p.graphite, 2.0f);
        inner.removeFromRight(8.0f);
    }
    g.setFont(f);
    g.setColour(p.ink2);
    const float tw = textWidth(f, text_);
    g.drawText(text_, inner.removeFromLeft(juce::jmin(inner.getWidth(), tw + 1.0f)), juce::Justification::centredLeft, true);
    if (value_.isNotEmpty()) {
        inner.removeFromLeft(4.0f);
        g.setColour(p.graphite);
        g.drawText(juce::String(juce::CharPointer_UTF8("\xc2\xb7 ")) + value_, inner, juce::Justification::centredLeft, true);
    }
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
    const float textW = juce::jmax(10.0f, float(width) - 12.0f * 2.0f - 26.0f);
    return juce::jmax(40, juce::roundToInt(wrappedHeight(uiFont(12.5f), text_, textW, 4.0f)) + 20);
}

void Banner::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    juce::Colour bg = p.warnBg, fg = p.ink, edge = p.hairline2, iconC = p.warn;
    if (style_ == Style::Info) { bg = p.inset; iconC = p.graphite; }
    if (style_ == Style::Dark) { bg = p.ink; fg = p.onInk; edge = p.ink; iconC = p.onInk; }
    if (style_ == Style::Outline) { bg = p.paper.overlaidWith(p.offBg); edge = p.ink; iconC = p.ink; }
    g.setColour(bg);
    g.fillRoundedRectangle(r, 12.0f);
    g.setColour(edge);
    g.drawRoundedRectangle(r, 12.0f, 1.0f);
    auto inner = r.reduced(12.0f, 10.0f);
    const float h = wrappedHeight(uiFont(12.5f), text_, inner.getWidth() - 26.0f, 4.0f);
    auto iconArea = inner.removeFromLeft(16.0f);
    icons::draw(g, icon_, iconArea.withHeight(16.0f).withY(inner.getCentreY() - juce::jmin(h, inner.getHeight()) * 0.5f + 2.0f), iconC);
    inner.removeFromLeft(10.0f);
    drawWrapped(g, text_, uiFont(12.5f), fg, inner.withSizeKeepingCentre(inner.getWidth(), h), 4.0f);
}

// ---------------------------------------------------------------------------------------------
PrimaryButton::PrimaryButton(icons::Icon icon, float height) : juce::Button({}), icon_(icon), height_(height) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, theme::radius::primary);
}

void PrimaryButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.75f);
    const float radius = r.getHeight() >= 44.0f ? theme::radius::primary : 12.0f;
    setFocusShape(*this, radius);
    if (active_) {
        g.setColour(pressed(juce::Colours::transparentBlack, p, highlighted, down));
        g.fillRoundedRectangle(r, radius);
    } else {
        g.setColour(pressed(p.ink, p, highlighted, down));
        g.fillRoundedRectangle(r, radius);
    }
    g.setColour(p.ink.withMultipliedAlpha(isEnabled() ? 1.0f : 0.4f));
    g.drawRoundedRectangle(r, radius, 1.5f);
    const auto fg = (active_ ? p.ink : p.onInk).withMultipliedAlpha(isEnabled() ? 1.0f : 0.6f);
    const auto f = uiFont(r.getHeight() >= 44.0f ? 14.0f : 13.0f, Weight::SemiBold);
    const auto label = getButtonText();
    const float tw = textWidth(f, label);
    const float is = hasIcon_ ? 18.0f : 0.0f;
    const float x = r.getCentreX() - (is + (hasIcon_ ? 10.0f : 0.0f) + tw) * 0.5f;
    if (hasIcon_) icons::draw(g, icon_, { x, r.getCentreY() - is * 0.5f, is, is }, fg);
    g.setColour(fg);
    g.setFont(f);
    g.drawText(label, juce::Rectangle<float>(x + (hasIcon_ ? is + 10.0f : 0.0f), r.getY(), tw + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);
    if (!isEnabled() && !active_) { g.setColour(p.paper.withAlpha(0.5f)); g.fillRoundedRectangle(r, radius); }
}

MuteButton::MuteButton() : juce::Button({}) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 12.0f);
    setActive(false);
    active_ = false;
}

void MuteButton::setActive(bool a) {
    active_ = a;
    setButtonText(tr(a ? Str::MuteOn : Str::MuteOff));
    setTitle(getButtonText());
    setTooltip(iconOnly_ ? getButtonText() : juce::String());
    repaint();
}

int MuteButton::idealWidth() const {
    if (iconOnly_) return 40;
    const auto f = uiFont(13.0f, Weight::SemiBold);
    return juce::roundToInt(juce::jmax(textWidth(f, tr(Str::MuteOff)), textWidth(f, tr(Str::MuteOn))) + 16.0f + 8.0f + 34.0f);
}

void MuteButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.75f);
    if (active_) {
        g.setColour(pressed(p.dangerBg, p, highlighted, down));
        g.fillRoundedRectangle(r, 12.0f);
    } else {
        g.setColour(down ? p.ink.withAlpha(0.10f) : highlighted ? p.ink.withAlpha(0.05f) : juce::Colours::transparentBlack);
        g.fillRoundedRectangle(r, 12.0f);
        g.setColour(p.ink);
        g.drawRoundedRectangle(r, 12.0f, 1.5f);
    }
    const auto fg = active_ ? juce::Colours::white : p.ink;
    const auto icon = active_ ? icons::Icon::SpeakerOn : icons::Icon::SpeakerOff;
    if (iconOnly_) {
        icons::draw(g, icon, r.withSizeKeepingCentre(18.0f, 18.0f), fg);
        return;
    }
    const auto f = uiFont(13.0f, Weight::SemiBold);
    const float tw = textWidth(f, getButtonText());
    const float x = r.getCentreX() - (16.0f + 8.0f + tw) * 0.5f;
    icons::draw(g, icon, { x, r.getCentreY() - 8.0f, 16.0f, 16.0f }, fg);
    g.setColour(fg);
    g.setFont(f);
    g.drawText(getButtonText(), juce::Rectangle<float>(x + 24.0f, r.getY(), tw + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);
}

IconButton::IconButton(icons::Icon icon, const juce::String& tip) : juce::Button({}), icon_(icon) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 12.0f);
    if (tip.isNotEmpty()) { setTooltip(tip); setTitle(tip); }
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float radius = r.getHeight() >= 38.0f ? 12.0f : 11.0f;
    setFocusShape(*this, radius);
    if (on_) {
        g.setColour(pressed(p.ink, p, highlighted, down));
        g.fillRoundedRectangle(r, radius);
    } else {
        g.setColour(highlighted || down ? p.paper.overlaidWith(p.offBg).overlaidWith(p.ink.withAlpha(down ? 0.06f : 0.0f)) : p.chipBg);
        g.fillRoundedRectangle(r, radius);
        g.setColour(highlighted ? p.hairline3 : p.hairline2);
        g.drawRoundedRectangle(r, radius, 1.0f);
    }
    const float is = r.getHeight() >= 38.0f ? 18.0f : 17.0f;
    icons::draw(g, icon_, r.withSizeKeepingCentre(is, is), on_ ? p.onInk : p.ink2.withMultipliedAlpha(isEnabled() ? 1.0f : 0.4f));
}

MoreButton::MoreButton() : juce::Button({}) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 10.0f);
}

void MoreButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat().withSizeKeepingCentre(32.0f, 32.0f);
    if (highlighted || down || open_) {
        g.setColour(p.hairline2.withMultipliedAlpha(down ? 1.6f : 1.0f));
        g.fillRoundedRectangle(r, 10.0f);
    }
    icons::draw(g, icons::Icon::More, r.withSizeKeepingCentre(18.0f, 18.0f), highlighted || open_ ? p.ink : p.graphite);
}

GhostButton::GhostButton(const juce::String& text, Style s) : juce::Button(text), style_(s) {
    setButtonText(text);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 12.0f);
}

int GhostButton::idealWidth() const {
    const auto f = uiFont(small_ ? 12.0f : 13.0f, Weight::Medium);
    const float pad = small_ ? 10.0f : 14.0f;
    return juce::roundToInt(textWidth(f, getButtonText()) + pad * 2.0f + (hasIcon_ ? (small_ ? 14.0f : 16.0f) + (getButtonText().isNotEmpty() ? 8.0f : 0.0f) : 0.0f)) + 2;
}

void GhostButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float radius = small_ ? 9.0f : 12.0f;
    setFocusShape(*this, radius);
    const bool solid = style_ == Style::Solid || style_ == Style::DangerSolid;
    const auto accent = (style_ == Style::Danger || style_ == Style::DangerSolid) ? (solid ? p.dangerBg : p.danger) : p.ink;
    if (solid) {
        g.setColour(pressed(accent == p.ink ? p.ink : accent, p, highlighted, down));
        g.fillRoundedRectangle(r, radius);
    } else {
        g.setColour(highlighted || down ? p.paper.overlaidWith(p.offBg).overlaidWith(p.ink.withAlpha(down ? 0.06f : 0.0f)) : p.chipBg);
        g.fillRoundedRectangle(r, radius);
        g.setColour(style_ == Style::Danger ? p.danger : highlighted ? p.sliderRail : p.hairline3);
        g.drawRoundedRectangle(r, radius, 1.0f);
    }
    auto fg = solid ? (style_ == Style::DangerSolid ? juce::Colours::white : p.onInk) : (style_ == Style::Danger ? p.danger : p.ink);
    if (!isEnabled()) fg = fg.withMultipliedAlpha(0.4f);
    const auto f = uiFont(small_ ? 12.0f : 13.0f, Weight::Medium);
    const float is = hasIcon_ ? (small_ ? 14.0f : 16.0f) : 0.0f;
    const float gap = hasIcon_ && getButtonText().isNotEmpty() ? 8.0f : 0.0f;
    const float tw = textWidth(f, getButtonText());
    const float total = juce::jmin(r.getWidth() - 12.0f, is + gap + tw);
    const float x = r.getCentreX() - total * 0.5f;
    if (hasIcon_) icons::draw(g, icon_, { x, r.getCentreY() - is * 0.5f, is, is }, fg);
    g.setColour(fg);
    g.setFont(f);
    g.drawText(getButtonText(), juce::Rectangle<float>(x + is + gap, r.getY(), total - is - gap + 1.0f, r.getHeight()), juce::Justification::centredLeft, true);
    if (!isEnabled() && solid) { g.setColour(p.paper.withAlpha(0.5f)); g.fillRoundedRectangle(r, radius); }
}

LinkButton::LinkButton(const juce::String& text, float size) : juce::Button(text), size_(size) {
    setButtonText(text);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 6.0f);
}

int LinkButton::idealWidth() const { return juce::roundToInt(textWidth(uiFont(size_, Weight::Medium), getButtonText())) + 4; }

void LinkButton::paintButton(juce::Graphics& g, bool highlighted, bool) {
    const auto& p = paletteOf(*this);
    const auto f = uiFont(size_, Weight::Medium);
    const auto c = (highlighted ? p.ink : p.ink).withMultipliedAlpha(isEnabled() ? 1.0f : 0.4f);
    g.setColour(c);
    g.setFont(f);
    const auto r = getLocalBounds().toFloat();
    g.drawText(getButtonText(), r, juce::Justification::centredLeft, true);
    const float tw = juce::jmin(r.getWidth(), textWidth(f, getButtonText()));
    const float y = r.getCentreY() + f.getAscent() * 0.5f + 1.5f;
    g.fillRect(juce::Rectangle<float>(r.getX(), y, tw, highlighted ? 1.4f : 1.0f));
}

BackButton::BackButton(bool withText) : juce::Button({}), withText_(withText) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 10.0f);
}

int BackButton::idealWidth() const {
    const bool big = getHeight() >= 38;
    return withText_ ? juce::roundToInt(textWidth(uiFont(big ? 13.0f : 12.5f, Weight::Medium), tr(Str::Back)) + (big ? 10.0f + 14.0f + 6.0f : 8.0f + 12.0f + 4.0f) + 16.0f) + 2
                     : juce::jmax(34, getHeight());
}

void BackButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const bool big = r.getHeight() >= 38.0f;
    const float radius = big ? 12.0f : 10.0f;
    setFocusShape(*this, radius);
    g.setColour(highlighted || down ? p.paper.overlaidWith(p.offBg).overlaidWith(p.ink.withAlpha(down ? 0.06f : 0.0f)) : p.chipBg);
    g.fillRoundedRectangle(r, radius);
    g.setColour(highlighted ? p.hairline3 : p.hairline2);
    g.drawRoundedRectangle(r, radius, 1.0f);
    if (!withText_) {
        icons::draw(g, icons::Icon::ChevronLeft, r.withSizeKeepingCentre(18.0f, 18.0f), p.ink, 2.0f);
        return;
    }
    auto inner = r.withTrimmedLeft(big ? 10.0f : 8.0f).withTrimmedRight(big ? 14.0f : 12.0f);
    icons::draw(g, icons::Icon::ChevronLeft, inner.removeFromLeft(16.0f).withSizeKeepingCentre(16.0f, 16.0f), p.ink, 2.0f);
    inner.removeFromLeft(big ? 6.0f : 4.0f);
    g.setColour(p.ink);
    g.setFont(uiFont(big ? 13.0f : 12.5f, Weight::Medium));
    g.drawText(tr(Str::Back), inner, juce::Justification::centredLeft, false);
}

} // namespace hearaside
