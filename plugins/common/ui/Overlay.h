// Menus, dropdown lists, popovers and toasts drawn inside the editor (prompt 3.2 "Menu (⋯)",
// "Dropdown", "Popover", "Toast"): one look on every screen and every host, keyboard friendly,
// and visible in ui-snapshot. Each editor content owns one Overlay (always on top); controls
// find it with Overlay::find().
#pragma once

#include "Components.h"

#include <functional>
#include <vector>

namespace hearaside {

struct MenuItem {
    enum class Kind { Action, Switch, Check, Header, Separator };
    Kind kind = Kind::Action;
    juce::String text, value;          // value: right-hand grey text ("Center", "40 ms")
    bool arrow = false;                // "›": opens another screen
    bool on = false;                   // Switch / Check state
    bool danger = false, enabled = true;
    Dot dot = Dot::None;               // status dot after the text (a friend's state, recording)
    icons::Icon icon = icons::Icon::Count;   // Count = none
    juce::String sub;                  // second line (grey), e.g. "in “Audio 7”"
    std::function<void()> action;      // Switch: runs on toggle (the menu stays open)
};

class Menu {
public:
    explicit Menu(int width = 236) : width_(width) {}
    Menu& header(const juce::String& text);
    Menu& item(const juce::String& text, std::function<void()> fn, const juce::String& value = {}, bool arrow = false);
    Menu& toggle(const juce::String& text, bool on, std::function<void()> fn);
    Menu& check(const juce::String& text, bool selected, std::function<void()> fn);
    Menu& separator();
    MenuItem& last() { return items_.back(); }
    Menu& add(MenuItem it) { items_.push_back(std::move(it)); return *this; }
    std::vector<MenuItem>& items() noexcept { return items_; }
    const std::vector<MenuItem>& items() const noexcept { return items_; }
    int width() const noexcept { return width_; }
    void setWidth(int w) { width_ = w; }
    bool empty() const noexcept { return items_.empty(); }
private:
    std::vector<MenuItem> items_;
    int width_;
};

class Overlay : public juce::Component, private juce::Timer {
public:
    Overlay();
    ~Overlay() override;
    static Overlay* find(juce::Component& from);   // the Overlay of the editor `from` is in

    // Opens below `anchor` (above when there is no room), right-aligned unless alignLeft.
    // onClose: when the menu goes away (picked, Escape, a click outside, scrolling).
    void showMenu(Menu, juce::Component& anchor, bool alignLeft = false, std::function<void()> onClose = {});
    // Any content in a menu-styled card (padding 16, radius 16): the OBS popover, "paste a link".
    void showPopover(std::unique_ptr<juce::Component> content, juce::Component& anchor, bool alignLeft = false,
                     std::function<void()> onClose = {});
    void close();
    bool isOpen() const noexcept { return card_ != nullptr; }
    juce::Component* anchor() const { return anchor_.getComponent(); }

    // Ink card at the bottom centre for 2.4 s (rename: 2.6 s).
    void toast(const juce::String& text, int ms = 2400);
    juce::String currentToast() const { return toastText_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void place(juce::Component& card, int w, int h, bool alignLeft);
    void updateVisibility();

    std::unique_ptr<juce::Component> card_;
    juce::Component::SafePointer<juce::Component> anchor_;
    std::function<void()> onClose_;
    bool alignLeft_ = false;
    juce::String toastText_;
    juce::uint32 toastUntil_ = 0;
    juce::Rectangle<float> toastBox_;
    friend class MenuCard;
};

// A button that opens a list in the Overlay (the design's dropdown, 38 high, radius 10).
class Dropdown : public juce::Button {
public:
    Dropdown();
    void setItems(const juce::StringArray& items) { items_ = items; repaint(); }
    void setSelected(int index) { selected_ = index; repaint(); }
    int selected() const noexcept { return selected_; }
    juce::String selectedText() const { return items_[selected_]; }
    void setSmall(bool s) { small_ = s; repaint(); }
    void setDot(juce::Colour c) { dot_ = c; hasDot_ = true; repaint(); }   // the track colour before the text
    std::function<void(int)> onChange;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    void clicked() override;
private:
    juce::StringArray items_;
    int selected_ = 0;
    bool open_ = false, small_ = false, hasDot_ = false;
    juce::Colour dot_;
};

// Big button with an icon tile that opens a list of sources (App Audio, Track S7, Manage program
// audio). small = the row version (no tile, 26 high). The list is built fresh every time it opens.
class SourcePicker : public juce::Button {
public:
    explicit SourcePicker(bool small = false);
    void set(icons::Icon icon, const juce::String& text);
    std::function<Menu()> buildMenu;
    void setSmall(bool s) { small_ = s; repaint(); }
    int idealWidth() const;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    void clicked() override;
private:
    icons::Icon icon_ = icons::Icon::Window;
    juce::String text_;
    bool small_, open_ = false;
};

} // namespace hearaside
