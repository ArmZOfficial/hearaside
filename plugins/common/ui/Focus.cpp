#include "Focus.h"
#include "LookAndFeel.h"

namespace hearaside {

namespace {

std::atomic<bool> gKeyboard { false };

// Watches the mouse for every HEARASIDE window in this process: a press ends keyboard mode.
struct MouseWatch : juce::MouseListener {
    MouseWatch() { juce::Desktop::getInstance().addGlobalMouseListener(this); }
    ~MouseWatch() override { juce::Desktop::getInstance().removeGlobalMouseListener(this); }
    void mouseDown(const juce::MouseEvent&) override {
        lastPress = juce::Time::getMillisecondCounter();
        if (gKeyboard.exchange(false))
            if (auto* f = juce::Component::getCurrentlyFocusedComponent()) f->repaint();
    }
    juce::uint32 lastPress = 0;
};

const juce::Identifier kRadius { "hsFocusRadius" }, kSelf { "hsFocusSelf" }, kAlways { "hsFocusAlways" };

bool isTextField(const juce::Component& c) {
    return dynamic_cast<const juce::TextEditor*>(&c) != nullptr || c.getProperties().contains(kAlways);
}

} // namespace

bool keyboardFocusMode() { return gKeyboard.load(); }

bool focusVisible(const juce::Component& c) {
    if (!c.hasKeyboardFocus(false)) return false;
    return isTextField(c) || gKeyboard.load();
}

void setFocusShape(juce::Component& c, float radius) { c.getProperties().set(kRadius, radius); }
void setFocusDrawnBySelf(juce::Component& c) { c.getProperties().set(kSelf, true); }
void setFocusAlways(juce::Component& c) { c.getProperties().set(kAlways, true); }

FocusRingOverlay::FocusRingOverlay(juce::Component& root) : root_(root) {
    static juce::SharedResourcePointer<MouseWatch> watch;   // lives as long as the process uses it
    juce::ignoreUnused(watch);
    setInterceptsMouseClicks(false, false);
    setAlwaysOnTop(true);
    juce::Desktop::getInstance().addFocusChangeListener(this);
}

FocusRingOverlay::~FocusRingOverlay() { juce::Desktop::getInstance().removeFocusChangeListener(this); }

void FocusRingOverlay::globalFocusChanged(juce::Component* c) {
    static juce::SharedResourcePointer<MouseWatch> watch;
    // focus that did not follow a mouse press came from the keyboard (Tab, arrows, a shortcut)
    if (c != nullptr && root_.isParentOf(c) && juce::Time::getMillisecondCounter() - watch->lastPress > 200) gKeyboard = true;
    focused_ = (c != nullptr && root_.isParentOf(c)) ? c : nullptr;
    repaint();
    if (focused_ != nullptr) startTimerHz(20);
    else stopTimer();
}

void FocusRingOverlay::timerCallback() {
    const auto r = ring();
    const auto now = r ? r->first : juce::Rectangle<float>();
    if (now != last_) repaint();
    if (focused_ == nullptr) stopTimer();
}

std::optional<std::pair<juce::Rectangle<float>, float>> FocusRingOverlay::ring() const {
    auto* c = focused_.getComponent();
    if (c == nullptr || !c->isShowing() || !focusVisible(*c) || c->getProperties().contains(kSelf)) return std::nullopt;
    auto area = getLocalArea(c, c->getLocalBounds()).toFloat();
    float radius = c->getProperties().contains(kRadius) ? float(c->getProperties()[kRadius]) : 10.0f;
    if (radius < 0.0f) radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
    return std::make_pair(area, radius);
}

void FocusRingOverlay::paint(juce::Graphics& g) {
    const auto r = ring();
    last_ = r ? r->first : juce::Rectangle<float>();
    if (!r) return;
    // only where the control is visible: viewports between it and the root clip it
    for (auto* p = focused_->getParentComponent(); p != nullptr && p != &root_; p = p->getParentComponent())
        if (auto* vp = dynamic_cast<juce::Viewport*>(p))
            g.reduceClipRegion(getLocalArea(vp, vp->getLocalBounds()).expanded(4));
    const auto& pal = paletteOf(*this);
    g.setColour(pal.focusRing);
    g.drawRoundedRectangle(r->first.expanded(1.5f), r->second + 1.5f, 3.0f);
}

} // namespace hearaside
