// HEARASIDE engines: the real-time core of the Track and Hub plug-ins, free of JUCE so it
// can be unit tested and simulated with arbitrary host behaviour.
// SPDX-License-Identifier: MIT
#pragma once

#if defined(HEARASIDE_USE_RUST_CORE) && HEARASIDE_USE_RUST_CORE
#include "ssengine/engine_rust.h"   // the Rust core (PLAN-rust-migration R2.5)
#else

#include "ssbus/bus.h"
#include "ssdsp/consumer.h"
#include "ssdsp/dsp.h"

#include <optional>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ssengine {

constexpr int64_t kNoTime = INT64_MIN;

// The sweep a friend input plays when the Hub probes it (300 ms, logarithmic, -24 dBFS peak): one definition for the
// Track that plays it and the tests that stand in for a DAW chain.
std::vector<float> probeSweep(double sampleRate);

// =============================================================================================
// TrackPublisher - lives in every HEARASIDE Track instance.

class TrackPublisher {
public:
    enum class Status { Disconnected, Connected, SlotsFull, ShmError };

    TrackPublisher();
    ~TrackPublisher();

    // ---- message thread ------------------------------------------------------------------
    // Opens the bus and claims a slot. If the uuid is already used by another live slot
    // (duplicated track) a fresh uuid is generated; check uuid() afterwards.
    Status connect(const std::string& busName, std::string uuid, uint32_t sampleRate, uint32_t numChannels);
    void   disconnect();
    void   prepare(uint32_t sampleRate, uint32_t numChannels);   // sample-rate / layout change
    void   setIdentity(const std::string& name, uint32_t colorARGB);

    Status status() const noexcept { return status_; }
    const std::string& uuid() const noexcept { return uuid_; }
    const std::string& busName() const noexcept { return busName_; }
    int    slotIndex() const noexcept { return slotIndex_; }
    ssbus::BusLayout*  bus() const noexcept { return bus_.load(std::memory_order_acquire); }
    ssbus::SlotHeader* slot() const noexcept { return slot_.load(std::memory_order_acquire); }
    bool   hubPresent() const noexcept;
    bool   soloActive() const noexcept;

    // ---- any thread ---------------------------------------------------------------------
    struct Mirror { bool mon, str, solo; float gainDb, pan, delayMs, trimDb; int stem; bool app = false; };   // app: an App Audio's slot
    void mirror(const Mirror& m) noexcept;

    // ---- audio thread -------------------------------------------------------------------
    // Publishes the post-FX block. timeSamples = kNoTime when the host gives no playhead.
    // The Track itself sends what viewers hear to the DAW; soloActive() tells it whether any
    // track on the bus is soloed for the viewers.
    void process(const float* const* ch, int numCh, int n, int64_t timeSamples, bool playing,
                 bool offline, bool bypassed) noexcept;

private:
    std::atomic<ssbus::BusLayout*>  bus_{ nullptr };
    std::atomic<ssbus::SlotHeader*> slot_{ nullptr };
    std::unique_ptr<ssbus::SharedMemory> shm_;
    std::vector<std::unique_ptr<ssbus::SharedMemory>> retired_;   // kept mapped: audio thread may still touch them
    std::string busName_, uuid_;
    int      slotIndex_ = -1;
    Status   status_ = Status::Disconnected;
    uint32_t sampleRate_ = 48000;
    Mirror   lastMirror_{ true, true, false, 0, 0, 0, 0, -1 };
    float    peakIn_[2] = { 0, 0 };
};

// =============================================================================================
// HubEngine - lives in the HEARASIDE Hub on the master bus. Each Track sends what viewers hear
// into the DAW, so the master bus (after any plug-ins above the Hub) is the Stream Mix; the Hub
// rebuilds what you hear in your headphones from the Tracks' signals on the bus.

struct HubParams {
    float masterDb    = 0.0f;
    bool  limiterOn   = true;
    float ceilingDb   = -1.0f;
    bool  preview     = false;
    bool  panic       = false;
    int   syncSafety  = 0;      // extra blocks of latency (0..2)
    bool  bypassed    = false;
    bool  silence     = false;  // nothing to the viewers (auto sync is measuring); the master bus is still analysed
};

class HubEngine {
public:
    enum class Role { Disconnected, Owner, Secondary };

    HubEngine();
    ~HubEngine();

    // ---- message thread -----------------------------------------------------------------
    bool connect(const std::string& busName);
    void disconnect();
    // Call periodically (e.g. 2 Hz): claims ownership if free/stale, reclaims dead slots.
    void maintain();
    void prepare(double sampleRate, int maxBlock);
    void setOutputName(int output, const std::string& name);   // 0 = Stream Mix, 1..8 stems

    Role role() const noexcept { return role_.load(std::memory_order_acquire); }
    ssbus::BusLayout* bus() const noexcept { return bus_.load(std::memory_order_acquire); }
    const std::string& busName() const noexcept { return busName_; }
    int latencyFrames() const noexcept;
    // Plug-in latency, measured by finding how late each Track's viewers signal arrives on the
    // master bus. The DAW delays every track to line up with the slowest chain, so with two or
    // more tracks sounding the spread gives each Track's chain latency (kept in
    // SlotHeader::chainLatencyBits, which the Track saves). Live, with one track sounding, the
    // remembered chain latencies give the master plug-ins above the Hub.
    // tracksMs = slowest remembered chain among active tracks. -1 = not measured yet.
    // Any non-audio thread, a few times a second.
    struct FxLatency { double masterMs = -1.0, tracksMs = -1.0; };
    FxLatency measureLatencies();
    // How late (ms) one Track's (or App Audio's, source = true) signal reaches the master bus right
    // now, -1 = not found (silent or not correlated above minScore). anyPolarity also accepts an
    // inverted copy (a microphone). score (optional) = the best correlation found, even when -1.
    // faint: a weak, filtered copy (music leaking from headphones into a mic, through noise
    // suppressors): 1 s window, whitened, also up to 400 ms early (negative result).
    // removeMs: a direct copy of the signal that reaches the master this many ms late (a channel
    // without HEARASIDE Track, an FX return) is subtracted first, so it can't outvote the mic.
    std::optional<double> measureLag(int index, double minScore, bool anyPolarity, bool source = false,
                                     double* score = nullptr, bool faint = false,
                                     std::optional<double> removeMs = std::nullopt);
    // Analysed history so far, in frames at the sample rate (moves with the audio, not the clock).
    uint64_t historyFrames() const noexcept { return uint64_t(capWrite_.load(std::memory_order_acquire)) * 4u; }
    // How the Hub has been reading an App Audio's signal since the last call (auto sync log): how far
    // App Audio's writing was ahead of the Hub's reading, and how often the Hub had to jump to catch up.
    struct SourceReadStats { int64_t leadMin = 0, leadMax = 0; uint32_t jumps = 0, blocks = 0; };
    SourceReadStats takeSourceReadStats(int index) noexcept;
    int lastBlockSize() const noexcept { return lastBlock_.load(std::memory_order_relaxed); }
    uint64_t activeSlotsMask() const noexcept { return activeSlotsMask_.load(std::memory_order_relaxed); }

    // ---- friends room (S2 / S4) ---------------------------------------------------------
    // What the Hub does with each friend (set from the message thread, read on the audio thread).
    enum FriendFlags : uint32_t { kFriendMon = 1u << 0, kFriendStr = 1u << 1, kFriendSolo = 1u << 2 };
    struct FriendControl {
        std::atomic<uint32_t> flags{ kFriendMon | kFriendStr };   // you hear / viewers hear / solo for viewers
        std::atomic<uint32_t> gainBits{ 0 };                      // dB, for you and the viewers
        std::atomic<uint32_t> panBits{ 0 };                       // -1 .. +1, viewers only
    };
    // What the audio thread sees of each friend (read by the UI).
    struct FriendInfo {
        std::atomic<uint32_t> playing{ 0 };       // audio is being played now
        std::atomic<uint32_t> bufferMsBits{ 0 };  // jitter buffer the Hub keeps for this friend (ms)
        std::atomic<uint32_t> totalMsBits{ 0 };   // how late this friend is against the music, all included (ms)
        std::atomic<uint32_t> lineMsBits{ 0 };    // extra delay the Hub gives this friend so they line up (ms)
        std::atomic<uint32_t> overLimit{ 0 };     // slower than the Line-up limit: not counted in D
        std::atomic<uint32_t> underruns{ 0 };     // short gaps the jitter buffer had to hide (counters restart when no packet came for 0.8 s)
        std::atomic<uint32_t> concealedMs{ 0 };   // silence played for them (gaps + restarts), ms
        std::atomic<uint32_t> jitterP95MsBits{ 0 }; // how late packets arrive over the last ~5 s, p95 (ms, 0 = not measured yet)
        std::atomic<uint32_t> leakDetected{ 0 };    // speaker leak detected (friendMix leaked into friend mic)
        std::atomic<uint32_t> echoInMaster{ 0 };    // friend voice present in master bus while route is Direct
    };
    FriendControl& friendControl(int i) noexcept { return friendCtl_[size_t(i)]; }
    const FriendInfo& friendInfo(int i) const noexcept { return friendInfo_[size_t(i)]; }
    // Line up friends for viewers: the Stream Mix waits D = the slowest friend (at most limitMs).
    void setLineUp(bool on, float limitMs) noexcept {
        lineUpLimitBits_.store(ssbus::floatBits(std::max(0.0f, limitMs)), std::memory_order_relaxed);
        lineUpOn_.store(on, std::memory_order_release);
    }
    float lineUpMs() const noexcept { return ssbus::bitsFloat(lineUpMsBits_.load(std::memory_order_relaxed)); }   // D now
    int   lineUpSlowest() const noexcept { return lineUpSlowest_.load(std::memory_order_relaxed); }                // friend index or -1
    // The Line-up delay changed (ms): lets the Hub say so on screen. Counts changes; read lineUpMs() for the value.
    uint32_t lineUpChanges() const noexcept { return lineUpChanges_.load(std::memory_order_relaxed); }

    // Audio tap for recording / metering friends: called on the audio thread during friendsMix.
    using FriendTap = void (*)(void* user, int friendIndex, const float* const* ch, int n, int64_t timeSamples, bool playing) noexcept;
    void setFriendTap(FriendTap tap, void* user) noexcept {
        friendTap_ = tap;
        friendTapUser_ = user;
    }

    // ---- pairing a friend input with the Track at the end of its channel (S7) --------------
    // Message thread. By hand: arg = slot of that Track, or ssbus::kPairAutomatic to let the Hub find it by name /
    // sound again. false = refused (no such friend input, the slot is not an active Track, an App Audio, or it
    // already carries another friend). A manual pair is kept until its Track or the friend input goes away.
    bool pairWith(int feeder, uint32_t arg);
    // Pairs chosen by hand or found by the probe are remembered by the friend input's track name and the Track's uuid,
    // so the project can keep them: pairMemory() is what to save, setPairMemory() gives it back on load, and
    // maintain() pairs again whenever both are there (the DAW loads its plug-ins one by one, or wakes a resting track).
    struct PairMemory { std::string feeder, slotUuid; };
    std::vector<PairMemory> pairMemory() const;
    void setPairMemory(std::vector<PairMemory> memory);

    // Probe: find the Track at the end of a friend input's channel by sound. startProbe (message thread) silences the
    // Stream Mix and what friends hear; probeStep (latency thread, every ~20 ms while probing()) then makes the feeder
    // play the sweep, looks for it in the Tracks that carry no friend, stores the best as a manual pair and answers the
    // feeder (reply 1 = paired, kReplyNoMatch = not found). Over after about half a second. false = refused (not a
    // linked friend input, or a probe is already running).
    bool startProbe(int feeder);
    bool probing() const noexcept { return probeFeeder_.load(std::memory_order_acquire) >= 0; }
    void probeStep();

    // ---- audio thread -------------------------------------------------------------------
    // io in: the master bus = Stream Mix. io out: the headphone mix. Writes Stream Mix + stems.
    void process(float* const* io, int numCh, int n, const HubParams& p,
                 int64_t timeSamples, bool playing, bool offline) noexcept;

private:
    struct SlotState {
        bool     known = false;
        uint32_t epoch = 0;
        uint64_t claimNs = 0;
        bool     needAnchor = true;
        uint64_t cursor = 0;            // FIFO position aligned with the Hub "now"
        uint32_t delay = 0;             // current delay in frames
        // crossfade from an old read position (seek / delay change / re-anchor)
        uint64_t xfFrom = 0;
        uint32_t xfRemain = 0;
        bool     xfActive = false;
        int      mismatch = 0;
        int      aheadBlocks = 0;
        bool     timelineLocked = false;
        ssdsp::Smoother gain, panL, panR;   // stems
        ssdsp::Smoother mon;                // headphones
        float    peak[2] = { 0, 0 };
    };

    void processChunk(float* const* io, int numCh, int n, const HubParams& p,
                      int64_t timeSamples, bool playing) noexcept;
    // hub_friends.cpp: the friends' direct path. Called with the Stream Mix as the DAW made it in
    // outs_ (all used outputs), the headphone mix in phones_.
    void friendsPrepare();
    void friendsReset() noexcept;
    bool friendsSoloActive() const noexcept;
    void friendsMix(int n, const HubParams& p, std::array<bool, ssbus::kNumStreamOuts>& used, bool soloActive,
                    int64_t timeSamples = ssengine::kNoTime, bool playing = false,
                    uint32_t w0 = 0, uint32_t w = 0, int phase = 0) noexcept;
    uint64_t anchorFor(int i, int n, int64_t timeSamples, bool playing, bool& viaTimeline) noexcept;
    void startCrossfade(SlotState& s, uint64_t oldReadPos) noexcept;

    std::atomic<ssbus::BusLayout*> bus_{ nullptr };
    std::unique_ptr<ssbus::SharedMemory> shm_;
    std::unique_ptr<ssbus::BeaconMap> beacon_;   // the Hub's version for plug-ins of another version to see
    std::vector<std::unique_ptr<ssbus::SharedMemory>> retired_;
    std::string busName_;
    uint64_t token_ = 0;
    std::atomic<Role> role_{ Role::Disconnected };
    std::atomic<uint32_t> gen_{ 0 };   // bumped on (re)connect so the audio thread resets slot state
    uint32_t seenGen_ = ~0u;

    double sampleRate_ = 48000;
    int    maxBlock_ = 0;
    std::atomic<int> syncSafety_{ 0 };   // audio thread writes, measureLatencies() / measureLag() read
    uint32_t xfLen_ = 240;

    std::array<SlotState, ssbus::kMaxSlots> slots_;
    std::vector<float> tmpA_[2], tmpB_[2], phones_[2];
    // decimated (sample rate / 4) mono history for measureLatencies(): each Track's viewers signal
    // and the master bus input. Audio thread writes, the measuring thread reads behind capWrite_.
    std::vector<float> slotBlock_;                  // this chunk, kMaxSlots x maxBlock_
    std::vector<float> capSlots_, capIn_;           // kMaxSlots x capLen_, capLen_
    std::array<bool, ssbus::kMaxSlots> slotLive_{};
    std::array<float, ssbus::kMaxSlots> decSlot_{};
    std::array<uint32_t, ssbus::kMaxSlots> quiet_{};   // zero history samples written since the slot went quiet
    // App Audio signals (what each adds to its track), same history layout as the Tracks
    std::vector<float> capSources_;                 // kMaxSources x capLen_
    std::array<uint64_t, ssbus::kMaxSources> srcCursor_{};
    std::array<bool, ssbus::kMaxSources> srcKnown_{};
    std::array<float, ssbus::kMaxSources> decSrc_{};
    std::array<uint32_t, ssbus::kMaxSources> srcQuiet_{};
    struct SrcStats { std::atomic<int64_t> leadMin { INT64_MAX }, leadMax { INT64_MIN }; std::atomic<uint32_t> jumps { 0 }, blocks { 0 }; };
    std::array<SrcStats, ssbus::kMaxSources> srcStats_{};
    // Friend audio signals for echo and leak detection
    std::vector<float> capFriendMix_;               // capLen_
    std::vector<float> capFriends_;                 // kMaxFriends x capLen_
    float decFriendMix_ = 0.0f;
    std::array<float, ssbus::kMaxFriends> decFriends_{};
    std::array<uint32_t, ssbus::kMaxFriends> friendQuiet_{};
    uint32_t capLen_ = 0;
    std::atomic<uint32_t> capWrite_{ 0 };
    float decIn_ = 0;
    int decCount_ = 0;
    std::atomic<int> lastBlock_{ 256 };   // likewise
    std::mutex measureMutex_;                       // prepare() vs measureLatencies()
    // Pairs chosen by hand (or found by the probe), per friend: slot + that slot's claim + friend id in one word, so the
    // message thread and the measuring thread never see half of one. 0 = none. manualLi_ = Li in 0.01 ms + 1, 0 = not measured.
    std::array<std::atomic<uint64_t>, ssbus::kMaxFriends> manualPair_{};
    std::array<std::atomic<uint32_t>, ssbus::kMaxFriends> manualLi_{};
    std::atomic<int> probeFeeder_{ -1 };   // the feeder being probed, -1 = none; the audio thread silences the viewers and friends while >= 0
    std::atomic<int> probeStage_{ 0 };     // 0 = starting, 1 = silenced and waiting, 2 = sweeping, 3 = answer known, waiting for the sweep to leave (probeStep only)
    int probeBest_ = -1;                   // stage 3: the Track found (-1 = none), its Li, and when to stop being silent
    double probeLi_ = 0.0;
    uint32_t probeHoldTo_ = 0;
    uint64_t probeWord_ = 0;               // stage 3: the pair word of that Track when it was found
    std::atomic<float> slotToMasterMs_{ 200.0f };   // the longest a Track's sound took to reach the master at the last measuring (200 = not measured yet)
    uint32_t probeFrom_ = 0;               // capWrite_ at the start of the stage
    uint64_t probeWallNs_ = 0;
    void endProbe(int feeder, uint32_t reply);
    mutable std::mutex memoryMutex_;       // message thread (save, load, maintain) vs the measuring thread (probe)
    std::vector<PairMemory> memory_;
    void remember(int feeder, int slot);
    void applyMemory(ssbus::BusLayout& b);

    std::vector<float> outs_[ssbus::kNumStreamOuts][2];
    std::array<bool, ssbus::kNumStreamOuts> outUsed_{};
    std::array<ssdsp::Limiter, ssbus::kNumStreamOuts> limiters_;
    ssdsp::LoudnessMeter loudness_;
    ssdsp::Smoother master_, preview_, friendMixGain_;   // friendMixGain_: what friends hear fades out and in around a probe
    float outPeak_[ssbus::kNumStreamOuts][2]{};

    // ---- friends (hub_friends.cpp) ----
    struct FriendPlayer;
    struct DelayLine {
        std::vector<float> buf[2];
        uint32_t pos = 0;
        void assign(uint32_t cap) { for (auto& b : buf) b.assign(cap, 0.0f); pos = 0; }
    };
    std::array<FriendControl, ssbus::kMaxFriends> friendCtl_;
    std::array<FriendInfo, ssbus::kMaxFriends> friendInfo_;
    std::array<std::unique_ptr<FriendPlayer>, ssbus::kMaxFriends> friends_;
    std::array<DelayLine, ssbus::kNumStreamOuts> outDelay_;      // the Stream Mix and the stems, delayed by D
    std::atomic<bool> lineUpOn_{ false };
    std::atomic<uint32_t> lineUpLimitBits_{ ssbus::floatBits(600.0f) };
    std::atomic<uint32_t> lineUpMsBits_{ 0 };
    std::atomic<int> lineUpSlowest_{ -1 };
    std::atomic<uint32_t> lineUpChanges_{ 0 };
    uint32_t lineD_ = 0, lineOldD_ = 0;                          // delay (frames) now / before the crossfade
    uint32_t lineXfRemain_ = 0, lineHold_ = 0;
    std::array<uint32_t, ssbus::kMaxFriends> usedTot_{}, oldTot_{};   // each friend's own delay (frames) D was set against
    // the viewers' side of Tracks that carry a friend (kFlagViewersViaHub), per output: added after the
    // Line-up delay and kept out of the mix friends hear, so no friend ever hears their own voice back
    std::vector<float> late_[ssbus::kNumStreamOuts][2];
    bool lateAny_ = false;
    std::vector<float> voice_[ssbus::kMaxFriends];   // each friend's voice for the other friends (mono, undelayed)
    std::vector<float> delayScratch_[2];
    uint32_t delayCap_ = 0;
    FriendTap friendTap_ = nullptr;
    void* friendTapUser_ = nullptr;

    bool    wasPlaying_ = false;
    int64_t expectedTime_ = kNoTime;
    uint64_t blockCounter_ = 0;
    std::atomic<uint64_t> activeSlotsMask_{ 0 };
    uint32_t scanCountdown_ = 0;
    bool    wasOwner_ = false;
    bool    wasOffline_ = false;
};

} // namespace ssengine

#endif // HEARASIDE_USE_RUST_CORE
