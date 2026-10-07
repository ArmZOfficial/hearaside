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

// Hub side: frees slots whose owner process died, returns number reclaimed.
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
// Returns true and fills `name` if a new rename request arrived since lastSeq.
bool pollRename(const SlotHeader& slot, uint32_t& lastSeq, std::string& name) noexcept;

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
