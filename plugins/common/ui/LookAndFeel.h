// "Frosted Studio" look: Anuphan font, monochrome palette (light / dark), custom sliders,
// menus and tooltips. Tokens come from Theme.h (generated from design/tokens.json).
#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside {

enum class Weight { Regular, Medium, SemiBold };

// Anuphan at `px` CSS pixels (em size).
juce::Font uiFont(float px, Weight w = Weight::Regular);

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    LookAndFeel();

    void setDark(bool dark);
    bool isDark() const noexcept { return dark_; }
    const theme::Palette& pal() const noexcept { return *pal_; }

    juce::Typeface::Ptr getTypefaceForFont(const juce::Font&) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float sliderPos, float minPos,
                          float maxPos, juce::Slider::SliderStyle, juce::Slider&) override;
    int  getSliderThumbRadius(juce::Slider&) override { return 9; }

    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText, const juce::Drawable* icon,
                           const juce::Colour* textColour) override;
    juce::Font getPopupMenuFont() override { return uiFont(13.0f); }
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                   int& idealWidth, int& idealHeight) override;

    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;

    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical,
                       int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    int getDefaultScrollbarWidth() override { return 8; }

    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override { return uiFont(13.0f); }
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;

    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return uiFont(13.0f, Weight::Medium); }

    void drawLabel(juce::Graphics&, juce::Label&) override;

private:
    void applyColours();
    bool dark_ = false;
    const theme::Palette* pal_ = &theme::light();
};

// Draws the keyboard focus ring (2 px ink, 2 px outside) used by every control.
void drawFocusRing(juce::Graphics&, juce::Rectangle<float> bounds, float radius, const theme::Palette&);

// Palette of the LookAndFeel attached to `c` (falls back to light).
const theme::Palette& paletteOf(const juce::Component& c);

} // namespace hearaside
