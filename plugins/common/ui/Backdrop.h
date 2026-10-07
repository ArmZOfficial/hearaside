// Native "frosted glass" (docs/plan.md 7.12): everything behind the cards is ours (paper, grid,
// soft blobs), so it is rendered once per size/scale, blurred once, and each glass card is the
// blurred backdrop clipped to a rounded rect + a white veil. Per-frame cost is one image blit.
#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside {

struct BackdropBlob {
    juce::Rectangle<float> bounds;   // ellipse, component coordinates
    bool warm = true;                // blobWarm or blobCool token
};

struct GlassCard {
    juce::Rectangle<float> bounds;
    float radius = theme::radius::card;
};

class Backdrop {
public:
    void setBlobs(std::vector<BackdropBlob> blobs) { blobs_ = std::move(blobs); invalidate(); }
    void setCards(std::vector<GlassCard> cards) { cards_ = std::move(cards); invalidate(); }
    void invalidate() { cache_ = {}; }

    // Draws the cached composite (re-rendering it when size, scale or look changed).
    void paint(juce::Graphics& g, juce::Rectangle<int> area, const theme::Palette& p, float glassAlpha, bool dark);

private:
    void render(juce::Rectangle<int> area, float scale, const theme::Palette& p, float glassAlpha);

    std::vector<BackdropBlob> blobs_;
    std::vector<GlassCard> cards_;
    juce::Image cache_;
    juce::Rectangle<int> cacheArea_;
    float cacheScale_ = 0.0f, cacheAlpha_ = -1.0f;
    bool cacheDark_ = false;
};

// In-place separable box blur (3 passes ~ Gaussian). radius in pixels of the image.
void boxBlur(juce::Image& img, int radius);

} // namespace hearaside
