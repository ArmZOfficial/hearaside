// Base for the plug-in editors: owns the LookAndFeel, follows the shared Settings (theme,
// language, UI size 100-200 %), scales a "content" component with a transform. Resizable editors
// remember their size per plug-in (Settings, content units) and never open bigger than the
// screen; the first time HEARASIDE runs it picks a UI size that fits the screen.
#pragma once

#include "../Settings.h"
#include "../Strings.h"
#include "LookAndFeel.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace hearaside {

// Text typed into a TextEditor keeps the colour of the moment: a panel that filled its fields
// before getting the editor's LookAndFeel would show them in the default (white) colour.
inline void recolourTextEditors(juce::Component& root) {
    for (auto* c : root.getChildren()) {
        if (auto* e = dynamic_cast<juce::TextEditor*>(c)) e->applyColourToAllText(e->findColour(juce::TextEditor::textColourId));
        recolourTextEditors(*c);
    }
}

// CallOutBox content: a panel at the editor's UI size, never wider or taller than the screen
// (it scrolls instead).
class PanelFrame : public juce::Component {
public:
    PanelFrame(std::unique_ptr<juce::Component> panel, float scale, juce::Rectangle<int> screen) : panel_(std::move(panel)) {
        holder_.addAndMakeVisible(*panel_);
        panel_->setTransform(juce::AffineTransform::scale(scale));
        holder_.setSize(juce::roundToInt(float(panel_->getWidth()) * scale), juce::roundToInt(float(panel_->getHeight()) * scale));
        viewport_.setViewedComponent(&holder_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        const int margin = juce::roundToInt(theme::layout::panelMargin);
        const int maxW = juce::jmax(200, screen.getWidth() - margin), maxH = juce::jmax(160, screen.getHeight() - margin * 2);
        const bool scrolls = holder_.getHeight() > maxH;
        setSize(juce::jmin(maxW, holder_.getWidth() + (scrolls ? viewport_.getScrollBarThickness() : 0)),
                juce::jmin(maxH, holder_.getHeight()));
    }
    void resized() override { viewport_.setBounds(getLocalBounds()); }

private:
    std::unique_ptr<juce::Component> panel_;
    juce::Component holder_;
    juce::Viewport viewport_;
};

class EditorShell : public juce::AudioProcessorEditor, private juce::ChangeListener {
public:
    // minW 0 = fixed size. key: where the window size is remembered ("hub", "track", "app").
    EditorShell(juce::AudioProcessor& p, int baseW, int baseH, int minW = 0, int minH = 0, juce::String key = {},
                int maxW = 4000, int maxH = 3000)
        : juce::AudioProcessorEditor(p), baseW_(baseW), baseH_(baseH), minW_(minW), minH_(minH), maxW_(maxW), maxH_(maxH),
          key_(std::move(key)) {
        setLookAndFeel(&lnf_);
        lnf_.setDark(settings_->isDark());
        settings_->addChangeListener(this);
        setOpaque(true);
        saveTimer_.fn = [this] { if (content_ != nullptr) settings_->setWindowSize(key_, contentSize()); };
    }

    ~EditorShell() override {
        saveTimer_.stopTimer();
        settings_->removeChangeListener(this);
        setLookAndFeel(nullptr);
    }

    void resized() override {
        if (content_ == nullptr) return;
        const float s = settings_->uiScale();
        content_->setTransform(juce::AffineTransform::scale(s));
        content_->setBounds(0, 0, juce::roundToInt(float(getWidth()) / s), juce::roundToInt(float(getHeight()) / s));
        // hosts resize in bursts while an edge is dragged (some call back twice): save once it settles
        if (minW_ > 0 && key_.isNotEmpty() && sizeSet_) saveTimer_.startTimer(400);
    }

    void paint(juce::Graphics& g) override { g.fillAll(lnf_.pal().paper); }

protected:
    // Call at the end of the derived constructor.
    void setContent(juce::Component& c, int initialW = 0, int initialH = 0) {
        content_ = &c;
        addAndMakeVisible(c);
        const auto screen = screenArea();
        if (!settings_->uiScaleChosen()) {
            // first run: the biggest UI size whose default window still fits the screen
            float pick = 1.0f;
            for (float s : { 2.0f, 1.75f, 1.5f, 1.25f })
                if (float(baseW_) * s <= float(screen.getWidth()) * theme::layout::screenFit
                    && float(baseH_) * s <= float(screen.getHeight()) * theme::layout::screenFit) { pick = s; break; }
            settings_->setUiScaleAuto(pick);
        }
        const float s = settings_->uiScale();
        lastScale_ = s;
        auto size = juce::Point<int>(initialW > 0 ? initialW : baseW_, initialH > 0 ? initialH : baseH_);
        if (minW_ > 0) {
            setResizable(true, true);
            applyLimits(s);
            if (const auto saved = settings_->windowSize(key_); saved.x > 0 && saved.y > 0 && initialW <= 0) size = saved;
            size = fitted(size, s, screen);
        }
        setSize(juce::roundToInt(float(size.x) * s), juce::roundToInt(float(size.y) * s));
        sizeSet_ = true;
    }

    // Unscaled content size (what the layout works with).
    juce::Point<int> contentSize() const {
        const float s = settings_->uiScale();
        return { juce::roundToInt(float(getWidth()) / s), juce::roundToInt(float(getHeight()) / s) };
    }

    virtual void lookChanged() {}   // theme / language / glass / colour mode changed

    // Shows a sub-panel next to `anchor` at the UI size, limited to the screen (scrolls if needed).
    void launchPanel(std::unique_ptr<juce::Component> panel, juce::Component& anchor) {
        panel->setLookAndFeel(&lnf_);
        recolourTextEditors(*panel);
        auto frame = std::make_unique<PanelFrame>(std::move(panel), settings_->uiScale(), screenArea());
        frame->setLookAndFeel(&lnf_);
        auto& box = juce::CallOutBox::launchAsynchronously(std::move(frame), anchor.getScreenBounds(), nullptr);
        box.setLookAndFeel(&lnf_);
    }

    // The screen the editor is on (or opens on: where the mouse is), without the taskbar.
    juce::Rectangle<int> screenArea() const {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* d = isShowing() ? displays.getDisplayForRect(getScreenBounds())
                                    : displays.getDisplayForPoint(juce::Desktop::getMousePosition().toFloat());
        if (d == nullptr) d = displays.getPrimaryDisplay();
        return d != nullptr ? d->userBounds.toNearestInt() : juce::Rectangle<int>(0, 0, 1366, 728);
    }

    SharedSettings settings_;
    LookAndFeel lnf_;
    juce::TooltipWindow tooltips_ { this, 600 };

private:
    void applyLimits(float s) {
        setResizeLimits(juce::roundToInt(float(minW_) * s), juce::roundToInt(float(minH_) * s),
                        juce::roundToInt(float(maxW_) * s), juce::roundToInt(float(maxH_) * s));
    }

    // A content size within the limits that keeps the window on the screen.
    juce::Point<int> fitted(juce::Point<int> size, float s, juce::Rectangle<int> screen) const {
        const int fitW = juce::roundToInt(float(screen.getWidth()) * theme::layout::screenFit / s);
        const int fitH = juce::roundToInt(float(screen.getHeight()) * theme::layout::screenFit / s);
        return { juce::jlimit(minW_, juce::jmax(minW_, juce::jmin(maxW_, fitW)), size.x),
                 juce::jlimit(minH_, juce::jmax(minH_, juce::jmin(maxH_, fitH)), size.y) };
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        const float s = settings_->uiScale();
        if (std::abs(s - lastScale_) > 0.001f && content_ != nullptr) {
            auto cs = (juce::Point<float>(float(getWidth()), float(getHeight())) / lastScale_).roundToInt();
            lastScale_ = s;
            if (minW_ > 0) {
                applyLimits(s);
                cs = fitted(cs, s, screenArea());   // a bigger UI size must not push the window off the screen
            }
            setSize(juce::roundToInt(float(cs.x) * s), juce::roundToInt(float(cs.y) * s));
            resized();
        }
        lnf_.setDark(settings_->isDark());
        lookChanged();
        sendLookAndFeelChange();
        repaint();
    }

    struct SaveTimer : juce::Timer {
        std::function<void()> fn;
        void timerCallback() override { stopTimer(); if (fn) fn(); }
    };

    juce::Component* content_ = nullptr;
    int baseW_, baseH_, minW_, minH_, maxW_, maxH_;
    juce::String key_;
    float lastScale_ = SharedSettings()->uiScale();
    bool sizeSet_ = false;
    SaveTimer saveTimer_;
};

// ChangeListener that calls a lambda (avoids inheriting ChangeListener twice).
struct FnChangeListener : juce::ChangeListener {
    std::function<void()> fn;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { if (fn) fn(); }
};

// Links a LevelSlider (-30..+6 shown) to a dB parameter with a wider range, with proper
// begin/end gestures. Call update() from the editor timer.
class DbSliderLink {
public:
    DbSliderLink(juce::Slider& s, juce::RangedAudioParameter& p) : slider_(s), param_(p) {
        slider_.onDragStart = [this] { param_.beginChangeGesture(); dragging_ = true; };
        slider_.onDragEnd = [this] { param_.endChangeGesture(); dragging_ = false; };
        slider_.onValueChange = [this] {
            if (updating_) return;
            const float db = slider_.getValue() <= -30.0 + 1.0e-6 ? param_.getNormalisableRange().start : float(slider_.getValue());
            if (!dragging_) param_.beginChangeGesture();
            param_.setValueNotifyingHost(param_.convertTo0to1(db));
            if (!dragging_) param_.endChangeGesture();
        };
        update();
    }
    void update() {
        if (dragging_) return;
        const float db = param_.convertFrom0to1(param_.getValue());
        const double v = juce::jlimit(-30.0, 6.0, double(db));
        if (std::abs(v - slider_.getValue()) > 1.0e-4) {
            const juce::ScopedValueSetter<bool> svs(updating_, true);
            slider_.setValue(v, juce::dontSendNotification);
        }
    }
    float db() const { return param_.convertFrom0to1(param_.getValue()); }

private:
    juce::Slider& slider_;
    juce::RangedAudioParameter& param_;
    bool dragging_ = false, updating_ = false;
};

// The value next to a slider: click it and type a number (Enter to set). Writes the parameter in
// its own units, so it also reaches values past the slider's travel (a dB slider shows -30..+6).
class ValueField : public juce::Label {
public:
    enum class Unit { Db, Ms };
    ValueField(juce::RangedAudioParameter& p, Unit u) : param_(p), unit_(u) {
        setEditable(true, true, false);
        setJustificationType(juce::Justification::centredRight);
        setBorderSize({ 1, 4, 1, 0 });   // the number ends where the slider ends
        setFont(uiFont(12.0f));
        setMouseCursor(juce::MouseCursor::IBeamCursor);
        onTextChange = [this] { commit(); };
        update();
    }
    // editor timer: follow the parameter (automation, the Hub, the slider) unless being typed in
    void update() {
        if (isBeingEdited()) return;
        const auto t = text(param_.convertFrom0to1(param_.getValue()));
        if (t != getText()) setText(t, juce::dontSendNotification);
    }

private:
    juce::String text(float v) const {
        if (unit_ == Unit::Ms) return juce::String(juce::roundToInt(v)) + " ms";
        const float floor = param_.getNormalisableRange().start;
        if (v > -30.0f || v <= floor + 0.05f) return formatDb(v <= floor + 0.05f ? -60.0f : v);   // -inf at the bottom
        return juce::String(juce::CharPointer_UTF8("\xe2\x88\x92")) + juce::String(-v, 1) + " dB";
    }
    void commit() {
        auto t = getText().trim().replace(juce::CharPointer_UTF8("\xe2\x88\x92"), "-");   // U+2212 as typed from the display
        const auto& range = param_.getNormalisableRange();
        float v;
        if (t.containsIgnoreCase("inf") || t.contains(juce::CharPointer_UTF8("\xe2\x88\x9e"))) {
            v = range.start;
        } else {
            const auto num = t.retainCharacters("-+0123456789.,").replaceCharacter(',', '.');
            if (num.containsOnly("-+.")) { update(); return; }   // nothing usable: show the value again
            v = num.getFloatValue();
        }
        v = juce::jlimit(range.start, range.end, v);
        param_.beginChangeGesture();
        param_.setValueNotifyingHost(param_.convertTo0to1(v));
        param_.endChangeGesture();
        setText(text(param_.convertFrom0to1(param_.getValue())), juce::dontSendNotification);
    }

    juce::RangedAudioParameter& param_;
    Unit unit_;
};

} // namespace hearaside
