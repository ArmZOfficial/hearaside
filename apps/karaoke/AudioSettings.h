#pragma once

#include "Settings.h"
#include "Theme.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace hearaside::karaoke {

struct LatencyInfo {
    double inputMs = 0.0;
    double outputMs = 0.0;
    double totalMs = 0.0;
    int sampleRate = 0;
    int bufferSize = 0;
    juce::String driverType;
    juce::String deviceName;
};

class AudioSettings {
public:
    static LatencyInfo getLatencyInfo(juce::AudioDeviceManager& dm);
    static void saveState(juce::AudioDeviceManager& dm, SharedSettings& settings);
    static void loadState(juce::AudioDeviceManager& dm, SharedSettings& settings);
    static void showSettingsDialog(juce::AudioDeviceManager& dm, const theme::Palette& pal,
                                   std::function<void()> onClosed = nullptr);
};

} // namespace hearaside::karaoke
