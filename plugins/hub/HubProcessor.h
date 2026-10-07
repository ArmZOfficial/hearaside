// HEARASIDE Hub: last insert on the master bus. Mixes every Track's "viewers" signal into the
// Stream Mix (+ stems) for OBS, and acts as the remote control for all Tracks.
#pragma once

#include "Settings.h"
#include "MasteringChain.h"
#include "ssengine/engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <map>

namespace hearaside {

namespace hubparam {
inline constexpr const char* Master     = "streamMaster";
inline constexpr const char* LimiterOn  = "limiterOn";
inline constexpr const char* Ceiling    = "ceiling";
inline constexpr const char* Preview    = "preview";
inline constexpr const char* Panic      = "panic";
inline constexpr const char* SyncSafety = "syncSafety";
inline constexpr const char* Scene      = "scene";
inline constexpr const char* Headphones = "monitorMaster";
}

constexpr int kNumScenes = 8;

struct SceneTrack {
    bool mon = true, str = true, solo = false;
    float gainDb = 0, pan = 0, trimDb = 0, delayMs = 0;
};

struct Scene {
    juce::String name;                          // empty = default name for slots 1-3
    std::map<juce::String, SceneTrack> tracks;  // by Track uuid
    bool saved() const { return !tracks.empty(); }
};

// Snapshot of one Track slot, read from shared memory on the message thread.
struct TrackView {
    int slot = -1;
    uint32_t sequence = 0;
    juce::String uuid, name;
    juce::uint32 colourARGB = 0;
    bool mon = false, str = false, solo = false, bypassed = false, mono = false, active = false;
    float gainDb = 0, pan = 0, delayMs = 0, trimDb = 0;
    int stem = -1;
    float peakIn = 0, peakStream = 0;
    uint32_t hubStatus = 0;
};

class HubProcessor : public juce::AudioProcessor, private juce::Timer,
                     private juce::AudioProcessorValueTreeState::Listener {
public:
    HubProcessor();
    ~HubProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { mastering_.release(); }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "HEARASIDE Hub"; }
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

    // ---- message thread API for the editor --------------------------------------------------
    juce::AudioProcessorValueTreeState& params() noexcept { return apvts_; }
    ssengine::HubEngine& engine() noexcept { return engine_; }
    MasteringChain& mastering() noexcept { return mastering_; }
    bool connected() const noexcept { return engine_.bus() != nullptr; }
    juce::String busName() const { return busName_; }
    void setBusName(const juce::String&);

    std::vector<TrackView> tracks() const;       // active slots, in insertion order
    void send(int slot, ssbus::ParamId id, float value);
    void rename(int slot, const juce::String& name);

    const Scene& scene(int i) const { return scenes_[size_t(juce::jlimit(0, kNumScenes - 1, i))]; }
    juce::String sceneName(int i) const;
    void recallScene(int i);                     // via the "scene" parameter (host-visible)
    void saveScene(int i);
    void renameScene(int i, const juce::String&);
    void clearScene(int i);
    int  activeScene() const noexcept { return activeScene_; }   // -1 = custom

    juce::String stemName(int i) const { return stemNames_[size_t(juce::jlimit(0, ssbus::kMaxStems - 1, i))]; }
    void setStemName(int i, const juce::String&);

    void setParam(const char* id, float plain);
    bool obsConnected() const;
    double latencyToObsMs() const;

    struct LatencyInfo {
        int block = 0;                 // DAW buffer (frames) of the latest block
        double sampleRate = 48000.0;
        double dawMs = 0, hubMs = 0, masteringMs = 0, obsMs = 0;
        bool obs = false;
        double total() const { return dawMs + hubMs + masteringMs + obsMs; }
    };
    LatencyInfo latency() const;

    juce::ChangeBroadcaster stateChanged;   // scenes / bus / stem names changed

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void parameterChanged(const juce::String& id, float value) override;
    void applyScene(int i);
    void pushStemNames();
    bool sceneMatches(int i) const;
    void run(juce::AudioBuffer<float>&, bool bypassed);
    void serviceRemote();                                   // OBS dock / hotkeys -> Hub
    void applyRemote(int target, uint32_t paramId, float value);

    juce::AudioProcessorValueTreeState apvts_;
    MasteringChain mastering_;
    ssengine::HubEngine engine_;
    std::atomic<float>* master_ = nullptr;
    std::atomic<float>* limiter_ = nullptr;
    std::atomic<float>* ceiling_ = nullptr;
    std::atomic<float>* preview_ = nullptr;
    std::atomic<float>* panic_ = nullptr;
    std::atomic<float>* sync_ = nullptr;
    std::atomic<float>* headphones_ = nullptr;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> headphoneGain_;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;

    juce::String busName_ = defaultBusName();
    std::array<Scene, kNumScenes> scenes_;
    std::array<juce::String, ssbus::kMaxStems> stemNames_;
    std::atomic<int> pendingScene_ { 0 };
    int activeScene_ = -1;
    juce::uint32 sceneRecallMs_ = 0, stateLoadMs_ = 0;
    juce::uint32 lastMaintain_ = 0, lastConnect_ = 0;
    uint32_t remoteCursor_ = 0;
    ssbus::BusLayout* remoteBus_ = nullptr;   // bus the cursor belongs to

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HubProcessor)
};

} // namespace hearaside
