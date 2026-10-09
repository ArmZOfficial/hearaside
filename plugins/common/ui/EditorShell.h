// Base for the plug-in editors: owns the LookAndFeel, follows the shared Settings (theme,
// language, UI size 100-200 %), scales a "content" component with a transform. Resizable editors
// remember their size per plug-in (Settings, content units) and never open bigger than the
// screen; the first time HEARASIDE runs it picks a UI size that fits the screen. The content gets
// the Overlay (menus, dropdowns, popovers, toasts) and the keyboard focus ring on top.
#pragma once

#include "../Settings.h"
#include "../Strings.h"
#include "Controls.h"
#include "LookAndFeel.h"
#include "Overlay.h"

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

class EditorShell : public juce::AudioProcessorEditor, private juce::ChangeListener, private juce::ComponentListener {
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
        if (content_ != nullptr) content_->removeComponentListener(this);
        focusRing_.reset();
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

    Overlay& overlay() noexcept { return overlay_; }
    void toast(const juce::String& text, int ms = 2400) { overlay_.toast(text, ms); }

protected:
    // Call at the end of the derived constructor.
    void setContent(juce::Component& c, int initialW = 0, int initialH = 0) {
        content_ = &c;
        addAndMakeVisible(c);
        focusRing_ = std::make_unique<FocusRingOverlay>(c);
        c.addAndMakeVisible(*focusRing_);
        c.addAndMakeVisible(overlay_);
        c.addComponentListener(this);
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
    void componentMovedOrResized(juce::Component& c, bool, bool wasResized) override {
        if (!wasResized || &c != content_) return;
        focusRing_->setBounds(c.getLocalBounds());
        overlay_.setBounds(c.getLocalBounds());
        focusRing_->toFront(false);
        overlay_.toFront(false);
    }

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
    Overlay overlay_;
    std::unique_ptr<FocusRingOverlay> focusRing_;
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

// Writes a parameter as one undo step for the host.
inline void setParamWithGesture(juce::RangedAudioParameter& p, float plain) {
    p.beginChangeGesture();
    p.setValueNotifyingHost(p.convertTo0to1(plain));
    p.endChangeGesture();
}
inline float paramPlain(const juce::RangedAudioParameter& p) { return p.convertFrom0to1(p.getValue()); }

// Links a LevelSlider (-30..+6 shown) to a dB parameter with a wider range, with proper
// begin/end gestures. Call update() from the editor timer.
class DbSliderLink {
public:
    DbSliderLink(juce::Slider& s, juce::RangedAudioParameter& p) : slider_(s), param_(p) {
        slider_.onDragStart = [this] { param_.beginChangeGesture(); dragging_ = true; };
        slider_.onDragEnd = [this] { param_.endChangeGesture(); dragging_ = false; };
        slider_.onValueChange = [this] {
            if (updating_) return;
            const float db = slider_.getValue() <= slider_.getMinimum() + 1.0e-6 ? param_.getNormalisableRange().start : float(slider_.getValue());
            if (!dragging_) param_.beginChangeGesture();
            param_.setValueNotifyingHost(param_.convertTo0to1(db));
            if (!dragging_) param_.endChangeGesture();
        };
        update();
    }
    void update() {
        if (dragging_) return;
        const double v = juce::jlimit(slider_.getMinimum(), slider_.getMaximum(), double(paramPlain(param_)));
        if (std::abs(v - slider_.getValue()) > 1.0e-4) {
            const juce::ScopedValueSetter<bool> svs(updating_, true);
            slider_.setValue(v, juce::dontSendNotification);
        }
    }
    float db() const { return paramPlain(param_); }

private:
    juce::Slider& slider_;
    juce::RangedAudioParameter& param_;
    bool dragging_ = false, updating_ = false;
};

// An EditableValue showing / typing a parameter in its own units (a dB value can go past the
// slider's travel). The bottom of a dB range shows as −∞. Call update() from the editor timer.
class ParamValueLink {
public:
    ParamValueLink(EditableValue& v, juce::RangedAudioParameter& p) : value_(v), param_(p) {
        value_.onCommit = [this](float x) {   // −∞ (the slider's bottom) = the parameter's bottom
            const bool inf = value_.kind() == EditableValue::Kind::Db && x <= valuetext::kFloorDb + 1.0e-3f;
            setParamWithGesture(param_, inf ? param_.getNormalisableRange().start : x);
        };
        update();
    }
    void update() { if (!value_.isEditing()) value_.setValue(paramPlain(param_)); }
private:
    EditableValue& value_;
    juce::RangedAudioParameter& param_;
};

} // namespace hearaside
