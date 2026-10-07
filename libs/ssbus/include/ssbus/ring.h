// Broadcast ring buffer: one writer, any number of readers, writer never waits.
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/protocol.h"

#include <algorithm>
#include <cstring>

namespace ssbus {

enum class ReadResult { Ok, Underrun, Overrun };

namespace detail {
inline void copyIn(float* ring, uint64_t pos, const float* src, uint32_t n) noexcept {
    const uint32_t start = uint32_t(pos & kRingMask);
    const uint32_t first = std::min<uint32_t>(n, kRingFrames - start);
    std::memcpy(ring + start, src, first * sizeof(float));
    if (first < n) std::memcpy(ring, src + first, (n - first) * sizeof(float));
}
inline void copyOut(float* dst, const float* ring, uint64_t pos, uint32_t n) noexcept {
    const uint32_t start = uint32_t(pos & kRingMask);
    const uint32_t first = std::min<uint32_t>(n, kRingFrames - start);
    std::memcpy(dst, ring + start, first * sizeof(float));
    if (first < n) std::memcpy(dst + first, ring, (n - first) * sizeof(float));
}
inline void fillZero(float* ring, uint64_t pos, uint32_t n) noexcept {
    const uint32_t start = uint32_t(pos & kRingMask);
    const uint32_t first = std::min<uint32_t>(n, kRingFrames - start);
    std::memset(ring + start, 0, first * sizeof(float));
    if (first < n) std::memset(ring, 0, (n - first) * sizeof(float));
}
} // namespace detail

// Writes n frames. numSrc == 1 is written as dual mono. src == nullptr writes silence.
// Real-time safe (memcpy only).
inline void ringWrite(ChannelRing& ring, std::atomic<uint64_t>& writePos,
                      const float* const* src, uint32_t numSrc, uint32_t n) noexcept {
    uint64_t w = writePos.load(std::memory_order_relaxed);   // single writer
    uint32_t done = 0;
    constexpr uint32_t kChunk = kRingFrames - kGuardFrames;
    while (done < n) {
        const uint32_t m = std::min(n - done, kChunk);
        for (int c = 0; c < kMaxChannels; ++c) {
            if (src == nullptr || numSrc == 0) {
                detail::fillZero(ring.ch[c], w, m);
            } else {
                const float* s = src[std::min<uint32_t>(uint32_t(c), numSrc - 1)];
                detail::copyIn(ring.ch[c], w, s + done, m);
            }
        }
        w += m;
        done += m;
        writePos.store(w, std::memory_order_release);
    }
}

// Reads n frames starting at absolute frame position pos into dst[0..1] (both channels).
// Frames not yet written are zero-filled (Underrun); if any requested frame was already
// overwritten the whole block is zeroed and Overrun is returned.
inline ReadResult ringReadAt(const ChannelRing& ring, const std::atomic<uint64_t>& writePos,
                             uint64_t pos, float* const* dst, uint32_t n,
                             uint32_t* framesAvailable = nullptr) noexcept {
    const uint64_t w = writePos.load(std::memory_order_acquire);
    auto zeroAll = [&] { for (int c = 0; c < kMaxChannels; ++c) std::memset(dst[c], 0, n * sizeof(float)); };

    if (w > pos && w - pos > kRingFrames - kGuardFrames) {
        zeroAll();
        if (framesAvailable) *framesAvailable = 0;
        return ReadResult::Overrun;
    }
    const uint32_t avail = (w > pos) ? uint32_t(std::min<uint64_t>(w - pos, n)) : 0u;
    for (int c = 0; c < kMaxChannels; ++c) {
        if (avail > 0) detail::copyOut(dst[c], ring.ch[c], pos, avail);
        if (avail < n) std::memset(dst[c] + avail, 0, (n - avail) * sizeof(float));
    }
    const uint64_t w2 = writePos.load(std::memory_order_acquire);
    if (w2 > pos && w2 - pos > kRingFrames - kGuardFrames) {   // overwritten while copying
        zeroAll();
        if (framesAvailable) *framesAvailable = 0;
        return ReadResult::Overrun;
    }
    if (framesAvailable) *framesAvailable = avail;
    return avail == n ? ReadResult::Ok : ReadResult::Underrun;
}

// Cursor-style read: advances r by the number of frames actually available.
inline ReadResult ringRead(const ChannelRing& ring, const std::atomic<uint64_t>& writePos,
                           uint64_t& r, float* const* dst, uint32_t n) noexcept {
    uint32_t got = 0;
    const ReadResult res = ringReadAt(ring, writePos, r, dst, n, &got);
    r += got;
    return res;
}

} // namespace ssbus
