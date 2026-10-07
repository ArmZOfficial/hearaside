// HEARASIDE Track: last insert on every track. Publishes the post-FX signal to the bus (the Hub
// decides what reaches the viewers) and controls what the DAW / headphones get.
#pragma once

#include "Settings.h"
#include "ssengine/engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace hearaside {

namespace trackparam {
inline constexpr const char* Mon      = "mon";
inline constexpr const char* Str      = "str";
inline constexpr const char* StrGain  = "strGain";
inline constexpr const char* StrPan   = "strPan";
inline constexpr const char* StrDelay = "strDelay";
inline constexpr const char* MonTrim  = "monTrim";
inline constexpr const char* StrSolo  = "strSolo";
}

class TrackProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    TrackProcessor();
    ~TrackProcessor() override;

    // ---- AudioProcessor -----------------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "HEARASIDE Track"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    void updateTrackProperties(const TrackProperties&) override;

    // ---- for the editor (message thread) --------------------------------------------------
    juce::AudioProcessorValueTreeState& params() noexcept { return apvts_; }
    ssengine::TrackPublisher& publisher() noexcept { return pub_; }
    juce::String displayName() const;                 // what the UI / Hub shows
    juce::Colour trackColour() const;                 // host colour, or transparent
    void setDisplayNameOverride(const juce::String&); // "" = follow the host
    juce::String busName() const;
    void setBusName(const juce::String&);
    int  stemIndex() const noexcept { return stem_.load(); }
    void setStemIndex(int);
    bool isBypassedNow() const noexcept { return bypassed_.load(std::memory_order_relaxed); }
    float inputPeak(int ch) const noexcept { return peak_[juce::jlimit(0, 1, ch)].load(std::memory_order_relaxed); }
    int lastBlockSize() const noexcept { return lastBlock_.load(std::memory_order_relaxed); }
    double currentSampleRate() const noexcept { return sampleRate_; }

    // Notifies the editor (message thread) that identity / connection changed.
    juce::ChangeBroadcaster stateChanged;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void connect();
    void applyCommand(ssbus::ParamId id, float value);
    void setParamPlain(const char* id, float plain);
    void pushIdentity();
    void process(juce::AudioBuffer<float>&, bool bypassed);

    juce::AudioProcessorValueTreeState apvts_;
    ssengine::TrackPublisher pub_;

    std::atomic<float>* mon_ = nullptr;
    std::atomic<float>* str_ = nullptr;
    std::atomic<float>* gain_ = nullptr;
    std::atomic<float>* pan_ = nullptr;
    std::atomic<float>* delay_ = nullptr;
    std::atomic<float>* trim_ = nullptr;
    std::atomic<float>* solo_ = nullptr;
    std::atomic<int> stem_ { -1 };
    std::atomic<bool> bypassed_ { false };
    std::atomic<float> peak_[2] {};
    std::atomic<int> lastBlock_ { 0 };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> monitorGain_;
    double sampleRate_ = 48000.0;
    int numChannels_ = 2;

    // identity (message thread), host properties may arrive on any thread
    juce::CriticalSection hostLock_;
    juce::String hostName_, pendingHostName_;
    juce::Colour hostColour_, pendingHostColour_;
    bool hostDirty_ = false;
    juce::String nameOverride_, busName_ = defaultBusName(), uuid_;
    juce::String pushedName_;
    juce::uint32 pushedColour_ = 0;
    uint32_t cmdCursor_ = 0, renameSeq_ = 0;
    juce::uint32 lastConnectAttempt_ = 0;
    ssengine::TrackPublisher::Status lastStatus_ = ssengine::TrackPublisher::Status::Disconnected;
    bool lastHub_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackProcessor)
};

} // namespace hearaside
