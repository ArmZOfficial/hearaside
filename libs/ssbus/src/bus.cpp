// SPDX-License-Identifier: MIT
#include "ssbus/bus.h"

#include <cstdio>
#include <cstring>
#include <random>

namespace ssbus {

namespace {

void copyName(char* dst, const std::string& src, size_t cap) noexcept {
    // Truncate on a UTF-8 code point boundary so we never store half a Thai character.
    size_t n = std::min(src.size(), cap - 1);
    while (n > 0 && n < src.size() && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, src.data(), n);
    std::memset(dst + n, 0, cap - n);
}

std::string readCString(const char* src, size_t cap) {
    size_t n = 0;
    while (n < cap && src[n] != 0) ++n;
    return std::string(src, n);
}

} // namespace

std::string makeUuid() {
    std::random_device rd;
    std::mt19937_64 gen((uint64_t(rd()) << 32) ^ rd() ^ nowNs());
    uint64_t a = gen(), b = gen();
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;   // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;   // variant
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx",
                  uint32_t(a >> 32), uint32_t((a >> 16) & 0xFFFF), uint32_t(a & 0xFFFF),
                  uint32_t(b >> 48), (unsigned long long)(b & 0xFFFFFFFFFFFFull));
    return buf;
}

uint64_t randomToken() noexcept {
    std::random_device rd;
    uint64_t t = 0;
    while (t == 0) t = (uint64_t(rd()) << 32) ^ rd() ^ (nowNs() * 0x9E3779B97F4A7C15ull);
    return t;
}

const char* statusText(SharedMemory::Status s) noexcept {
    switch (s) {
        case SharedMemory::Status::Ok:              return "ok";
        case SharedMemory::Status::CreateFailed:    return "cannot create shared memory";
        case SharedMemory::Status::MapFailed:       return "cannot map shared memory";
        case SharedMemory::Status::InitTimeout:     return "shared memory was never initialised";
        case SharedMemory::Status::VersionMismatch: return "protocol version mismatch";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// slots

bool uuidInUse(const BusLayout& bus, const std::string& uuid, int ignoreIndex) noexcept {
    for (int i = 0; i < kMaxSlots; ++i) {
        if (i == ignoreIndex) continue;
        const SlotHeader& s = bus.slots[i];
        if (s.state.load(std::memory_order_acquire) != kSlotActive) continue;
        std::string name, id; uint32_t col = 0;
        if (readSlotIdentity(s, name, id, col) && id == uuid) return true;
    }
    return false;
}

int claimSlot(BusLayout& bus, const std::string& uuid, uint32_t sampleRate, uint32_t numChannels,
              bool& duplicateUuid) noexcept {
    duplicateUuid = uuidInUse(bus, uuid);
    if (duplicateUuid) return -1;

    for (int i = 0; i < kMaxSlots; ++i) {
        SlotHeader& s = bus.slots[i];
        uint32_t expected = kSlotFree;
        if (!s.state.compare_exchange_strong(expected, kSlotClaiming, std::memory_order_acq_rel)) continue;

        const uint64_t now = nowNs();
        s.ownerPid.store(currentPid(), std::memory_order_relaxed);
        s.sequence.store(bus.header.slotSequence.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);

        s.nameSeq.fetch_add(1, std::memory_order_relaxed);   // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        copyName(s.uuid, uuid, kUuidBytes);
        std::memset(s.name, 0, kNameBytes);
        s.colorARGB = 0;
        s.nameSeq.fetch_add(1, std::memory_order_release);   // even: stable

        s.sampleRate.store(sampleRate, std::memory_order_relaxed);
        s.numChannels.store(numChannels, std::memory_order_relaxed);
        s.heartbeatNs.store(now, std::memory_order_relaxed);
        s.claimNs.store(now, std::memory_order_relaxed);
        s.flags.store(kFlagMon | kFlagStr, std::memory_order_relaxed);
        s.strGainBits.store(floatBits(0.0f), std::memory_order_relaxed);
        s.strPanBits.store(floatBits(0.0f), std::memory_order_relaxed);
        s.strDelayBits.store(floatBits(0.0f), std::memory_order_relaxed);
        s.monTrimBits.store(floatBits(0.0f), std::memory_order_relaxed);
        s.stemIndex.store(-1, std::memory_order_relaxed);
        for (int c = 0; c < kMaxChannels; ++c) {
            s.peakInBits[c].store(0, std::memory_order_relaxed);
            s.peakStreamBits[c].store(0, std::memory_order_relaxed);
        }
        s.hubStatus.store(kHubStatusNone, std::memory_order_relaxed);
        s.hubLeadFrames.store(0, std::memory_order_relaxed);
        s.epoch.fetch_add(1, std::memory_order_relaxed);
        s.state.store(kSlotActive, std::memory_order_release);
        return i;
    }
    return -1;
}

void releaseSlot(BusLayout& bus, int index) noexcept {
    if (index < 0 || index >= kMaxSlots) return;
    SlotHeader& s = bus.slots[index];
    s.flags.store(0, std::memory_order_relaxed);
    s.epoch.fetch_add(1, std::memory_order_relaxed);
    s.state.store(kSlotFree, std::memory_order_release);
}

void setSlotIdentity(SlotHeader& slot, const std::string& name, uint32_t colorARGB) noexcept {
    slot.nameSeq.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    copyName(slot.name, name, kNameBytes);
    slot.colorARGB = colorARGB;
    slot.nameSeq.fetch_add(1, std::memory_order_release);
}

bool readSlotIdentity(const SlotHeader& slot, std::string& name, std::string& uuid, uint32_t& colorARGB) noexcept {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = slot.nameSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        char n[kNameBytes], u[kUuidBytes];
        std::memcpy(n, slot.name, kNameBytes);
        std::memcpy(u, slot.uuid, kUuidBytes);
        const uint32_t col = slot.colorARGB;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.nameSeq.load(std::memory_order_relaxed) != s1) continue;
        name = readCString(n, kNameBytes);
        uuid = readCString(u, kUuidBytes);
        colorARGB = col;
        return true;
    }
    return false;
}

int reclaimDeadSlots(BusLayout& bus, uint64_t staleNs) noexcept {
    const uint64_t now = nowNs();
    int count = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        SlotHeader& s = bus.slots[i];
        const uint32_t st = s.state.load(std::memory_order_acquire);
        if (st == kSlotFree) continue;
        const uint64_t hb = s.heartbeatNs.load(std::memory_order_relaxed);
        if (now > hb && now - hb > staleNs && !processAlive(s.ownerPid.load(std::memory_order_relaxed))) {
            uint32_t expected = st;
            if (s.state.compare_exchange_strong(expected, kSlotFree, std::memory_order_acq_rel)) {
                s.flags.store(0, std::memory_order_relaxed);
                s.epoch.fetch_add(1, std::memory_order_relaxed);
                ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// hub

bool hubAlive(const BusLayout& bus, uint64_t staleNs) noexcept {
    const BusHeader& h = bus.header;
    if (h.hubOwnerToken.load(std::memory_order_acquire) == 0) return false;
    const uint64_t hb = h.hubHeartbeatNs.load(std::memory_order_relaxed);
    const uint64_t now = nowNs();
    if (now > hb && now - hb > staleNs) return false;
    return processAlive(h.hubOwnerPid.load(std::memory_order_relaxed));
}

bool tryClaimHub(BusLayout& bus, uint64_t token, uint64_t staleNs) noexcept {
    BusHeader& h = bus.header;
    uint64_t cur = h.hubOwnerToken.load(std::memory_order_acquire);
    if (cur == token) return true;
    if (cur != 0 && hubAlive(bus, staleNs)) return false;
    if (!h.hubOwnerToken.compare_exchange_strong(cur, token, std::memory_order_acq_rel)) return false;
    h.hubOwnerPid.store(currentPid(), std::memory_order_relaxed);
    h.hubHeartbeatNs.store(nowNs(), std::memory_order_release);
    return true;
}

void releaseHub(BusLayout& bus, uint64_t token) noexcept {
    uint64_t cur = token;
    if (bus.header.hubOwnerToken.compare_exchange_strong(cur, 0, std::memory_order_acq_rel)) {
        bus.header.hubFlags.store(0, std::memory_order_relaxed);
        bus.streamHeader.active.store(0, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------------------------
// mailbox

void postCommand(SlotHeader& slot, ParamId id, float value) noexcept {
    const uint32_t w = slot.cmdWrite.load(std::memory_order_relaxed);
    slot.cmds[w % kMailboxSize] = Command{ static_cast<uint32_t>(id), value };
    slot.cmdWrite.store(w + 1, std::memory_order_release);
}

void requestRename(SlotHeader& slot, const std::string& name) noexcept {
    const uint32_t s = slot.renameSeq.load(std::memory_order_relaxed) & ~1u;
    slot.renameSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    copyName(slot.renameTo, name, kNameBytes);
    slot.renameSeq.store(s + 2, std::memory_order_release);
}

bool pollRename(const SlotHeader& slot, uint32_t& lastSeq, std::string& name) noexcept {
    const uint32_t s1 = slot.renameSeq.load(std::memory_order_acquire);
    if ((s1 & 1u) || s1 == lastSeq) return false;
    char buf[kNameBytes];
    std::memcpy(buf, slot.renameTo, kNameBytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.renameSeq.load(std::memory_order_relaxed) != s1) return false;
    lastSeq = s1;
    name = readCString(buf, kNameBytes);
    return true;
}

// ---------------------------------------------------------------------------------------------
// timeline

TagHit findTimelinePosition(const SlotHeader& slot, const SlotTags& tags, int64_t timeSamples,
                            int maxTags) noexcept {
    const uint64_t written = slot.tagWrite.load(std::memory_order_acquire);
    const uint64_t count = std::min<uint64_t>(written, uint64_t(std::min(maxTags, kTagRingSize - 4)));
    for (uint64_t k = 0; k < count; ++k) {
        const uint64_t idx = written - 1 - k;
        const TimelineTag& t = tags.tags[idx % kTagRingSize];
        const uint64_t s1 = t.seq.load(std::memory_order_acquire);
        if (s1 != idx + 1) continue;   // being rewritten or lapped
        const uint64_t fifo  = t.fifoPos.load(std::memory_order_relaxed);
        const int64_t  time  = t.timeSamples.load(std::memory_order_relaxed);
        const uint32_t n     = t.numFrames.load(std::memory_order_relaxed);
        const uint32_t play  = t.playing.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (t.seq.load(std::memory_order_relaxed) != s1) continue;
        if (!play) return { 0, false };   // newest relevant history is "stopped": no timeline
        if (timeSamples >= time && timeSamples < time + int64_t(n))
            return { fifo + uint64_t(timeSamples - time), true };
        if (timeSamples >= time + int64_t(n)) return { 0, false };   // requested time is newer than track
    }
    return { 0, false };
}

} // namespace ssbus
