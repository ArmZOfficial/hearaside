#include "Overlay.h"
#include "../Settings.h"

namespace hearaside {

namespace {

constexpr float kPad = 6.0f, kRowH = 36.0f, kRowSubH = 46.0f, kHeaderH = 25.0f, kSepH = 11.0f;

float itemHeight(const MenuItem& it) {
    switch (it.kind) {
        case MenuItem::Kind::Header:    return kHeaderH;
        case MenuItem::Kind::Separator: return kSepH;
        default: break;
    }
    return it.sub.isNotEmpty() ? kRowSubH : kRowH;
}

bool selectable(const MenuItem& it) {
    return it.enabled && it.kind != MenuItem::Kind::Header && it.kind != MenuItem::Kind::Separator;
}

bool reduceMotion() {
    static SharedSettings s;
    return s->reduceMotion();
}

} // namespace

// ---------------------------------------------------------------------------------------------
Menu& Menu::header(const juce::String& text) { MenuItem i; i.kind = MenuItem::Kind::Header; i.text = text; items_.push_back(i); return *this; }
Menu& Menu::separator() { MenuItem i; i.kind = MenuItem::Kind::Separator; items_.push_back(i); return *this; }
Menu& Menu::item(const juce::String& text, std::function<void()> fn, const juce::String& value, bool arrow) {
    MenuItem i; i.text = text; i.action = std::move(fn); i.value = value; i.arrow = arrow; items_.push_back(i); return *this;
}
Menu& Menu::toggle(const juce::String& text, bool on, std::function<void()> fn) {
    MenuItem i; i.kind = MenuItem::Kind::Switch; i.text = text; i.on = on; i.action = std::move(fn); items_.push_back(i); return *this;
}
Menu& Menu::check(const juce::String& text, bool selected, std::function<void()> fn) {
    MenuItem i; i.kind = MenuItem::Kind::Check; i.text = text; i.on = selected; i.action = std::move(fn); items_.push_back(i); return *this;
}

// ---------------------------------------------------------------------------------------------
// The list of a menu: rows painted here, hover / click / keyboard.
class MenuCard : public juce::Component {
public:
    MenuCard(Overlay& o, Menu m) : overlay_(o), menu_(std::move(m)) {
        setWantsKeyboardFocus(true);
        setFocusDrawnBySelf(*this);
        setTitle("Menu");
        float h = kPad * 2.0f;
        for (auto& it : menu_.items()) h += itemHeight(it);
        setSize(menu_.width(), juce::roundToInt(h));
        // a dropdown opens on its selected entry
        for (size_t i = 0; i < menu_.items().size(); ++i)
            if (menu_.items()[i].kind == MenuItem::Kind::Check && menu_.items()[i].on) hot_ = int(i);
    }

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        float y = kPad;
        for (size_t i = 0; i < menu_.items().size(); ++i) {
            const auto& it = menu_.items()[i];
            const float h = itemHeight(it);
            const juce::Rectangle<float> row(kPad, y, float(getWidth()) - kPad * 2.0f, h);
            y += h;
            if (it.kind == MenuItem::Kind::Separator) {
                g.setColour(p.hairline2);
                g.fillRect(row.withSizeKeepingCentre(row.getWidth() - 0.0f, 1.0f));
                continue;
            }
            if (it.kind == MenuItem::Kind::Header) {
                g.setColour(p.graphite);
                g.setFont(uiFont(11.5f));
                g.drawText(ellipsize(uiFont(11.5f), it.text, row.getWidth() - 20.0f), row.reduced(10.0f, 0.0f).withTrimmedBottom(4.0f),
                           juce::Justification::bottomLeft, false);
                continue;
            }
            if (int(i) == hot_ && it.enabled) {
                g.setColour(p.inset.withMultipliedAlpha(keyboard_ ? 1.6f : 1.0f));
                g.fillRoundedRectangle(row, 9.0f);
            }
            const auto fg = !it.enabled ? p.muted : it.danger ? p.danger : p.ink;
            auto inner = row.reduced(10.0f, 0.0f);
            if (it.icon != icons::Icon::Count) {
                auto tile = inner.removeFromLeft(22.0f).withSizeKeepingCentre(22.0f, 22.0f);
                icons::draw(g, it.icon, tile.withSizeKeepingCentre(16.0f, 16.0f), it.enabled ? p.ink2 : p.muted);
                inner.removeFromLeft(8.0f);
            }
            if (it.kind == MenuItem::Kind::Switch) {
                Switch::paintAt(g, inner.removeFromRight(36.0f).withSizeKeepingCentre(36.0f, 20.0f), it.on ? 1.0f : 0.0f, p);
                inner.removeFromRight(10.0f);
            }
            if (it.kind == MenuItem::Kind::Check) {
                auto ck = inner.removeFromRight(16.0f);
                if (it.on) icons::draw(g, icons::Icon::Check, ck.withSizeKeepingCentre(16.0f, 16.0f), p.ink, 2.0f);
                inner.removeFromRight(8.0f);
            }
            if (it.arrow) {
                icons::draw(g, icons::Icon::ChevronRight, inner.removeFromRight(14.0f).withSizeKeepingCentre(14.0f, 14.0f), p.graphite, 2.0f);
                inner.removeFromRight(4.0f);
            }
            if (it.value.isNotEmpty()) {
                const auto vf = uiFont(12.0f);
                const float vw = juce::jmin(inner.getWidth() * 0.5f, textWidth(vf, it.value) + 2.0f);
                g.setColour(p.graphite);
                g.setFont(vf);
                g.drawText(it.value, inner.removeFromRight(vw), juce::Justification::centredRight, true);
                inner.removeFromRight(10.0f);
            }
            if (it.dot != Dot::None) {
                drawStatusDot(g, { inner.getRight() - 4.0f, inner.getCentreY() }, it.dot, p);
                inner.removeFromRight(16.0f);
            }
            const auto f = uiFont(13.0f, it.kind == MenuItem::Kind::Check && it.on ? Weight::SemiBold : Weight::Regular);
            g.setColour(fg);
            g.setFont(f);
            if (it.sub.isNotEmpty()) {
                auto lines = inner.withSizeKeepingCentre(inner.getWidth(), 34.0f);
                g.drawText(ellipsize(f, it.text, lines.getWidth()), lines.removeFromTop(18.0f), juce::Justification::centredLeft, false);
                g.setColour(p.graphite);
                g.setFont(uiFont(11.5f));
                g.drawText(ellipsize(uiFont(11.5f), it.sub, lines.getWidth()), lines, juce::Justification::centredLeft, false);
            } else {
                g.drawText(ellipsize(f, it.text, inner.getWidth()), inner, juce::Justification::centredLeft, false);
            }
        }
    }

    int rowAt(float y) const {
        float top = kPad;
        for (size_t i = 0; i < menu_.items().size(); ++i) {
            const float h = itemHeight(menu_.items()[i]);
            if (y >= top && y < top + h) return int(i);
            top += h;
        }
        return -1;
    }
    void mouseMove(const juce::MouseEvent& e) override {
        const int r = rowAt(e.position.y);
        keyboard_ = false;
        if (r != hot_) { hot_ = r; repaint(); }
    }
    void mouseExit(const juce::MouseEvent&) override { if (!keyboard_) { hot_ = -1; repaint(); } }
    void mouseUp(const juce::MouseEvent& e) override {
        if (e.mouseWasDraggedSinceMouseDown()) return;
        activate(rowAt(e.position.y));
    }
    bool keyPressed(const juce::KeyPress& k) override {
        const int n = int(menu_.items().size());
        auto step = [&](int dir) {
            for (int tries = 0, i = hot_; tries < n; ++tries) {
                i = (i + dir + n) % n;
                if (selectable(menu_.items()[size_t(i)])) { hot_ = i; break; }
            }
            keyboard_ = true;
            repaint();
        };
        if (k == juce::KeyPress::downKey || (k.getKeyCode() == juce::KeyPress::tabKey && !k.getModifiers().isShiftDown())) { step(1); return true; }
        if (k == juce::KeyPress::upKey || (k.getKeyCode() == juce::KeyPress::tabKey && k.getModifiers().isShiftDown())) { step(-1); return true; }
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey) { activate(hot_); return true; }
        if (k == juce::KeyPress::escapeKey) { overlay_.close(); return true; }
        return false;
    }

private:
    void activate(int i) {
        if (i < 0 || i >= int(menu_.items().size())) return;
        auto& it = menu_.items()[size_t(i)];
        if (!selectable(it)) return;
        if (it.kind == MenuItem::Kind::Switch) {   // stays open, like the mock
            it.on = !it.on;
            repaint();
            if (it.action) it.action();
            return;
        }
        auto fn = it.action;
        overlay_.close();   // deletes this
        if (fn) fn();
    }

    Overlay& overlay_;
    Menu menu_;
    int hot_ = -1;
    bool keyboard_ = false;
};

// Card frame around a popover's content.
class PopoverCard : public juce::Component {
public:
    explicit PopoverCard(std::unique_ptr<juce::Component> c) : content_(std::move(c)) {
        addAndMakeVisible(*content_);
        setSize(content_->getWidth() + 32, content_->getHeight() + 32);
    }
    void resized() override { content_->setBounds(getLocalBounds().reduced(16)); }
    void childBoundsChanged(juce::Component*) override { setSize(content_->getWidth() + 32, content_->getHeight() + 32); }
private:
    std::unique_ptr<juce::Component> content_;
};

// ---------------------------------------------------------------------------------------------
Overlay::Overlay() {
    setAlwaysOnTop(true);
    setInterceptsMouseClicks(true, true);
}

Overlay::~Overlay() { card_.reset(); }

Overlay* Overlay::find(juce::Component& from) {
    for (auto* c = &from; c != nullptr; c = c->getParentComponent())
        for (auto* k : c->getChildren())
            if (auto* o = dynamic_cast<Overlay*>(k)) return o;
    return nullptr;
}

void Overlay::place(juce::Component& card, int w, int h, bool alignLeft) {
    juce::Rectangle<int> a(getWidth() / 2, getHeight() / 3, 0, 0);
    if (auto* an = anchor_.getComponent()) a = getLocalArea(an, an->getLocalBounds());
    h = juce::jmin(h, getHeight() - 24);
    int left = alignLeft ? a.getX() : a.getRight() - w;
    left = juce::jlimit(12, juce::jmax(12, getWidth() - 12 - w), left);
    int top = a.getBottom() + 6;
    if (top + h > getHeight() - 12) top = juce::jmax(12, a.getY() - 6 - h);
    card.setBounds(left, top, w, h);
}

void Overlay::showMenu(Menu m, juce::Component& anchor, bool alignLeft, std::function<void()> onClose) {
    close();
    anchor_ = &anchor;
    onClose_ = std::move(onClose);
    alignLeft_ = alignLeft;
    auto list = std::make_unique<MenuCard>(*this, std::move(m));
    const int w = list->getWidth(), h = list->getHeight();
    if (h > getHeight() - 24) {   // too tall for the window: the list scrolls inside the card
        auto vp = std::make_unique<juce::Viewport>();
        vp->setScrollBarsShown(true, false);
        vp->setScrollBarThickness(10);
        auto* raw = list.release();
        vp->setViewedComponent(raw, true);
        vp->setSize(w, getHeight() - 24);
        card_ = std::move(vp);
    } else {
        card_ = std::move(list);
    }
    addAndMakeVisible(*card_);
    place(*card_, w, card_->getHeight(), alignLeft);
    toFront(false);
    card_->setAlpha(reduceMotion() ? 1.0f : 0.0f);
    startTimerHz(60);
    if (auto* vp = dynamic_cast<juce::Viewport*>(card_.get())) vp->getViewedComponent()->grabKeyboardFocus();
    else card_->grabKeyboardFocus();
    repaint();
}

void Overlay::showPopover(std::unique_ptr<juce::Component> content, juce::Component& anchor, bool alignLeft,
                          std::function<void()> onClose) {
    close();
    anchor_ = &anchor;
    onClose_ = std::move(onClose);
    alignLeft_ = alignLeft;
    card_ = std::make_unique<PopoverCard>(std::move(content));
    addAndMakeVisible(*card_);
    place(*card_, card_->getWidth(), card_->getHeight(), alignLeft);
    toFront(false);
    card_->setAlpha(reduceMotion() ? 1.0f : 0.0f);
    startTimerHz(60);
    repaint();
}

void Overlay::close() {
    if (card_ == nullptr) return;
    auto done = std::move(onClose_);
    onClose_ = nullptr;
    // the card may be the caller (a menu row): delete it after this event
    auto* old = card_.release();
    removeChildComponent(old);
    juce::MessageManager::callAsync([old] { delete old; });
    if (auto* a = anchor_.getComponent()) a->repaint();
    anchor_ = nullptr;
    repaint();
    if (done) done();
}

void Overlay::toast(const juce::String& text, int ms) {
    toastText_ = text;
    toastUntil_ = juce::Time::getMillisecondCounter() + juce::uint32(ms);
    toFront(false);
    startTimerHz(60);
    repaint();
}

void Overlay::timerCallback() {
    bool busy = false;
    if (card_ != nullptr && card_->getAlpha() < 1.0f) {
        card_->setAlpha(juce::jmin(1.0f, card_->getAlpha() + 1.0f / (0.12f * 60.0f)));
        busy = true;
    }
    if (toastText_.isNotEmpty()) {
        if (juce::Time::getMillisecondCounter() >= toastUntil_) { toastText_.clear(); repaint(); }
        else busy = true;
    }
    if (!busy) stopTimer();
}

void Overlay::resized() { close(); }

bool Overlay::hitTest(int, int) { return card_ != nullptr; }

void Overlay::mouseDown(const juce::MouseEvent&) { close(); }   // a click outside the card

void Overlay::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    if (card_ != nullptr) {
        const auto r = card_->getBounds().toFloat();
        const bool pop = dynamic_cast<PopoverCard*>(card_.get()) != nullptr;
        const float radius = pop ? 16.0f : 14.0f;
        g.setOpacity(card_->getAlpha());
        drawCardShadow(g, r, radius, card_->getAlpha());
        g.setColour(p.menuBg.withMultipliedAlpha(card_->getAlpha()));
        g.fillRoundedRectangle(r, radius);
        g.setColour(p.hairline2.withMultipliedAlpha(card_->getAlpha()));
        g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
    }
    if (toastText_.isNotEmpty()) {
        const auto f = uiFont(13.0f, Weight::Medium);
        const float maxW = float(getWidth()) * 0.8f;
        const float tw = juce::jmin(maxW - 32.0f - 24.0f, textWidth(f, toastText_));
        const float lines = float(wrapText(f, toastText_, tw).size());
        const float h = lines * f.getHeight() + (lines - 1.0f) * 3.0f + 20.0f;
        const float w = tw + 32.0f + 24.0f;
        toastBox_ = juce::Rectangle<float>(w, h).withCentre({ float(getWidth()) * 0.5f, float(getHeight()) - 26.0f - h * 0.5f });
        juce::Path shape;
        shape.addRoundedRectangle(toastBox_, 12.0f);
        juce::DropShadow(juce::Colours::black.withAlpha(0.22f), 24, { 0, 10 }).drawForPath(g, shape);
        g.setColour(p.ink);
        g.fillPath(shape);
        auto inner = toastBox_.reduced(16.0f, 10.0f);
        icons::draw(g, icons::Icon::Check, inner.removeFromLeft(16.0f).withSizeKeepingCentre(16.0f, 16.0f), p.onInk, 2.0f);
        inner.removeFromLeft(8.0f);
        drawWrapped(g, toastText_, f, p.onInk, inner, 3.0f);
    }
}

// ---------------------------------------------------------------------------------------------
Dropdown::Dropdown() : juce::Button({}) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, 10.0f);
}

void Dropdown::clicked() {
    auto* o = Overlay::find(*this);
    if (o == nullptr) return;
    if (open_) { o->close(); return; }
    Menu m(juce::jmax(getWidth(), 160));
    for (int i = 0; i < items_.size(); ++i)
        m.check(items_[i], i == selected_, [this, i, safe = juce::Component::SafePointer<Dropdown>(this)] {
            if (safe == nullptr) return;
            selected_ = i;
            repaint();
            if (onChange) onChange(i);
        });
    open_ = true;
    repaint();
    o->showMenu(std::move(m), *this, true, [safe = juce::Component::SafePointer<Dropdown>(this)] {
        if (safe != nullptr) { safe->open_ = false; safe->repaint(); }
    });
}

void Dropdown::paintButton(juce::Graphics& g, bool highlighted, bool) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const float radius = small_ ? 9.0f : 10.0f;
    g.setColour(p.paper.overlaidWith(p.offBg));
    g.fillRoundedRectangle(r, radius);
    g.setColour(open_ ? p.ink : highlighted ? p.sliderRail : p.hairline3);
    g.drawRoundedRectangle(r, radius, 1.0f);
    if (open_) drawFocusRing(g, r.reduced(2.5f), radius - 2.5f, p);
    auto inner = r.withTrimmedLeft(small_ ? 10.0f : 12.0f).withTrimmedRight(10.0f);
    const auto chev = inner.removeFromRight(16.0f).withSizeKeepingCentre(16.0f, 16.0f);
    if (open_) {
        g.saveState();
        g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::pi, chev.getCentreX(), chev.getCentreY()));
        icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
        g.restoreState();
    } else {
        icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
    }
    inner.removeFromRight(8.0f);
    if (hasDot_) {
        g.setColour(dot_);
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({ inner.getX() + 4.0f, inner.getCentreY() }));
        inner.removeFromLeft(16.0f);
    }
    const auto f = uiFont(small_ ? 12.5f : 13.0f, Weight::Medium);
    g.setColour(p.ink.withMultipliedAlpha(isEnabled() ? 1.0f : 0.45f));
    g.setFont(f);
    g.drawText(ellipsize(f, items_[selected_], inner.getWidth()), inner, juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------------------------------------
SourcePicker::SourcePicker(bool small) : juce::Button({}), small_(small) {
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setFocusShape(*this, small ? 8.0f : 12.0f);
}

void SourcePicker::set(icons::Icon icon, const juce::String& label) {
    if (icon == icon_ && label == text_) return;
    icon_ = icon;
    text_ = label;
    setTitle(tr(Str::ChooseProgramAria).replace("%s", label));
    repaint();
}

int SourcePicker::idealWidth() const {
    const auto f = uiFont(small_ ? 14.0f : 15.0f, small_ ? Weight::Medium : Weight::SemiBold);
    return juce::roundToInt(textWidth(f, text_) + (small_ ? 16.0f + 6.0f + 14.0f + 6.0f + 12.0f : 30.0f + 12.0f + 16.0f + 32.0f)) + 2;
}

void SourcePicker::clicked() {
    auto* o = Overlay::find(*this);
    if (o == nullptr || !buildMenu) return;
    if (open_) { o->close(); return; }
    auto m = buildMenu();   // refreshed every time it opens (programs come and go)
    if (m.empty()) return;
    if (!small_) m.setWidth(juce::jmax(m.width(), getWidth()));
    open_ = true;
    repaint();
    o->showMenu(std::move(m), *this, true, [safe = juce::Component::SafePointer<SourcePicker>(this)] {
        if (safe != nullptr) { safe->open_ = false; safe->repaint(); }
    });
}

void SourcePicker::paintButton(juce::Graphics& g, bool highlighted, bool) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().toFloat();
    if (small_) {
        const auto f = uiFont(14.0f, Weight::Medium);
        if (highlighted || open_) { g.setColour(p.hairline2); g.fillRoundedRectangle(r, 8.0f); }
        auto inner = r.reduced(6.0f, 0.0f);
        icons::draw(g, icon_, inner.removeFromLeft(16.0f).withSizeKeepingCentre(16.0f, 16.0f), p.ink2);
        inner.removeFromLeft(6.0f);
        const auto chev = inner.removeFromRight(14.0f).withSizeKeepingCentre(14.0f, 14.0f);
        inner.removeFromRight(6.0f);
        g.setColour(p.ink);
        g.setFont(f);
        g.drawText(ellipsize(f, text_, inner.getWidth()), inner, juce::Justification::centredLeft, false);
        icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
        return;
    }
    r = r.reduced(0.5f);
    g.setColour(p.paper.overlaidWith(p.offBg));
    g.fillRoundedRectangle(r, 12.0f);
    g.setColour(open_ ? p.ink : highlighted ? p.sliderRail : p.hairline3);
    g.drawRoundedRectangle(r, 12.0f, 1.0f);
    if (open_) drawFocusRing(g, r.reduced(2.5f), 9.5f, p);
    auto inner = r.withTrimmedLeft(9.0f).withTrimmedRight(14.0f);
    auto tile = inner.removeFromLeft(30.0f).withSizeKeepingCentre(30.0f, 30.0f);
    g.setColour(p.ink);
    g.fillRoundedRectangle(tile, 8.0f);
    icons::draw(g, icon_, tile.withSizeKeepingCentre(16.0f, 16.0f), p.onInk);
    inner.removeFromLeft(12.0f);
    const auto chev = inner.removeFromRight(16.0f).withSizeKeepingCentre(16.0f, 16.0f);
    if (open_) {
        g.saveState();
        g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::pi, chev.getCentreX(), chev.getCentreY()));
        icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
        g.restoreState();
    } else {
        icons::draw(g, icons::Icon::ChevronDown, chev, p.graphite, 2.0f);
    }
    inner.removeFromRight(8.0f);
    const auto f = uiFont(15.0f, Weight::SemiBold);
    g.setColour(p.ink);
    g.setFont(f);
    g.drawText(ellipsize(f, text_, inner.getWidth()), inner, juce::Justification::centredLeft, false);
}

} // namespace hearaside
