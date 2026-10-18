// FriendReader's jitter buffer (plan 0.1 + 1.5): measured arrival jitter, short gaps hidden with a fade
// instead of a re-prime, a long outage re-primes and plays again. Simulated, no threads: packets of 480
// frames at 48 kHz are written into a ChannelRing when their arrival time passes, blocks of 256 are pulled.
#include "testing.h"

#include "ssengine/friend_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

using namespace ssengine;
using namespace ssbus;

namespace {

constexpr int kBlock = 256, kPkt = 480;
constexpr double kPi = 3.14159265358979323846;

double rnd(uint32_t& s) { s = s * 1664525u + 1013904223u; return double(s >> 8) / double(1 << 24); }   // [0, 1)

struct Result {
    std::vector<float> out;          // left channel, only kept when asked
    double seconds = 0, avgBufMs = 0, p95 = 0, concealedMs = 0;
    int gaps10 = 0, underruns = 0, reprimes = 0;
    double maxStep = 0;              // largest sample-to-sample step after the first second
    bool playingAtEnd = false;
};

// arrive(i) = arrival time (ms) of packet i; sig(frame) = what the friend sends. Packets keep their order (TCP).
Result simulate(double seconds, const std::function<double(int)>& arrive, const std::function<float(uint64_t)>& sig, bool keep = false) {
    auto ring = std::make_unique<ChannelRing>();
    std::atomic<uint64_t> wp{ 0 };
    FriendReader r;
    r.prepare(48000.0, kBlock);
    Result res;
    std::vector<float> l(kBlock), rr(kBlock), pk(kPkt);
    float* o[2] = { l.data(), rr.data() };
    const double blockMs = kBlock * 1000.0 / 48000.0;
    const int blocks = int(seconds * 1000.0 / blockMs);
    int nextPkt = 0;
    double lastArrival = 0, bufSum = 0;
    int bufN = 0, quiet = 0;
    bool started = false, wasPrimed = false;
    float prev = 0;
    for (int k = 0; k < blocks; ++k) {
        const double now = k * blockMs;
        for (;;) {
            const double raw = arrive(nextPkt), a = std::max(lastArrival, raw);
            if (raw < 1.0e17 && a > now) break;
            if (raw < 1.0e17) {   // a huge arrival time = the packet is dropped (lost, not delayed)
                for (int i = 0; i < kPkt; ++i) pk[size_t(i)] = sig(uint64_t(nextPkt) * kPkt + uint64_t(i));
                const float* src[1] = { pk.data() };
                ringWrite(*ring, wp, src, 1, kPkt);
                lastArrival = a;
            }
            ++nextPkt;
        }
        r.pull(*ring, wp, 48000, o, kBlock);
        if (k > 0 && wasPrimed && !r.primed()) ++res.reprimes;
        wasPrimed = r.primed();
        for (int i = 0; i < kBlock; ++i) {
            const float x = l[size_t(i)];
            if (std::abs(x) > 0.02f) started = true;
            if (started) {
                if (std::abs(x) < 0.005f) { if (++quiet == 480) ++res.gaps10; } else quiet = 0;
                if (now > 1000.0) res.maxStep = std::max(res.maxStep, double(std::abs(x - prev)));
            }
            prev = x;
            if (keep) res.out.push_back(x);
        }
        if (now > 30000.0 && r.primed()) { bufSum += r.bufferMs(); ++bufN; }
    }
    res.seconds = seconds;
    res.avgBufMs = bufN ? bufSum / bufN : 0;
    res.p95 = r.jitterP95Ms();
    res.underruns = int(r.underruns());
    res.concealedMs = r.concealedMs();
    res.playingAtEnd = quiet < 480 && started;
    return res;
}

// packet i is due at i * 10 ms + 40 ms; jitter uniform in +-j ms
std::function<double(int)> jittery(double j, uint32_t seed) {
    auto s = std::make_shared<uint32_t>(seed);
    return [=](int i) { return 40.0 + i * 10.0 + (rnd(*s) * 2.0 - 1.0) * j; };
}

float dc(uint64_t) { return 0.5f; }
float sine(uint64_t f) { return 0.5f * float(std::sin(2.0 * kPi * 440.0 * double(f) / 48000.0)); }

} // namespace

TEST_CASE("jitter: the measured p95 follows a 20 ms spread of arrival times") {
    const Result r = simulate(60.0, jittery(10.0, 1), dc);
    std::printf("    p95 %.1f ms (uniform +-10 ms)\n", r.p95);
    CHECK(r.p95 >= 15.0);
    CHECK(r.p95 <= 25.0);
    const Result steady = simulate(20.0, jittery(0.0, 1), dc);
    std::printf("    no jitter: p95 %.1f ms, short gaps %d, concealed %.0f ms\n", steady.p95, steady.underruns, steady.concealedMs);
    CHECK(steady.p95 <= 8.0);   // the pull block alone, 5.3 ms
    CHECK(steady.underruns == 0);
}

TEST_CASE("jitter: +-10 ms for ten minutes gaps at most once a minute and keeps the buffer small") {
    const Result r = simulate(600.0, jittery(10.0, 7), dc);
    std::printf("    10 min: gaps>=10ms %d, short gaps hidden %d, re-primes %d, concealed %.0f ms, avg buffer %.1f ms, p95 %.1f ms\n",
                r.gaps10, r.underruns, r.reprimes, r.concealedMs, r.avgBufMs, r.p95);
    CHECK(r.gaps10 <= 10);
    CHECK(r.reprimes <= 1);
    CHECK(r.avgBufMs <= r.p95 + 10.0 + kBlock * 1000.0 / 48000.0);
    CHECK(r.playingAtEnd);
}

TEST_CASE("jitter: a line with 3 % spikes of up to 40 ms learns its headroom and gaps rarely") {
    auto s = std::make_shared<uint32_t>(3);
    auto spiky = [s](int i) { return 40.0 + i * 10.0 + rnd(*s) * 6.0 + (rnd(*s) < 0.03 ? rnd(*s) * 40.0 : 0.0); };
    const Result r = simulate(600.0, spiky, dc);
    std::printf("    spiky 10 min: gaps>=10ms %d, short gaps hidden %d, re-primes %d, concealed %.0f ms, avg buffer %.1f ms, p95 %.1f ms\n",
                r.gaps10, r.underruns, r.reprimes, r.concealedMs, r.avgBufMs, r.p95);
    CHECK(r.gaps10 <= 10);
    CHECK(r.reprimes == 0);
    CHECK(r.playingAtEnd);
}

TEST_CASE("jitter: a single 15 ms late packet is faded over, not re-primed, and does not click") {
    auto late = [](int i) { return 40.0 + i * 10.0 + (i == 1500 ? 15.0 : 0.0); };   // packet 1500 arrives at 15 s
    const Result r = simulate(25.0, late, sine);
    std::printf("    late packet: short gaps %d, re-primes %d, max step %.4f, concealed %.1f ms\n", r.underruns, r.reprimes, r.maxStep, r.concealedMs);
    CHECK(r.underruns >= 1);
    CHECK(r.reprimes == 0);
    CHECK(r.maxStep <= 0.1);
    CHECK(r.concealedMs < 30.0);
    CHECK(r.playingAtEnd);
}

TEST_CASE("jitter: a 500 ms outage re-primes and then plays again") {
    for (int dropped = 0; dropped < 2; ++dropped) {   // 0: TCP delivers the backlog afterwards, 1: nothing arrives meanwhile
        auto outage = [dropped](int i) {
            const double due = 40.0 + i * 10.0;
            if (due < 10000.0 || due >= 10500.0) return due;
            return dropped ? 1.0e18 : 10500.0;
        };
        const Result r = simulate(20.0, outage, sine, true);
        std::printf("    outage (%s): re-primes %d, concealed %.0f ms\n", dropped ? "lost" : "backlog", r.reprimes, r.concealedMs);
        CHECK(r.reprimes >= 1);
        CHECK(r.playingAtEnd);
        // sound again within 150 ms of the end of the outage
        double peak = 0;
        const size_t from = size_t(10.65 * 48000), to = size_t(10.8 * 48000);
        for (size_t i = from; i < to && i < r.out.size(); ++i) peak = std::max(peak, double(std::abs(r.out[i])));
        CHECK(peak > 0.3);
    }
}
