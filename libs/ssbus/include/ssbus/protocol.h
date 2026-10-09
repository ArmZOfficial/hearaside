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
constexpr uint32_t kProtocolVersion = 13;   // v13: friends room (8 friend rings) + friend input feeders (a Track mixes a friend in the DAW); v2: remote-control queue; v4: no scenes / Hub state mirror; v6: stream = master bus; v7: chain latency; v8: App Audio sources; v9: Hub mute for auto sync; v10: App Audio signal + delay for auto sync; v11: link input; v12: App Audio track slot (you hear / viewers hear)
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
constexpr int      kRemoteQueueSize = 64;              // remote commands (OBS hotkeys -> Hub)
constexpr int      kMaxSources      = 16;              // HEARASIDE App Audio instances per bus
constexpr int      kMaxFriends      = 8;               // friends in the room (each has its own send-in link)
constexpr int      kMaxFeeders      = 16;              // HEARASIDE Track instances in "friend input" mode
constexpr int      kRequestQueueSize = 16;             // plug-in -> Hub requests
constexpr int      kTapFrames       = 8192;            // feeder correlation tap: mono, decimated by kTapDecimation
constexpr int      kTapDecimation   = 4;

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

// Remote control (OBS hotkeys, any consumer) -> Hub. target = -1 addresses the Hub itself
// (paramId is a RemoteParam), target = 0..63 a Track slot (paramId is a ParamId; the Hub forwards
// it through that Track's mailbox so the Track's host still sees the change).
enum class RemoteParam : uint32_t {
    Panic = 1, Preview,
    AutoSync,   // target = microphone slot, value = reference (music) slot: measure and line them up
    Share,      // value 1 / 0: share links on / off
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

    char                  pad[4096 - 72 - 8 - kRemoteQueueSize * 16];
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
    kFlagApp      = 1u << 6,   // the slot of an App Audio (the Hub lists it with the program audio, not the tracks)
    kFlagViewersViaHub = 1u << 7,   // Line up is on and this Track carries a friend: it sends the viewers' side to the Hub
                                    // (which delays it) instead of into the DAW, so the friend lines up with the music
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
    std::atomic<uint32_t> hubMute;          // 1 = the Hub is measuring: send nothing to the DAW for a moment
                                            // (the Track still publishes; ignored when the Hub is gone)
    std::atomic<uint32_t> chainLatencyBits; // ms (float bits): latency of the plug-ins before this Track. The Hub
                                            // measures it, the Track keeps it in its state for live use

    // v13: a friend mixed through the DAW (written by the Hub when it pairs a feeder with this Track)
    std::atomic<uint32_t> fedBy;            // FriendHeader::id of the friend whose voice reaches this Track, 0 = a normal track
    std::atomic<uint32_t> fxLatencyBits;    // ms (float bits): what the plug-ins between the feeder and this Track add

    // timeline tags
    std::atomic<uint64_t> tagWrite;         // number of tags written

    alignas(64) std::atomic<uint64_t> writePos;  // monotonic frame counter of the audio FIFO
    char                  pad[48];
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
// App sources (HEARASIDE App Audio). Their audio goes through the DAW track like any other
// signal; only status and remote control live here so the Hub can list and switch them.

enum SourceFlags : uint32_t {
    kSrcOn        = 1u << 0,   // capture switched on
    kSrcRecording = 1u << 1,   // printing a take to a file
    kSrcFollowRec = 1u << 2,   // records whenever the DAW records
    kSrcSystem    = 1u << 3,   // captures the whole computer except the DAW
    kSrcDropped   = 1u << 4,   // the take lost audio (disk too slow)
};

enum class SourceParam : uint32_t { On = 1, LevelDb, Record, FollowRecord, DelayMs };

struct alignas(64) SourceHeader {
    std::atomic<uint32_t> state;            // SlotState
    std::atomic<uint32_t> ownerPid;
    std::atomic<uint32_t> sequence;         // copy of BusHeader::slotSequence at claim time
    std::atomic<uint32_t> nameSeq;          // seqlock over name / app / colour (odd = writing)
    char                  name[kNameBytes]; // track name from the host
    char                  app[kNameBytes];  // "chrome.exe", "" = none
    uint32_t              colorARGB;
    std::atomic<uint32_t> flags;            // SourceFlags
    std::atomic<uint64_t> heartbeatNs;      // last processBlock()
    std::atomic<uint32_t> capture;          // capture state (0 idle, 1 starting, 2 running, 3 program closed, 4 failed)
    std::atomic<uint32_t> levelBits;        // dB
    std::atomic<uint32_t> peakBits;         // linear
    std::atomic<uint32_t> latencyBits;      // ms from the program to the DAW output
    std::atomic<uint32_t> recordMs;         // length of the take being recorded (or the last one)
    std::atomic<uint32_t> delayBits;        // ms: delay of the program audio (set by auto sync)
    std::atomic<uint32_t> hubMute;          // 1 = the Hub is measuring: add nothing to the track for a moment
    std::atomic<int32_t>  trackSlot;        // slot (kFlagApp) where it publishes the program for the headphones, -1 = none

    // Hub -> App Audio mailbox (single writer: Hub message thread), SourceParam ids
    std::atomic<uint32_t> cmdWrite;
    Command               cmds[kMailboxSize];

    // Hub -> App Audio program choice (seqlock like SlotHeader::renameSeq)
    std::atomic<uint32_t> appSeq;
    char                  appTo[kNameBytes];

    // what this App Audio adds to its track, before its own delay (the Hub's auto sync reference)
    alignas(64) std::atomic<uint64_t> writePos;
};

// ---------------------------------------------------------------------------------------------
// Link input: audio someone sends in through the Hub's "send to me" link (browser microphone).
// The Hub's share server writes it, App Audio set to "*link*" plays it on its track.

struct alignas(64) LinkInHeader {
    std::atomic<uint32_t> sampleRate;       // of the sender (the browser's audio rate)
    std::atomic<uint32_t> active;           // 1 while a sender is connected
    std::atomic<uint64_t> heartbeatNs;      // last audio received
    alignas(64) std::atomic<uint64_t> writePos;
};

// ---------------------------------------------------------------------------------------------
// Friends room (v13). Up to kMaxFriends browsers send their microphone to the Hub, one link each.
// The Hub's share server writes friendAudio[i]; the Hub engine, HEARASIDE App Audio and the
// HEARASIDE Track in "friend input" mode read it (every reader keeps its own cursor).

enum FriendState : uint32_t { kFriendFree = 0, kFriendWaiting = 1, kFriendLive = 2, kFriendOffline = 3 };
// Who plays the friend for the viewers. Only the Hub decides, so nobody hears the same voice twice.
enum FriendRoute : uint32_t { kRouteDirect = 0, kRouteDaw = 1 };

struct alignas(64) FriendHeader {
    std::atomic<uint32_t> state;            // FriendState
    std::atomic<uint32_t> id;               // never reused while the Hub lives (0 = free); kept in the Hub's state
    std::atomic<uint32_t> nameSeq;          // seqlock over name (odd = writing)
    char                  name[kNameBytes];
    std::atomic<uint32_t> sampleRate;       // of the friend's browser
    std::atomic<uint64_t> heartbeatNs;      // last packet
    std::atomic<uint32_t> delayBits;        // ms (float bits): how late this friend arrives (measured)
    std::atomic<uint32_t> route;            // FriendRoute
    std::atomic<int32_t>  feeder;           // feeders[] index that feeds this friend into a DAW track, -1 = none
    std::atomic<int32_t>  outSlot;          // slot of the Track at the end of that channel, -1 = not paired
    std::atomic<uint32_t> peakBits;         // linear peak of the microphone
    alignas(64) std::atomic<uint64_t> writePos;   // frames written to friendAudio[i]
};

// A HEARASIDE Track set to "friend input": it plays one friend's voice on its DAW channel so the
// plug-ins after it (EQ, compressor, reverb) work on that voice. The friend is chosen by id: the link
// itself (token) never leaves the Hub.
enum FeederStatus : uint32_t { kFeederFlowing = 0, kFeederBypassed = 1, kFeederOffline = 2, kFeederNoFriend = 3 };

struct alignas(64) FeederRecord {
    std::atomic<uint32_t> state;            // SlotState
    std::atomic<uint32_t> ownerPid;
    std::atomic<uint32_t> friendId;         // chosen friend, 0 = none yet
    std::atomic<uint32_t> nameSeq;          // seqlock over trackName / colorARGB
    char                  trackName[kNameBytes];
    uint32_t              colorARGB;
    std::atomic<uint64_t> heartbeatNs;      // last processBlock()
    std::atomic<uint64_t> blockCount;       // processed blocks (the Hub sees a DAW that rests the track)
    std::atomic<uint32_t> status;           // FeederStatus
    std::atomic<uint32_t> reply;            // answer to the last request (see kReply*)
    std::atomic<uint32_t> replySeq;
    std::atomic<uint32_t> hubCommand;       // Hub -> feeder: 1 = go back to this track's own sound ("Bring back to the Hub")
    std::atomic<uint32_t> hubCommandSeq;
    std::atomic<uint32_t> sampleRate;       // of the DAW
    alignas(64) std::atomic<uint64_t> tapWrite;   // decimated frames written to feederTap[i]
};

enum RequestKind : uint32_t {
    kReqResolveToken = 1,   // text = a send-in link token pasted into the Track; reply = friend id
    kReqCreateFriend = 2,   // text = name for the new friend; reply = friend id
    kReqRelease      = 3,   // this feeder lets go of its friend (the Hub plays the friend directly again)
    kReqCopyLink     = 4,   // arg = friend id: the Hub puts that friend's send-in link on the clipboard
};
constexpr uint32_t kReplyNotInRoom = 0xFFFF0001u;   // the link belongs to another room / is unknown
constexpr uint32_t kReplyRoomFull  = 0xFFFF0002u;
constexpr uint32_t kReplyFailed    = 0xFFFF0003u;

// Multi-writer queue (like RemoteCommand): writers claim an index with fetch_add on requestReserve,
// fill the entry and publish it with seq = index + 1. The Hub (single reader) consumes in order and
// wipes `text` the moment it has read it (a pasted token must not stay in shared memory).
struct PluginRequest {
    std::atomic<uint32_t> seq;
    std::atomic<int32_t>  feeder;           // who asks; the answer goes to feeders[feeder].reply
    std::atomic<uint32_t> kind;             // RequestKind
    std::atomic<uint32_t> arg;
    char                  text[kNameBytes];
};

// A tiny mono copy (decimated by kTapDecimation) of what a feeder puts into the DAW. The Hub
// correlates it with the Track histories it already keeps to find the Track at the end of the channel.
struct FeederTap {
    float mono[kTapFrames];
};

// Fixed forever: a separate 64-byte segment (no version in its name) so a Track can tell
// "there is a Hub, but it is another version" instead of just "no Hub".
constexpr uint32_t kBeaconMagic = 0x48425143;   // 'HBQC'
struct alignas(64) BeaconHeader {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> protocolVersion;  // of the Hub that owns this bus
    std::atomic<uint32_t> hubPid;
    std::atomic<uint64_t> heartbeatNs;
};
static_assert(sizeof(BeaconHeader) == 64, "BeaconHeader is part of every version's contract");

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
    SourceHeader    sources[kMaxSources];
    alignas(64) ChannelRing slotAudio[kMaxSlots];
    alignas(64) ChannelRing sourceAudio[kMaxSources];
    LinkInHeader    linkIn;
    alignas(64) ChannelRing linkInAudio;
    alignas(64) ChannelRing streamAudio[kNumStreamOuts];

    // ---- v13 ---------------------------------------------------------------------------------
    alignas(64) FriendHeader friends[kMaxFriends];
    FeederRecord    feeders[kMaxFeeders];
    PluginRequest   requests[kRequestQueueSize];
    alignas(64) std::atomic<uint32_t> requestReserve;   // number of requests ever claimed
    alignas(64) FeederTap feederTap[kMaxFeeders];
    alignas(64) ChannelRing friendAudio[kMaxFriends];
    // What friends hear: the Stream Mix as the DAW makes it, NOT delayed by Line up (a delayed copy would
    // make every friend sing later and later). The Hub writes it; friendMixWrite counts its frames and is
    // the timeline friends report back against (S3).
    alignas(64) std::atomic<uint64_t> friendMixWrite;
    alignas(64) ChannelRing friendMixAudio;
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
