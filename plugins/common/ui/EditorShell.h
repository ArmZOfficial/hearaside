// Base for both plug-in editors: owns the LookAndFeel, follows the shared Settings (theme,
// language, UI size 100-200 %), scales a fixed-design "content" component with a transform.
#pragma once

#include "../Settings.h"
#include "../Strings.h"
#include "LookAndFeel.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace hearaside {

class EditorShell : public juce::AudioProcessorEditor, private juce::ChangeListener {
public:
    EditorShell(juce::AudioProcessor& p, int baseW, int baseH, int minW = 0, int minH = 0)
        : juce::AudioProcessorEditor(p), baseW_(baseW), baseH_(baseH), minW_(minW), minH_(minH) {
        setLookAndFeel(&lnf_);
        lnf_.setDark(settings_->isDark());
        settings_->addChangeListener(this);
        setOpaque(true);
    }

    ~EditorShell() override {
        settings_->removeChangeListener(this);
        setLookAndFeel(nullptr);
    }

    void resized() override {
        if (content_ == nullptr) return;
        const float s = settings_->uiScale();
        content_->setTransform(juce::AffineTransform::scale(s));
        content_->setBounds(0, 0, juce::roundToInt(float(getWidth()) / s), juce::roundToInt(float(getHeight()) / s));
    }

    void paint(juce::Graphics& g) override { g.fillAll(lnf_.pal().paper); }

protected:
    // Call at the end of the derived constructor.
    void setContent(juce::Component& c, int initialW = 0, int initialH = 0) {
        content_ = &c;
        addAndMakeVisible(c);
        const float s = settings_->uiScale();
        const bool resizable = minW_ > 0;
        if (resizable) {
            setResizable(true, true);
            setResizeLimits(juce::roundToInt(float(minW_) * s), juce::roundToInt(float(minH_) * s), 4000, 3000);
        }
        setSize(juce::roundToInt(float(initialW > 0 ? initialW : baseW_) * s),
                juce::roundToInt(float(initialH > 0 ? initialH : baseH_) * s));
    }

    // Unscaled content size (what the layout works with).
    juce::Point<int> contentSize() const {
        const float s = settings_->uiScale();
        return { juce::roundToInt(float(getWidth()) / s), juce::roundToInt(float(getHeight()) / s) };
    }

    virtual void lookChanged() {}   // theme / language / glass / colour mode changed

    SharedSettings settings_;
    LookAndFeel lnf_;
    juce::TooltipWindow tooltips_ { this, 600 };

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        const float s = settings_->uiScale();
        if (std::abs(s - lastScale_) > 0.001f && content_ != nullptr) {
            const auto cs = juce::Point<float>(float(getWidth()), float(getHeight())) / lastScale_;
            lastScale_ = s;
            if (minW_ > 0) setResizeLimits(juce::roundToInt(float(minW_) * s), juce::roundToInt(float(minH_) * s), 4000, 3000);
            setSize(juce::roundToInt(cs.x * s), juce::roundToInt(cs.y * s));
            resized();
        }
        lnf_.setDark(settings_->isDark());
        lookChanged();
        sendLookAndFeelChange();
        repaint();
    }

    juce::Component* content_ = nullptr;
    int baseW_, baseH_, minW_, minH_;
    float lastScale_ = SharedSettings()->uiScale();
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

} // namespace hearaside
