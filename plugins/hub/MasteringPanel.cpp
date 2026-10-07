#include "MasteringPanel.h"
#include "Settings.h"
#include "Strings.h"

namespace hearaside {

namespace {
constexpr int kWidth = 420, kRowH = 44, kHintH = 58, kPickerH = 360;

} // namespace

void MasteringPanel::Row::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    drawInset(g, getLocalBounds().toFloat().reduced(0.5f, 2.0f), 12.0f, p);
    g.setColour(bypassed ? p.muted : p.ink);
    g.setFont(uiFont(13.0f, Weight::Medium));
    g.drawText(name, getLocalBounds().withTrimmedLeft(12).withRight(bypass.getX() - 6), juce::Justification::centredLeft, true);
}

void MasteringPanel::Row::resized() {
    auto r = getLocalBounds().reduced(6, 8);
    remove.setBounds(r.removeFromRight(28));
    r.removeFromRight(4);
    down.setBounds(r.removeFromRight(28));
    r.removeFromRight(4);
    up.setBounds(r.removeFromRight(28));
    r.removeFromRight(6);
    open.setBounds(r.removeFromRight(54));
    r.removeFromRight(6);
    bypass.setBounds(r.removeFromRight(54));
}

MasteringPanel::MasteringPanel(MasteringChain& chain) : chain_(chain) {
    addButton_.setButtonText(tr(Str::AddPlugin));
    addButton_.onClick = [this] { showPicker(true); };
    backButton_.setButtonText(juce::String(juce::CharPointer_UTF8("\xe2\x86\x90 ")) + tr(Str::Mastering));
    backButton_.onClick = [this] { showPicker(false); };
    chooseButton_.setButtonText(tr(Str::AddPlugin).trimCharactersAtStart("+ "));
    chooseButton_.onClick = [this] { addSelected(); };
    search_.setTextToShowWhenEmpty(tr(Str::SearchPlugins), paletteOf(*this).muted);
    search_.setFont(uiFont(13.0f));
    search_.setIndents(10, 6);
    search_.onTextChange = [this] { filter(); };
    search_.onReturnKey = [this] { addSelected(); };
    list_.setRowHeight(30);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &addButton_, &backButton_, &chooseButton_, &search_, &list_ })
        addChildComponent(c);
    chain_.changed.addChangeListener(this);
    rebuild();
}

MasteringPanel::~MasteringPanel() { chain_.changed.removeChangeListener(this); }

void MasteringPanel::rebuild() {
    rows_.clear();
    if (!picking_) {
        for (int i = 0; i < chain_.size(); ++i) {
            auto* row = rows_.add(new Row());
            row->name = juce::String(i + 1) + ". " + chain_.name(i);
            row->bypassed = chain_.isBypassed(i);
            row->bypass.setButtonText(tr(Str::Bypass));
            row->bypass.setClickingTogglesState(false);
            row->bypass.setToggleState(row->bypassed, juce::dontSendNotification);
            row->bypass.onClick = [this, i] { chain_.setBypassed(i, !chain_.isBypassed(i)); };
            row->open.setButtonText(currentLanguage() == Language::English ? "Open" : juce::String(juce::CharPointer_UTF8("เปิด")));
            row->open.onClick = [this, i] { chain_.showEditor(i); };
            row->up.setButtonText(juce::String(juce::CharPointer_UTF8("\xe2\x86\x91")));
            row->up.onClick = [this, i] { chain_.move(i, i - 1); };
            row->up.setEnabled(i > 0);
            row->down.setButtonText(juce::String(juce::CharPointer_UTF8("\xe2\x86\x93")));
            row->down.onClick = [this, i] { chain_.move(i, i + 1); };
            row->down.setEnabled(i < chain_.size() - 1);
            row->remove.setButtonText(juce::String(juce::CharPointer_UTF8("\xe2\x9c\x95")));
            row->remove.onClick = [this, i] { chain_.remove(i); };
            for (auto* b : { &row->bypass, &row->open, &row->up, &row->down, &row->remove }) row->addAndMakeVisible(b);
            addAndMakeVisible(row);
        }
        addButton_.setEnabled(chain_.size() < MasteringChain::kMaxPlugins);
    }
    const int h = picking_ ? kPickerH : kHintH + juce::jmax(1, chain_.size()) * kRowH + 52 + (message_.isNotEmpty() ? 24 : 0);
    setSize(kWidth, h);
    resized();
    repaint();
}

void MasteringPanel::showPicker(bool on) {
    picking_ = on;
    message_ = {};
    if (on && files_.isEmpty()) files_ = MasteringChain::findPluginFiles();
    addButton_.setVisible(!on);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &backButton_, &chooseButton_, &search_, &list_ }) c->setVisible(on);
    if (on) { search_.clear(); filter(); search_.grabKeyboardFocus(); }
    rebuild();
}

void MasteringPanel::filter() {
    filtered_.clear();
    const auto q = search_.getText().trim();
    for (const auto& f : files_)
        if (q.isEmpty() || f.getFileNameWithoutExtension().containsIgnoreCase(q)) filtered_.add(f);
    list_.updateContent();
    list_.selectRow(filtered_.isEmpty() ? -1 : 0);
    repaint();
}

void MasteringPanel::addSelected() {
    const int row = list_.getSelectedRow();
    if (row < 0 || row >= filtered_.size()) return;
    const auto file = filtered_[row];
    auto types = chain_.typesIn(file);
    if (types.isEmpty()) { message_ = tr(Str::PluginLoadFailed); repaint(); return; }

    auto addType = [this](const juce::PluginDescription& d) {
        const auto err = chain_.add(d);
        if (err.isEmpty()) { showPicker(false); return; }
        message_ = err == "instrument" ? tr(Str::PluginInstrument) : err == "full" ? tr(Str::MasteringFull) : tr(Str::PluginLoadFailed);
        repaint();
    };
    if (types.size() == 1) { addType(*types[0]); return; }

    // shell files (e.g. one .vst3 with several plug-ins): let the user pick
    juce::PopupMenu m;
    m.setLookAndFeel(&getLookAndFeel());
    for (int i = 0; i < types.size(); ++i) m.addItem(i + 1, types[i]->name, !types[i]->isInstrument);
    auto shared = std::make_shared<juce::OwnedArray<juce::PluginDescription>>();
    shared->swapWith(types);
    juce::Component::SafePointer<MasteringPanel> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&chooseButton_), [self, shared, addType](int r) {
        if (self != nullptr && r > 0) addType(*(*shared)[r - 1]);
    });
}

void MasteringPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (row < 0 || row >= filtered_.size()) return;
    const auto& p = paletteOf(*this);
    if (selected) {
        g.setColour(p.ink);
        g.fillRoundedRectangle(juce::Rectangle<float>(0, 1, float(width), float(height - 2)), 8.0f);
    }
    const auto f = filtered_[row];
    g.setColour(selected ? p.onInk : p.ink);
    g.setFont(uiFont(13.0f));
    g.drawText(f.getFileNameWithoutExtension(), 10, 0, width - 20, height, juce::Justification::centredLeft, true);
}

void MasteringPanel::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    auto r = getLocalBounds().reduced(10);
    if (!picking_) {
        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText(tr(Str::Mastering), r.removeFromTop(22), juce::Justification::centredLeft, false);
        drawWrapped(g, tr(Str::MasteringHint), uiFont(11.5f), p.graphite, r.removeFromTop(kHintH - 22).toFloat(), 1.0f);
        if (chain_.size() == 0) {
            g.setColour(p.muted);
            g.setFont(uiFont(13.0f));
            g.drawText(tr(Str::MasteringNone), r.removeFromTop(kRowH), juce::Justification::centred, false);
        }
    } else if (files_.isEmpty()) {
        g.setColour(p.graphite);
        g.setFont(uiFont(13.0f));
        g.drawText(tr(Str::NoPluginsFound), list_.getBounds(), juce::Justification::centred, false);
    }
    if (message_.isNotEmpty()) {
        g.setColour(p.ink);
        g.setFont(uiFont(12.0f, Weight::Medium));
        g.drawText(message_, getLocalBounds().reduced(12, 8).removeFromBottom(picking_ ? 54 : 46).removeFromTop(20),
                   juce::Justification::centredLeft, true);
    }
}

void MasteringPanel::resized() {
    auto r = getLocalBounds().reduced(10);
    if (picking_) {
        auto top = r.removeFromTop(30);
        backButton_.setBounds(top.removeFromLeft(juce::jmin(200, top.getWidth())));
        r.removeFromTop(8);
        search_.setBounds(r.removeFromTop(32));
        r.removeFromTop(8);
        auto bottom = r.removeFromBottom(32);
        chooseButton_.setBounds(bottom.removeFromRight(130));
        r.removeFromBottom(30);
        list_.setBounds(r);
        return;
    }
    r.removeFromTop(kHintH);
    for (auto* row : rows_) row->setBounds(r.removeFromTop(kRowH));
    if (rows_.isEmpty()) r.removeFromTop(kRowH);
    if (message_.isNotEmpty()) r.removeFromTop(24);
    r.removeFromTop(8);
    addButton_.setBounds(r.removeFromTop(34));
}

} // namespace hearaside
