// Virtual mic: plays the Stream Mix into a Windows output device such as VB-Cable's "CABLE Input",
// so programs without a HEARASIDE source (TikTok LIVE Studio, Discord...) can pick "CABLE Output" as
// their microphone. Reads the bus like the OBS source (ssdsp::StreamConsumer: drift + rate), on the
// output device's own thread - never on the DAW's audio thread.
#pragma once

#include "ssdsp/consumer.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <mutex>
#include <vector>

namespace hearaside {

class VirtualMic : private juce::AudioIODeviceCallback {
public:
    ~VirtualMic() override;

    static juce::StringArray outputDevices();     // Windows Audio (WASAPI shared) outputs
    static int cableIndex(const juce::StringArray& devices);   // "CABLE Input ..." or -1

    // Message thread. Opens `device` and starts sending; "" = off. Returns an error ("" = ok).
    juce::String start(const juce::String& device, const juce::String& busName);
    void stop();
    juce::String device() const { return device_; }
    juce::String busName() const { return bus_; }
    ssdsp::ConsumerStatus status() const;

private:
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}

    std::unique_ptr<juce::AudioIODevice> dev_;
    juce::String device_, bus_;
    ssdsp::StreamConsumer consumer_;
    double rate_ = 48000.0;
    mutable std::mutex statusMutex_;
    ssdsp::ConsumerStatus status_;
};

} // namespace hearaside
