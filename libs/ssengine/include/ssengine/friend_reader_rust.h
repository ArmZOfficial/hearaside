// FriendReader backed by the Rust core (HEARASIDE_USE_RUST_CORE, PLAN-rust-migration R2.5): the same public
// API as friend_reader.h. Removed in R9.
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/bus.h"
#include "ssdsp/consumer.h"
#include "ssdsp/dsp.h"

#include <memory>
#include <vector>

struct HsReader;

namespace ssengine {

class FriendReader {
public:
    FriendReader();
    ~FriendReader();
    FriendReader(const FriendReader&) = delete;
    FriendReader& operator=(const FriendReader&) = delete;

    void prepare(double sampleRate, int maxBlock);
    void reset() noexcept;
    bool pull(const ssbus::ChannelRing& ring, const std::atomic<uint64_t>& writePos, uint32_t rate, float* const* out, int n) noexcept;

    double   bufferMs() const noexcept;
    bool     primed() const noexcept;
    uint32_t underruns() const noexcept;
    uint32_t reprimes() const noexcept;
    double   concealedMs() const noexcept;
    double   jitterP95Ms() const noexcept;

private:
    HsReader* r_;
};

} // namespace ssengine
