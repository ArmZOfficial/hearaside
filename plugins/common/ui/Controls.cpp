#include "Controls.h"

namespace hearaside {

namespace {

// Keeps a TextEditor's UTF-8 size within a limit (cut at a code point).
struct Utf8Limit : juce::TextEditor::InputFilter {
    explicit Utf8Limit(int b) : bytes(b) {}
    juce::String filterNewText(juce::TextEditor& e, const juce::String& in) override {
        const auto sel = e.getHighlightedRegion();
        const auto text = e.getText();
        const int kept = int(text.substring(0, sel.getStart()).getNumBytesAsUTF8() + text.substring(sel.getEnd()).getNumBytesAsUTF8());
        return valuetext::truncateUtf8(in.removeCharacters("\r\n\t"), juce::jmax(0, bytes - kept));
    }
    int bytes;
};

// deletes a field after the event that ended it (it may be the caller)
void disposeLater(std::unique_ptr<TextField>& f) {
    if (f == nullptr) return;
    auto* raw = f.release();
    raw->onReturnKey = nullptr;
    raw->onEscapeKey = nullptr;
    raw->onFocusLost = nullptr;
    raw->setVisible(false);
    if (auto* parent = raw->getParentComponent()) parent->removeChildComponent(raw);
    juce::MessageManager::callAsync([raw] { delete raw; });
}

} // namespace

// ---------------------------------------------------------------------------------------------
TextField::TextField(const juce::String& placeholder) {
    setFont(uiFont(13.0f));
    setIndents(12, 0);
    setJustification(juce::Justification::centredLeft);
    setSelectAllWhenFocused(false);
    setFocusAlways(*this);
    setFocusShape(*this, 10.0f);
    setPlaceholder(placeholder);
}

void TextField::setPlaceholder(const juce::String& p) {
    placeholder_ = p;
    recolour();
}

void TextField::recolour() {
    // applyColourToAllText(c, true) pins c on this component: a field first coloured before it had the
    // editor's LookAndFeel (default = white) would stay white in the light theme. Always ask the LookAndFeel.
    removeColour(juce::TextEditor::textColourId);
    applyColourToAllText(findColour(juce::TextEditor::textColourId), false);
    setTextToShowWhenEmpty(placeholder_, paletteOf(*this).graphite);
}

void TextField::setMaxUtf8Bytes(int bytes) { setInputFilter(new Utf8Limit(bytes), true); }

void TextField::setInline(bool inPlace) {
    if (inPlace) {
        getProperties().set("hsInline", true);
        getProperties().set("hsRadius", 7.0f);
        setIndents(6, 0);
        setFocusShape(*this, 7.0f);
    }
}

// ---------------------------------------------------------------------------------------------
EditableValue::EditableValue(Kind k, float lo, float hi) : kind_(k), lo_(lo), hi_(hi) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::IBeamCursor);
    setRepaintsOnMouseActivity(true);
    setFocusShape(*this, 7.0f);
    setTooltip(tr(kind_ == Kind::Pan ? Str::PanTypeTip : Str::DoubleClickToType));
}

juce::String EditableValue::text() const {
    switch (kind_) {
        case Kind::Db:      return valuetext::formatDb(value_, floor_);
        case Kind::Ms:      return valuetext::formatMs(value_);
        case Kind::Pan:     return valuetext::formatPan(value_);
        case Kind::Percent: return valuetext::formatPercent(value_);
    }
    return {};
}

void EditableValue::setValue(float v) {
    if (std::abs(v - value_) < 1.0e-5f) return;
    value_ = v;
    setTitle(text());
    repaint();
}

void EditableValue::startEditing() {
    if (editor_ != nullptr || !isEnabled()) return;
    editor_ = std::make_unique<TextField>();
    editor_->setInline(true);
    editor_->setFont(uiFont(size_));
    editor_->setJustification(juce::Justification::centredRight);
    editor_->setIndents(6, 0);
    switch (kind_) {
        case Kind::Db:      editor_->setText(valuetext::editDb(value_, floor_), false); break;
        case Kind::Ms:      editor_->setText(juce::String(juce::roundToInt(value_)), false); break;
        case Kind::Pan:     editor_->setText(valuetext::formatPan(value_), false); break;
        case Kind::Percent: editor_->setText(juce::String(juce::roundToInt(value_ * 100.0f)), false); break;
    }
    editor_->onReturnKey = [this] { finish(true); };
    editor_->onEscapeKey = [this] { finish(false); };
    editor_->onFocusLost = [this] { finish(true); };
    addAndMakeVisible(*editor_);
    resized();
    editor_->selectAll();
    editor_->grabKeyboardFocus();
    repaint();
}

void EditableValue::finish(bool commit) {
    if (editor_ == nullptr) return;
    const auto typed = editor_->getText();
    disposeLater(editor_);
    repaint();
    if (!commit) return;
    std::optional<float> v;
    switch (kind_) {
        case Kind::Db:      v = valuetext::parseDb(typed, lo_, hi_); break;
        case Kind::Ms:      v = valuetext::parseMs(typed, lo_, hi_); break;
        case Kind::Pan:     v = valuetext::parsePan(typed); break;
        case Kind::Percent: v = valuetext::parsePercent(typed, lo_, hi_); break;
    }
    if (!v) return;   // nonsense: the old value stays
    if (kind_ == Kind::Pan) v = juce::jlimit(lo_, hi_, *v);
    value_ = *v;
    setTitle(text());
    if (onCommit) onCommit(*v);
}

bool EditableValue::keyPressed(const juce::KeyPress& k) {
    if (k == juce::KeyPress::returnKey || k == juce::KeyPress::F2Key) { startEditing(); return true; }
    return false;
}

void EditableValue::resized() {
    if (editor_ != nullptr) editor_->setBounds(getLocalBounds().withSizeKeepingCentre(getWidth(), juce::jmin(getHeight(), 26)));
}

void EditableValue::paint(juce::Graphics& g) {
    if (editor_ != nullptr) return;
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat().withSizeKeepingCentre(float(getWidth()), juce::jmin(float(getHeight()), 26.0f)).reduced(0.5f);
    const bool hover = isMouseOver() && isEnabled();
    if (hover) {
        g.setColour(p.hairline1);
        g.fillRoundedRectangle(r, 7.0f);
        g.setColour(p.hairline2);
        g.drawRoundedRectangle(r, 7.0f, 1.0f);
    }
    g.setColour(hover ? p.ink : dim_ ? p.graphite : p.ink2);
    g.setFont(uiFont(size_));
    g.drawText(text(), r.reduced(6.0f, 0.0f), just_, false);
}

// ---------------------------------------------------------------------------------------------
InlineName::InlineName(float fontSize, Weight w) : size_(fontSize), weight_(w) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::IBeamCursor);
    setRepaintsOnMouseActivity(true);
    setFocusShape(*this, 7.0f);
}

void InlineName::setName(const juce::String& shown, const juce::String& daw) {
    if (shown == name_ && daw == daw_) return;
    name_ = shown;
    daw_ = daw;
    setTitle(shown);
    setTooltip(tr(Str::DoubleClickToRename) + (daw.isNotEmpty() ? "\n" + tr(Str::NameInDaw).replace("%s", daw) : juce::String()));
    repaint();
}

int InlineName::idealWidth() const { return juce::roundToInt(textWidth(uiFont(size_, weight_), name_)) + 14; }

void InlineName::startEditing() {
    if (editor_ != nullptr || !isEnabled()) return;
    editor_ = std::make_unique<TextField>();
    editor_->setInline(true);
    editor_->setFont(uiFont(size_, weight_));
    editor_->setMaxUtf8Bytes(63);
    editor_->setText(name_, false);
    editor_->setPlaceholder(daw_);
    editor_->onReturnKey = [this] { finish(true); };
    editor_->onEscapeKey = [this] { finish(false); };
    editor_->onFocusLost = [this] { finish(true); };
    addAndMakeVisible(*editor_);
    resized();
    editor_->selectAll();
    editor_->grabKeyboardFocus();
    repaint();
}

void InlineName::finish(bool commit) {
    if (editor_ == nullptr) return;
    const auto typed = valuetext::truncateUtf8(editor_->getText().trim(), 63);
    disposeLater(editor_);
    repaint();
    if (!commit) return;
    if (typed == name_) return;
    if (typed.isEmpty() && name_ == daw_) return;
    if (onRename) onRename(typed);
}

bool InlineName::keyPressed(const juce::KeyPress& k) {
    if (k == juce::KeyPress::returnKey || k == juce::KeyPress::F2Key) { startEditing(); return true; }
    return false;
}

void InlineName::resized() {
    if (editor_ != nullptr) editor_->setBounds(getLocalBounds().withSizeKeepingCentre(getWidth(), juce::jmin(getHeight(), 26)));
}

void InlineName::paint(juce::Graphics& g) {
    if (editor_ != nullptr) return;
    const auto& p = paletteOf(*this);
    const auto f = uiFont(size_, weight_);
    const float tw = juce::jmin(float(getWidth()), textWidth(f, name_) + 13.0f);
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, tw, float(getHeight())).withSizeKeepingCentre(tw, juce::jmin(float(getHeight()), 26.0f)).reduced(0.5f);
    if (isMouseOver() && isEnabled()) {
        g.setColour(p.hairline1);
        g.fillRoundedRectangle(r, 7.0f);
        g.setColour(p.hairline2);
        g.drawRoundedRectangle(r, 7.0f, 1.0f);
    }
    g.setColour(p.ink);
    g.setFont(f);
    g.drawText(ellipsize(f, name_, r.getWidth() - 12.0f), r.reduced(6.0f, 0.0f), juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------------------------------------
SegmentedControl::SegmentedControl() {
    setWantsKeyboardFocus(true);
    setRepaintsOnMouseActivity(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 10.0f);
}

void SegmentedControl::setSegments(std::vector<Segment> s) {
    segs_ = std::move(s);
    selected_ = juce::jlimit(0, juce::jmax(0, int(segs_.size()) - 1), selected_);
    repaint();
}

void SegmentedControl::setSelected(int i, bool notify) {
    i = juce::jlimit(0, juce::jmax(0, int(segs_.size()) - 1), i);
    if (i == selected_) return;
    selected_ = i;
    repaint();
    if (notify && onChange) onChange(i);
}

int SegmentedControl::idealWidth() const {
    const auto f = uiFont(12.0f, Weight::Medium);
    float w = 8.0f;
    for (const auto& s : segs_) w += textWidth(f, s.text) + 18.0f + (s.icon != icons::Icon::Count ? 18.0f : 0.0f) + 2.0f;
    return juce::roundToInt(w);
}

std::vector<juce::Rectangle<float>> SegmentedControl::boxes() const {
    std::vector<juce::Rectangle<float>> out;
    if (segs_.empty()) return out;
    auto r = getLocalBounds().toFloat().reduced(4.0f);
    // segments share the width in proportion to their text, like CSS flex: 1 with content
    const auto f = uiFont(12.0f, Weight::Medium);
    float total = 0.0f;
    std::vector<float> need;
    for (const auto& s : segs_) { need.push_back(textWidth(f, s.text) + 18.0f + (s.icon != icons::Icon::Count ? 18.0f : 0.0f)); total += need.back(); }
    const float gap = 2.0f;
    const float extra = (r.getWidth() - gap * float(segs_.size() - 1) - total) / float(segs_.size());
    float x = r.getX();
    for (size_t i = 0; i < segs_.size(); ++i) {
        const float w = need[i] + extra;
        out.push_back({ x, r.getY(), w, r.getHeight() });
        x += w + gap;
    }
    return out;
}

void SegmentedControl::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float radius = tall_ ? 11.0f : 10.0f;
    g.setColour(p.inset);
    g.fillRoundedRectangle(r, radius);
    g.setColour(p.hairline1);
    g.drawRoundedRectangle(r, radius, 1.0f);
    const auto bs = boxes();
    const auto f = uiFont(tall_ ? 12.5f : 12.0f, Weight::Medium);
    for (size_t i = 0; i < bs.size(); ++i) {
        // (the selected segment: menuBg card with a hairline ring, like the mock)
        const auto b = bs[i];
        const bool on = int(i) == selected_;
        if (on) {
            juce::Path sh;
            sh.addRoundedRectangle(b, radius - 2.0f);
            juce::DropShadow(p.shadow.withMultipliedAlpha(2.0f), 3, { 0, 1 }).drawForPath(g, sh);
            g.setColour(p.menuBg);
            g.fillRoundedRectangle(b, radius - 2.0f);
            g.setColour(p.hairline2);
            g.drawRoundedRectangle(b.reduced(0.5f), radius - 2.0f, 1.0f);
        } else if (int(i) == hot_) {
            g.setColour(p.hairline1);
            g.fillRoundedRectangle(b, radius - 2.0f);
        }
        const auto c = on || int(i) == hot_ ? p.ink : p.graphite;
        const float tw = textWidth(f, segs_[i].text);
        const bool icon = segs_[i].icon != icons::Icon::Count;
        const float total = tw + (icon ? 18.0f : 0.0f);
        float x = b.getCentreX() - total * 0.5f;
        if (icon) { icons::draw(g, segs_[i].icon, { x, b.getCentreY() - 6.5f, 13.0f, 13.0f }, c); x += 18.0f; }
        g.setColour(c);
        g.setFont(f);
        g.drawText(segs_[i].text, juce::Rectangle<float>(x, b.getY(), juce::jmin(tw + 2.0f, b.getRight() - x), b.getHeight()), juce::Justification::centredLeft, false);
    }
}

void SegmentedControl::mouseMove(const juce::MouseEvent& e) {
    const auto bs = boxes();
    int h = -1;
    for (size_t i = 0; i < bs.size(); ++i) if (bs[i].expanded(1.0f).contains(e.position)) h = int(i);
    if (h != hot_) { hot_ = h; repaint(); }
}

void SegmentedControl::mouseUp(const juce::MouseEvent& e) {
    const auto bs = boxes();
    for (size_t i = 0; i < bs.size(); ++i)
        if (bs[i].expanded(1.0f).contains(e.position)) { setSelected(int(i), true); return; }
}

bool SegmentedControl::keyPressed(const juce::KeyPress& k) {
    if (k == juce::KeyPress::leftKey) { setSelected(selected_ - 1, true); return true; }
    if (k == juce::KeyPress::rightKey) { setSelected(selected_ + 1, true); return true; }
    return false;
}

// ---------------------------------------------------------------------------------------------
void TextLine::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const auto f = uiFont(size_, weight_);
    auto r = getLocalBounds().toFloat();
    if (dot_ != Dot::None) {
        const float tw = juce::jmin(r.getWidth() - 13.0f, textWidth(f, text_));
        const float dx = just_.testFlags(juce::Justification::right) ? r.getRight() - tw - 13.0f : r.getX();
        drawStatusDot(g, { dx + 3.5f, r.getCentreY() }, dot_, p);
        if (!just_.testFlags(juce::Justification::right)) r.removeFromLeft(13.0f);
    }
    g.setColour(tone_ == Tone::Ink ? p.ink : tone_ == Tone::Ink2 ? p.ink2 : tone_ == Tone::Danger ? p.danger : p.graphite);
    g.setFont(f);
    g.drawText(ellipsize(f, text_, r.getWidth()), r, just_, false);
}

// ---------------------------------------------------------------------------------------------
namespace { const juce::Font stepFont() { return uiFont(12.5f); } }

int NumberedSteps::idealHeight(int width) const {
    float h = 0.0f;
    for (const auto& st : steps_) h += juce::jmax(20.0f, wrappedHeight(stepFont(), st, float(width) - 30.0f, 6.0f)) + 8.0f;
    return juce::roundToInt(juce::jmax(0.0f, h - 8.0f));
}

void NumberedSteps::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    float y = 0.0f;
    for (int i = 0; i < steps_.size(); ++i) {
        const float th = wrappedHeight(stepFont(), steps_[i], float(getWidth()) - 30.0f, 6.0f);
        const juce::Rectangle<float> circle(0.0f, y, 20.0f, 20.0f);
        g.setColour(p.ink);
        g.fillEllipse(circle);
        g.setColour(p.onInk);
        g.setFont(uiFont(11.0f, Weight::SemiBold));
        g.drawText(juce::String(i + 1), circle, juce::Justification::centred, false);
        drawWrapped(g, steps_[i], stepFont(), p.ink2, { 30.0f, y + 1.0f, float(getWidth()) - 30.0f, th + 2.0f }, 6.0f);
        y += juce::jmax(20.0f, th) + 8.0f;
    }
}

Disclosure::Disclosure(const juce::String& text) : juce::Button(text) {
    setButtonText(text);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 10.0f);
}

void Disclosure::paintButton(juce::Graphics& g, bool highlighted, bool) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    if (highlighted) { g.setColour(p.hairline1); g.fillRoundedRectangle(r, 10.0f); }
    g.setColour(p.hairline2);
    g.drawRoundedRectangle(r, 10.0f, 1.0f);
    auto inner = r.withTrimmedLeft(12.0f).withTrimmedRight(10.0f);
    const auto chev = inner.removeFromRight(16.0f).withSizeKeepingCentre(16.0f, 16.0f);
    g.saveState();
    if (open_) g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::pi, chev.getCentreX(), chev.getCentreY()));
    icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
    g.restoreState();
    g.setColour(p.ink);
    g.setFont(uiFont(12.5f, Weight::Medium));
    g.drawText(getButtonText(), inner, juce::Justification::centredLeft, true);
}

// ---------------------------------------------------------------------------------------------
Stepper::Stepper(int lo, int hi) : lo_(lo), hi_(hi) {
    for (auto* b : { &minus_, &plus_ }) { b->setSmall(true); addAndMakeVisible(*b); }
    minus_.setTitle("-");
    plus_.setTitle("+");
    minus_.onClick = [this] { setValue(value_ - 1); if (onChange) onChange(value_); };
    plus_.onClick = [this] { setValue(value_ + 1); if (onChange) onChange(value_); };
    update();
}

void Stepper::update() {
    minus_.setEnabled(value_ > lo_);
    plus_.setEnabled(value_ < hi_);
}

void Stepper::resized() {
    auto r = getLocalBounds();
    minus_.setBounds(r.removeFromLeft(32).withSizeKeepingCentre(32, 30));
    plus_.setBounds(r.removeFromRight(32).withSizeKeepingCentre(32, 30));
}

void Stepper::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    g.setColour(p.ink);
    g.setFont(uiFont(13.0f, Weight::Medium));
    g.drawText(juce::String(value_) + (unit_.isNotEmpty() ? " " + unit_ : juce::String()), getLocalBounds().reduced(34, 0), juce::Justification::centred, false);
}

} // namespace hearaside
