#pragma once

#include "Components.h"
#include "Controls.h"
#include "LookAndFeel.h"
#include "Overlay.h"
#include "Settings.h"
#include "Strings.h"
#include "Theme.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace hearaside::karaoke {

class KaraokeContent;

class MainWindow : public juce::DocumentWindow, private juce::ChangeListener {
public:
    MainWindow();
    ~MainWindow() override;

    void closeButtonPressed() override;
    void handleCommandLine(const juce::String& cmd);
    void handleDeepLink(const juce::String& link);

    juce::AudioDeviceManager& audioDeviceManager() noexcept { return deviceManager_; }
    KaraokeContent* content() noexcept { return content_.get(); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void applyTheme(bool dark);

    SharedSettings settings_;
    hearaside::LookAndFeel lnf_;
    juce::AudioDeviceManager deviceManager_;
    std::unique_ptr<KaraokeContent> content_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace hearaside::karaoke
