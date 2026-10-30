// HEARASIDE engines backed by the Rust core (HEARASIDE_USE_RUST_CORE, PLAN-rust-migration R2.5): the same
// public API as engine.h, implemented over rust/crates/hs-ffi (see rust_core.cpp). Removed in R9.
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/bus.h"
#include "ssdsp/consumer.h"
#include "ssdsp/dsp.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct HsTrack;
struct HsHub;

namespace ssengine {

constexpr int64_t kNoTime = INT64_MIN;

std::vector<float> probeSweep(double sampleRate);

class TrackPublisher {
public:
    enum class Status { Disconnected, Connected, SlotsFull, ShmError };

    TrackPublisher();
    ~TrackPublisher();
    TrackPublisher(const TrackPublisher&) = delete;
    TrackPublisher& operator=(const TrackPublisher&) = delete;

    Status connect(const std::string& busName, std::string uuid, uint32_t sampleRate, uint32_t numChannels);
    void   disconnect();
    void   prepare(uint32_t sampleRate, uint32_t numChannels);
    void   setIdentity(const std::string& name, uint32_t colorARGB);

    Status status() const noexcept;
    const std::string& uuid() const noexcept { return uuid_; }
    const std::string& busName() const noexcept { return busName_; }
    int    slotIndex() const noexcept;
    ssbus::BusLayout*  bus() const noexcept;
    ssbus::SlotHeader* slot() const noexcept;
    bool   hubPresent() const noexcept;
    bool   soloActive() const noexcept;

    struct Mirror { bool mon, str, solo; float gainDb, pan, delayMs, trimDb; int stem; bool app = false; };
    void mirror(const Mirror& m) noexcept;

    void process(const float* const* ch, int numCh, int n, int64_t timeSamples, bool playing,
                 bool offline, bool bypassed) noexcept;

private:
    HsTrack* h_;
    std::string uuid_, busName_;
};

struct HubParams {
    float masterDb    = 0.0f;
    bool  limiterOn   = true;
    float ceilingDb   = -1.0f;
    bool  preview     = false;
    bool  panic       = false;
    int   syncSafety  = 0;
    bool  bypassed    = false;
    bool  silence     = false;
};

class HubEngine {
public:
    enum class Role { Disconnected, Owner, Secondary };

    HubEngine();
    ~HubEngine();
    HubEngine(const HubEngine&) = delete;
    HubEngine& operator=(const HubEngine&) = delete;

    bool connect(const std::string& busName);
    void disconnect();
    void maintain();
    void prepare(double sampleRate, int maxBlock);
    void setOutputName(int output, const std::string& name);

    Role role() const noexcept;
    ssbus::BusLayout* bus() const noexcept;
    const std::string& busName() const noexcept { return busName_; }
    int latencyFrames() const noexcept;
    struct FxLatency { double masterMs = -1.0, tracksMs = -1.0; };
    FxLatency measureLatencies();
    std::optional<double> measureLag(int index, double minScore, bool anyPolarity, bool source = false,
                                     double* score = nullptr, bool faint = false,
                                     std::optional<double> removeMs = std::nullopt);
    uint64_t historyFrames() const noexcept;
    struct SourceReadStats { int64_t leadMin = 0, leadMax = 0; uint32_t jumps = 0, blocks = 0; };
    SourceReadStats takeSourceReadStats(int index) noexcept;
    int lastBlockSize() const noexcept;
    uint64_t activeSlotsMask() const noexcept;

    // ---- friends room: the same C layout as the Rust structs (hs_engine::measure) ---------------
    enum FriendFlags : uint32_t { kFriendMon = 1u << 0, kFriendStr = 1u << 1, kFriendSolo = 1u << 2 };
    struct FriendControl {
        std::atomic<uint32_t> flags;
        std::atomic<uint32_t> gainBits;
        std::atomic<uint32_t> panBits;
    };
    struct FriendInfo {
        std::atomic<uint32_t> playing;
        std::atomic<uint32_t> bufferMsBits;
        std::atomic<uint32_t> totalMsBits;
        std::atomic<uint32_t> lineMsBits;
        std::atomic<uint32_t> overLimit;
        std::atomic<uint32_t> underruns;
        std::atomic<uint32_t> concealedMs;
        std::atomic<uint32_t> jitterP95MsBits;
        std::atomic<uint32_t> leakDetected;
        std::atomic<uint32_t> echoInMaster;
    };
    FriendControl& friendControl(int i) noexcept;
    const FriendInfo& friendInfo(int i) const noexcept;
    void setLineUp(bool on, float limitMs) noexcept;
    float lineUpMs() const noexcept;
    int   lineUpSlowest() const noexcept;
    uint32_t lineUpChanges() const noexcept;

    using FriendTap = void (*)(void* user, int friendIndex, const float* const* ch, int n, int64_t timeSamples, bool playing) noexcept;
    void setFriendTap(FriendTap tap, void* user) noexcept;

    bool pairWith(int feeder, uint32_t arg);
    struct PairMemory { std::string feeder, slotUuid; };
    std::vector<PairMemory> pairMemory() const;
    void setPairMemory(std::vector<PairMemory> memory);
    bool startProbe(int feeder);
    bool probing() const noexcept;
    void probeStep();

    void process(float* const* io, int numCh, int n, const HubParams& p,
                 int64_t timeSamples, bool playing, bool offline) noexcept;

private:
    HsHub* h_;
    std::string busName_;
    FriendTap tap_ = nullptr;
    void* tapUser_ = nullptr;
};

} // namespace ssengine
