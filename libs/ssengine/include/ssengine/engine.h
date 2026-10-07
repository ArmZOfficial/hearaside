// HEARASIDE engines: the real-time core of the Track and Hub plug-ins, free of JUCE so it
// can be unit tested and simulated with arbitrary host behaviour.
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/bus.h"
#include "ssdsp/dsp.h"

#include <array>
#include <atomic>
#include <memory>
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

    // ---- any thread ---------------------------------------------------------------------
    struct Mirror { bool mon, str, solo; float gainDb, pan, delayMs, trimDb; int stem; };
    void mirror(const Mirror& m) noexcept;

    // ---- audio thread -------------------------------------------------------------------
    // Publishes the post-FX block. timeSamples = kNoTime when the host gives no playhead.
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
// HubEngine - lives in the HEARASIDE Hub on the master bus.

struct HubParams {
    float masterDb    = 0.0f;
    bool  limiterOn   = true;
    float ceilingDb   = -1.0f;
    bool  preview     = false;
    bool  panic       = false;
    int   syncSafety  = 0;      // extra blocks of latency (0..2)
    bool  bypassed    = false;
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

    // ---- audio thread -------------------------------------------------------------------
    // io: the master bus block (in place). Writes Stream Mix + stems to shared memory.
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
        ssdsp::Smoother gain, panL, panR;
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
    std::vector<float> tmpA_[2], tmpB_[2];
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
