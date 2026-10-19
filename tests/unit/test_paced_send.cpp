#include "testing.h"

#include "ssbus/paced_send.h"

#include <algorithm>
#include <functional>
#include <string>

using ssbus::SendQueue;

// A socket that takes at most `room` bytes per call round, like a full kernel buffer.
struct FakeSocket {
    std::string got;
    long room = 0;     // bytes it will take before it says "full"
    bool gone = false;
    long operator()(const uint8_t* p, size_t n) {
        if (gone) return -1;
        const size_t k = std::min<size_t>(n, size_t(room));
        got.append(reinterpret_cast<const char*>(p), k);
        room -= long(k);
        return long(k);
    }
};

TEST_CASE("send queue: a packet the socket takes in pieces arrives whole and in order") {
    SendQueue q;
    FakeSocket s;
    const std::string a(100, 'a'), b(50, 'b');
    CHECK(q.offer(a.data(), a.size()));
    s.room = 30;
    CHECK(q.flush(std::ref(s)));
    CHECK(!q.idle());
    CHECK(q.pending() == 70);
    s.room = 1000;
    CHECK(q.flush(std::ref(s)));
    CHECK(q.idle());
    CHECK(q.offer(b.data(), b.size()));
    CHECK(q.flush(std::ref(s)));
    CHECK(s.got == a + b);
}

TEST_CASE("send queue: audio is dropped while a packet waits, a control message is kept behind it") {
    SendQueue q;
    FakeSocket s;
    const std::string a(100, 'a'), late(100, 'x'), pong(4, 'p');
    CHECK(q.offer(a.data(), a.size()));
    s.room = 0;
    CHECK(q.flush(std::ref(s)));       // "full" is not an error
    CHECK(!q.offer(late.data(), late.size()));   // dropped: the queue stays at one packet
    q.append(pong.data(), pong.size());
    CHECK(q.pending() == 104);
    s.room = 1000;
    CHECK(q.flush(std::ref(s)));
    CHECK(s.got == a + pong);
    CHECK(q.idle());
}

TEST_CASE("send queue: a closed connection is reported, a full one is not") {
    SendQueue q;
    FakeSocket s;
    const std::string a(10, 'a');
    q.offer(a.data(), a.size());
    s.gone = true;
    CHECK(!q.flush(std::ref(s)));
}

TEST_CASE("catch up: a line that falls behind jumps near the newest audio, a healthy one never does") {
    const uint32_t rate = 48000;
    CHECK(ssbus::catchUp(1000, 1000 + 4800, rate, 150, 30) == 1000);   // 100 ms behind, allowed 150
    CHECK(ssbus::catchUp(1000, 1000 + 7200, rate, 150, 30) == 1000);   // exactly 150 ms: stay
    const uint64_t w = 100000;
    CHECK(ssbus::catchUp(1000, w, rate, 150, 30) == w - 1440);          // far behind: 30 ms before now
    CHECK(ssbus::catchUp(w + 5, w, rate, 150, 30) == w + 5);            // Hub restarted: not our business here
    CHECK(ssbus::catchUp(0, 500, rate, 1, 1000) == 0);                  // keep is longer than all there is: from the start
}

TEST_CASE("send queue: pongs behind a stuck line stop at the cap, however many pings come") {
    SendQueue q;
    FakeSocket s;   // takes nothing: the line is stuck
    const std::string audio(2000, 'a'), pong(127, 'p');
    q.offer(audio.data(), audio.size());
    int kept = 0;
    for (int i = 0; i < 1000; ++i) {
        kept += q.appendIfRoom(pong.data(), pong.size(), 1024) ? 1 : 0;
        CHECK(q.flush(std::ref(s)));
    }
    CHECK(kept == 9);                       // the audio packet does not count against the pongs: 9 x 127 bytes pass 1 KB
    CHECK(q.pending() == audio.size() + 9 * pong.size());
    CHECK(q.controlPending() == 9 * pong.size());
}

TEST_CASE("send queue: memory stays small when a slow line never lets the queue go empty") {
    SendQueue q;
    FakeSocket s;
    const std::string pong(127, 'p');
    size_t most = 0;
    std::string all;
    for (int i = 0; i < 10000; ++i) {
        q.appendIfRoom(pong.data(), pong.size(), 1024);
        all += pong;
        s.room = 100;   // the line takes a little each round, so there is always something waiting
        CHECK(q.flush(std::ref(s)));
        most = std::max(most, q.stored());
    }
    CHECK(!q.idle());
    CHECK(most < 8192);
    s.room = 1 << 20;
    CHECK(q.flush(std::ref(s)));
    CHECK(s.got.size() <= all.size() && all.compare(0, s.got.size(), s.got) == 0);   // what went out is a true prefix: the compaction lost and moved nothing
}
