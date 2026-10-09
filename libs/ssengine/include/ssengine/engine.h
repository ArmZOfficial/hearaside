// HEARASIDE engines: the real-time core of the Track and Hub plug-ins, free of JUCE so it
// can be unit tested and simulated with arbitrary host behaviour.
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/bus.h"
#include "ssdsp/dsp.h"

#include <optional>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ssengine {

constexpr int64_t kNoTime = INT64_MIN;

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
    std::optional<double> measureLag(int index, double minScore, bool anyPolarity, bool source = false,
                                     double* score = nullptr, bool faint = false);
    // Analysed history so far, in frames at the sample rate (moves with the audio, not the clock).
    uint64_t historyFrames() const noexcept { return uint64_t(capWrite_.load(std::memory_order_acquire)) * 4u; }

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
    uint64_t anchorFor(int i, int n, int64_t timeSamples, bool playing, bool& viaTimeline) noexcept;
    void startCrossfade(SlotState& s, uint64_t oldReadPos) noexcept;

    std::atomic<ssbus::BusLayout*> bus_{ nullptr };
    std::unique_ptr<ssbus::SharedMemory> shm_;
    std::vector<std::unique_ptr<ssbus::SharedMemory>> retired_;
    std::string busName_;
    uint64_t token_ = 0;
    std::atomic<Role> role_{ Role::Disconnected };
    std::atomic<uint32_t> gen_{ 0 };   // bumped on (re)connect so the audio thread resets slot state
    uint32_t seenGen_ = ~0u;

    double sampleRate_ = 48000;
    int    maxBlock_ = 0;
    int    syncSafety_ = 0;
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
    uint32_t capLen_ = 0;
    std::atomic<uint32_t> capWrite_{ 0 };
    float decIn_ = 0;
    int decCount_ = 0;
    int lastBlock_ = 256;
    std::mutex measureMutex_;                       // prepare() vs measureLatencies()

    std::vector<float> outs_[ssbus::kNumStreamOuts][2];
    std::array<bool, ssbus::kNumStreamOuts> outUsed_{};
    std::array<ssdsp::Limiter, ssbus::kNumStreamOuts> limiters_;
    ssdsp::LoudnessMeter loudness_;
    ssdsp::Smoother master_, preview_;
    float outPeak_[ssbus::kNumStreamOuts][2]{};

    bool    wasPlaying_ = false;
    int64_t expectedTime_ = kNoTime;
    uint64_t blockCounter_ = 0;
    bool    wasOwner_ = false;
    bool    wasOffline_ = false;
};

} // namespace ssengine
