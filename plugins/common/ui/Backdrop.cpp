#include "Backdrop.h"

namespace hearaside {

namespace {

// Box-blurs `count` pixels read from `src` (stride bytes apart) into the compact buffer `dst`.
void blurLine(const juce::uint8* src, int stride, juce::uint8* dst, int count, int r) {
    const int window = 2 * r + 1;
    int sum[4] = { 0, 0, 0, 0 };
    auto px = [&](int i) { return src + ptrdiff_t(juce::jlimit(0, count - 1, i)) * stride; };
    for (int i = -r; i <= r; ++i)
        for (int c = 0; c < 4; ++c) sum[c] += px(i)[c];
    for (int i = 0; i < count; ++i) {
        auto* d = dst + size_t(i) * 4;
        for (int c = 0; c < 4; ++c) d[c] = juce::uint8(sum[c] / window);
        const auto* add = px(i + r + 1);
        const auto* sub = px(i - r);
        for (int c = 0; c < 4; ++c) sum[c] += int(add[c]) - int(sub[c]);
    }
}

void copyBack(const juce::uint8* compact, juce::uint8* dst, int stride, int count) {
    for (int i = 0; i < count; ++i) std::memcpy(dst + ptrdiff_t(i) * stride, compact + size_t(i) * 4, 4);
}

} // namespace

void boxBlur(juce::Image& img, int radius) {
    if (radius < 1 || !img.isValid()) return;
    if (img.getFormat() != juce::Image::ARGB) img = img.convertedToFormat(juce::Image::ARGB);
    const int w = img.getWidth(), h = img.getHeight();
    juce::Image::BitmapData bd(img, juce::Image::BitmapData::readWrite);
    jassert(bd.pixelStride == 4);
    std::vector<juce::uint8> line(size_t(juce::jmax(w, h)) * 4);
    const int r = juce::jmax(1, radius / 2);   // three passes of +-r approximate sigma ~ radius / 1.6
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            auto* row = bd.getLinePointer(y);
            blurLine(row, bd.pixelStride, line.data(), w, r);
            copyBack(line.data(), row, bd.pixelStride, w);
        }
        for (int x = 0; x < w; ++x) {
            auto* col = bd.getPixelPointer(x, 0);
            blurLine(col, bd.lineStride, line.data(), h, r);
            copyBack(line.data(), col, bd.lineStride, h);
        }
    }
}

void Backdrop::paint(juce::Graphics& g, juce::Rectangle<int> area, const theme::Palette& p, float glassAlpha, bool dark) {
    const float scale = juce::jlimit(1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (!cache_.isValid() || area != cacheArea_ || std::abs(scale - cacheScale_) > 0.01f
        || std::abs(glassAlpha - cacheAlpha_) > 0.001f || dark != cacheDark_) {
        render(area, scale, p, glassAlpha);
        cacheArea_ = area;
        cacheScale_ = scale;
        cacheAlpha_ = glassAlpha;
        cacheDark_ = dark;
    }
    g.drawImageTransformed(cache_, juce::AffineTransform::scale(1.0f / scale).translated(float(area.getX()), float(area.getY())));
}

void Backdrop::render(juce::Rectangle<int> area, float scale, const theme::Palette& p, float glassAlpha) {
    const int W = juce::jmax(1, juce::roundToInt(float(area.getWidth()) * scale));
    const int H = juce::jmax(1, juce::roundToInt(float(area.getHeight()) * scale));
    const auto origin = area.getPosition().toFloat();

    // 1. paper + grid + soft blobs
    juce::Image base(juce::Image::ARGB, W, H, true);
    {
        juce::Graphics bg(base);
        bg.addTransform(juce::AffineTransform::scale(scale));
        const float w = float(area.getWidth()), h = float(area.getHeight());
        bg.fillAll(p.paper);
        bg.setColour(p.grid);
        for (float x = 0; x < w; x += 40.0f) bg.fillRect(x, 0.0f, 1.0f, h);
        for (float y = 0; y < h; y += 40.0f) bg.fillRect(0.0f, y, w, 1.0f);
        for (const auto& b : blobs_) {
            const auto r = b.bounds - origin;
            const auto c = b.warm ? p.blobWarm : p.blobCool;
            // CSS radial-gradient(closest-side) on an ellipse, plus the mock-up's blur: fade to 0 at the edge
            juce::ColourGradient grad(c, r.getCentre(), c.withAlpha(0.0f), { r.getCentreX() + r.getWidth() * 0.5f, r.getCentreY() }, true);
            grad.addColour(0.55, c.withMultipliedAlpha(0.55f));
            bg.saveState();
            bg.addTransform(juce::AffineTransform::scale(1.0f, r.getHeight() / juce::jmax(1.0f, r.getWidth()), r.getCentreX(), r.getCentreY()));
            bg.setGradientFill(grad);
            bg.fillEllipse(r.getCentreX() - r.getWidth() * 0.5f, r.getCentreY() - r.getWidth() * 0.5f, r.getWidth(), r.getWidth());
            bg.restoreState();
        }
    }

    // 2. blurred copy for the glass
    juce::Image blurred = base.createCopy();
    boxBlur(blurred, juce::roundToInt(theme::glass::blur * scale));

    // 3. composite: base, card shadows, glass cards
    cache_ = base;
    juce::Graphics g(cache_);
    g.addTransform(juce::AffineTransform::scale(scale));
    const auto veil = p.glass.withAlpha(glassAlpha);
    for (const auto& c : cards_) {
        const auto r = c.bounds - origin;
        juce::Path shape;
        shape.addRoundedRectangle(r, c.radius);
        juce::DropShadow(p.shadow, 32, { 0, 12 }).drawForPath(g, shape);
        juce::DropShadow(p.shadow.withMultipliedAlpha(0.7f), 2, { 0, 1 }).drawForPath(g, shape);
    }
    for (const auto& c : cards_) {
        const auto r = c.bounds - origin;
        juce::Path shape;
        shape.addRoundedRectangle(r, c.radius);
        g.saveState();
        g.reduceClipRegion(shape);
        g.drawImageTransformed(blurred, juce::AffineTransform::scale(1.0f / scale));
        g.setColour(veil);
        g.fillPath(shape);
        g.restoreState();
        g.setColour(p.glassEdge);
        g.strokePath(shape, juce::PathStrokeType(1.0f));
    }
}

} // namespace hearaside
