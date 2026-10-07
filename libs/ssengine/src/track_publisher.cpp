// SPDX-License-Identifier: MIT
#include "ssengine/engine.h"

#include <cmath>

namespace ssengine {

using namespace ssbus;

TrackPublisher::TrackPublisher() = default;

TrackPublisher::~TrackPublisher() { disconnect(); }

TrackPublisher::Status TrackPublisher::connect(const std::string& busName, std::string uuid,
                                               uint32_t sampleRate, uint32_t numChannels) {
    disconnect();
    busName_ = busName.empty() ? std::string("Main") : busName;
    sampleRate_ = sampleRate;
    if (uuid.empty()) uuid = makeUuid();

    SharedMemory::Status s{};
    auto shm = SharedMemory::open(busName_, s);
    if (!shm) { status_ = Status::ShmError; uuid_ = uuid; return status_; }

    bool dup = false;
    int idx = claimSlot(shm->layout(), uuid, sampleRate, numChannels, dup);
    if (dup) {   // duplicated track: pick a new identity
        uuid = makeUuid();
        idx = claimSlot(shm->layout(), uuid, sampleRate, numChannels, dup);
    }
    uuid_ = uuid;
    if (idx < 0) {
        status_ = Status::SlotsFull;
        retired_.push_back(std::move(shm));
        return status_;
    }
    shm_ = std::move(shm);
    slotIndex_ = idx;
    SlotHeader& slot = shm_->layout().slots[idx];
    bus_.store(&shm_->layout(), std::memory_order_release);
    slot_.store(&slot, std::memory_order_release);
    status_ = Status::Connected;
    mirror(lastMirror_);
    return status_;
}

void TrackPublisher::disconnect() {
    SlotHeader* s = slot_.exchange(nullptr, std::memory_order_acq_rel);
    BusLayout* b = bus_.exchange(nullptr, std::memory_order_acq_rel);
    if (s && b) releaseSlot(*b, slotIndex_);
    slotIndex_ = -1;
    if (shm_) retired_.push_back(std::move(shm_));
    status_ = Status::Disconnected;
}

void TrackPublisher::prepare(uint32_t sampleRate, uint32_t numChannels) {
    sampleRate_ = sampleRate;
    if (SlotHeader* s = slot()) {
        s->sampleRate.store(sampleRate, std::memory_order_relaxed);
        s->numChannels.store(numChannels, std::memory_order_relaxed);
        uint32_t f = s->flags.load(std::memory_order_relaxed);
        f = numChannels == 1 ? (f | kFlagMono) : (f & ~uint32_t(kFlagMono));
        s->flags.store(f, std::memory_order_relaxed);
        s->epoch.fetch_add(1, std::memory_order_release);
    }
}

void TrackPublisher::setIdentity(const std::string& name, uint32_t colorARGB) {
    if (SlotHeader* s = slot()) setSlotIdentity(*s, name, colorARGB);
}

bool TrackPublisher::hubPresent() const noexcept {
    if (BusLayout* b = bus()) return hubAlive(*b, 1000000000ull);
    return false;
}

void TrackPublisher::mirror(const Mirror& m) noexcept {
    lastMirror_ = m;
    SlotHeader* s = slot();
    if (!s) return;
    uint32_t keep = s->flags.load(std::memory_order_relaxed) & (kFlagBypassed | kFlagOffline | kFlagMono);
    uint32_t f = keep | (m.mon ? kFlagMon : 0u) | (m.str ? kFlagStr : 0u) | (m.solo ? kFlagSolo : 0u);
    s->strGainBits.store(floatBits(m.gainDb), std::memory_order_relaxed);
    s->strPanBits.store(floatBits(m.pan), std::memory_order_relaxed);
    s->strDelayBits.store(floatBits(m.delayMs), std::memory_order_relaxed);
    s->monTrimBits.store(floatBits(m.trimDb), std::memory_order_relaxed);
    s->stemIndex.store(m.stem, std::memory_order_relaxed);
    s->flags.store(f, std::memory_order_release);
}

void TrackPublisher::process(const float* const* ch, int numCh, int n, int64_t timeSamples, bool playing,
                             bool offline, bool bypassed) noexcept {
    SlotHeader* s = slot_.load(std::memory_order_acquire);
    BusLayout* b = bus_.load(std::memory_order_acquire);
    if (s == nullptr || b == nullptr || n <= 0 || numCh <= 0) return;

    s->heartbeatNs.store(nowNs(), std::memory_order_relaxed);

    // status bits that only the audio thread knows
    uint32_t f = s->flags.load(std::memory_order_relaxed);
    const uint32_t want = (f & ~uint32_t(kFlagBypassed | kFlagOffline))
                        | (bypassed ? kFlagBypassed : 0u) | (offline ? kFlagOffline : 0u);
    if (want != f) s->flags.store(want, std::memory_order_relaxed);

    if (offline) return;   // never send an export/bounce to the viewers

    const int idx = int(s - b->slots);
    const uint64_t w = s->writePos.load(std::memory_order_relaxed);
    writeTag(*s, b->slotTags[idx], w, timeSamples == kNoTime ? 0 : timeSamples, uint32_t(n),
             playing && timeSamples != kNoTime);
    ringWrite(b->slotAudio[idx], s->writePos, ch, uint32_t(std::max(1, std::min(numCh, kMaxChannels))), uint32_t(n));

    // meters: peak with ~300 ms decay
    const uint32_t sr = std::max<uint32_t>(sampleRate_, 8000);
    const float decay = std::exp(-float(n) / (0.3f * float(sr)));
    for (int c = 0; c < kMaxChannels; ++c) {
        const float* x = ch[std::min(c, numCh - 1)];
        const float p = ssdsp::blockPeak(x, n);
        peakIn_[c] = std::max(p, peakIn_[c] * decay);
        s->peakInBits[c].store(floatBits(peakIn_[c]), std::memory_order_relaxed);
    }
}

} // namespace ssengine
