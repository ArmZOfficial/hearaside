// "Mastering ฝั่งคนดู" panel: the Hub's hosted plug-in chain for the viewers' mix, plus a
// searchable picker over the installed VST3 plug-ins (files are listed, not loaded).
#pragma once

#include "MasteringChain.h"
#include "ui/Components.h"

namespace hearaside {

class MasteringPanel : public juce::Component, private juce::ChangeListener, private juce::ListBoxModel {
public:
    explicit MasteringPanel(MasteringChain& chain);
    ~MasteringPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Row : juce::Component {
        juce::TextButton bypass, open, up, down, remove;
        juce::String name;
        bool bypassed = false;
        void paint(juce::Graphics&) override;
        void resized() override;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override { rebuild(); }
    void rebuild();
    void showPicker(bool);
    void filter();
    void addSelected();

    // ListBoxModel (picker)
    int getNumRows() override { return filtered_.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { list_.selectRow(row); addSelected(); }
    void returnKeyPressed(int) override { addSelected(); }

    MasteringChain& chain_;
    juce::OwnedArray<Row> rows_;
    juce::TextButton addButton_, backButton_, chooseButton_;
    juce::TextEditor search_;
    juce::ListBox list_ { "plugins", this };
    juce::Array<juce::File> files_, filtered_;
    juce::String message_;
    bool picking_ = false;
};

} // namespace hearaside
