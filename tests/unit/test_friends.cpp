// The friends room in the Hub engine (S2 + S4): friends reach the headphones and the Stream Mix,
// Line up delays the Stream Mix so every voice lands on the beat, panic and the route decide
// who hears what.
#include "testing.h"

#include "ssengine/engine.h"

#include <memory>
#include <vector>

using namespace ssengine;
using namespace ssbus;

namespace {

std::string uniqueBus(const char* tag) {
    return std::string("fr_") + tag + "_" + std::to_string(currentPid()) + "_" + std::to_string(nowNs() % 1000000);
}

float noiseAt(int64_t t, uint32_t seed) {
    uint32_t h = uint32_t(t) * 2654435761u ^ seed * 40503u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (float(h & 0xffff) / 65535.0f - 0.5f) * 0.5f;
}

constexpr uint32_t kMusic = 11, kVoice = 29, kVoice2 = 47;
constexpr int kRate = 48000;

// A DAW cycle by cycle: the master bus carries `music`, friends write packets like a browser does.
struct Room {
    std::string bus;
    HubEngine hub;
    HubParams params;
    int64_t t = 0;              // global frame counter (the DAW clock; friends use it too)
    int block = 256;
    bool music = true;
    std::vector<float> phones, stream;   // what came out (headphones L, Stream Mix L), one entry per frame
    std::vector<float> friendMix;        // what friends hear
    uint64_t streamRead = 0, mixRead = 0;
    bool readInit = false;

    struct Friend {
        uint32_t id = 0, seed = 0;
        int64_t netFrames = 0;        // network + browser: a finished 20 ms packet reaches the Hub this much later
        double measuredMs = -1.0;     // what the share server measures: how late the AVERAGE sample is when it arrives
                                      // (network + half a packet); < 0 = not measured
        int64_t sent = 0;             // friend frames written so far
        bool open = false;
        uint32_t rate = kRate;
    };
    Friend friends[kMaxFriends];

    explicit Room(const char* tag) : bus(uniqueBus(tag)) {
        hub.connect(bus);
        hub.prepare(kRate, 1024);
        hub.maintain();
        BusLayout* b = hub.bus();
        b->streamHeader.sampleRate.store(kRate);
    }

    // the friend sings along with the music: the voice at friend time i belongs to music time i
    void openFriend(int j, uint32_t seed, double netMs, bool measured = true) {
        Friend& f = friends[j];
        f.id = uint32_t(100 + j); f.seed = seed; f.netFrames = int64_t(netMs * kRate / 1000.0 + 0.5);
        f.measuredMs = measured ? netMs + 10.0 : -1.0; f.open = true; f.sent = (t / 960) * 960;
        FriendHeader& h = hub.bus()->friends[j];
        h.id.store(f.id);
        h.sampleRate.store(f.rate);
        h.delayBits.store(floatBits(float(f.measuredMs)));
        h.route.store(kRouteDirect);
        h.state.store(kFriendLive);
    }
    void closeFriend(int j) { friends[j].open = false; hub.bus()->friends[j].state.store(kFriendOffline); }

    void cycle() {
        BusLayout* b = hub.bus();
        // friends: 20 ms packets as the clock passes
        for (int j = 0; j < kMaxFriends; ++j) {
            Friend& f = friends[j];
            if (!f.open) continue;
            FriendHeader& h = b->friends[j];
            h.heartbeatNs.store(nowNs());
            while (f.sent + 960 + f.netFrames <= t) {   // a packet arrives once its last frame was captured plus the network
                std::vector<float> pk(960);
                for (int i = 0; i < 960; ++i) pk[size_t(i)] = noiseAt(f.sent + i, f.seed);
                const float* src[1] = { pk.data() };
                ringWrite(b->friendAudio[j], h.writePos, src, 1, 960);
                f.sent += 960;
            }
        }
        std::vector<float> l(static_cast<size_t>(block), 0.0f), r(static_cast<size_t>(block), 0.0f);
        for (int i = 0; i < block; ++i) l[size_t(i)] = music ? noiseAt(t + i, kMusic) : 0.0f;
        r = l;
        float* io[2] = { l.data(), r.data() };
        hub.process(io, 2, block, params, t, true, false);
        phones.insert(phones.end(), l.begin(), l.end());
        // capture the Stream Mix and what friends hear
        const uint64_t w = b->streamHeader.writePos[0].load();
        if (!readInit) { streamRead = w - uint64_t(block); mixRead = b->friendMixWrite.load() - uint64_t(block); readInit = true; }
        std::vector<float> a(size_t(w - streamRead)), bb(a.size());
        float* d[2] = { a.data(), bb.data() };
        ringReadAt(b->streamAudio[0], b->streamHeader.writePos[0], streamRead, d, uint32_t(a.size()));
        streamRead = w;
        stream.insert(stream.end(), a.begin(), a.end());
        const uint64_t mw = b->friendMixWrite.load();
        std::vector<float> m(size_t(mw - mixRead)), mb(m.size());
        float* dm[2] = { m.data(), mb.data() };
        ringReadAt(b->friendMixAudio, b->friendMixWrite, mixRead, dm, uint32_t(m.size()));
        mixRead = mw;
        friendMix.insert(friendMix.end(), m.begin(), m.end());
        t += block;
    }
    void run(double seconds) { for (int c = 0, n = int(seconds * kRate / block); c < n; ++c) cycle(); }

    // normalised correlation of x[from, from+len) with noise(seed) delayed by `lag` frames
    static double corr(const std::vector<float>& x, int64_t offset, size_t from, size_t len, uint32_t seed, int64_t lag) {
        double xy = 0, xx = 0, yy = 0;
        for (size_t i = from; i < from + len && i < x.size(); ++i) {
            const double a = x[i], y = noiseAt(offset + int64_t(i) - lag, seed);
            xy += a * y; xx += a * a; yy += y * y;
        }
        return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0.0;
    }
    // lag (frames) in [lo, hi] where x best matches noise(seed), -1 when nothing correlates
    static int64_t bestLag(const std::vector<float>& x, int64_t offset, size_t from, size_t len, uint32_t seed, int64_t lo, int64_t hi, double* score = nullptr) {
        int64_t best = -1; double bs = 0.15;
        for (int64_t k = lo; k <= hi; ++k) { const double c = corr(x, offset, from, len, seed, k); if (c > bs) { bs = c; best = k; } }   // white noise: every lag, one frame apart
        if (score) *score = bs;
        return best;
    }
    // frame index in the output arrays <-> noise time: output frame i was processed at DAW time (t0 + i), t0 = 0 here
    static double ms(int64_t frames) { return double(frames) * 1000.0 / kRate; }
};

} // namespace

TEST_CASE("friends: a live friend reaches the headphones and the Stream Mix; the headphones are not delayed") {
    Room r("basic");
    r.music = false;
    r.openFriend(0, kVoice, 90.0);
    r.run(2.0);
    CHECK(r.hub.friendInfo(0).playing.load() == 1);
    const size_t from = size_t(1.2 * kRate), len = size_t(0.06 * kRate);
    double sp = 0, sh = 0;
    const int64_t lagPhones = Room::bestLag(r.phones, 0, from, len, kVoice, 0, int64_t(0.6 * kRate), &sp);
    const int64_t lagStream = Room::bestLag(r.stream, 0, from, len, kVoice, 0, int64_t(0.6 * kRate), &sh);
    CHECK(lagPhones >= 0);
    CHECK(lagStream >= 0);
    // with Line up off both hear the friend as it arrives: 90 ms of network + the Hub's small jitter buffer
    // (cycles are 5.3 ms long, so a packet is taken up to one cycle late)
    CHECK_NEAR(Room::ms(lagPhones), 90.0 + 10.0 + double(bitsFloat(r.hub.friendInfo(0).bufferMsBits.load())), 7.0);
    CHECK_NEAR(Room::ms(lagStream), Room::ms(lagPhones), 2.0);
    CHECK(sp > 0.8);   // the same voice (resampled, so not bit for bit)
    CHECK(sh > 0.8);
}

TEST_CASE("friends: you hear / viewers hear / panic are separate") {
    Room r("gates");
    r.music = false;
    r.openFriend(0, kVoice, 40.0);
    r.run(1.5);
    const size_t m0 = r.phones.size();
    r.hub.friendControl(0).flags.store(HubEngine::kFriendStr);   // viewers only
    r.run(1.0);
    float pPhones = 0, pStream = 0;
    for (size_t i = m0 + 8000; i < r.phones.size(); ++i) { pPhones = std::max(pPhones, std::abs(r.phones[i])); pStream = std::max(pStream, std::abs(r.stream[i])); }
    CHECK_LT(pPhones, 1e-4);
    CHECK(pStream > 0.05f);

    const size_t m1 = r.phones.size();
    r.hub.friendControl(0).flags.store(HubEngine::kFriendMon | HubEngine::kFriendStr);
    r.params.panic = true;
    r.run(1.0);
    pPhones = pStream = 0;
    for (size_t i = m1 + 8000; i < r.phones.size(); ++i) { pPhones = std::max(pPhones, std::abs(r.phones[i])); pStream = std::max(pStream, std::abs(r.stream[i])); }
    CHECK(pPhones > 0.05f);       // panic never touches the headphones
    CHECK_LT(pStream, 1e-4);      // and cuts the viewers at once
}

TEST_CASE("friends: gain and pan apply to the viewers' side") {
    Room r("pan");
    r.music = false;
    r.openFriend(0, kVoice, 30.0);
    r.hub.friendControl(0).gainBits.store(floatBits(-6.0f));
    r.hub.friendControl(0).panBits.store(floatBits(1.0f));   // hard right
    r.run(2.0);
    // read the right channel of the stream: pan 1.0 = all right, none left
    BusLayout* b = r.hub.bus();
    std::vector<float> l(4096), rr(4096);
    float* d[2] = { l.data(), rr.data() };
    const uint64_t w = b->streamHeader.writePos[0].load();
    ringReadAt(b->streamAudio[0], b->streamHeader.writePos[0], w - 4096, d, 4096);
    float pl = 0, pr = 0;
    for (int i = 0; i < 4096; ++i) { pl = std::max(pl, std::abs(l[size_t(i)])); pr = std::max(pr, std::abs(rr[size_t(i)])); }
    CHECK_LT(pl, 1e-4);
    CHECK(pr > 0.05f);
}

TEST_CASE("friends: Line up puts the music and every friend on the same beat") {
    Room r("lineup");
    r.openFriend(0, kVoice, 100.0);
    r.openFriend(1, kVoice2, 250.0);   // the slowest
    r.hub.setLineUp(true, 600.0f);
    r.run(4.0);
    const double D = r.hub.lineUpMs();
    const double slowestTot = double(bitsFloat(r.hub.friendInfo(1).totalMsBits.load()));
    CHECK(r.hub.lineUpSlowest() == 1);
    CHECK_NEAR(D, slowestTot, 3.0);
    CHECK(D > 250.0);

    // the stream: music delayed by D, both friends delayed so they land on the same beat as the music
    const size_t from = size_t(3.0 * kRate), len = size_t(0.06 * kRate);
    const int64_t lag = int64_t(D * kRate / 1000.0 + 0.5);
    const int64_t lo = lag - 600, hi = lag + 600;
    double sm = 0, s1 = 0, s2 = 0;
    const int64_t lagMusic = Room::bestLag(r.stream, 0, from, len, kMusic, lo, hi, &sm);
    const int64_t lagA = Room::bestLag(r.stream, 0, from, len, kVoice, lo, hi, &s1);
    const int64_t lagB = Room::bestLag(r.stream, 0, from, len, kVoice2, lo, hi, &s2);
    CHECK(lagMusic >= 0); CHECK(lagA >= 0); CHECK(lagB >= 0);
    CHECK_NEAR(Room::ms(lagMusic), D, 2.0);
    CHECK_NEAR(Room::ms(lagA), Room::ms(lagMusic), 7.0);   // each singer is as late as the music: on the beat
    CHECK_NEAR(Room::ms(lagB), Room::ms(lagMusic), 7.0);

    // the headphones are never delayed and what friends hear is the live mix
    // the headphones: friend A is not delayed by Line up (100 ms + the Hub's buffer, not D)
    const int64_t lagPh = Room::bestLag(r.phones, 0, from, len, kVoice, 0, int64_t(0.3 * kRate), nullptr);
    CHECK(lagPh >= 0);
    CHECK_NEAR(Room::ms(lagPh), 100.0 + 10.0 + double(bitsFloat(r.hub.friendInfo(0).bufferMsBits.load())), 8.0);
    const int64_t lagFm = Room::bestLag(r.friendMix, 0, from, len, kMusic, 0, 400, nullptr);
    CHECK(lagFm >= 0);
    CHECK_LT(Room::ms(lagFm), 3.0);
}

TEST_CASE("friends: Line up off means live again; the change is a crossfade, not a jump") {
    Room r("lineoff");
    r.openFriend(0, kVoice, 200.0);
    r.hub.setLineUp(true, 600.0f);
    r.run(3.0);
    CHECK(r.hub.lineUpMs() > 150.0);
    const uint32_t changes = r.hub.lineUpChanges();
    const size_t m = r.stream.size();
    r.hub.setLineUp(false, 600.0f);
    r.run(2.0);
    CHECK(r.hub.lineUpMs() == 0.0f);
    CHECK(r.hub.lineUpChanges() > changes);
    // after the switch the music is live again (no delay)
    const int64_t lag = Room::bestLag(r.stream, 0, m + size_t(1.2 * kRate), size_t(0.4 * kRate), kMusic, 0, int64_t(0.6 * kRate), nullptr);
    CHECK(lag >= 0);
    CHECK_LT(Room::ms(lag), 3.0);
    // no click: the largest sample step stays in the range of the material itself
    float maxStep = 0;
    for (size_t i = m + 1; i < r.stream.size(); ++i) maxStep = std::max(maxStep, std::abs(r.stream[i] - r.stream[i - 1]));
    CHECK_LT(maxStep, 1.3f);   // noise at +-0.25 per voice, three voices at most: a hard cut would not stay below this anyway
}

TEST_CASE("friends: a friend slower than the limit is not waited for") {
    Room r("limit");
    r.openFriend(0, kVoice, 100.0);
    r.openFriend(1, kVoice2, 900.0);   // over a 400 ms limit
    r.hub.setLineUp(true, 400.0f);
    r.run(4.0);
    CHECK(r.hub.friendInfo(1).overLimit.load() == 1);
    CHECK(r.hub.friendInfo(0).overLimit.load() == 0);
    CHECK(r.hub.lineUpSlowest() == 0);
    CHECK(r.hub.lineUpMs() < 250.0f);
}

TEST_CASE("friends: the route decides who plays the friend, never both") {
    Room r("route");
    r.music = false;
    r.openFriend(0, kVoice, 40.0);
    r.run(1.5);
    r.hub.bus()->friends[0].route.store(kRouteDaw);   // a DAW track carries this friend now
    r.run(1.0);
    float pp = 0, ps = 0;
    for (size_t i = r.phones.size() - 20000; i < r.phones.size(); ++i) { pp = std::max(pp, std::abs(r.phones[i])); ps = std::max(ps, std::abs(r.stream[i])); }
    CHECK_LT(pp, 1e-4);   // out of the headphones...
    CHECK_LT(ps, 1e-4);   // ...and out of the Stream Mix: the DAW track is the only way
    r.hub.bus()->friends[0].route.store(kRouteDirect);
    r.run(1.0);
    pp = ps = 0;
    for (size_t i = r.phones.size() - 20000; i < r.phones.size(); ++i) { pp = std::max(pp, std::abs(r.phones[i])); ps = std::max(ps, std::abs(r.stream[i])); }
    CHECK(pp > 0.05f);
    CHECK(ps > 0.05f);
}

TEST_CASE("friends: a friend who stops is silent within a second and does not hang the Hub") {
    Room r("gone");
    r.music = false;
    r.openFriend(0, kVoice, 40.0);
    r.run(1.5);
    r.closeFriend(0);
    r.run(1.5);
    float p = 0;
    for (size_t i = r.phones.size() - 20000; i < r.phones.size(); ++i) p = std::max(p, std::abs(r.phones[i]));
    CHECK_LT(p, 1e-4);
    CHECK(r.hub.friendInfo(0).playing.load() == 0);
    r.openFriend(0, kVoice, 40.0);   // and comes back
    r.run(1.5);
    CHECK(r.hub.friendInfo(0).playing.load() == 1);
}

TEST_CASE("friends: eight friends at once (the worst case) stay under the stream's limiter") {
    Room r("eight");
    for (int j = 0; j < kMaxFriends; ++j) r.openFriend(j, kVoice + uint32_t(j) * 7u, 60.0 + 30.0 * j);
    r.hub.setLineUp(true, 600.0f);
    r.params.ceilingDb = -3.0f;
    r.run(4.0);
    float peak = 0;
    for (size_t i = size_t(2.5 * kRate); i < r.stream.size(); ++i) peak = std::max(peak, std::abs(r.stream[i]));
    CHECK(peak > 0.05f);
    CHECK_LT(peak, ssdsp::dbToGain(-3.0f) + 1e-3f);   // the limiter sees the final sum, friends included
    for (int j = 0; j < kMaxFriends; ++j) CHECK(r.hub.friendInfo(j).playing.load() == 1);
}

TEST_CASE("friends: S7 feeder pairing by track identity, Li measurement, and Line up route") {
    Room r("s7_pair");
    BusLayout* b = r.hub.bus();
    REQUIRE(b != nullptr);

    // Friend 0 with id = 101
    b->friends[0].id.store(101);
    b->friends[0].state.store(kFriendLive);

    // Claim a feeder
    int fidx = claimFeeder(*b);
    REQUIRE(fidx >= 0);
    setFeederIdentity(b->feeders[fidx], "Lead Vocal", 0);
    b->feeders[fidx].friendId.store(101);
    b->feeders[fidx].status.store(kFeederFlowing);
    b->feeders[fidx].heartbeatNs.store(nowNs());
    b->friends[0].feeder.store(fidx);

    // Claim an end-of-track slot with the same track name
    bool dup = false;
    int sidx = claimSlot(*b, makeUuid(), kRate, 2, dup);
    REQUIRE(sidx >= 0);
    setSlotIdentity(b->slots[sidx], "Lead Vocal", 0);
    b->slots[sidx].chainLatencyBits.store(floatBits(12.5f));

    // Run latency measurement in HubEngine
    r.hub.measureLatencies();

    // Verify pairing
    CHECK(b->slots[sidx].fedBy.load() == 101);
    CHECK(b->friends[0].outSlot.load() == sidx);
    CHECK_NEAR(bitsFloat(b->slots[sidx].fxLatencyBits.load()), 12.5f, 0.1f);

    // With Line up enabled, kFlagViewersViaHub should be set on the paired slot
    r.hub.setLineUp(true, 400.0f);
    r.hub.measureLatencies();
    CHECK((b->slots[sidx].flags.load() & kFlagViewersViaHub) != 0);

    // With Line up disabled, kFlagViewersViaHub should be cleared
    r.hub.setLineUp(false, 400.0f);
    r.hub.measureLatencies();
    CHECK((b->slots[sidx].flags.load() & kFlagViewersViaHub) == 0);

    // Release feeder: slot should be unpaired and fedBy cleared
    b->friends[0].feeder.store(-1);
    r.hub.measureLatencies();
    CHECK(b->slots[sidx].fedBy.load() == 0);
    CHECK(b->friends[0].outSlot.load() == -1);

    releaseSlot(*b, sidx);
    releaseFeeder(*b, fidx);
}

