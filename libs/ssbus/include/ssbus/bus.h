// HEARASIDE bus: shared-memory segment + slot registry + helpers.
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/protocol.h"
#include "ssbus/ring.h"

#include <memory>
#include <string>

namespace ssbus {

// ---------------------------------------------------------------------------------------------
// Platform helpers

uint64_t nowNs() noexcept;                  // monotonic, comparable across processes on one machine
uint32_t currentPid() noexcept;
bool     processAlive(uint32_t pid) noexcept;
uint64_t randomToken() noexcept;            // non-zero random 64-bit value
std::string makeUuid();                     // 36 chars, RFC 4122 v4 style

// ---------------------------------------------------------------------------------------------
// Shared memory mapping (platform specific implementation in shm_win.cpp / shm_posix.cpp)

class SharedMemory {
public:
    enum class Status { Ok, CreateFailed, MapFailed, InitTimeout, VersionMismatch };

    ~SharedMemory();
    // Opens (or creates) the segment for busName. Must not be called on an audio thread.
    static std::unique_ptr<SharedMemory> open(const std::string& busName, Status& status);
    // Opens only if the segment already exists (consumers such as OBS use this to poll).
    static std::unique_ptr<SharedMemory> openExisting(const std::string& busName, Status& status);

    BusLayout& layout() noexcept { return *static_cast<BusLayout*>(base_); }
    const std::string& busName() const noexcept { return busName_; }
    static std::string segmentName(const std::string& busName);

private:
    SharedMemory() = default;
    static std::unique_ptr<SharedMemory> openImpl(const std::string& busName, bool create, Status& status);

    void*       base_   = nullptr;
    size_t      size_   = 0;
    void*       handle_ = nullptr;   // HANDLE on Windows
    int         fd_     = -1;        // POSIX
    std::string busName_;
};

const char* statusText(SharedMemory::Status s) noexcept;

// ---------------------------------------------------------------------------------------------
// Beacon: a 64-byte segment whose name and layout never change between versions. The Hub keeps it
// fresh so a Track / App Audio / OBS that finds no bus of its own version can still tell "a Hub is
// running, but it is another HEARASIDE version" apart from "no Hub".

class BeaconMap {
public:
    ~BeaconMap();
    static std::unique_ptr<BeaconMap> open(const std::string& busName);           // creates when missing (Hub)
    static std::unique_ptr<BeaconMap> openExisting(const std::string& busName);   // readers
    BeaconHeader& header() noexcept { return *static_cast<BeaconHeader*>(base_); }
    static std::string segmentName(const std::string& busName);

private:
    BeaconMap() = default;
    static std::unique_ptr<BeaconMap> openImpl(const std::string& busName, bool create);
    void*       base_   = nullptr;
    void*       handle_ = nullptr;
    int         fd_     = -1;
};

// Hub: call about every 500 ms from a non-audio thread.
void beaconPublish(BeaconHeader& b) noexcept;
// Reader: the protocol version of a running Hub on this bus, 0 when there is none (or it is stale).
uint32_t beaconHubVersion(const BeaconHeader& b, uint64_t staleNs = 3'000'000'000ull) noexcept;

// ---------------------------------------------------------------------------------------------
// Slot registry (Track side)

// Claims a free slot. If another active slot already uses `uuid`, the caller must pick a new
// UUID (duplicateUuid = true is reported and -1 returned). Message thread only.
int  claimSlot(BusLayout& bus, const std::string& uuid, uint32_t sampleRate, uint32_t numChannels,
               bool& duplicateUuid) noexcept;
void releaseSlot(BusLayout& bus, int index) noexcept;
bool uuidInUse(const BusLayout& bus, const std::string& uuid, int ignoreIndex = -1) noexcept;

void setSlotIdentity(SlotHeader& slot, const std::string& name, uint32_t colorARGB) noexcept;
// Copies name/uuid/colour consistently. Returns false if a writer kept interfering.
bool readSlotIdentity(const SlotHeader& slot, std::string& name, std::string& uuid, uint32_t& colorARGB) noexcept;

// Hub side: frees slots and App Audio sources whose owner process died, returns number reclaimed.
int  reclaimDeadSlots(BusLayout& bus, uint64_t staleNs) noexcept;

// ---------------------------------------------------------------------------------------------
// Hub ownership

// Tries to become the bus' Hub. Succeeds if no Hub owns it, or the owner is stale.
bool tryClaimHub(BusLayout& bus, uint64_t token, uint64_t staleNs) noexcept;
void releaseHub(BusLayout& bus, uint64_t token) noexcept;
bool hubAlive(const BusLayout& bus, uint64_t staleNs) noexcept;

// ---------------------------------------------------------------------------------------------
// Mailbox (Hub -> Track)

void postCommand(SlotHeader& slot, ParamId id, float value) noexcept;
// Reads new commands since `readCursor`; returns number delivered to fn.
template <typename Fn>
int pollCommands(SlotHeader& slot, uint32_t& readCursor, Fn&& fn) {
    const uint32_t w = slot.cmdWrite.load(std::memory_order_acquire);
    if (w - readCursor > uint32_t(kMailboxSize)) readCursor = w - uint32_t(kMailboxSize); // dropped oldest
    int count = 0;
    while (readCursor != w) {
        const Command c = slot.cmds[readCursor % kMailboxSize];
        fn(static_cast<ParamId>(c.paramId), c.value);
        ++readCursor;
        ++count;
    }
    return count;
}

void requestRename(SlotHeader& slot, const std::string& name) noexcept;

// ---------------------------------------------------------------------------------------------
// Remote control (any process -> Hub). Thread / process safe for any number of writers.

void postRemote(BusLayout& bus, int target, uint32_t paramId, float value) noexcept;
inline void postRemote(BusLayout& bus, RemoteParam p, float value) noexcept { postRemote(bus, -1, uint32_t(p), value); }

// Hub side (single reader). Delivers complete commands in order; returns the number delivered.
// An entry still being written stops delivery until the next call.
template <typename Fn>
int pollRemote(BusLayout& bus, uint32_t& cursor, Fn&& fn) {
    auto& h = bus.header;
    const uint32_t reserve = h.remoteReserve.load(std::memory_order_acquire);
    if (reserve - cursor > uint32_t(kRemoteQueueSize)) cursor = reserve - uint32_t(kRemoteQueueSize);   // dropped oldest
    int count = 0;
    while (cursor != reserve) {
        RemoteCommand& e = h.remote[cursor % kRemoteQueueSize];
        const uint32_t s = e.seq.load(std::memory_order_acquire);
        if (s == cursor + 1) {
            const int32_t target = e.target.load(std::memory_order_relaxed);
            const uint32_t pid = e.paramId.load(std::memory_order_relaxed);
            const float v = bitsFloat(e.valueBits.load(std::memory_order_relaxed));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.seq.load(std::memory_order_relaxed) == s) { fn(int(target), pid, v); ++count; }
        } else if (int32_t(s - (cursor + 1)) <= 0) {
            break;   // claimed but not published yet: try again next time
        }
        ++cursor;    // delivered, or overwritten by a newer command (queue overflow)
    }
    return count;
}

// Returns true and fills `name` if a new rename request arrived since lastSeq.
bool pollRename(const SlotHeader& slot, uint32_t& lastSeq, std::string& name) noexcept;

// ---------------------------------------------------------------------------------------------
// App sources (HEARASIDE App Audio side claims, Hub side lists / controls). Message thread only.

int  claimSource(BusLayout& bus) noexcept;   // -1 = all kMaxSources in use
void releaseSource(BusLayout& bus, int index) noexcept;
void setSourceIdentity(SourceHeader& src, const std::string& name, const std::string& app, uint32_t colorARGB) noexcept;
bool readSourceIdentity(const SourceHeader& src, std::string& name, std::string& app, uint32_t& colorARGB) noexcept;

void postSourceCommand(SourceHeader& src, SourceParam id, float value) noexcept;
template <typename Fn>
int pollSourceCommands(SourceHeader& src, uint32_t& readCursor, Fn&& fn) {
    const uint32_t w = src.cmdWrite.load(std::memory_order_acquire);
    if (w - readCursor > uint32_t(kMailboxSize)) readCursor = w - uint32_t(kMailboxSize);
    int count = 0;
    for (; readCursor != w; ++readCursor, ++count) {
        const Command c = src.cmds[readCursor % kMailboxSize];
        fn(static_cast<SourceParam>(c.paramId), c.value);
    }
    return count;
}
// Hub asks the App Audio to capture another program ("" = none).
void requestSourceApp(SourceHeader& src, const std::string& exe) noexcept;
bool pollSourceApp(const SourceHeader& src, uint32_t& lastSeq, std::string& exe) noexcept;

// ---------------------------------------------------------------------------------------------
// Friends room + feeders (v13)

void setFriendName(FriendHeader& f, const std::string& name) noexcept;
std::string readFriendName(const FriendHeader& f);   // consistent copy (empty if a writer kept interfering)

// Feeder side (HEARASIDE Track in "friend input" mode). Message thread only.
int  claimFeeder(BusLayout& bus) noexcept;           // -1 = all kMaxFeeders in use
void releaseFeeder(BusLayout& bus, int index) noexcept;
void setFeederIdentity(FeederRecord& f, const std::string& trackName, uint32_t colorARGB) noexcept;
bool readFeederIdentity(const FeederRecord& f, std::string& trackName, uint32_t& colorARGB) noexcept;

// Any plug-in -> Hub (multi-writer). `text` may be empty. The answer arrives in feeders[feeder].reply / replySeq.
void postRequest(BusLayout& bus, int feeder, RequestKind kind, uint32_t arg, const std::string& text) noexcept;

// Hub side (single reader): delivers complete requests in order, then wipes the text. fn(feeder, kind, arg, text).
template <typename Fn>
int pollRequests(BusLayout& bus, uint32_t& cursor, Fn&& fn) {
    const uint32_t reserve = bus.requestReserve.load(std::memory_order_acquire);
    if (reserve - cursor > uint32_t(kRequestQueueSize)) cursor = reserve - uint32_t(kRequestQueueSize);   // dropped oldest
    int count = 0;
    while (cursor != reserve) {
        PluginRequest& e = bus.requests[cursor % kRequestQueueSize];
        const uint32_t s = e.seq.load(std::memory_order_acquire);
        if (s == cursor + 1) {
            const int32_t feeder = e.feeder.load(std::memory_order_relaxed);
            const uint32_t kind = e.kind.load(std::memory_order_relaxed);
            const uint32_t arg = e.arg.load(std::memory_order_relaxed);
            char text[kNameBytes];
            std::memcpy(text, e.text, kNameBytes);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.seq.load(std::memory_order_relaxed) == s) {
                std::memset(e.text, 0, kNameBytes);   // a pasted token must not stay in shared memory
                text[kNameBytes - 1] = 0;
                fn(int(feeder), static_cast<RequestKind>(kind), arg, std::string(text));
                ++count;
            }
        } else if (int32_t(s - (cursor + 1)) <= 0) {
            break;   // claimed but not published yet
        }
        ++cursor;
    }
    return count;
}

// Hub -> feeder answer (single writer per feeder).
void postReply(FeederRecord& f, uint32_t reply) noexcept;
// Feeder: true (and the answer) once replySeq moved past lastSeq. Poll from a timer; never block the message thread.
bool pollReply(const FeederRecord& f, uint32_t& lastSeq, uint32_t& reply) noexcept;

// ---------------------------------------------------------------------------------------------
// Timeline tags

// Audio thread of the Track: call before writing the block to the FIFO.
inline void writeTag(SlotHeader& slot, SlotTags& tags, uint64_t fifoPos, int64_t timeSamples,
                     uint32_t numFrames, bool playing) noexcept {
    const uint64_t n = slot.tagWrite.load(std::memory_order_relaxed);
    TimelineTag& t = tags.tags[n % kTagRingSize];
    t.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    t.fifoPos.store(fifoPos, std::memory_order_relaxed);
    t.timeSamples.store(timeSamples, std::memory_order_relaxed);
    t.numFrames.store(numFrames, std::memory_order_relaxed);
    t.playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
    t.seq.store(n + 1, std::memory_order_release);
    slot.tagWrite.store(n + 1, std::memory_order_release);
}

struct TagHit {
    uint64_t fifoPos;   // FIFO frame that corresponds to the requested timeline sample
    bool     found;
};

// Searches the most recent tags (newest first) for the block that contains timeline sample
// `timeSamples` while playing. Lock-free; tolerates concurrent writes.
TagHit findTimelinePosition(const SlotHeader& slot, const SlotTags& tags, int64_t timeSamples,
                            int maxTags = kTagRingSize) noexcept;

} // namespace ssbus
