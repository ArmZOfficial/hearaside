#include "testing.h"

#include "ssdsp/consumer.h"
#include "ssdsp/dsp.h"

#include <random>
#include <vector>

using namespace ssdsp;

TEST_CASE("dsp: smoother ramps linearly without overshoot") {
    Smoother s;
    s.prepare(48000, 10.0);   // 480 samples
    s.snap(0.0f);
    s.setTarget(1.0f);
    float prev = 0.0f, maxStep = 0.0f;
    for (int i = 0; i < 480; ++i) { const float v = s.next(); maxStep = std::max(maxStep, v - prev); CHECK(v >= prev && v <= 1.0f); prev = v; }
    CHECK(prev == 1.0f);
    CHECK_NEAR(maxStep, 1.0 / 480.0, 1e-5);
    CHECK(!s.isSmoothing());
}

TEST_CASE("dsp: equal-power pan") {
    float l, r;
    panGains(0.0f, l, r);   CHECK(l == 1.0f && r == 1.0f);
    panGains(1.0f, l, r);   CHECK_NEAR(l, 0.0, 1e-6); CHECK_NEAR(r, 1.41421356, 1e-5);
    panGains(-1.0f, l, r);  CHECK_NEAR(r, 0.0, 1e-6);
    panGains(0.5f, l, r);   CHECK_NEAR(l * l + r * r, 2.0, 1e-5);
}

TEST_CASE("dsp: limiter never exceeds ceiling and is transparent below it") {
    Limiter lim;
    lim.prepare(48000, 1.0, 100.0);
    const float ceiling = dbToGain(-1.0f);
    lim.setCeiling(ceiling);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> d(-4.0f, 4.0f);
    std::vector<float> l(512), r(512);
    float maxOut = 0;
    for (int b = 0; b < 400; ++b) {
        for (int i = 0; i < 512; ++i) { l[size_t(i)] = d(rng) * (b % 7 == 0 ? 1.0f : 0.3f); r[size_t(i)] = d(rng) * 0.5f; }
        lim.process(l.data(), r.data(), 512, true);
        for (int i = 0; i < 512; ++i) maxOut = std::max({ maxOut, std::abs(l[size_t(i)]), std::abs(r[size_t(i)]) });
    }
    CHECK(maxOut <= ceiling + 1e-6f);

    // quiet signal passes unchanged (just delayed by the lookahead)
    Limiter q;
    q.prepare(48000, 1.0, 100.0);
    q.setCeiling(ceiling);
    std::vector<float> in(2048), ql(2048), qr(2048);
    for (int i = 0; i < 2048; ++i) in[size_t(i)] = 0.5f * std::sin(float(i) * 0.05f);
    ql = in; qr = in;
    q.process(ql.data(), qr.data(), 2048, true);
    const int la = q.latency();
    double err = 0;
    for (int i = la; i < 2048; ++i) err = std::max(err, double(std::abs(ql[size_t(i)] - in[size_t(i - la)])));
    CHECK_LT(err, 1e-6);
}

TEST_CASE("dsp: loudness of 1 kHz sine at 0 dBFS in both channels is ~0 LUFS") {
    LoudnessMeter m;
    m.prepare(48000);
    std::vector<float> x(48000 * 4);
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(2.0f * 3.14159265f * 1000.0f * float(i) / 48000.0f);
    m.process(x.data(), x.data(), int(x.size()));
    // BS.1770: a 0 dBFS 1 kHz sine in one channel reads -3.01 LUFS, so two channels read ~0.
    CHECK_NEAR(m.momentary(), 0.0, 0.1);
    CHECK_NEAR(m.shortTerm(), 0.0, 0.1);
}

TEST_CASE("dsp: loudness at 44.1 kHz, -20 dBFS stereo sine") {
    LoudnessMeter m;
    m.prepare(44100);
    std::vector<float> x(44100 * 4);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.1f * std::sin(2.0f * 3.14159265f * 1000.0f * float(i) / 44100.0f);
    m.process(x.data(), x.data(), int(x.size()));
    CHECK_NEAR(m.shortTerm(), -20.0, 0.1);
}

// Simulates DAW (audio-interface clock, irregular blocks) -> consumer (system clock, 10 ms ticks
// with jitter) for 4 hours at +/-200 ppm drift and checks the buffer stays near its target.
static void simulateDrift(double ppm, int dawBlock, uint32_t dawRate, uint32_t outRate, double& worstErrMs, int& underruns) {
    DriftController ctl;
    // same minimum as StreamConsumer::resync(): large host blocks need room for the sawtooth
    const double target = std::max(0.030 * dawRate, 1.5 * dawBlock + 0.012 * dawRate);
    ctl.reset(target);
    double fill = target;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-0.002, 0.002);
    const double producerPeriod = double(dawBlock) / (dawRate * (1.0 + ppm * 1e-6));   // DAW clock runs fast/slow
    double tProd = 0, tCons = 0.010, lastCons = 0;
    double frac = 0, filtered = target;
    worstErrMs = 0; underruns = 0;
    const double end = 4 * 3600.0;
    while (tCons < end) {
        if (tProd <= tCons) { fill += dawBlock; tProd += producerPeriod; continue; }
        const double now = tCons + jitter(rng) * 0.25;
        const double dt = now - lastCons; lastCons = now;
        const double c = ctl.update(fill, std::max(1e-4, dt));
        const int ppmQ = int(std::lround(c * 1e6));
        const double ratio = double(dawRate) / outRate * (1.0 + ppmQ * 1e-6);
        const double want = 480.0 * outRate / 48000.0 * ratio + frac;
        const double take = std::floor(want);
        frac = want - take;
        // the controller (and the consumer's status) look at the fill before the read
        filtered += (fill - filtered) * (1.0 - std::exp(-dt / 2.0));
        if (fill < take) ++underruns;
        fill -= take;
        if (tCons > 120.0) worstErrMs = std::max(worstErrMs, std::abs(filtered - target) * 1000.0 / dawRate);
        tCons += 0.010;
    }
}

TEST_CASE("drift: PI controller holds buffer within +/-2 ms over 4 h (+200 ppm, 256 blocks)") {
    double err; int under;
    simulateDrift(+200, 256, 48000, 48000, err, under);
    CHECK_LT(err, 2.0 + 256.0 / 48.0 / 2.0);   // filtered fill still contains half a block of sawtooth
    CHECK(under == 0);
}

TEST_CASE("drift: -200 ppm, 44.1k -> 48k, 1024 blocks") {
    double err; int under;
    simulateDrift(-200, 1024, 44100, 48000, err, under);
    CHECK_LT(err, 2.0 + 1024.0 / 44.1 / 2.0);
    CHECK(under == 0);
}

TEST_CASE("resampler: ratio control and steady output length") {
    VarResampler rs(44100, 48000);
    std::vector<float> in(4096), out(480), out2(480);
    for (size_t i = 0; i < in.size(); ++i) in[i] = std::sin(float(i) * 0.01f);
    const float* ip[2] = { in.data(), in.data() };
    float* op[2] = { out.data(), out2.data() };
    uint64_t consumedTotal = 0;
    for (int k = 0; k < 200; ++k) {
        uint32_t inN = rs.inputFor(480), outN = 480;
        rs.process(ip, inN, op, outN);
        CHECK(outN == 480);
        consumedTotal += inN;
    }
    CHECK_NEAR(double(consumedTotal) / (200.0 * 480.0), 44100.0 / 48000.0, 0.002);
    rs.setCorrectionPpm(5000);
    consumedTotal = 0;
    for (int k = 0; k < 400; ++k) { uint32_t inN = rs.inputFor(480), outN = 480; rs.process(ip, inN, op, outN); consumedTotal += inN; }
    CHECK_NEAR(double(consumedTotal) / (400.0 * 480.0), 44100.0 / 48000.0 * 1.005, 0.001);
}
