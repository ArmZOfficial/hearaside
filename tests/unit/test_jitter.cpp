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

constexpr int kBlock = 256;
constexpr double kPi = 3.14159265358979323846;

double rnd(uint32_t& s) { s = s * 1664525u + 1013904223u; return double(s >> 8) / double(1 << 24); }   // [0, 1)

struct Result {
    std::vector<float> out;          // left channel, only kept when asked
    double seconds = 0, avgBufMs = 0, p95 = 0, concealedMs = 0;
    int gaps10 = 0, underruns = 0, reprimes = 0;   // reprimes: seen from outside (primed() went false)
    int reprimesCounted = 0;
    double maxStep = 0;              // largest sample-to-sample step after the first second
    bool playingAtEnd = false;
};

// arrive(i) = arrival time (ms) of packet i; sig(frame) = what the friend sends. Packets keep their order (TCP).
Result simulate(double seconds, const std::function<double(int)>& arrive, const std::function<float(uint64_t, uint32_t)>& sig, bool keep = false,
                int kBlock = ::kBlock, uint32_t rate = 48000, int pktFrames = 0) {
    const int kPkt = pktFrames > 0 ? pktFrames : int(rate / 100);   // 10 ms packets unless asked
    auto ring = std::make_unique<ChannelRing>();
    std::atomic<uint64_t> wp{ 0 };
    FriendReader r;
    r.prepare(48000.0, kBlock);
    const double blockMs = kBlock * 1000.0 / 48000.0;
    Result res;
    std::vector<float> l(kBlock), rr(kBlock), pk(static_cast<size_t>(kPkt));
    float* o[2] = { l.data(), rr.data() };
    const int blocks = int(seconds * 1000.0 / blockMs);
    int nextPkt = 0, drawn = -1;
    double lastArrival = 0, bufSum = 0, rawNow = 0;
    int bufN = 0, quiet = 0;
    bool started = false, wasPrimed = false;
    float prev = 0;
    for (int k = 0; k < blocks; ++k) {
        const double now = k * blockMs;
        for (;;) {
            if (drawn != nextPkt) { rawNow = arrive(nextPkt); drawn = nextPkt; }   // one random draw per packet, not per block
            const double raw = rawNow, a = std::max(lastArrival, raw);
            if (raw < 1.0e17 && a > now) break;
            if (raw < 1.0e17) {   // a huge arrival time = the packet is dropped (lost, not delayed)
                for (int i = 0; i < kPkt; ++i) pk[size_t(i)] = sig(uint64_t(nextPkt) * uint64_t(kPkt) + uint64_t(i), rate);
                const float* src[1] = { pk.data() };
                ringWrite(*ring, wp, src, 1, kPkt);
                lastArrival = a;
            }
            ++nextPkt;
        }
        r.pull(*ring, wp, rate, o, kBlock);
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
    res.reprimesCounted = int(r.reprimes());
    res.concealedMs = r.concealedMs();
    res.playingAtEnd = quiet < 480 && started;
    return res;
}

// packet i is due at i * 10 ms + 40 ms; jitter uniform in +-j ms
std::function<double(int)> jittery(double j, uint32_t seed) {
    auto s = std::make_shared<uint32_t>(seed);
    return [=](int i) { return 40.0 + i * 10.0 + (rnd(*s) * 2.0 - 1.0) * j; };
}

float dc(uint64_t, uint32_t) { return 0.5f; }
float sine(uint64_t f, uint32_t rate) { return 0.5f * float(std::sin(2.0 * kPi * 440.0 * double(f) / double(rate))); }

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

// jitter +-10 ms for ten minutes: gaps <= once a minute, and the buffer is the jitter plus the block plus ~8 ms
// (2 ms safety + half a packet; the plan said p95 + 10 ms, which leaves no room for a block of its own)
void tenMinutes(const char* what, int block, uint32_t rate) {
    const Result r = simulate(600.0, jittery(10.0, 7), dc, false, block, rate);
    const double blockMs = block * 1000.0 / 48000.0;
    std::printf("    10 min %s: gaps>=10ms %d, short gaps hidden %d, re-primes %d, concealed %.0f ms, avg buffer %.1f ms, p95 %.1f ms\n",
                what, r.gaps10, r.underruns, r.reprimesCounted, r.concealedMs, r.avgBufMs, r.p95);
    CHECK(r.gaps10 <= 10);
    CHECK(r.reprimesCounted <= 1);
    CHECK(r.avgBufMs <= r.p95 + blockMs + 8.0);
    CHECK(r.playingAtEnd);
}

TEST_CASE("jitter: +-10 ms for ten minutes (block 256) gaps at most once a minute and keeps the buffer small") { tenMinutes("block 256", 256, 48000); }
TEST_CASE("jitter: the same at block 32") { tenMinutes("block 32", 32, 48000); }
TEST_CASE("jitter: the same at block 2048") { tenMinutes("block 2048", 2048, 48000); }
TEST_CASE("jitter: the same from a 44.1 kHz friend (resampler path)") { tenMinutes("44.1 kHz", 256, 44100); }

TEST_CASE("jitter: a line with 3 % spikes of up to 40 ms learns its headroom and gaps rarely") {
    auto s = std::make_shared<uint32_t>(3);
    auto spiky = [s](int i) { return 40.0 + i * 10.0 + rnd(*s) * 6.0 + (rnd(*s) < 0.03 ? rnd(*s) * 40.0 : 0.0); };
    const Result r = simulate(600.0, spiky, dc);
    std::printf("    spiky 10 min: gaps>=10ms %d, short gaps hidden %d, re-primes %d, concealed %.0f ms, avg buffer %.1f ms, p95 %.1f ms\n",
                r.gaps10, r.underruns, r.reprimesCounted, r.concealedMs, r.avgBufMs, r.p95);
    CHECK(r.gaps10 <= 10);
    CHECK(r.reprimesCounted == 0);
    CHECK(r.playingAtEnd);
}

TEST_CASE("jitter: stalls of 60-80 ms on 2 % of the packets (longer than a short gap) are learned, not repeated") {
    auto s = std::make_shared<uint32_t>(5);
    auto stalls = [s](int i) { return 40.0 + i * 10.0 + (rnd(*s) < 0.02 ? 60.0 + rnd(*s) * 20.0 : 0.0); };
    const Result r = simulate(600.0, stalls, dc);
    std::printf("    stalls 10 min: gaps>=10ms %d, short gaps hidden %d, re-primes counted %d, concealed %.0f ms, avg buffer %.1f ms, p95 %.1f ms\n",
                r.gaps10, r.underruns, r.reprimesCounted, r.concealedMs, r.avgBufMs, r.p95);
    CHECK(r.gaps10 <= 10);
    CHECK(r.p95 <= 30.0);
    CHECK(r.reprimesCounted >= 1);   // the long-gap path really ran
    CHECK(r.reprimesCounted <= 10);
    CHECK(r.playingAtEnd);
}

TEST_CASE("jitter: four 80 ms stalls 3 s apart gap at most twice (a long gap raises the target too)") {
    auto stall = [](int i) {
        const double due = 40.0 + i * 10.0;
        for (double at : { 10000.0, 13000.0, 16000.0, 19000.0 }) if (due >= at && due < at + 80.0) return at + 80.0;
        return due;
    };
    const Result r = simulate(25.0, stall, dc);
    std::printf("    4 stalls of 80 ms: gaps>=10ms %d, short gaps hidden %d, re-primes counted %d, concealed %.0f ms\n", r.gaps10, r.underruns, r.reprimesCounted, r.concealedMs);
    CHECK(r.reprimesCounted >= 1);
    CHECK(r.gaps10 <= 2);   // without learning from long gaps all four gap
    CHECK(r.playingAtEnd);
}

TEST_CASE("jitter: a cold start on a line with +-20 ms jitter gaps at most twice in the first seconds") {
    auto s = std::make_shared<uint32_t>(11);
    auto cold = [s](int i) { return 40.0 + i * 10.0 + (rnd(*s) * 2.0 - 1.0) * 20.0; };
    const Result r = simulate(5.0, cold, dc);
    std::printf("    cold start +-20 ms, 5 s: gaps>=10ms %d, short gaps hidden %d, re-primes counted %d, concealed %.0f ms\n", r.gaps10, r.underruns, r.reprimesCounted, r.concealedMs);
    CHECK(r.gaps10 <= 2);
    CHECK(r.reprimesCounted <= 1);
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

// DC at block 32 / 64: a fade-in longer than a block must not step between blocks (two late packets, the second right after the first)
void doubleLate(int block) {
    auto late = [](int i) { return 40.0 + i * 10.0 + (i == 1500 || i == 1502 ? 15.0 : 0.0); };
    const Result r = simulate(25.0, late, dc, false, block);
    std::printf("    late packets, block %d: short gaps %d, re-primes %d, max step %.4f, concealed %.1f ms\n", block, r.underruns, r.reprimes, r.maxStep, r.concealedMs);
    CHECK(r.maxStep <= 0.03);
    CHECK(r.reprimes == 0);
    CHECK(r.playingAtEnd);
}
TEST_CASE("jitter: two late packets in a row do not click at block 32") { doubleLate(32); }
TEST_CASE("jitter: two late packets in a row do not click at block 64") { doubleLate(64); }

TEST_CASE("jitter: 2048-frame packets (43 ms) with +-5 ms jitter at block 256 do not gap") {
    auto s = std::make_shared<uint32_t>(13);
    const double step = 2048.0 * 1000.0 / 48000.0;
    auto big = [s, step](int i) { return 100.0 + i * step + (rnd(*s) * 2.0 - 1.0) * 5.0; };
    const Result r = simulate(60.0, big, dc, false, kBlock, 48000, 2048);
    std::printf("    43 ms packets, 60 s: gaps>=10ms %d, short gaps hidden %d, re-primes counted %d, concealed %.0f ms, avg buffer %.1f ms, p95 %.1f ms\n",
                r.gaps10, r.underruns, r.reprimesCounted, r.concealedMs, r.avgBufMs, r.p95);
    CHECK(r.gaps10 == 0);
    CHECK(r.underruns <= 2);
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
        CHECK(r.reprimesCounted >= 1);
        CHECK(r.playingAtEnd);
        // sound again within 150 ms of the end of the outage
        double peak = 0;
        const size_t from = size_t(10.65 * 48000), to = size_t(10.8 * 48000);
        for (size_t i = from; i < to && i < r.out.size(); ++i) peak = std::max(peak, double(std::abs(r.out[i])));
        CHECK(peak > 0.3);
    }
}
