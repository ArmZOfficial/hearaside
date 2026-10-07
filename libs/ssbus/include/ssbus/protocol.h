// HEARASIDE shared-memory protocol (v1).
// SPDX-License-Identifier: MIT
//
// Every struct in this file lives inside a shared-memory segment that is mapped by
// several processes at once (DAW plug-ins, OBS, Bridge, bus-inspector). Rules:
//   * standard layout only, no pointers, no virtuals, fixed-size arrays
//   * every field that is written by one party while another reads it is a lock-free atomic
//   * changing anything here => bump kProtocolVersion (it is part of the segment name)
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)   // padding from alignas(64) is intended (cache-line separation)
#endif

namespace ssbus {

constexpr uint32_t kMagic           = 0x53535031; // 'SSP1'
constexpr uint32_t kProtocolVersion = 2;   // v2: remote-control queue + Hub state mirror in BusHeader
constexpr int      kMaxSlots        = 64;
constexpr int      kMaxStems        = 8;
constexpr int      kNumStreamOuts   = 1 + kMaxStems;   // [0] = Stream Mix, [1..8] = stems
constexpr int      kMaxChannels     = 2;
constexpr uint32_t kRingFrames      = 65536;           // power of two, ~1.36 s @ 48 kHz
constexpr uint32_t kRingMask        = kRingFrames - 1;
constexpr uint32_t kGuardFrames     = 4096;            // never read closer than this to the write head - ring size
constexpr int      kMailboxSize     = 32;
constexpr int      kTagRingSize     = 512;             // timeline tags per slot (one per processed block)
constexpr int      kNameBytes       = 64;              // UTF-8, NUL terminated
constexpr int      kUuidBytes       = 40;
constexpr int      kRemoteQueueSize = 64;              // remote commands (OBS dock / hotkeys -> Hub)
constexpr int      kMaxScenes       = 8;

static_assert((kRingFrames & kRingMask) == 0, "kRingFrames must be a power of two");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "need lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");
static_assert(std::atomic<int64_t>::is_always_lock_free, "need lock-free 64-bit atomics");

// ---------------------------------------------------------------------------------------------
// Bus header

enum HubFlags : uint32_t {
    kHubBypassed = 1u << 0,   // Hub instance exists but host bypassed it
    kHubPreview  = 1u << 1,   // "ฟังแบบคนดู" active
    kHubPanic    = 1u << 2,   // "ตัดเสียงคนดู" active
    kHubOffline  = 1u << 3,   // host is rendering offline (export/bounce)
    kHubLimiter  = 1u << 4,
};

// Remote control (OBS dock, hotkeys, any consumer) -> Hub. target = -1 addresses the Hub itself
// (paramId is a RemoteParam), target = 0..63 a Track slot (paramId is a ParamId; the Hub forwards
// it through that Track's mailbox so the Track's host still sees the change).
enum class RemoteParam : uint32_t {
    Panic = 1, Preview, MasterDb, LimiterOn, HeadphonesDb, RecallScene, SyncSafety,
};

// Multi-writer entry: writers claim an index with fetch_add on remoteReserve, fill the entry and
// publish it by storing seq = index + 1 (release). The Hub (single reader) consumes in order.
struct RemoteCommand {
    std::atomic<uint32_t> seq;
    std::atomic<int32_t>  target;
    std::atomic<uint32_t> paramId;
    std::atomic<uint32_t> valueBits;
};
static_assert(sizeof(RemoteCommand) == 16, "RemoteCommand size");

struct alignas(64) BusHeader {
    std::atomic<uint32_t> magic;            // written last during init (release)
    uint32_t              version;
    uint32_t              layoutSize;       // sizeof(BusLayout) of the creator
    uint32_t              reserved0;

    std::atomic<uint64_t> hubOwnerToken;    // random token of the Hub instance that owns the bus (0 = none)
    std::atomic<uint32_t> hubOwnerPid;
    std::atomic<uint32_t> hubSampleRate;
    std::atomic<uint64_t> hubHeartbeatNs;   // monotonic ns (see ssbus::nowNs)
    std::atomic<uint32_t> hubFlags;
    std::atomic<uint32_t> hubBlockSize;
    std::atomic<uint32_t> hubLatencyFrames; // extra latency inside the Hub (sync safety + limiter lookahead)
    std::atomic<uint32_t> slotSequence;     // monotonically increasing claim counter (ordering hint)

    // Consumers (OBS sources, Bridge) report themselves so the Hub can show "OBS เชื่อมแล้ว".
    std::atomic<uint64_t> consumerHeartbeatNs;
    std::atomic<uint32_t> consumerCount;    // best-effort, refreshed by heartbeat
    std::atomic<uint32_t> consumerBufferMs; // buffer target of the most recent consumer

    // ---- v2: remote control queue -------------------------------------------------------------
    std::atomic<uint32_t> remoteReserve;    // number of commands ever claimed
    uint32_t              reserved1;
    RemoteCommand         remote[kRemoteQueueSize];

    // ---- v2: Hub state mirror for remote UIs (Hub message thread writes, seqlock: odd = writing)
    std::atomic<uint32_t> hubStateSeq;
    std::atomic<uint32_t> hubMasterBits;     // dB
    std::atomic<uint32_t> hubHeadphonesBits; // dB
    std::atomic<uint32_t> hubCeilingBits;    // dBFS
    std::atomic<int32_t>  hubActiveScene;    // -1 = custom
    std::atomic<uint32_t> hubSceneMask;      // bit i = scene i is shown
    std::atomic<uint32_t> hubSyncSafety;
    uint32_t              reserved2;
    char                  sceneNames[kMaxScenes][kNameBytes];   // UTF-8

    char                  pad[4096 - 72 - 8 - kRemoteQueueSize * 16 - 32 - kMaxScenes * kNameBytes];
};
static_assert(sizeof(BusHeader) == 4096, "BusHeader must stay 4 KB");

// ---------------------------------------------------------------------------------------------
// Per-track slot

enum SlotState : uint32_t { kSlotFree = 0, kSlotClaiming = 1, kSlotActive = 2 };

enum SlotFlags : uint32_t {
    kFlagMon      = 1u << 0,
    kFlagStr      = 1u << 1,
    kFlagSolo     = 1u << 2,
    kFlagBypassed = 1u << 3,   // host bypass active (monitor passes audio through!)
    kFlagOffline  = 1u << 4,   // host rendering offline
    kFlagMono     = 1u << 5,   // track is mono (written as dual mono)
};

enum class ParamId : uint32_t {
    Mon = 1, Str, StrGainDb, StrPan, StrDelayMs, MonTrimDb, StrSolo, StemIndex,
};

struct Command {
    uint32_t paramId;
    float    value;
};

// One entry per processed block: where this block landed in the FIFO and where it sits on
// the host timeline. Written with a per-entry sequence number (seqlock style).
struct TimelineTag {
    std::atomic<uint64_t> seq;          // block counter + 1 when entry is complete; 0 while writing
    std::atomic<uint64_t> fifoPos;      // writePos at the start of the block
    std::atomic<int64_t>  timeSamples;  // host playhead in samples at the start of the block
    std::atomic<uint32_t> numFrames;
    std::atomic<uint32_t> playing;      // 1 if transport was playing
};
static_assert(sizeof(TimelineTag) == 32, "TimelineTag size");

struct alignas(64) SlotHeader {
    std::atomic<uint32_t> state;
    std::atomic<uint32_t> epoch;            // ++ on reset / prepare / sample-rate change
    std::atomic<uint32_t> ownerPid;
    std::atomic<uint32_t> sequence;         // copy of BusHeader::slotSequence at claim time

    // identity (message thread only, guarded by nameSeq seqlock: odd = writing)
    std::atomic<uint32_t> nameSeq;
    uint32_t              colorARGB;        // 0 = host did not provide a colour
    char                  uuid[kUuidBytes];
    char                  name[kNameBytes];

    std::atomic<uint32_t> sampleRate;
    std::atomic<uint32_t> numChannels;
    std::atomic<uint64_t> heartbeatNs;      // last processBlock()
    std::atomic<uint64_t> claimNs;

    // parameter mirror (Track writes, Hub reads) - floats as bit patterns
    std::atomic<uint32_t> flags;
    std::atomic<uint32_t> strGainBits;      // dB
    std::atomic<uint32_t> strPanBits;       // -1 .. +1
    std::atomic<uint32_t> strDelayBits;     // ms
    std::atomic<uint32_t> monTrimBits;      // dB
    std::atomic<int32_t>  stemIndex;        // -1 = none, 0..7

    // Hub -> Track command mailbox (single writer: Hub message thread)
    std::atomic<uint32_t> cmdWrite;
    Command               cmds[kMailboxSize];

    // Hub -> Track rename request (seqlock, single writer)
    std::atomic<uint32_t> renameSeq;        // even = stable; increments by 2 per request
    char                  renameTo[kNameBytes];

    // meters (linear peak, bit patterns). Track writes monitor/input, Hub writes stream.
    std::atomic<uint32_t> peakInBits[kMaxChannels];
    std::atomic<uint32_t> peakStreamBits[kMaxChannels];

    // Hub -> Track status feedback
    std::atomic<uint32_t> hubStatus;        // see SlotHubStatus
    std::atomic<uint32_t> hubLeadFrames;    // measured lead of this slot vs the Hub cursor

    // timeline tags
    std::atomic<uint64_t> tagWrite;         // number of tags written

    alignas(64) std::atomic<uint64_t> writePos;  // monotonic frame counter of the audio FIFO
    char                  pad[56];
};

enum SlotHubStatus : uint32_t {
    kHubStatusNone           = 0,
    kHubStatusOk             = 1u << 0,
    kHubStatusRateMismatch   = 1u << 1,
    kHubStatusAhead          = 1u << 2,
    kHubStatusUnderrun       = 1u << 3,
};

struct SlotTags {
    TimelineTag tags[kTagRingSize];
};

// ---------------------------------------------------------------------------------------------
// Stream outputs (written by the Hub)

struct alignas(64) StreamOutHeader {
    std::atomic<uint32_t> nameSeq;
    std::atomic<uint32_t> sampleRate;
    std::atomic<uint32_t> active;               // 1 when the Hub writes this output
    uint32_t              reserved;
    char                  names[kNumStreamOuts][kNameBytes];
    std::atomic<uint32_t> peakBits[kNumStreamOuts][kMaxChannels];
    std::atomic<uint32_t> loudnessMBits;        // LUFS momentary of Stream Mix
    std::atomic<uint32_t> loudnessSBits;        // LUFS short-term of Stream Mix
    alignas(64) std::atomic<uint64_t> writePos[kNumStreamOuts];
};

// ---------------------------------------------------------------------------------------------
// Whole segment

struct ChannelRing {
    float ch[kMaxChannels][kRingFrames];
};

struct BusLayout {
    BusHeader       header;
    SlotHeader      slots[kMaxSlots];
    SlotTags        slotTags[kMaxSlots];
    StreamOutHeader streamHeader;
    alignas(64) ChannelRing slotAudio[kMaxSlots];
    alignas(64) ChannelRing streamAudio[kNumStreamOuts];
};

static_assert(std::is_standard_layout_v<BusLayout>, "BusLayout must be standard layout");

constexpr size_t kLayoutSize = sizeof(BusLayout);

// helpers ---------------------------------------------------------------------------------------
inline uint32_t floatBits(float f) noexcept { uint32_t u; static_assert(sizeof u == sizeof f); std::memcpy(&u, &f, 4); return u; }
inline float    bitsFloat(uint32_t u) noexcept { float f; std::memcpy(&f, &u, 4); return f; }

} // namespace ssbus

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
