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

// Deterministic noise (music correlates like noise, not like a few sines).
float noiseAt(int64_t t, uint32_t seed) {
    uint32_t h = uint32_t(t) * 2654435761u ^ seed * 40503u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (float(h & 0xffff) / 65535.0f - 0.5f) * 0.5f;
}

struct SimTrack {
    TrackPublisher pub;
    float sign = 1.0f;
    int64_t contentDelay = 0;   // content = sign * signalAt(t - contentDelay)
    uint32_t seed = 0;          // != 0: noise material instead of signalAt
    int64_t chain = 0;          // latency of the plug-ins before the Track (the DAW lines the master up)
    float at(int64_t t) const { return sign * (seed != 0 ? noiseAt(t, seed) : signalAt(t)); }
    int aheadBlocks = 0;        // host renders this many cycles ahead (anticipative FX / ASIO-Guard)
    bool afterHub = false;      // processed after the master bus in each cycle
    bool suspended = false;
    int64_t renderedCycles = 0; // number of cycles already rendered
    std::vector<float> buf;

    void render(int64_t cycle, const std::vector<int>& sizes, const std::vector<int64_t>& times, bool playing) {
        const int n = sizes[size_t(cycle)];
        buf.resize(size_t(n));
        for (int i = 0; i < n; ++i) buf[size_t(i)] = at(times[size_t(cycle)] + i - contentDelay - chain);
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
    std::vector<float> outL, outR;         // Hub output = what you hear in your headphones
    std::vector<float> streamL, streamR;   // everything the Hub wrote to the Stream Mix
    std::vector<float> masterOut;
    int stemToCapture = -1;
    int64_t masterFxDelay = 0;   // a plug-in on the master bus above the Hub: latency in frames...
    float masterFxGain = 1.0f;   // ...and gain
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
        // master bus input: every Track sends what viewers hear (Viewers Hear / Level / Delay)
        std::vector<float> l(size_t(n), 0.0f), r(size_t(n), 0.0f);
        int64_t slowest = 0;   // delay compensation: every track reaches the master as late as the slowest chain
        for (auto& tr : tracks) slowest = std::max(slowest, tr->chain);
        for (auto& tr : tracks) {
            const SlotHeader* sh = tr->pub.slot();
            if (tr->suspended || sh == nullptr || !(sh->flags.load() & kFlagStr)) continue;
            const float g = ssdsp::dbToGain(bitsFloat(sh->strGainBits.load()));
            const int64_t d = int64_t(bitsFloat(sh->strDelayBits.load()) * 48.0f + 0.5f);
            for (int i = 0; i < n; ++i)
                l[size_t(i)] += masterFxGain * g * tr->at(times[size_t(c)] + i - tr->contentDelay - d - slowest - masterFxDelay);
        }
        r = l;
        float* io[2] = { l.data(), r.data() };
        hub.process(io, 2, n, params, playing ? times[size_t(c)] : kNoTime, playing, false);
        masterOut.insert(masterOut.end(), l.begin(), l.end());
        outL.insert(outL.end(), l.begin(), l.end());
        outR.insert(outR.end(), r.begin(), r.end());

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
        streamL.insert(streamL.end(), a.begin(), a.end());
        streamR.insert(streamR.end(), bb.begin(), bb.end());
    }

    void run(int from, int to) { for (int c = from; c < to; ++c) runCycle(c); }

    float streamPeakAfter(size_t skipFrames) const {
        float p = 0;
        for (size_t i = skipFrames; i < streamL.size(); ++i) p = std::max({ p, std::abs(streamL[i]), std::abs(streamR[i]) });
        return p;
    }
    // peak of the headphone output after skipping `skipFrames`
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

TEST_CASE("sim: You Hear off, Viewers Hear, panic and solo") {
    Sim s("gates", 600);
    SimTrack& a = s.addTrack(+1);
    SimTrack& b = s.addTrack(+1);
    s.run(0, 100);
    CHECK(s.peakAfter(s.outL.size() - 10000) > 0.5f);         // you hear both
    a.pub.mirror({ false, true, false, 0, 0, 0, 0, -1 });   // You Hear off: viewers only
    b.pub.mirror({ false, true, false, 0, 0, 0, 0, -1 });
    s.run(100, 200);
    CHECK_LT(s.peakAfter(s.outL.size() - 15000), kNull);
    CHECK(s.streamPeakAfter(s.streamL.size() - 10000) > 0.5f);   // ...and viewers still hear them

    a.pub.mirror({ true, true, false, 0, 0, 0, 0, -1 });
    s.params.panic = true;
    s.run(200, 300);
    CHECK_LT(s.streamPeakAfter(s.streamL.size() - 15000), kNull);
    CHECK(s.peakAfter(s.outL.size() - 10000) > 0.1f);   // panic never touches the headphones
    s.params.panic = false;

    CHECK(!a.pub.soloActive());
    b.pub.mirror({ false, true, true, 0, 0, 0, 0, -1 });   // the Tracks mute themselves for the viewers' solo
    CHECK(a.pub.soloActive());
}

TEST_CASE("sim: gain is smoothed (no step larger than the material allows)") {
    Sim s("ramp", 300);
    SimTrack& a = s.addTrack(+1);
    s.run(0, 100);
    const size_t m = s.outL.size();
    a.pub.mirror({ false, true, false, 0, 0, 0, 0, -1 });   // You Hear off: headphones fade out
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
    CHECK(s.streamPeakAfter(4096) > 0.8f);
    CHECK(s.streamPeakAfter(0) <= ssdsp::dbToGain(-1.0f) + 1e-5f);
}

TEST_CASE("sim: preview swaps the headphone mix for the stream mix") {
    Sim s("preview", 300);
    SimTrack& a = s.addTrack(+1);
    a.pub.mirror({ false, true, false, 0, 0, 0, 0, -1 });   // viewers only
    s.run(0, 100);
    float m = 0;
    for (size_t i = 4096; i < s.masterOut.size(); ++i) m = std::max(m, std::abs(s.masterOut[i]));
    CHECK(m == 0.0f);
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

TEST_CASE("sim: measures track and master plug-in latency") {
    for (const int64_t master : { int64_t(0), int64_t(960) }) {   // master plug-ins: none / 20 ms
        Sim s("lat", 600);
        SimTrack& backing = s.addTrack(+1);
        backing.seed = 11;                    // no plug-in latency
        SimTrack& vocal = s.addTrack(+1);
        vocal.seed = 22;
        vocal.chain = 1440;                   // vocal plug-ins: 30 ms
        s.masterFxDelay = master;
        s.masterFxGain = 0.5f;
        s.run(0, 400);
        const auto r = s.hub.measureLatencies();
        CHECK_NEAR(r.masterMs, double(master) / 48.0, 0.3);
        CHECK_NEAR(r.tracksMs, 30.0, 0.3);
        CHECK_NEAR(bitsFloat(vocal.pub.slot()->chainLatencyBits.load()), 30.0, 0.3);
        CHECK_NEAR(bitsFloat(backing.pub.slot()->chainLatencyBits.load()), 0.0, 0.3);

        // live: the backing stops, only the vocal sounds -> its remembered chain latency still counts
        backing.pub.mirror({ true, false, false, 0, 0, 0, 0, -1 });
        s.run(400, 520);
        const auto live = s.hub.measureLatencies();
        CHECK_NEAR(live.masterMs, double(master) / 48.0, 0.3);
        CHECK_NEAR(live.tracksMs, 30.0, 0.3);
    }
    {   // a Viewers Delay (sync offset) on the backing is not plug-in latency of the other tracks
        Sim s("latdelay", 600);
        SimTrack& backing = s.addTrack(+1);
        backing.seed = 11;
        backing.pub.mirror({ true, true, false, 0, 0, 40.0f, 0, -1 });
        SimTrack& vocal = s.addTrack(+1);
        vocal.seed = 22;
        vocal.chain = 1440;                   // vocal plug-ins: 30 ms
        s.run(0, 400);
        const auto r = s.hub.measureLatencies();
        CHECK_NEAR(r.masterMs, 0.0, 0.3);
        CHECK_NEAR(r.tracksMs, 30.0, 0.3);
        CHECK_NEAR(bitsFloat(vocal.pub.slot()->chainLatencyBits.load()), 30.0, 0.3);
        CHECK_NEAR(bitsFloat(backing.pub.slot()->chainLatencyBits.load()), 0.0, 0.3);
    }
    {   // one track, plug-ins on the master only
        Sim s("latone", 400);
        s.addTrack(+1).seed = 5;
        s.masterFxDelay = 4321;               // 90 ms
        s.run(0, 400);
        const auto r = s.hub.measureLatencies();
        CHECK_NEAR(r.masterMs, 4321.0 / 48.0, 0.3);
        CHECK_NEAR(r.tracksMs, 0.0, 0.3);
    }
    Sim quiet("latquiet", 100);
    quiet.run(0, 100);
    CHECK(quiet.hub.measureLatencies().masterMs < 0.0);   // nothing to compare: no estimate
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
            std::copy(in.begin(), in.end(), l.begin()); std::copy(in.begin(), in.end(), r.begin());   // master bus = the Track
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

TEST_CASE("e2e: adaptive buffer settles low (5 ms ticks, 256-frame DAW blocks) without underruns") {
    const std::string bus = uniqueBus("auto");
    HubEngine hub;
    hub.connect(bus);
    hub.prepare(48000, 256);
    hub.maintain();
    TrackPublisher tr;
    tr.connect(bus, "", 48000, 1);
    tr.prepare(48000, 1);
    ssdsp::StreamConsumer cons;
    cons.configure(bus, 0, 0.0, 48000);   // 0 = adaptive
    const double dawRate = 48000.0 * (1.0 - 80e-6);
    const int block = 256;
    double tDaw = 0, tObs = 0;
    int64_t timeline = 0;
    std::vector<float> in(block), l(block), r(block), ol(240), orr(240);
    uint64_t underAtWarm = 0;
    const double end = 180.0;
    while (tObs < end) {
        if (tDaw <= tObs) {
            for (int i = 0; i < block; ++i) in[size_t(i)] = 0.3f * float(std::sin(double((timeline + i) % 100) * 2.0 * 3.14159265358979 / 100.0));
            const float* ch[1] = { in.data() };
            tr.process(ch, 1, block, timeline, true, false, false);
            std::copy(in.begin(), in.end(), l.begin()); std::copy(in.begin(), in.end(), r.begin());   // master bus = the Track
            float* io[2] = { l.data(), r.data() };
            hub.process(io, 2, block, HubParams{}, timeline, true, false);
            timeline += block;
            tDaw += block / dawRate;
            continue;
        }
        float* o[2] = { ol.data(), orr.data() };
        cons.pull(o, 240, 0.005);
        if (tObs < 100.0 && tObs + 0.005 >= 100.0) underAtWarm = cons.status().underruns;
        tObs += 0.005;
    }
    const auto st = cons.status();
    std::printf("    adaptive target %.1f ms, fill %.1f ms, underruns %llu (at 100 s: %llu)\n", st.targetMs, st.bufferMs,
                (unsigned long long)st.underruns, (unsigned long long)underAtWarm);
    CHECK(st.autoBuffer);
    CHECK(st.underruns == underAtWarm);
    CHECK_LT(st.targetMs, 18.0);
    CHECK_NEAR(st.bufferMs, st.targetMs, 2.0);
}
