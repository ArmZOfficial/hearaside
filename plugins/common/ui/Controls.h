// Inputs of the design system (prompt 3.2): values you double-click to type, names you rename in
// place, segmented controls, a stepper and the text field. Values travel through std::function so
// the same control works on a Hub row (the bus), a parameter or a setting.
#pragma once

#include "Components.h"
#include "../ValueText.h"

namespace hearaside {

// A single-line text field: 38 high, radius 10, offBg; typing = graphite edge + soft ring.
class TextField : public juce::TextEditor {
public:
    explicit TextField(const juce::String& placeholder = {});
    void setPlaceholder(const juce::String& p);
    void setMaxUtf8Bytes(int bytes);   // names: 63 bytes + NUL in shared memory
    void setInline(bool inPlace);      // the 26-high field of EditableValue / InlineName
    // text typed before the field got the editor's look keeps its colour: refresh it
    void lookAndFeelChanged() override { juce::TextEditor::lookAndFeelChanged(); recolour(); }
    void parentHierarchyChanged() override { juce::TextEditor::parentHierarchyChanged(); recolour(); }
private:
    void recolour();
    juce::String placeholder_;
};

// ---- EditableValue: "−3.0 dB" / "40 ms" / "L40" right-aligned; double-click = type ------------
// Enter / clicking elsewhere = save, Esc = cancel, out of range = clamped, nonsense = old value.
class EditableValue : public juce::Component, public juce::SettableTooltipClient {
public:
    enum class Kind { Db, Ms, Pan, Percent };
    EditableValue(Kind, float lo, float hi);
    void setValue(float v);                 // what is shown (call from the editor timer)
    float value() const noexcept { return value_; }
    Kind kind() const noexcept { return kind_; }
    void setFloorDb(float f) { floor_ = f; repaint(); }   // at or below: "−∞ dB"
    void setDim(bool d) { if (d != dim_) { dim_ = d; repaint(); } }
    void setFontSize(float px) { size_ = px; repaint(); }
    void setJustification(juce::Justification j) { just_ = j; repaint(); }
    std::function<void(float)> onCommit;    // the user typed a value
    bool isEditing() const noexcept { return editor_ != nullptr; }
    void startEditing();
    juce::String text() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent&) override { startEditing(); }
    bool keyPressed(const juce::KeyPress&) override;
    void focusLost(FocusChangeType) override {}

private:
    void finish(bool commit);
    Kind kind_;
    float lo_, hi_, value_ = 0.0f, floor_ = valuetext::kFloorDb, size_ = 12.5f;
    bool dim_ = false;
    juce::Justification just_ = juce::Justification::centredRight;
    std::unique_ptr<TextField> editor_;
};

// ---- InlineName: double-click to rename in place; empty + Enter = back to the DAW's name -------
class InlineName : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit InlineName(float fontSize = 14.0f, Weight w = Weight::Medium);
    void setName(const juce::String& shown, const juce::String& dawName);   // dawName: for the tooltip
    juce::String name() const { return name_; }
    std::function<void(const juce::String&)> onRename;   // trimmed, <= 63 bytes UTF-8; "" = DAW name
    void startEditing();
    bool isEditing() const noexcept { return editor_ != nullptr; }
    int idealWidth() const;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent&) override { startEditing(); }
    bool keyPressed(const juce::KeyPress&) override;

private:
    void finish(bool commit);
    juce::String name_, daw_;
    float size_;
    Weight weight_;
    std::unique_ptr<TextField> editor_;
};

// ---- SegmentedControl: [Headphones | Viewers], [All 7 | You hear 4 ...] ------------------------
class SegmentedControl : public juce::Component, public juce::SettableTooltipClient {
public:
    struct Segment { juce::String text; icons::Icon icon = icons::Icon::Count; };
    SegmentedControl();
    void setSegments(std::vector<Segment>);
    void setSelected(int i, bool notify = false);
    int selected() const noexcept { return selected_; }
    void setTall(bool t) { tall_ = t; repaint(); }   // 32-high segments (Settings, Compact tabs)
    int idealWidth() const;
    std::function<void(int)> onChange;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override { hot_ = -1; repaint(); }
    bool keyPressed(const juce::KeyPress&) override;
private:
    std::vector<juce::Rectangle<float>> boxes() const;
    std::vector<Segment> segs_;
    int selected_ = 0, hot_ = -1;
    bool tall_ = false;
};

// ---- TextLine: one line of painted text with a tooltip (ellipsis when too long) ---------------
class TextLine : public juce::Component, public juce::SettableTooltipClient {
public:
    enum class Tone { Ink, Ink2, Graphite, Danger };
    TextLine(float size = 12.0f, Weight w = Weight::Regular, Tone t = Tone::Graphite) : size_(size), weight_(w), tone_(t) { setInterceptsMouseClicks(true, false); }
    void setText(const juce::String& t) { if (t != text_) { text_ = t; setTitle(t); repaint(); } }
    const juce::String& text() const noexcept { return text_; }
    void setTone(Tone t) { if (t != tone_) { tone_ = t; repaint(); } }
    void setJustification(juce::Justification j) { just_ = j; repaint(); }
    void setDot(Dot d) { if (d != dot_) { dot_ = d; repaint(); } }
    int idealWidth() const { return juce::roundToInt(textWidth(uiFont(size_, weight_), text_) + (dot_ != Dot::None ? 13.0f : 0.0f)) + 2; }
    void paint(juce::Graphics&) override;
private:
    juce::String text_;
    float size_;
    Weight weight_;
    Tone tone_;
    Dot dot_ = Dot::None;
    juce::Justification just_ = juce::Justification::centredLeft;
};

// ---- NumberedSteps: 1 2 3 in ink circles, wrapped text (setup steps, S7 how-to) ---------------
class NumberedSteps : public juce::Component {
public:
    void setSteps(const juce::StringArray& s) { steps_ = s; repaint(); }
    int idealHeight(int width) const;
    void paint(juce::Graphics&) override;
private:
    juce::StringArray steps_;
};

// ---- Disclosure: "Other DAWs ˅" that opens a section below ------------------------------------
class Disclosure : public juce::Button {
public:
    explicit Disclosure(const juce::String& text = {});
    void setOpen(bool o) { open_ = o; repaint(); }
    bool isOpen() const noexcept { return open_; }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
private:
    bool open_ = false;
};

// ---- Stepper: − 2 block + ----------------------------------------------------------------------
class Stepper : public juce::Component {
public:
    Stepper(int lo, int hi);
    void setValue(int v) { value_ = juce::jlimit(lo_, hi_, v); repaint(); update(); }
    int value() const noexcept { return value_; }
    void setUnit(const juce::String& u) { unit_ = u; repaint(); }
    std::function<void(int)> onChange;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void update();
    int lo_, hi_, value_ = 0;
    juce::String unit_;
    GhostButton minus_ { juce::String(juce::CharPointer_UTF8("\xe2\x88\x92")) }, plus_ { "+" };
};

} // namespace hearaside
