// Host simulations for the Track publisher + Hub engine: null tests under the host behaviours
// described in plan sections 4.3 / 6.4 / 8 (block sizes, ahead processing, ordering, seek...).
#include "testing.h"

#include "ssdsp/consumer.h"
#include "ssengine/engine.h"

#include <map>
#include <memory>
#include <random>
#include <vector>

using namespace ssengine;
using namespace ssbus;

namespace {

std::string uniqueBus(const char* tag) {
    return std::string("sim_") + tag + "_" + std::to_string(currentPid()) + "_" + std::to_string(nowNs() % 1000000);
}

// Deterministic programme material as a function of the timeline sample.
float signalAt(int64_t t) {
    const double x = double(t);
    return float(0.25 * std::sin(x * 0.0131) + 0.15 * std::sin(x * 0.0577 + 1.0) + 0.05 * std::sin(x * 0.31));
}

struct SimTrack {
    TrackPublisher pub;
    float sign = 1.0f;
    int64_t contentDelay = 0;   // content = sign * signalAt(t - contentDelay)
    int aheadBlocks = 0;        // host renders this many cycles ahead (anticipative FX / ASIO-Guard)
    bool afterHub = false;      // processed after the master bus in each cycle
    bool suspended = false;
    int64_t renderedCycles = 0; // number of cycles already rendered
    std::vector<float> buf;

    void render(int64_t cycle, const std::vector<int>& sizes, const std::vector<int64_t>& times, bool playing) {
        const int n = sizes[size_t(cycle)];
        buf.resize(size_t(n));
        for (int i = 0; i < n; ++i) buf[size_t(i)] = sign * signalAt(times[size_t(cycle)] + i - contentDelay);
        const float* ch[1] = { buf.data() };
        pub.process(ch, 1, n, times[size_t(cycle)], playing, false, false);
    }
};

struct Sim {
    std::string bus;
    HubEngine hub;
    std::vector<std::unique_ptr<SimTrack>> tracks;
    HubParams params;
    std::vector<int> sizes;
    std::vector<int64_t> times;
    bool playing = true;
    uint64_t streamRead = 0;
    bool streamCursorInit = false;
    std::vector<float> outL, outR;   // everything the Hub wrote to the Stream Mix
    std::vector<float> masterOut;
    int stemToCapture = -1;
    std::vector<float> stemL;

    explicit Sim(const char* tag, int cycles, int blockMin = 256, int blockMax = 256, uint32_t seed = 7) : bus(uniqueBus(tag)) {
        REQUIRE_OK(hub.connect(bus));
        hub.prepare(48000, 1024);
        hub.maintain();
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> d(blockMin, blockMax);
        int64_t t = 100000;
        for (int c = 0; c < cycles + 64; ++c) { const int n = d(rng); sizes.push_back(n); times.push_back(t); t += n; }
    }
    static void REQUIRE_OK(bool b) { if (!b) std::printf("  (hub connect failed)\n"); }

    SimTrack& addTrack(float sign, int ahead = 0, bool afterHub = false) {
        auto t = std::make_unique<SimTrack>();
        t->sign = sign; t->aheadBlocks = ahead; t->afterHub = afterHub;
        t->pub.connect(bus, "", 48000, 1);
        t->pub.prepare(48000, 1);
        tracks.push_back(std::move(t));
        return *tracks.back();
    }

    // Seek: host restarts the timeline at newTime from cycle `from` on; ahead-rendered audio is
    // discarded by the host and re-rendered.
    void seek(int from, int64_t newTime) {
        int64_t t = newTime;
        for (size_t c = size_t(from); c < times.size(); ++c) { times[c] = t; t += sizes[c]; }
        for (auto& tr : tracks) tr->renderedCycles = std::min<int64_t>(tr->renderedCycles, from);
    }

    void runCycle(int c) {
        auto renderTrack = [&](SimTrack& tr) {
            if (tr.suspended) { tr.renderedCycles = std::max<int64_t>(tr.renderedCycles, c + 1); return; }
            const int64_t until = c + 1 + tr.aheadBlocks;
            while (tr.renderedCycles < until) { tr.render(tr.renderedCycles, sizes, times, playing); ++tr.renderedCycles; }
        };
        for (auto& tr : tracks) if (!tr->afterHub) renderTrack(*tr);

        const int n = sizes[size_t(c)];
        std::vector<float> l(size_t(n), 0.0f), r(size_t(n), 0.0f);
        float* io[2] = { l.data(), r.data() };
        hub.process(io, 2, n, params, playing ? times[size_t(c)] : kNoTime, playing, false);
        masterOut.insert(masterOut.end(), l.begin(), l.end());

        for (auto& tr : tracks) if (tr->afterHub) renderTrack(*tr);

        // capture what the Hub wrote this cycle
        BusLayout* b = hub.bus();
        const uint64_t w = b->streamHeader.writePos[0].load();
        if (!streamCursorInit) { streamRead = w - uint64_t(n); streamCursorInit = true; }
        std::vector<float> a(size_t(w - streamRead)), bb(size_t(w - streamRead));
        float* d[2] = { a.data(), bb.data() };
        ringReadAt(b->streamAudio[0], b->streamHeader.writePos[0], streamRead, d, uint32_t(w - streamRead));
        if (stemToCapture >= 0) {
            std::vector<float> s0(a.size()), s1(a.size());
            float* ds[2] = { s0.data(), s1.data() };
            ringReadAt(b->streamAudio[1 + stemToCapture], b->streamHeader.writePos[1 + stemToCapture], streamRead, ds, uint32_t(a.size()));
            stemL.insert(stemL.end(), s0.begin(), s0.end());
        }
        streamRead = w;
        outL.insert(outL.end(), a.begin(), a.end());
        outR.insert(outR.end(), bb.begin(), bb.end());
    }

    void run(int from, int to) { for (int c = from; c < to; ++c) runCycle(c); }

    // peak of the stream output after skipping `skipFrames`
    float peakAfter(size_t skipFrames) const {
        float p = 0;
        for (size_t i = skipFrames; i < outL.size(); ++i) p = std::max({ p, std::abs(outL[i]), std::abs(outR[i]) });
        return p;
    }
    float peakRange(size_t from, size_t to) const {
        float p = 0;
        for (size_t i = from; i < std::min(to, outL.size()); ++i) p = std::max({ p, std::abs(outL[i]), std::abs(outR[i]) });
        return p;
    }
};

constexpr float kNull = 3.2e-5f;   // -90 dBFS

} // namespace

TEST_CASE("sim: null test, two realtime tracks, fixed 256 blocks") {
    Sim s("null", 400);
    s.addTrack(+1);
    s.addTrack(-1);
    s.run(0, 400);
    CHECK(s.peakRange(0, 4000) < 1.0f);   // something happened
    CHECK_LT(s.peakAfter(2048), kNull);
}

TEST_CASE("sim: polarity check - same sign doubles (stream really carries audio)") {
    Sim s("sum", 200);
    s.addTrack(+1);
    s.addTrack(+1);
    s.run(0, 200);
    CHECK(s.peakAfter(4800) > 0.5f);
}

TEST_CASE("sim: null test with irregular block sizes (FL Studio style)") {
    Sim s("irregular", 600, 31, 900, 3);
    s.addTrack(+1);
    s.addTrack(-1);
    s.run(0, 600);
    CHECK_LT(s.peakAfter(4096), kNull);
}

TEST_CASE("sim: ahead processing (8 blocks) aligned through timeline tags") {
    Sim s("ahead", 500);
    s.addTrack(+1, 8);      // backing rendered ahead
    s.addTrack(-1, 0);      // live input, realtime
    s.run(0, 500);
    CHECK_LT(s.peakAfter(4096), kNull);
}

TEST_CASE("sim: ahead processing with irregular blocks and a seek") {
    Sim s("aheadseek", 900, 64, 600, 11);
    s.addTrack(+1, 5);
    s.addTrack(-1, 0);
    s.run(0, 400);
    const size_t mark = s.outL.size();
    s.seek(400, 5000000);
    s.run(400, 900);
    CHECK_LT(s.peakRange(4096, mark), kNull);
    CHECK_LT(s.peakAfter(mark + 4096), kNull);   // re-locked after the seek
}

TEST_CASE("sim: track processed after the master needs sync safety 1") {
    {
        Sim s("after0", 300);
        s.addTrack(+1);
        s.addTrack(-1, 0, true);
        s.run(0, 300);
        CHECK(s.peakAfter(4096) > 1e-3f);   // documented 1-block offset without sync safety
    }
    {
        Sim s("after1", 300);
        s.params.syncSafety = 1;
        s.addTrack(+1);
        s.addTrack(-1, 0, true);
        s.run(0, 300);
        CHECK_LT(s.peakAfter(4096), kNull);
    }
}

TEST_CASE("sim: transport stopped (no timeline) still nulls for tracks before the master") {
    Sim s("stopped", 300);
    s.playing = false;
    s.addTrack(+1);
    s.addTrack(-1);
    s.run(0, 300);
    CHECK_LT(s.peakAfter(4096), kNull);
}

TEST_CASE("sim: suspended track is re-anchored when it resumes") {
    Sim s("suspend", 600);
    s.addTrack(+1);
    SimTrack& b = s.addTrack(-1);
    s.run(0, 200);
    b.suspended = true;
    s.run(200, 300);
    b.suspended = false;
    const size_t mark = s.outL.size();
    s.run(300, 600);
    CHECK_LT(s.peakAfter(mark + 4096), kNull);
    CHECK(s.peakRange(mark - 20000, mark) > 0.1f);   // while suspended only A was audible
}

TEST_CASE("sim: stream delay compensates a later copy") {
    Sim s("delay", 500);
    SimTrack& a = s.addTrack(+1);
    SimTrack& b = s.addTrack(-1);
    b.contentDelay = 480;                       // B carries A's audio 10 ms late
    s.run(0, 50);
    a.pub.mirror({ true, true, false, 0.0f, 0.0f, 10.0f, 0.0f, -1 });   // delay A by 10 ms
    s.run(50, 500);
    CHECK_LT(s.peakAfter(48000), kNull);
}

TEST_CASE("sim: STR off, panic and solo") {
    Sim s("gates", 900);
    SimTrack& a = s.addTrack(+1);
    SimTrack& b = s.addTrack(+1);
    s.run(0, 100);
    a.pub.mirror({ true, false, false, 0, 0, 0, 0, -1 });   // A not sent
    b.pub.mirror({ true, false, false, 0, 0, 0, 0, -1 });   // B not sent
    s.run(100, 200);
    size_t m = s.outL.size();
    CHECK_LT(s.peakAfter(m - 15000), kNull);

    a.pub.mirror({ false, true, false, 0, 0, 0, 0, -1 });   // MON off, STR on -> viewers hear it
    s.run(200, 300);
    CHECK(s.peakAfter(s.outL.size() - 10000) > 0.1f);

    s.params.panic = true;
    s.run(300, 400);
    CHECK_LT(s.peakAfter(s.outL.size() - 15000), kNull);
    s.params.panic = false;

    b.pub.mirror({ true, true, true, 0, 0, 0, 0, -1 });     // solo B on the stream side
    a.pub.mirror({ true, true, false, 0, 0, 0, 0, -1 });
    s.run(400, 500);
    // only B is heard -> half the amplitude of A+B
    const float soloPeak = s.peakAfter(s.outL.size() - 15000);
    b.pub.mirror({ true, true, false, 0, 0, 0, 0, -1 });
    s.run(500, 600);
    const float bothPeak = s.peakAfter(s.outL.size() - 15000);
    CHECK_NEAR(bothPeak / std::max(soloPeak, 1e-6f), 2.0, 0.05);
}

TEST_CASE("sim: gain is smoothed (no step larger than the material allows)") {
    Sim s("ramp", 300);
    SimTrack& a = s.addTrack(+1);
    s.run(0, 100);
    const size_t m = s.outL.size();
    a.pub.mirror({ true, false, false, 0, 0, 0, 0, -1 });
    s.run(100, 300);
    float maxJump = 0;
    for (size_t i = m; i < s.outL.size(); ++i) maxJump = std::max(maxJump, std::abs(s.outL[i] - s.outL[i - 1]));
    CHECK_LT(maxJump, 0.05f);   // material slope is ~0.03 per sample; a hard cut would jump ~0.45
}

TEST_CASE("sim: stems carry only their tracks") {
    Sim s("stems", 200);
    SimTrack& a = s.addTrack(+1);
    s.addTrack(-1);   // B cancels A on the main mix but is not on the stem
    a.pub.mirror({ true, true, false, 0, 0, 0, 0, 2 });
    s.stemToCapture = 2;
    s.run(0, 200);
    CHECK_LT(s.peakAfter(4096), kNull);
    float stemPeak = 0;
    for (size_t i = 4096; i < s.stemL.size(); ++i) stemPeak = std::max(stemPeak, std::abs(s.stemL[i]));
    CHECK(stemPeak > 0.2f);
}

TEST_CASE("sim: sample-rate mismatch excludes the track and reports it") {
    Sim s("rate", 200);
    s.addTrack(+1);
    SimTrack& b = s.addTrack(+1);
    b.pub.prepare(44100, 1);
    s.run(0, 200);
    CHECK((b.pub.slot()->hubStatus.load() & kHubStatusRateMismatch) != 0);
    CHECK_NEAR(s.peakAfter(4096), 0.45, 0.06);   // only A
}

TEST_CASE("sim: limiter keeps the stream under the ceiling") {
    Sim s("limit", 300);
    for (int k = 0; k < 6; ++k) s.addTrack(+1);   // ~2.7 peak
    s.params.ceilingDb = -1.0f;
    s.run(0, 300);
    CHECK(s.peakAfter(0) <= ssdsp::dbToGain(-1.0f) + 1e-5f);
}

TEST_CASE("sim: preview swaps the master output for the stream mix") {
    Sim s("preview", 300);
    s.addTrack(+1);
    s.run(0, 100);
    float m = 0;
    for (float v : s.masterOut) m = std::max(m, std::abs(v));
    CHECK(m == 0.0f);   // master input was silent in the simulation
    s.params.preview = true;
    s.run(100, 300);
    m = 0;
    for (size_t i = s.masterOut.size() - 10000; i < s.masterOut.size(); ++i) m = std::max(m, std::abs(s.masterOut[i]));
    CHECK(m > 0.2f);
}

TEST_CASE("sim: second hub on the same bus is secondary and passes audio through") {
    Sim s("twohubs", 50);
    HubEngine second;
    second.connect(s.bus);
    second.prepare(48000, 1024);
    second.maintain();
    CHECK(s.hub.role() == HubEngine::Role::Owner);
    CHECK(second.role() == HubEngine::Role::Secondary);
    std::vector<float> l(256, 0.3f), r(256, 0.3f);
    float* io[2] = { l.data(), r.data() };
    HubParams p; p.preview = true;
    second.process(io, 2, 256, p, 0, true, false);
    CHECK(l[100] == 0.3f);
}

TEST_CASE("sim: duplicated track gets a fresh uuid") {
    const std::string bus = uniqueBus("dup");
    HubEngine hub; hub.connect(bus);
    TrackPublisher a, b;
    a.connect(bus, "same-uuid", 48000, 2);
    b.connect(bus, "same-uuid", 48000, 2);
    CHECK(a.uuid() == "same-uuid");
    CHECK(b.uuid() != "same-uuid" && b.status() == TrackPublisher::Status::Connected);
}

TEST_CASE("e2e: hub -> consumer (OBS side) at 44.1k -> 48k with drift, no underruns") {
    const std::string bus = uniqueBus("e2e");
    HubEngine hub;
    hub.connect(bus);
    hub.prepare(44100, 512);
    hub.maintain();
    TrackPublisher tr;
    tr.connect(bus, "", 44100, 1);
    tr.prepare(44100, 1);

    ssdsp::StreamConsumer cons;
    cons.configure(bus, 0, 30.0, 48000);

    const double dawRate = 44100.0 * (1.0 + 150e-6);   // interface clock 150 ppm fast
    const int block = 256;
    double tDaw = 0, tObs = 0;
    int64_t timeline = 0;
    std::vector<float> in(block), l(block), r(block), ol(480), orr(480);
    std::vector<float> captured;
    uint64_t lastUnder = 0;
    const double end = 120.0;   // two simulated minutes
    while (tObs < end) {
        if (tDaw <= tObs) {
            // 441 Hz has an exact 100-sample period at 44.1 kHz; reducing the phase keeps float
            // precision (a raw float(timeline) argument reaches ~3e5 rad and adds its own jitter)
            for (int i = 0; i < block; ++i) in[size_t(i)] = 0.5f * float(std::sin(double((timeline + i) % 100) * 2.0 * 3.14159265358979 / 100.0));
            const float* ch[1] = { in.data() };
            tr.process(ch, 1, block, timeline, true, false, false);
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            float* io[2] = { l.data(), r.data() };
            hub.process(io, 2, block, HubParams{}, timeline, true, false);
            timeline += block;
            tDaw += block / dawRate;
            continue;
        }
        float* o[2] = { ol.data(), orr.data() };
        cons.pull(o, 480, 0.010);
        if (tObs > 10.0) captured.insert(captured.end(), ol.begin(), ol.end());
        if (tObs > 10.0 && tObs < 10.02) lastUnder = cons.status().underruns;
        tObs += 0.010;
    }
    const auto st = cons.status();
    CHECK(st.connected && st.hubAlive && st.streaming);
    CHECK(st.underruns == lastUnder);
    CHECK_NEAR(st.bufferMs, st.targetMs, 3.0);
    CHECK_NEAR(st.driftPpm, 150.0, 60.0);
    // output should be a clean sine of amplitude 0.5 (limiter ceiling -1 dBFS does not touch it)
    float pk = 0;
    double maxStep = 0;
    for (size_t i = 1; i < captured.size(); ++i) {
        pk = std::max(pk, std::abs(captured[i]));
        maxStep = std::max(maxStep, double(std::abs(captured[i] - captured[i - 1])));
    }
    CHECK_NEAR(pk, 0.5, 0.02);
    CHECK_LT(maxStep, 0.5 * 2 * 3.14159265 * 441.0 / 48000.0 * 1.2);   // no clicks
}
