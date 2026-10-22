// Real-time safety audit unit tests for HEARASIDE audio thread engines.
// Asserts that no heap allocations (malloc / new) occur during processBlock / processChunk.
// SPDX-License-Identifier: MIT
#include "testing.h"

#include "ssbus/bus.h"
#include "ssengine/engine.h"
#include "ssengine/friend_reader.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace ssengine;
using namespace ssbus;

namespace {
thread_local bool t_countAllocations = false;
thread_local size_t t_allocations = 0;
}

// Global operator new/delete replacement for the unit test binary to detect RT allocations
void* operator new(size_t size) {
    if (t_countAllocations) ++t_allocations;
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](size_t size) {
    if (t_countAllocations) ++t_allocations;
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
    std::free(p);
}

TEST_CASE("rt_safety: audio thread HubEngine::process and TrackPublisher::process perform 0 heap allocations") {
    const std::string bus = "rt_safety_" + std::to_string(currentPid()) + "_" + std::to_string(nowNs() % 1000000);
    HubEngine hub;
    REQUIRE(hub.connect(bus));
    hub.prepare(48000, 256);
    hub.maintain();

    TrackPublisher pub;
    REQUIRE(pub.connect(bus, "rt-test-uuid", 48000, 2) == TrackPublisher::Status::Connected);
    pub.prepare(48000, 2);

    float inL[256], inR[256];
    for (int i = 0; i < 256; ++i) {
        inL[i] = std::sin(float(i) * 0.05f) * 0.5f;
        inR[i] = inL[i];
    }
    const float* trIn[2] = { inL, inR };
    float outL[256], outR[256];
    float* hubIo[2] = { outL, outR };

    // Warm-up cycles (let any initial state settle)
    for (int i = 0; i < 8; ++i) {
        pub.process(trIn, 2, 256, i * 256, true, false, false);
        std::memcpy(outL, inL, 256 * sizeof(float));
        std::memcpy(outR, inR, 256 * sizeof(float));
        hub.process(hubIo, 2, 256, HubParams{}, i * 256, true, false);
    }

    // Now audit 100 blocks strictly with allocation counter active
    t_allocations = 0;
    t_countAllocations = true;

    for (int i = 8; i < 108; ++i) {
        pub.process(trIn, 2, 256, i * 256, true, false, false);
        std::memcpy(outL, inL, 256 * sizeof(float));
        std::memcpy(outR, inR, 256 * sizeof(float));
        hub.process(hubIo, 2, 256, HubParams{}, i * 256, true, false);
    }

    t_countAllocations = false;
    CHECK(t_allocations == 0);
}

TEST_CASE("rt_safety: FriendReader::pull performs 0 heap allocations on audio thread") {
    FriendReader reader;
    reader.prepare(48000, 256);

    float bufL[256] = { 0 }, bufR[256] = { 0 };
    float* out[2] = { bufL, bufR };
    std::atomic<uint64_t> writePos{ 0 };
    static ChannelRing dummyRing{};
    for (int i = 0; i < 4; ++i) {
        reader.pull(dummyRing, writePos, 48000, out, 256);
    }

    t_allocations = 0;
    t_countAllocations = true;

    for (int i = 0; i < 50; ++i) {
        reader.pull(dummyRing, writePos, 48000, out, 256);
    }

    t_countAllocations = false;
    CHECK(t_allocations == 0);
}
