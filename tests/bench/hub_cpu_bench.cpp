// CPU of the Hub's audio thread (PLAN-rust-migration R2.6): HubEngine::process with 8 live friends, Line up,
// 4 Tracks, 48 kHz, 256-frame blocks. Built from the same source against the C++ core (default) and the Rust
// core (HEARASIDE_USE_RUST_CORE=ON), so the two numbers compare the engines only.
//   ss_hub_bench [seconds-of-audio = 60] [repeats = 5]
// SPDX-License-Identifier: MIT
#include "ssbus/bus.h"
#include "ssengine/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ssengine;
using namespace ssbus;

namespace {

float noiseAt(int64_t t, uint32_t seed) {
    uint32_t h = uint32_t(t) * 2654435761u ^ seed * 40503u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (float(h & 0xffff) / 65535.0f - 0.5f) * 0.5f;
}

struct Run { double meanUs, p99Us, maxUs; };

Run once(double seconds, int rep) {
    constexpr int kRate = 48000, kBlock = 256, kTracks = 4;
    const std::string bus = "bench_" + std::to_string(currentPid()) + "_" + std::to_string(rep);
    HubEngine hub;
    hub.connect(bus);
    hub.prepare(kRate, kBlock);
    hub.maintain();
    hub.setLineUp(true, 600.0f);
    BusLayout* b = hub.bus();
    std::vector<std::unique_ptr<TrackPublisher>> tracks;
    for (int k = 0; k < kTracks; ++k) {
        auto t = std::make_unique<TrackPublisher>();
        t->connect(bus, "bench-" + std::to_string(k), kRate, 2);
        t->prepare(kRate, 2);
        TrackPublisher::Mirror m{ true, true, false, -3.0f, float(k) * 0.2f - 0.3f, float(k) * 10.0f, 0.0f, k % 2, false };
        t->mirror(m);
        tracks.push_back(std::move(t));
    }
    for (int j = 0; j < kMaxFriends; ++j) {
        FriendHeader& h = b->friends[j];
        h.id.store(uint32_t(100 + j));
        h.sampleRate.store(j % 3 == 2 ? 44100u : uint32_t(kRate));   // some go through the resampler
        h.delayBits.store(floatBits(60.0f + 25.0f * float(j)));
        h.route.store(kRouteDirect);
        h.state.store(kFriendLive);
    }
    std::vector<float> l(kBlock), r(kBlock), x(kBlock), pk(960);
    std::vector<int64_t> sent(kMaxFriends, 0);
    std::vector<double> us;
    const int blocks = int(seconds * kRate / kBlock);
    us.reserve(size_t(blocks));
    int64_t t = 0;
    for (int c = 0; c < blocks; ++c) {
        for (int j = 0; j < kMaxFriends; ++j) {
            FriendHeader& h = b->friends[j];
            h.heartbeatNs.store(nowNs());
            const int pkt = h.sampleRate.load() == 44100u ? 882 : 960;
            const int64_t due = t * int64_t(h.sampleRate.load()) / kRate;
            while (sent[size_t(j)] + pkt + 2400 <= due) {
                for (int i = 0; i < pkt; ++i) pk[size_t(i)] = noiseAt(sent[size_t(j)] + i, 29u + uint32_t(j));
                const float* src[1] = { pk.data() };
                ringWrite(b->friendAudio[j], h.writePos, src, 1, uint32_t(pkt));
                sent[size_t(j)] += pkt;
            }
        }
        for (int i = 0; i < kBlock; ++i) x[size_t(i)] = noiseAt(t + i, 11);
        const float* ch[2] = { x.data(), x.data() };
        for (auto& tr : tracks) tr->process(ch, 2, kBlock, t, true, false, false);
        std::copy(x.begin(), x.end(), l.begin());
        std::copy(x.begin(), x.end(), r.begin());
        float* io[2] = { l.data(), r.data() };
        const auto a = std::chrono::steady_clock::now();
        hub.process(io, 2, kBlock, HubParams{}, t, true, false);
        const auto z = std::chrono::steady_clock::now();
        if (c >= blocks / 10) us.push_back(std::chrono::duration<double, std::micro>(z - a).count());   // skip the warm-up
        t += kBlock;
    }
    std::sort(us.begin(), us.end());
    double sum = 0;
    for (double v : us) sum += v;
    return { sum / double(us.size()), us[size_t(double(us.size()) * 0.99)], us.back() };
}

} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 5;
    std::vector<Run> runs;
    for (int k = 0; k < repeats; ++k) {
        runs.push_back(once(seconds, k));
        std::printf("run %d: mean %.2f us, p99 %.2f us, max %.2f us per 256-frame block\n", k, runs.back().meanUs, runs.back().p99Us, runs.back().maxUs);
    }
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.meanUs < b.meanUs; });
    const Run& med = runs[runs.size() / 2];
    const double budgetUs = 256.0 * 1e6 / 48000.0;
    std::printf("%s core: median mean %.2f us (%.2f %% of the 5.33 ms block), p99 %.2f us\n",
#if defined(HEARASIDE_USE_RUST_CORE) && HEARASIDE_USE_RUST_CORE
                "Rust",
#else
                "C++",
#endif
                med.meanUs, 100.0 * med.meanUs / budgetUs, med.p99Us);
    return 0;
}
