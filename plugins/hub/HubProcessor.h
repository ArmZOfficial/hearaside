// HEARASIDE Hub: last insert on the master bus. Mixes every Track's "viewers" signal into the
// Stream Mix (+ stems) for OBS, and acts as the remote control for all Tracks.
#pragma once

#include "Settings.h"
#include "ControlServer.h"
#include "FriendRoom.h"
#include "FriendDirectory.h"
#include "ShareDirectory.h"
#include "ShareServer.h"
#include "Strings.h"
#include "ssengine/engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

namespace hearaside {

namespace hubparam {
inline constexpr const char* Master     = "streamMaster";
inline constexpr const char* LimiterOn  = "limiterOn";
inline constexpr const char* Ceiling    = "ceiling";
inline constexpr const char* Preview    = "preview";
inline constexpr const char* Panic      = "panic";
inline constexpr const char* SyncSafety = "syncSafety";
inline constexpr const char* Headphones = "monitorMaster";
}

// Snapshot of one Track slot, read from shared memory on the message thread.
struct TrackView {
    int slot = -1;
    uint32_t sequence = 0;
    juce::String uuid, name;
    juce::uint32 colourARGB = 0;
    bool mon = false, str = false, solo = false, bypassed = false, mono = false, active = false;
    float gainDb = 0, pan = 0, delayMs = 0, trimDb = 0;
    float chainMs = 0;   // plug-in latency before the Track (measured by the Hub)
    int stem = -1;
    float peakIn = 0, peakStream = 0;
    uint32_t hubStatus = 0;
};

// Snapshot of one HEARASIDE App Audio instance on the bus.
struct SourceView {
    int index = -1;
    uint32_t sequence = 0;
    juce::String name, app;   // app: "chrome.exe", "*system*", "" = none
    juce::uint32 colourARGB = 0;
    uint32_t flags = 0, capture = 0;
    float levelDb = 0, peak = 0, latencyMs = 0, delayMs = 0;
    double recordSec = 0;
    bool active = false;
    // its headphone slot ("you hear" / "viewers hear", sent with send(slot, ...)); slot -1 = none yet
    int slot = -1;
    bool mon = false, str = false;
    float gainDb = 0, trimDb = 0;
    float takeShiftMs = 0;   // a friend source: the friend's delay + this App Audio's buffer
    bool on() const noexcept { return (flags & ssbus::kSrcOn) != 0; }
    bool recording() const noexcept { return (flags & ssbus::kSrcRecording) != 0; }
};

class HubProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    HubProcessor();
    ~HubProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
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
    const juce::String getProgramName(int) override { return "Default"; }   // VST3 validator: every program needs a name
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // ---- message thread API for the editor --------------------------------------------------
    juce::AudioProcessorValueTreeState& params() noexcept { return apvts_; }
    ssengine::HubEngine& engine() noexcept { return engine_; }
    bool connected() const noexcept { return engine_.bus() != nullptr; }
    juce::String busName() const { return busName_; }
    void setBusName(const juce::String&);

    std::vector<TrackView> tracks() const;       // active slots, in insertion order
    void send(int slot, ssbus::ParamId id, float value);
    void rename(int slot, const juce::String& name);
    std::vector<SourceView> sources() const;     // App Audio instances, in insertion order
    void sendSource(int index, ssbus::SourceParam id, float value);
    void chooseSourceApp(int index, const juce::String& exe);

    juce::String stemName(int i) const { return stemNames_[size_t(juce::jlimit(0, ssbus::kMaxStems - 1, i))]; }
    void setStemName(int i, const juce::String&);

    void setParam(const char* id, float plain);
    bool obsConnected() const;
    double latencyToObsMs() const;

    struct LatencyInfo {
        int block = 0;                 // DAW buffer (frames) of the latest block
        double sampleRate = 48000.0;
        double dawMs = 0, trackFxMs = 0, masterFxMs = 0, hubMs = 0, obsMs = 0;   // plug-ins in the tracks / above the Hub
        bool obs = false;
        double total() const { return dawMs + trackFxMs + masterFxMs + hubMs + obsMs; }
    };
    LatencyInfo latency() const;

    // Auto sync: lines the microphone track up with the music for the viewers. With the music
    // playing and the headphones held to the mic, it measures how late the music reaches the master
    // straight (App Audio or a music track) and through the microphone, then sets the delays
    // (Tracks' Viewers Delay, App Audio's Sync Delay). No extra plug-in is needed for App Audio.
    enum class SyncPhase { Idle, Countdown, Reference, Microphone, Done, Failed };
    struct SyncStatus {
        SyncPhase phase = SyncPhase::Idle;
        int mic = -1, ref = -1;
        bool refSource = false;   // ref is an App Audio (index), else a Track slot
        double deltaMs = 0.0;     // how late the voice was against the music (negative = early)
        Str error = Str::None;
    };
    void startAutoSync();                                                   // guess the tracks, 3 s countdown
    void startAutoSync(int micSlot, int ref, bool refSource, int countdownMs);
    void cancelAutoSync();
    bool defaultSyncTracks(int& mic, int& ref, bool& refSource) const;
    SyncStatus autoSync() const { return autoSync_; }

    // Share links (like LISTENTO): listen to the Stream Mix in a browser, or send a mic in.
    void setSharing(bool on);
    bool sharing() const noexcept { return shareWanted_; }
    const ShareServer& share() const noexcept { return share_; }
    const ShareDirectory& directory() const noexcept { return directory_; }
    bool permanentLinksSet() const;   // a share web site is set and permanent links are on
    juce::String permanentUrl(bool listen) const;   // https://<site>/l/<token> (or /s/), "" = not set
    const ControlServer& control() const noexcept { return control_; }   // REST API

    // ---- friends room (S2) ------------------------------------------------------------------
    // Each friend gets a send-in link of their own; the Hub plays them (headphones + Stream Mix) and
    // can line them up with the music for the viewers (Line up). Message thread.
    std::vector<FriendView> friends() const { return friendRoom_.views(engine_.bus(), engine_); }
    int friendCount() const noexcept { return friendRoom_.count(); }
    bool roomFull() const noexcept { return friendRoom_.full(); }
    uint32_t addFriend(const juce::String& name = {});          // 0 = full
    void removeFriend(uint32_t id);
    void renameFriend(uint32_t id, const juce::String& name);
    void setFriendVolumeDb(uint32_t id, float db) { friendRoom_.setVolumeDb(id, db); }
    void setFriendPan(uint32_t id, float pan) { friendRoom_.setPan(id, pan); }
    void setFriendMon(uint32_t id, bool on) { friendRoom_.setMon(id, on); }
    void setFriendStr(uint32_t id, bool on) { friendRoom_.setStr(id, on); }
    void setFriendSolo(uint32_t id, bool on) { friendRoom_.setSolo(id, on); }
    void spreadFriends() { friendRoom_.spreadOut(); }
    juce::String friendLink(uint32_t id) const;                 // the link to send them ("" when sharing can't start)
    juce::String newFriendLink(uint32_t id);                    // the old link stops working
    void remeasureFriend(uint32_t id);                          // "Measure again"
    void setLineUp(bool on);
    bool lineUp() const noexcept { return lineUp_; }
    float lineUpMs() const noexcept { return engine_.lineUpMs(); }          // delay of the live right now
    int lineUpSlowestFriend() const;                                         // friend id, 0 = none
    uint32_t lineUpChanges() const noexcept { return engine_.lineUpChanges(); }
    int lineUpLimitMs() const { return settings_->lineUpLimitMs(); }
    void setLineUpLimitMs(int ms) { settings_->setLineUpLimitMs(ms); }
    int friendsConnected() const noexcept { return share_.friendsConnected(); }
    juce::String feederTrackName(int feeder) const;             // the DAW track a feeder sits on ("" = unknown)
    void bringBackFriend(uint32_t id);                          // the DAW track plays its own sound again; the Hub plays the friend

    // "Viewers hear nothing" (docs/ux-roadmap.md 7.3): the Stream Mix has been silent for 5 s while a
    // track that goes to the viewers has signal. From the meters, on the message thread.
    bool viewersSilent() const noexcept { return viewersSilent_; }
    juce::String silentTrack() const { return silentTrack_; }   // one of the tracks that should be heard
    juce::String diagnostics() const;                           // "copy problem report" (no links, no secrets)
    int syncCountdown() const;                                              // seconds left, 0 = not counting
    juce::uint32 syncFinishedMs() const { return syncWall_; }              // when Done / Failed was reached

    juce::ChangeBroadcaster stateChanged;   // bus / stem names changed

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void pushStemNames();
    void run(juce::AudioBuffer<float>&, bool bypassed);
    void serviceRemote();                                   // OBS hotkeys -> Hub
    void serviceRequests();                                 // Track requests -> Hub
    void serviceFeeders();                                  // S7 feeder route & status
    void serviceAutoSync();
    void serviceShare();
    void finishAutoSync(Str error);
    Str steadyLag(double& ms, Str notFound);                // agree on this phase's readings
    void writeSyncLog(const juce::String& result);          // Documents\HEARASIDE\autosync.log
    void muteAllBut(int slot, int source);                  // keep one Track / App Audio; both -1 = nobody muted
    void applyRemote(int target, uint32_t paramId, float value);

    juce::AudioProcessorValueTreeState apvts_;
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
    std::array<juce::String, ssbus::kMaxStems> stemNames_;
    juce::uint32 lastMaintain_ = 0, lastConnect_ = 0;
    std::atomic<double> masterFxMs_ { 0.0 }, trackFxMs_ { 0.0 };
    // measures the plug-in latency 4x a second off the message thread
    struct LatencyMeter : juce::Thread {
        explicit LatencyMeter(HubProcessor& p) : juce::Thread("HEARASIDE latency"), proc(p) {}
        void run() override { while (!threadShouldExit()) { proc.measureLatency(); wait(250); } }
        HubProcessor& proc;
    };
    void measureLatency();
    LatencyMeter latencyMeter_ { *this };
    SyncStatus autoSync_;
    uint64_t syncFrom_ = 0;            // analysed history when the current phase started
    juce::uint32 syncWall_ = 0;
    double syncRefMs_ = 0.0;
    struct SyncRead { std::optional<double> ms; double score; };
    std::vector<SyncRead> syncReads_;        // this phase's readings
    uint64_t syncLastRead_ = 0;              // analysed history at the last reading
    juce::String syncLog_;                   // this run, for the log file
    std::atomic<bool> silence_ { false };
    void ensureStrongTokens();
    void serviceControl();
    void serviceSilence();
    juce::uint32 silentSince_ = 0;
    bool viewersSilent_ = false;
    juce::String silentTrack_;
    ControlServer::Response handleControl(const ControlServer::Request&);   // HubRestApi.cpp
    ControlServer control_;
    juce::String controlKey_;
    juce::uint32 controlTry_ = 0;
    ShareServer share_;
    ShareDirectory directory_;
    SharedSettings settings_;
    bool shareWanted_ = false;
    FriendRoom friendRoom_;
    bool lineUp_ = true;
    ssbus::BusLayout* shareBus_ = nullptr;
    juce::String listenToken_ = ShareServer::newToken(), sendToken_ = ShareServer::newToken();   // saved: links stay the same
    juce::String shareSecret_;   // saved: proves to the share web site that the permanent links are ours (never logged)
    uint32_t remoteCursor_ = 0;
    ssbus::BusLayout* remoteBus_ = nullptr;   // bus the cursor belongs to
    uint32_t requestCursor_ = 0;
    ssbus::BusLayout* requestBus_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HubProcessor)
};

} // namespace hearaside
