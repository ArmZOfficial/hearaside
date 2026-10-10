// HEARASIDE Track: last insert on every track. Sends what viewers hear to the DAW (so plug-ins
// on the master bus above the Hub master the stream) and publishes the post-FX signal to the bus,
// from which the Hub builds what you hear in your headphones.
#pragma once

#include "Settings.h"
#include "ssengine/engine.h"
#include "ssengine/friend_reader.h"

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
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "HEARASIDE Track"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return isFriendInput() ? std::numeric_limits<double>::infinity() : 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }   // VST3 validator: every program needs a name
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    void updateTrackProperties(const TrackProperties&) override;

    // ---- for the editor (message thread) --------------------------------------------------
    juce::AudioProcessorValueTreeState& params() noexcept { return apvts_; }
    ssengine::TrackPublisher& publisher() noexcept { return pub_; }
    juce::String displayName() const;                 // what the UI / Hub shows
    juce::Colour trackColour() const;                 // host colour, or transparent
    void setDisplayNameOverride(const juce::String&); // "" = follow the host; cut to 63 bytes UTF-8
    juce::String displayNameOverride() const { return nameOverride_; }
    juce::String hostTrackName() const { return hostName_; }   // the DAW's name for the track (message thread)
    juce::String busName() const;
    void setBusName(const juce::String&);
    int  stemIndex() const noexcept { return stem_.load(); }
    void setStemIndex(int);
    bool isBypassedNow() const noexcept { return bypassed_.load(std::memory_order_relaxed); }
    float inputPeak(int ch) const noexcept { return peak_[juce::jlimit(0, 1, ch)].load(std::memory_order_relaxed); }
    int lastBlockSize() const noexcept { return lastBlock_.load(std::memory_order_relaxed); }
    double currentSampleRate() const noexcept { return sampleRate_; }

    // ---- Role & friend input (S7) --------------------------------------------------------
    enum class Role { Track, FriendInput };
    Role role() const noexcept { return role_.load(std::memory_order_relaxed); }
    bool isFriendInput() const noexcept { return role() == Role::FriendInput; }
    void setRole(Role);

    uint32_t friendId() const noexcept { return friendId_.load(std::memory_order_relaxed); }
    void setFriendId(uint32_t);

    int feederIndex() const noexcept { return feederIndex_.load(std::memory_order_relaxed); }
    const ssbus::FeederRecord* feederRecord() const noexcept { return feeder_.load(std::memory_order_relaxed); }
    ssbus::BusLayout* bus() const noexcept;

    // Requests (plug-in -> Hub)
    void requestResolveToken(const juce::String& token);
    void requestCreateFriend(const juce::String& name);
    void requestReleaseFriend();
    void requestCopyLink(uint32_t friendId);
    uint32_t lastReply() const noexcept { return lastReply_.load(std::memory_order_relaxed); }
    void clearReply() noexcept { lastReply_.store(0, std::memory_order_relaxed); }

    // Paired friend (when role is normal Track at the end of the channel)
    uint32_t pairedFriendId() const noexcept;
    float fxLatencyMs() const noexcept;

    // Notifies the editor (message thread) that identity / connection changed.
    juce::ChangeBroadcaster stateChanged;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void connect();
    void connectFeeder();
    void disconnectFeeder();
    void pushFeederIdentity();
    void applyCommand(ssbus::ParamId id, float value);
    void setParamPlain(const char* id, float plain);
    void pushIdentity();
    void process(juce::AudioBuffer<float>&, bool bypassed);
    void processTrack(juce::AudioBuffer<float>&, bool bypassed, int n, int numCh);
    void processFriend(juce::AudioBuffer<float>&, bool bypassed, int n, int numCh);

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
    std::atomic<bool> busSolo_ { false };   // some Track on the bus is soloed for the viewers
    std::atomic<bool> hubMute_ { false };   // the Hub is measuring (auto sync)
    std::atomic<float> peak_[2] {};
    std::atomic<int> lastBlock_ { 0 };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> viewersGain_[2];   // gain x pan, per channel
    juce::AudioBuffer<float> delayLine_;
    int delayWrite_ = 0;
    double sampleRate_ = 48000.0;
    int numChannels_ = 2;

    // identity (message thread), host properties may arrive on any thread
    juce::CriticalSection hostLock_;
    juce::String hostName_, pendingHostName_;
    juce::Colour hostColour_, pendingHostColour_;
    bool hostDirty_ = false;
    juce::String nameOverride_, busName_ = defaultBusName(), uuid_;
    float chainMs_ = 0.0f;   // plug-in latency before this Track, measured by the Hub, saved for live use
    juce::String pushedName_;
    juce::uint32 pushedColour_ = 0;
    uint32_t cmdCursor_ = 0, renameSeq_ = 0;
    juce::uint32 lastConnectAttempt_ = 0;
    ssengine::TrackPublisher::Status lastStatus_ = ssengine::TrackPublisher::Status::Disconnected;
    bool lastHub_ = false;

    // friend input state
    std::atomic<Role> role_{ Role::Track };
    std::atomic<uint32_t> friendId_{ 0 };
    std::atomic<int> feederIndex_{ -1 };
    std::atomic<ssbus::FeederRecord*> feeder_{ nullptr };
    std::atomic<ssbus::BusLayout*> feederBus_{ nullptr };
    std::unique_ptr<ssbus::SharedMemory> feederShm_;
    std::vector<std::unique_ptr<ssbus::SharedMemory>> retiredFeederShm_;
    ssengine::FriendReader reader_;
    std::vector<float> monoScratch_;   // right channel of a mono track, sized in prepareToPlay (never on the audio thread)
    int friendChunk_ = 2048;           // the most frames reader_ takes per pull
    float tapSum_ = 0.0f;
    int tapPhase_ = 0;
    uint32_t replySeq_ = 0;
    std::atomic<uint32_t> lastReply_{ 0 };
    uint32_t feederCmdSeq_ = 0;
    uint32_t pendingRequest_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackProcessor)
};

} // namespace hearaside
