#include "testing.h"

#include "ssbus/bus.h"

#include <set>
#include <thread>
#include <vector>

using namespace ssbus;

static std::string uniqueBus(const char* tag) { return std::string("test_") + tag + "_" + std::to_string(currentPid()) + "_" + std::to_string(nowNs() % 100000); }

TEST_CASE("ring: write/read with wrap-around") {
    static ChannelRing ring;
    std::atomic<uint64_t> wp{ kRingFrames - 100 };   // force a wrap
    std::vector<float> src(300), l(300), r(300);
    for (int i = 0; i < 300; ++i) src[size_t(i)] = float(i);
    const float* s[1] = { src.data() };
    ringWrite(ring, wp, s, 1, 300);
    CHECK(wp.load() == kRingFrames + 200);
    uint64_t cur = kRingFrames - 100;
    float* d[2] = { l.data(), r.data() };
    CHECK(ringRead(ring, wp, cur, d, 300) == ReadResult::Ok);
    bool same = true;
    for (int i = 0; i < 300; ++i) same &= (l[size_t(i)] == float(i) && r[size_t(i)] == float(i));
    CHECK(same);
    CHECK(cur == wp.load());
}

TEST_CASE("ring: underrun zero-fills and advances only by available") {
    static ChannelRing ring;
    std::atomic<uint64_t> wp{ 0 };
    std::vector<float> src(64, 1.0f), l(128, 5.0f), r(128, 5.0f);
    const float* s[1] = { src.data() };
    ringWrite(ring, wp, s, 1, 64);
    uint64_t cur = 0;
    float* d[2] = { l.data(), r.data() };
    CHECK(ringRead(ring, wp, cur, d, 128) == ReadResult::Underrun);
    CHECK(cur == 64);
    CHECK(l[63] == 1.0f);
    CHECK(l[64] == 0.0f && r[127] == 0.0f);
}

TEST_CASE("ring: overrun detected") {
    static ChannelRing ring;
    std::atomic<uint64_t> wp{ 0 };
    std::vector<float> big(kRingFrames / 2, 0.5f), l(16), r(16);
    const float* s[1] = { big.data() };
    for (int i = 0; i < 4; ++i) ringWrite(ring, wp, s, 1, uint32_t(big.size()));
    uint64_t cur = 0;
    float* d[2] = { l.data(), r.data() };
    CHECK(ringRead(ring, wp, cur, d, 16) == ReadResult::Overrun);
}

TEST_CASE("ring: independent readers") {
    static ChannelRing ring;
    std::atomic<uint64_t> wp{ 0 };
    std::vector<float> src(100), a(50), b(50), c(100), e(100);
    for (int i = 0; i < 100; ++i) src[size_t(i)] = float(i);
    const float* s[1] = { src.data() };
    ringWrite(ring, wp, s, 1, 100);
    uint64_t r1 = 0, r2 = 0;
    float* d1[2] = { a.data(), b.data() };
    float* d2[2] = { c.data(), e.data() };
    ringRead(ring, wp, r1, d1, 50);
    ringRead(ring, wp, r2, d2, 100);
    CHECK(r1 == 50 && r2 == 100 && a[49] == 49.0f && c[99] == 99.0f);
}

TEST_CASE("shm: two mappings of one bus share memory") {
    const std::string name = uniqueBus("shm");
    SharedMemory::Status s1{}, s2{};
    auto a = SharedMemory::open(name, s1);
    REQUIRE(a != nullptr);
    auto b = SharedMemory::openExisting(name, s2);
    REQUIRE(b != nullptr);
    CHECK(&a->layout() != &b->layout());
    a->layout().header.slotSequence.store(1234);
    CHECK(b->layout().header.slotSequence.load() == 1234);
    CHECK(b->layout().header.magic.load() == kMagic);
    CHECK(b->layout().slots[5].stemIndex.load() == -1);
}

TEST_CASE("shm: openExisting fails for unknown bus") {
    SharedMemory::Status s{};
    auto x = SharedMemory::openExisting(uniqueBus("none"), s);
    CHECK(x == nullptr);
}

TEST_CASE("slots: claim 64, 65th fails, duplicate uuid detected, release") {
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(uniqueBus("slots"), st);
    REQUIRE(shm);
    auto& L = shm->layout();
    std::set<int> got;
    for (int i = 0; i < kMaxSlots; ++i) {
        bool dup = false;
        const int idx = claimSlot(L, "uuid-" + std::to_string(i), 48000, 2, dup);
        CHECK(idx >= 0 && !dup);
        got.insert(idx);
    }
    CHECK(int(got.size()) == kMaxSlots);
    bool dup = false;
    CHECK(claimSlot(L, "another", 48000, 2, dup) == -1);
    releaseSlot(L, 10);
    CHECK(claimSlot(L, "uuid-3", 48000, 2, dup) == -1);
    CHECK(dup);
    const int idx = claimSlot(L, "fresh", 48000, 2, dup);
    CHECK(idx == 10 && !dup);
}

TEST_CASE("slots: concurrent claims never share a slot") {
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(uniqueBus("race"), st);
    REQUIRE(shm);
    auto& L = shm->layout();
    std::vector<int> result(32, -2);
    std::vector<std::thread> threads;
    for (int t = 0; t < 32; ++t)
        threads.emplace_back([&, t] { bool d = false; result[size_t(t)] = claimSlot(L, "t" + std::to_string(t), 48000, 2, d); });
    for (auto& th : threads) th.join();
    std::set<int> uniq(result.begin(), result.end());
    CHECK(uniq.size() == 32 && *uniq.begin() >= 0);
}

TEST_CASE("slots: identity seqlock round trip (UTF-8 Thai, truncation on code point)") {
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(uniqueBus("ident"), st);
    REQUIRE(shm);
    bool dup = false;
    const int idx = claimSlot(shm->layout(), "abc", 48000, 2, dup);
    auto& slot = shm->layout().slots[idx];
    setSlotIdentity(slot, "เสียงร้อง", 0xFF336699);
    std::string n, u; uint32_t c = 0;
    CHECK(readSlotIdentity(slot, n, u, c));
    CHECK(n == "เสียงร้อง" && u == "abc" && c == 0xFF336699);
    std::string longThai;
    for (int i = 0; i < 40; ++i) longThai += "ก";   // 3 bytes each = 120 bytes
    setSlotIdentity(slot, longThai, 0);
    readSlotIdentity(slot, n, u, c);
    CHECK(n.size() == 63 && n.size() % 3 == 0);
}

TEST_CASE("hub: exclusive ownership, release, stale takeover") {
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(uniqueBus("hub"), st);
    REQUIRE(shm);
    auto& L = shm->layout();
    CHECK(tryClaimHub(L, 111, 2000000000ull));
    CHECK(!tryClaimHub(L, 222, 2000000000ull));
    CHECK(hubAlive(L, 2000000000ull));
    L.header.hubHeartbeatNs.store(nowNs() - 3000000000ull);   // stale
    CHECK(tryClaimHub(L, 222, 2000000000ull));
    releaseHub(L, 111);   // not owner any more: no effect
    CHECK(L.header.hubOwnerToken.load() == 222);
    releaseHub(L, 222);
    CHECK(L.header.hubOwnerToken.load() == 0);
}

TEST_CASE("mailbox: commands delivered in order, overflow keeps newest") {
    static SlotHeader slot;
    uint32_t cursor = slot.cmdWrite.load();
    postCommand(slot, ParamId::Mon, 0.0f);
    postCommand(slot, ParamId::StrGainDb, -3.0f);
    std::vector<std::pair<ParamId, float>> got;
    pollCommands(slot, cursor, [&](ParamId id, float v) { got.push_back({ id, v }); });
    CHECK(got.size() == 2 && got[0].first == ParamId::Mon && got[1].second == -3.0f);
    for (int i = 0; i < 100; ++i) postCommand(slot, ParamId::StrPan, float(i));
    got.clear();
    pollCommands(slot, cursor, [&](ParamId id, float v) { got.push_back({ id, v }); });
    CHECK(int(got.size()) == kMailboxSize && got.back().second == 99.0f);
}

TEST_CASE("rename: seqlock request") {
    static SlotHeader slot;
    uint32_t last = slot.renameSeq.load();
    std::string name;
    CHECK(!pollRename(slot, last, name));
    requestRename(slot, "ไมค์พูด");
    CHECK(pollRename(slot, last, name) && name == "ไมค์พูด");
    CHECK(!pollRename(slot, last, name));
}

TEST_CASE("sources: claim 16, identity, commands, program choice, dead owner reclaimed") {
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(uniqueBus("sources"), st);
    REQUIRE(shm);
    auto& L = shm->layout();
    std::set<int> got;
    for (int i = 0; i < kMaxSources; ++i) got.insert(claimSource(L));
    CHECK(int(got.size()) == kMaxSources && !got.count(-1));
    CHECK(claimSource(L) == -1);
    releaseSource(L, 5);
    CHECK(claimSource(L) == 5);

    SourceHeader& s = L.sources[5];
    setSourceIdentity(s, "เพลง YouTube", "chrome.exe", 0xFF112233u);
    std::string name, app;
    uint32_t col = 0;
    CHECK(readSourceIdentity(s, name, app, col) && name == "เพลง YouTube" && app == "chrome.exe" && col == 0xFF112233u);

    uint32_t cursor = s.cmdWrite.load();
    postSourceCommand(s, SourceParam::On, 0.0f);
    postSourceCommand(s, SourceParam::LevelDb, -6.0f);
    std::vector<std::pair<SourceParam, float>> cmds;
    pollSourceCommands(s, cursor, [&](SourceParam id, float v) { cmds.push_back({ id, v }); });
    CHECK(cmds.size() == 2 && cmds[0].first == SourceParam::On && cmds[1].second == -6.0f);

    uint32_t last = s.appSeq.load();
    std::string exe;
    CHECK(!pollSourceApp(s, last, exe));
    requestSourceApp(s, "spotify.exe");
    CHECK(pollSourceApp(s, last, exe) && exe == "spotify.exe");

    // owner process gone + stale heartbeat -> the Hub frees it
    s.ownerPid.store(0);
    s.heartbeatNs.store(nowNs() - 10000000000ull);
    CHECK(reclaimDeadSlots(L, 5000000000ull) == 1);
    CHECK(s.state.load() == kSlotFree);
}

TEST_CASE("tags: timeline lookup finds ahead-rendered block") {
    static SlotHeader slot;
    static SlotTags tags;
    // track rendered blocks of 128 at timeline 1000.. with FIFO starting at 5000
    for (int k = 0; k < 20; ++k) writeTag(slot, tags, 5000 + uint64_t(k) * 128, 1000 + k * 128, 128, true);
    TagHit h = findTimelinePosition(slot, tags, 1000 + 3 * 128 + 7);
    CHECK(h.found && h.fifoPos == 5000 + 3 * 128 + 7);
    h = findTimelinePosition(slot, tags, 1000 + 20 * 128);   // not rendered yet
    CHECK(!h.found);
    writeTag(slot, tags, 5000 + 20 * 128, 0, 128, false);     // stopped
    h = findTimelinePosition(slot, tags, 1000 + 3 * 128);
    CHECK(!h.found);
}

TEST_CASE("platform: uuid format and process liveness") {
    const std::string u = makeUuid();
    CHECK(u.size() == 36 && u[8] == '-' && u[14] == '4');
    CHECK(makeUuid() != u);
    CHECK(processAlive(currentPid()));
    CHECK(!processAlive(0));
    CHECK(randomToken() != 0);
}

TEST_CASE("remote: many writers, one reader, every command delivered once and in claim order") {
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(uniqueBus("remote"), st);
    CHECK(shm != nullptr);
    if (!shm) return;
    auto& bus = shm->layout();
    uint32_t cursor = bus.header.remoteReserve.load();
    constexpr int kWriters = 4, kEach = 2000;
    std::atomic<int> finished{ 0 };
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w)
        writers.emplace_back([&bus, &finished, w] {
            for (int i = 0; i < kEach; ++i) {
                postRemote(bus, w, uint32_t(i), float(i));
                if ((i & 15) == 0) std::this_thread::yield();
            }
            finished.fetch_add(1);
        });
    std::vector<int> next(kWriters, 0);
    int received = 0, outOfOrder = 0, corrupt = 0;
    auto drain = [&] {
        pollRemote(bus, cursor, [&](int target, uint32_t pid, float v) {
            if (target < 0 || target >= kWriters || float(pid) != v) { ++corrupt; return; }
            if (int(pid) < next[size_t(target)]) ++outOfOrder;
            next[size_t(target)] = int(pid) + 1;
            ++received;
        });
    };
    // the reader keeps polling like the Hub timer; under this flood the oldest commands may be
    // dropped (queue of 64), but nothing may be corrupted or delivered out of order
    while (finished.load() < kWriters) drain();
    for (auto& t : writers) t.join();
    drain();
    CHECK(corrupt == 0);
    CHECK(outOfOrder == 0);
    std::printf("    received %d of %d (queue %d)\n", received, kWriters * kEach, kRemoteQueueSize);
    CHECK(received > 0);
}


// ---------------------------------------------------------------------------------------------
// v13: friends room, feeders, requests, beacon

TEST_CASE("v13: layout stays within what the proposal promised") {
    CHECK(sizeof(BeaconHeader) == 64);
    CHECK(kLayoutSize < size_t(52) * 1024 * 1024);   // proposal: about 50 MB with 8 friend rings
    CHECK(kProtocolVersion == 13);
    CHECK(kMaxFriends == 8);
}

TEST_CASE("friends: eight friend rings written together, read by three readers") {
    const std::string name = uniqueBus("friends");
    SharedMemory::Status st{};
    auto a = SharedMemory::open(name, st);
    REQUIRE(a != nullptr);
    auto b = SharedMemory::openExisting(name, st);
    REQUIRE(b != nullptr);
    BusLayout& w = a->layout();

    std::vector<std::thread> writers;
    for (int f = 0; f < kMaxFriends; ++f) {
        w.friends[f].id.store(uint32_t(100 + f));
        w.friends[f].state.store(kFriendLive);
        writers.emplace_back([&w, f] {
            std::vector<float> buf(128);
            for (int block = 0; block < 40; ++block) {
                for (int i = 0; i < 128; ++i) buf[size_t(i)] = float(f) + float(block * 128 + i) * 1e-5f;
                const float* s[1] = { buf.data() };
                ringWrite(w.friendAudio[f], w.friends[f].writePos, s, 1, 128);
            }
        });
    }
    for (auto& t : writers) t.join();

    // three independent readers (Hub engine, App Audio, a feeder) each keep their own cursor
    for (int reader = 0; reader < 3; ++reader) {
        for (int f = 0; f < kMaxFriends; ++f) {
            std::vector<float> l(128), r(128);
            float* d[2] = { l.data(), r.data() };
            uint64_t cur = 0;
            CHECK(ringRead(b->layout().friendAudio[f], b->layout().friends[f].writePos, cur, d, 128) == ReadResult::Ok);
            CHECK(l[0] == float(f));
            CHECK(r[0] == l[0]);
            CHECK(cur == 128);
        }
    }
    CHECK(b->layout().friends[7].id.load() == 107);
}

TEST_CASE("friends: names are read consistently and cut on a code point") {
    static BusLayout* bus = new BusLayout();
    std::memset(static_cast<void*>(bus), 0, sizeof(BusLayout));
    std::string thai;
    for (int i = 0; i < 30; ++i) thai += "\xE0\xB8\x81";   // 30 x one Thai letter, 90 bytes > 63
    setFriendName(bus->friends[3], thai);
    const std::string got = readFriendName(bus->friends[3]);
    CHECK(got.size() <= size_t(kNameBytes - 1));
    CHECK(got.size() % 3 == 0);   // never half a character
    setFriendName(bus->friends[3], "Mint");
    CHECK(readFriendName(bus->friends[3]) == "Mint");
}

TEST_CASE("feeders: claim, identity, reclaim after the owner is gone") {
    static BusLayout* bus = new BusLayout();
    std::memset(static_cast<void*>(bus), 0, sizeof(BusLayout));
    std::set<int> got;
    for (int i = 0; i < kMaxFeeders; ++i) got.insert(claimFeeder(*bus));
    CHECK(got.size() == size_t(kMaxFeeders));
    CHECK(!got.count(-1));
    CHECK(claimFeeder(*bus) == -1);   // full

    setFeederIdentity(bus->feeders[2], "Audio 05", 0xFF112233u);
    std::string n; uint32_t col = 0;
    CHECK(readFeederIdentity(bus->feeders[2], n, col));
    CHECK(n == "Audio 05");
    CHECK(col == 0xFF112233u);

    releaseFeeder(*bus, 2);
    CHECK(claimFeeder(*bus) == 2);

    bus->feeders[5].ownerPid.store(0xFFFFFFF0u);   // a process that does not exist
    bus->feeders[5].heartbeatNs.store(1);
    CHECK(reclaimDeadSlots(*bus, 1000) >= 1);
    CHECK(bus->feeders[5].state.load() == kSlotFree);
}

TEST_CASE("requests: many writers, one reader, text is wiped once read") {
    static BusLayout* bus = new BusLayout();
    std::memset(static_cast<void*>(bus), 0, sizeof(BusLayout));
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([t] { for (int i = 0; i < 3; ++i) postRequest(*bus, t, kReqResolveToken, uint32_t(i), "tok" + std::to_string(t)); });
    for (auto& t : ts) t.join();

    uint32_t cursor = 0;
    int n = 0;
    std::set<std::string> texts;
    pollRequests(*bus, cursor, [&](int feeder, RequestKind kind, uint32_t, const std::string& text) {
        CHECK(kind == kReqResolveToken);
        CHECK(text == "tok" + std::to_string(feeder));
        texts.insert(text);
        ++n;
    });
    CHECK(n == 12);
    CHECK(texts.size() == 4);
    for (auto& e : bus->requests) CHECK(e.text[0] == 0);   // nothing readable left in shared memory
}

TEST_CASE("requests: overflow drops the oldest, never stalls") {
    static BusLayout* bus = new BusLayout();
    std::memset(static_cast<void*>(bus), 0, sizeof(BusLayout));
    for (int i = 0; i < kRequestQueueSize * 2; ++i) postRequest(*bus, 0, kReqCopyLink, uint32_t(i), {});
    uint32_t cursor = 0;
    std::vector<uint32_t> args;
    pollRequests(*bus, cursor, [&](int, RequestKind, uint32_t arg, const std::string&) { args.push_back(arg); });
    CHECK(args.size() == size_t(kRequestQueueSize));
    CHECK(args.front() == uint32_t(kRequestQueueSize));   // the first 16 were dropped
    CHECK(args.back() == uint32_t(kRequestQueueSize * 2 - 1));
}

TEST_CASE("reply: the feeder sees each answer once") {
    static BusLayout* bus = new BusLayout();
    std::memset(static_cast<void*>(bus), 0, sizeof(BusLayout));
    uint32_t last = 0, reply = 0;
    CHECK(!pollReply(bus->feeders[0], last, reply));
    postReply(bus->feeders[0], 101);
    CHECK(pollReply(bus->feeders[0], last, reply));
    CHECK(reply == 101);
    CHECK(!pollReply(bus->feeders[0], last, reply));
    postReply(bus->feeders[0], kReplyRoomFull);
    CHECK(pollReply(bus->feeders[0], last, reply));
    CHECK(reply == kReplyRoomFull);
}

TEST_CASE("beacon: a reader sees the Hub's version, and nothing when it is stale or absent") {
    const std::string name = uniqueBus("beacon");
    CHECK(BeaconMap::openExisting(name) == nullptr);
    auto hub = BeaconMap::open(name);
    REQUIRE(hub != nullptr);
    auto reader = BeaconMap::openExisting(name);
    REQUIRE(reader != nullptr);
    CHECK(beaconHubVersion(reader->header()) == 0);   // nothing published yet
    beaconPublish(hub->header());
    CHECK(beaconHubVersion(reader->header()) == kProtocolVersion);
    hub->header().protocolVersion.store(12);          // as an older Hub would write
    CHECK(beaconHubVersion(reader->header()) == 12);
    hub->header().heartbeatNs.store(1);               // long ago
    CHECK(beaconHubVersion(reader->header()) == 0);
}
