// The friends room in the Hub engine (S2 + S4): friends reach the headphones and the Stream Mix,
// Line up delays the Stream Mix so every voice lands on the beat, panic and the route decide
// who hears what.
#include "testing.h"

#include "ssengine/engine.h"
#include "ssengine/friend_reader.h"

#include <functional>
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
    // a HEARASIDE Track at the end of a DAW channel that carries a friend (S7): content = noise(t - lag, seed)
    std::unique_ptr<TrackPublisher> carrier;
    int64_t carrierLag = 0;
    uint32_t carrierSeed = 0;
    std::function<void(int64_t)> beforeHub;   // runs each cycle before the Hub processes (stand-in DAW tracks, a feeder)

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
        if (carrier) {
            std::vector<float> c(static_cast<size_t>(block));
            for (int i = 0; i < block; ++i) c[size_t(i)] = noiseAt(t + i - carrierLag, carrierSeed);
            const float* ch[1] = { c.data() };
            carrier->process(ch, 1, block, t, true, false, false);
            // the DAW plays the Track's output into the master, unless it sends the viewers' side to the Hub
            if (!(carrier->slot()->flags.load() & kFlagViewersViaHub))
                for (int i = 0; i < block; ++i) l[size_t(i)] += c[size_t(i)];
        }
        r = l;
        if (beforeHub) beforeHub(t);
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

    // a paired Track always sends the viewers' side through the Hub, Line up or not: only the Hub
    // can keep the friend out of the mix that friends hear
    r.hub.setLineUp(true, 400.0f);
    r.hub.measureLatencies();
    CHECK((b->slots[sidx].flags.load() & kFlagViewersViaHub) != 0);
    r.hub.setLineUp(false, 400.0f);
    r.hub.measureLatencies();
    CHECK((b->slots[sidx].flags.load() & kFlagViewersViaHub) != 0);

    // Release feeder: slot should be unpaired and fedBy cleared
    b->friends[0].feeder.store(-1);
    r.hub.measureLatencies();
    CHECK(b->slots[sidx].fedBy.load() == 0);
    CHECK(b->friends[0].outSlot.load() == -1);

    releaseSlot(*b, sidx);
    releaseFeeder(*b, fidx);
}

TEST_CASE("friends: a pair chosen by hand wins over the names, is refused for bad Tracks, and ends with its Track") {
    Room r("manual_pair");
    BusLayout* b = r.hub.bus();
    b->friends[0].id.store(101);
    b->friends[0].state.store(kFriendLive);
    const int fidx = claimFeeder(*b);
    REQUIRE(fidx >= 0);
    setFeederIdentity(b->feeders[fidx], "Friend 1", 0);   // what a host that sends no track names gives
    b->feeders[fidx].friendId.store(101);
    b->feeders[fidx].status.store(kFeederFlowing);
    b->feeders[fidx].heartbeatNs.store(nowNs());
    b->friends[0].feeder.store(fidx);

    bool dup = false;
    const int sidx = claimSlot(*b, makeUuid(), kRate, 2, dup);
    const int aidx = claimSlot(*b, makeUuid(), kRate, 2, dup);
    const int other = claimSlot(*b, makeUuid(), kRate, 2, dup);
    REQUIRE(sidx >= 0); REQUIRE(aidx >= 0); REQUIRE(other >= 0);
    setSlotIdentity(b->slots[sidx], "Track 3", 0);
    setSlotIdentity(b->slots[other], "Track 4", 0);
    b->slots[aidx].flags.fetch_or(kFlagApp);
    b->slots[sidx].chainLatencyBits.store(floatBits(12.5f));

    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == -1);   // names differ: nothing pairs

    CHECK(!r.hub.pairWith(fidx, uint32_t(aidx)));          // an App Audio
    CHECK(!r.hub.pairWith(fidx, uint32_t(kMaxSlots - 1)));  // not a Track
    CHECK(!r.hub.pairWith(fidx + 1, uint32_t(sidx)));       // not a friend input
    b->slots[other].fedBy.store(555);
    CHECK(!r.hub.pairWith(fidx, uint32_t(other)));          // carries another friend
    b->slots[other].fedBy.store(0);
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == -1);

    REQUIRE(r.hub.pairWith(fidx, uint32_t(sidx)));
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == sidx);
    CHECK(b->slots[sidx].fedBy.load() == 101);
    CHECK((b->slots[sidx].flags.load() & kFlagViewersViaHub) != 0);   // so the Hub routes the friend through the DAW
    CHECK_NEAR(bitsFloat(b->slots[sidx].fxLatencyBits.load()), 12.5f, 0.1f);
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == sidx);   // the names still differ: it stays

    CHECK(r.hub.pairWith(fidx, uint32_t(other)));   // changing one's mind
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == other);
    CHECK(b->slots[sidx].fedBy.load() == 0);

    releaseSlot(*b, other);   // the Track goes away: back to Direct, and a new Track on that index is not paired by mistake
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == -1);
    CHECK(b->slots[other].fedBy.load() == 0);
    const int again = claimSlot(*b, makeUuid(), kRate, 2, dup);
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == -1);
    CHECK(b->slots[again].fedBy.load() == 0);

    REQUIRE(r.hub.pairWith(fidx, uint32_t(sidx)));
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == sidx);
    CHECK(r.hub.pairWith(fidx, kPairAutomatic));   // back to automatic: the names differ, so unpaired
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == -1);
    CHECK(b->slots[sidx].fedBy.load() == 0);

    REQUIRE(r.hub.pairWith(fidx, uint32_t(sidx)));
    r.hub.measureLatencies();
    b->friends[0].feeder.store(-1);   // the friend input goes away
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == -1);
    b->friends[0].feeder.store(fidx);
    r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == -1);   // and it does not come back by itself
}


TEST_CASE("friends: a friend mixed in a DAW track never hears themselves, and is delayed once, not twice") {
    Room r("s7_minus");
    r.params.syncSafety = 0;
    r.openFriend(0, kVoice, 100.0);
    r.openFriend(1, kVoice2, 250.0);                 // the friend a DAW track carries
    BusLayout* b = r.hub.bus();
    b->friends[1].route.store(kRouteDaw);
    r.carrier = std::make_unique<TrackPublisher>();
    r.carrier->connect(r.bus, "", kRate, 1);
    r.carrier->prepare(kRate, 1);
    r.carrierSeed = kVoice2;
    r.carrierLag = int64_t(0.300 * kRate);           // the voice reaches the DAW track 300 ms after its beat
    SlotHeader* sh = r.carrier->slot();
    REQUIRE(sh != nullptr);
    sh->fedBy.store(101);
    sh->flags.fetch_or(kFlagViewersViaHub);          // paired (what measureLatencies does)
    r.hub.setLineUp(true, 600.0f);
    r.run(4.0);

    const size_t from = size_t(3.0 * kRate), len = size_t(0.06 * kRate);
    double s = 0;
    // what friends hear: the music, never a friend (direct or carried by the DAW)
    CHECK(Room::bestLag(r.friendMix, 0, from, len, kVoice2, 0, int64_t(1.2 * kRate), &s) < 0);
    CHECK(Room::bestLag(r.friendMix, 0, from, len, kVoice, 0, int64_t(1.2 * kRate), &s) < 0);
    CHECK(Room::bestLag(r.friendMix, 0, from, len, kMusic, 0, 400, &s) >= 0);

    // the viewers: the carried friend joins after the Line-up delay, placed at D - its own delay
    const double D = r.hub.lineUpMs();
    const double tot1 = double(bitsFloat(r.hub.friendInfo(1).totalMsBits.load()));
    CHECK(D > 250.0);
    const int64_t lagC = Room::bestLag(r.stream, 0, from, len, kVoice2, 0, int64_t(1.2 * kRate), &s);
    CHECK(lagC >= 0);
    CHECK_NEAR(Room::ms(lagC), 300.0 + D - tot1, 8.0);   // before the fix: + D twice
}

TEST_CASE("friends: friends hear each other as soon as each voice arrives (Jamulus style), Line up or not") {
    Room r("hear_each");
    r.music = false;
    r.openFriend(0, kVoice, 100.0);
    r.openFriend(1, kVoice2, 250.0);
    r.hub.setLineUp(true, 600.0f);
    r.run(4.0);
    BusLayout* b = r.hub.bus();
    const uint64_t w = b->friendVoiceWrite[0].load();
    REQUIRE(w == b->friendMixWrite.load());   // same positions as the mix friends hear
    const uint32_t len = uint32_t(0.5 * kRate);
    std::vector<float> v0(len), v1(len);
    float* d[2] = { v0.data(), v1.data() };
    ringReadAt(b->friendVoiceAudio[0], b->friendVoiceWrite[0], w - len, d, len);
    // position == DAW time here; each voice is only as late as the line + the Hub's jitter buffer (not D)
    const int64_t from = int64_t(w - len);
    const int64_t lag0 = Room::bestLag(v0, from, 0, size_t(0.06 * kRate), kVoice, 0, int64_t(0.5 * kRate));
    const int64_t lag1 = Room::bestLag(v1, from, 0, size_t(0.06 * kRate), kVoice2, 0, int64_t(0.5 * kRate));
    CHECK(lag0 >= 0); CHECK(lag1 >= 0);
    CHECK_NEAR(Room::ms(lag0), 100.0 + 10.0 + double(bitsFloat(r.hub.friendInfo(0).bufferMsBits.load())), 8.0);
    CHECK_LT(Room::ms(lag1), r.hub.lineUpMs() - 1.0 + 30.0);
    CHECK(r.hub.lineUpMs() > 250.0);
    // the base mix still carries no friend (the share server adds everyone but the listener)
    CHECK(Room::bestLag(r.friendMix, 0, size_t(3.0 * kRate), size_t(0.06 * kRate), kVoice, 0, int64_t(0.6 * kRate)) < 0);
}

TEST_CASE("friends: a Track prepared for its real block starts the friend's jitter buffer ~30 ms lower than for 2048") {
    auto startBufferMs = [](int block) {
        auto ring = std::make_unique<ChannelRing>();
        std::atomic<uint64_t> wp{ 0 };
        FriendReader rd;
        rd.prepare(kRate, block);
        std::vector<float> o0(static_cast<size_t>(block)), o1(static_cast<size_t>(block));
        float* out[2] = { o0.data(), o1.data() };
        std::vector<float> pk(960);
        int64_t sent = 0;
        for (int cycle = 0; cycle < 2 * kRate / block; ++cycle) {   // 2 s: the reader primes, then plays
            while (sent + 960 <= int64_t(cycle) * block) {
                for (int i = 0; i < 960; ++i) pk[size_t(i)] = noiseAt(sent + i, kVoice);
                const float* src[1] = { pk.data() };
                ringWrite(*ring, wp, src, 1, 960);
                sent += 960;
            }
            rd.pull(*ring, wp, kRate, out, block);
        }
        CHECK(rd.primed());
        return rd.bufferMs();
    };
    const double small = startBufferMs(256), big = startBufferMs(2048);
    CHECK_LT(30.0, big - small);   // at least the ~37 ms between the two blocks (a 2048 pull also sees lumpier arrivals)
}

// A friend input whose channel ends in one of three Tracks: the sweep it plays goes through a small EQ, a gain and a
// 7 ms delay into the "end" Track; the other two carry unrelated audio. Plays the part of the feeder and the DAW.
struct ProbeRig {
    Room r { "probe" };
    BusLayout* b = r.hub.bus();
    int fidx = -1, endSlot = -1;
    TrackPublisher tracks[3];
    std::vector<float> sweep = probeSweep(kRate), chain;
    uint32_t cmdSeen = 0;
    int64_t sweepFrom = -1;   // DAW frame the feeder began the sweep at
    uint64_t tw = 0;
    float tapAcc = 0.0f;
    bool endHearsSweep;

    explicit ProbeRig(bool endHears) : endHearsSweep(endHears) {
        r.params.syncSafety = 0;
        r.openFriend(0, kVoice, 40.0);
        fidx = claimFeeder(*b);
        setFeederIdentity(b->feeders[fidx], "Lead Vocal", 0);
        b->feeders[fidx].friendId.store(100);
        b->feeders[fidx].status.store(kFeederFlowing);
        b->friends[0].feeder.store(fidx);
        cmdSeen = b->feeders[fidx].hubCommandSeq.load();
        // 0.8 EQ-ish: y = 0.8 x[n] + 0.2 x[n-2], gain 0.5, then 7 ms
        chain.assign(sweep.size() + size_t(0.007 * kRate) + 4, 0.0f);
        const size_t d = size_t(0.007 * kRate + 0.5);
        for (size_t i = 0; i < sweep.size(); ++i)
            chain[i + d] = 0.5f * (0.8f * sweep[i] + 0.2f * (i >= 2 ? sweep[i - 2] : 0.0f));
        const char* names[3] = { "Track 1", "Track 2", "Track 3" };
        for (int k = 0; k < 3; ++k) {
            tracks[k].connect(r.bus, "", kRate, 1);
            tracks[k].prepare(kRate, 1);
            tracks[k].setIdentity(names[k], 0);
        }
        endSlot = tracks[1].slotIndex();
        r.beforeHub = [this](int64_t t) { feederAndTracks(t); };
    }

    void feederAndTracks(int64_t t) {
        FeederRecord& f = b->feeders[fidx];
        f.heartbeatNs.store(nowNs());
        if (const uint32_t cs = f.hubCommandSeq.load(); cs != cmdSeen) {   // the Hub's command: start the sweep
            cmdSeen = cs;
            if (f.hubCommand.load() == 2) sweepFrom = t;
        }
        std::vector<float> c(size_t(r.block));
        for (int i = 0; i < r.block; ++i) {
            const int64_t at = sweepFrom >= 0 ? t + i - sweepFrom : -1;
            tapAcc += at >= 0 && at < int64_t(sweep.size()) ? sweep[size_t(at)] : 0.0f;
            if ((i & 3) == 3) { b->feederTap[fidx].mono[tw++ % kTapFrames] = tapAcc * 0.25f; tapAcc = 0.0f; }
        }
        f.tapWrite.store(tw, std::memory_order_release);
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < r.block; ++i) {
                const int64_t at = sweepFrom >= 0 ? t + i - sweepFrom : -1;
                c[size_t(i)] = k == 1 ? (endHearsSweep && at >= 0 && at < int64_t(chain.size()) ? chain[size_t(at)] : 0.0f)
                                      : noiseAt(t + i, 500u + uint32_t(k));
            }
            const float* ch[1] = { c.data() };
            tracks[k].process(ch, 1, r.block, t, true, false, false);
        }
    }

    // runs the probe like the Hub's latency thread would; returns the DAW frames it took
    int64_t probe() {
        const uint32_t seq = b->feeders[fidx].replySeq.load();
        if (!r.hub.startProbe(fidx)) { CHECK(false); return -1; }
        const size_t from = r.stream.size();
        startOut = from;
        for (int c = 0; c < 400 && r.hub.probing(); ++c) { r.cycle(); r.hub.probeStep(); }
        endOut = r.stream.size();
        replied = b->feeders[fidx].replySeq.load() != seq;
        return int64_t(endOut - from);
    }
    size_t startOut = 0, endOut = 0;
    bool replied = false;
};

static float peakOf(const std::vector<float>& v, size_t from, size_t to) {
    float p = 0;
    for (size_t i = from; i < to && i < v.size(); ++i) p = std::max(p, std::abs(v[i]));
    return p;
}

TEST_CASE("friends: the probe sweep finds the Track at the end of the channel and nobody hears it") {
    ProbeRig rig(true);
    rig.r.run(1.0);   // the tap fills; music plays
    BusLayout* b = rig.b;
    CHECK(peakOf(rig.r.stream, rig.r.stream.size() - 4800, rig.r.stream.size()) > 0.05f);
    CHECK(peakOf(rig.r.friendMix, rig.r.friendMix.size() - 4800, rig.r.friendMix.size()) > 0.05f);

    const int64_t took = rig.probe();
    CHECK(rig.replied);
    CHECK(b->feeders[rig.fidx].reply.load() == 1);
    CHECK(b->friends[0].outSlot.load() == rig.endSlot);
    CHECK(b->slots[rig.endSlot].fedBy.load() == 100);
    CHECK_NEAR(bitsFloat(b->slots[rig.endSlot].fxLatencyBits.load()), 7.0f, 1.0f);
    CHECK_LT(double(took) / kRate, 0.7);   // silent for well under a second
    // the Stream Mix is silent after the 20 ms ramp, what friends hear at once; the sweep never reaches either
    CHECK_LT(peakOf(rig.r.stream, rig.startOut + size_t(0.05 * kRate), rig.endOut), 1e-5f);
    CHECK_LT(peakOf(rig.r.friendMix, rig.startOut + 512, rig.endOut), 1e-6f);

    rig.r.run(0.5);   // and they come back
    CHECK(peakOf(rig.r.stream, rig.r.stream.size() - 4800, rig.r.stream.size()) > 0.05f);
    CHECK(peakOf(rig.r.friendMix, rig.r.friendMix.size() - 4800, rig.r.friendMix.size()) > 0.05f);

    // the pair is kept like a manual one: the names differ and it stays
    rig.r.hub.measureLatencies();
    CHECK(b->friends[0].outSlot.load() == rig.endSlot);
}

TEST_CASE("friends: the probe says so when no Track plays the sweep, and leaves nothing paired") {
    ProbeRig rig(false);
    rig.r.run(1.0);
    rig.probe();
    CHECK(rig.replied);
    CHECK(rig.r.hub.probing() == false);
    CHECK(rig.b->feeders[rig.fidx].reply.load() == kReplyNoMatch);
    CHECK(rig.b->friends[0].outSlot.load() == -1);
    rig.r.run(0.5);
    CHECK(peakOf(rig.r.stream, rig.r.stream.size() - 4800, rig.r.stream.size()) > 0.05f);
}
