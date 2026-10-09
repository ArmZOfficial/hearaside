// ui-snapshot: renders the Track, Hub and App Audio editors to PNG files (light + dark, English + Thai)
// so the native UI can be checked against the design mock-up without a DAW.
//   ui-snapshot <output folder>
#include "track/TrackProcessor.h"
#include "track/TrackEditor.h"
#include "hub/HubProcessor.h"
#include "hub/HubEditor.h"
#include "Settings.h"
#include "Links.h"
#include "FriendDirectory.h"
#include "ValueText.h"
#include "ui/Overlay.h"
#ifdef _WIN32
#include "app/AppAudioProcessor.h"
#include "app/AppAudioEditor.h"
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);   // winmm, no <windows.h> here
#pragma comment(lib, "winmm.lib")
#endif

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <thread>

using namespace hearaside;

namespace {

void save(juce::Component& c, const juce::File& file, float scale = 2.0f) {
    c.setVisible(true);
    const auto img = c.createComponentSnapshot(c.getLocalBounds(), true, scale);
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat().writeImageToStream(img, out);
    std::printf("wrote %s (%dx%d)\n", file.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
}

std::function<void(int)> gAudio;   // runs n audio blocks (keeps heartbeats / meters alive)

// Layout check: visible sibling components must not overlap and must stay inside their parent
// (a Viewport's scrolled content is exempt). Returns the problems found, one per line.
juce::StringArray layoutProblems(juce::Component& root) {
    juce::StringArray out;
    std::function<void(juce::Component&, const juce::String&)> walk = [&](juce::Component& c, const juce::String& path) {
        if (dynamic_cast<juce::Viewport*>(&c) != nullptr) {
            if (auto* v = static_cast<juce::Viewport&>(c).getViewedComponent()) walk(*v, path + "/" + v->getName());
            return;
        }
        std::vector<juce::Component*> kids;
        for (auto* k : c.getChildren())
            if (k->isVisible() && !k->getBounds().isEmpty() && dynamic_cast<juce::TooltipWindow*>(k) == nullptr
                && dynamic_cast<juce::ScrollBar*>(k) == nullptr && dynamic_cast<juce::ResizableCornerComponent*>(k) == nullptr
                && dynamic_cast<Overlay*>(k) == nullptr && dynamic_cast<FocusRingOverlay*>(k) == nullptr   // layers over everything
                && !k->getProperties().contains("hsCovers"))                                             // a sheet over a card
                kids.push_back(k);
        auto name = [](juce::Component* k) {
            const auto t = k->getTitle().isNotEmpty() ? k->getTitle() : k->getName();
            return juce::String(typeid(*k).name()).fromLastOccurrenceOf(":", false, false) + (t.isNotEmpty() ? "(" + t + ")" : juce::String());
        };
        for (size_t i = 0; i < kids.size(); ++i) {
            const auto b = kids[i]->getBounds();
            if (!c.getLocalBounds().contains(b) && dynamic_cast<juce::Viewport*>(c.getParentComponent()) == nullptr)
                out.add(path + ": " + name(kids[i]) + " " + b.toString() + " outside " + c.getLocalBounds().toString());
            for (size_t j = i + 1; j < kids.size(); ++j)
                if (b.intersects(kids[j]->getBounds()))
                    out.add(path + ": " + name(kids[i]) + " " + b.toString() + " overlaps " + name(kids[j]) + " " + kids[j]->getBounds().toString());
            walk(*kids[i], path + "/" + name(kids[i]));
        }
    };
    walk(root, "editor");
    return out;
}


// A tiny WebSocket client for --test-friends (what a browser does): handshake, masked binary frames out,
// plain frames in. One socket, one thread.
struct WsClient {
    juce::StreamingSocket sock;
    int status = 0;
    bool open(int port, const juce::String& path, const juce::String& origin = {}) {
        if (!sock.connect("127.0.0.1", port, 2000)) return false;
        const auto req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + juce::String(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + (origin.isNotEmpty() ? "Origin: " + origin + "\r\n" : juce::String()) + "\r\n";
        sock.write(req.toRawUTF8(), int(req.getNumBytesAsUTF8()));
        std::string head;
        char c = 0;
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 4096) {
            if (sock.waitUntilReady(true, 3000) != 1 || sock.read(&c, 1, true) != 1) return false;
            head += c;
        }
        status = juce::String(head.c_str()).fromFirstOccurrenceOf(" ", false, false).getIntValue();
        return status == 101;
    }
    bool send(const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> f;
        f.push_back(0x82);
        const uint8_t mask[4] = { 1, 2, 3, 4 };
        if (payload.size() < 126) f.push_back(uint8_t(0x80 | payload.size()));
        else { f.push_back(0x80 | 126); f.push_back(uint8_t(payload.size() >> 8)); f.push_back(uint8_t(payload.size())); }
        f.insert(f.end(), mask, mask + 4);
        for (size_t i = 0; i < payload.size(); ++i) f.push_back(payload[i] ^ mask[i & 3]);
        return sock.write(f.data(), int(f.size())) == int(f.size());
    }
    // one binary message; empty = nothing within the time (or closed)
    std::vector<uint8_t> receive(int timeoutMs, bool* closed = nullptr) {
        auto readN = [&](uint8_t* dst, size_t n) {
            while (n > 0) {
                if (sock.waitUntilReady(true, timeoutMs) != 1) return false;
                const int got = sock.read(dst, int(n), false);
                if (got <= 0) { if (closed) *closed = true; return false; }
                dst += got; n -= size_t(got);
            }
            return true;
        };
        uint8_t h[2];
        if (!readN(h, 2)) return {};
        uint64_t len = h[1] & 0x7F;
        if (len == 126) { uint8_t e[2]; if (!readN(e, 2)) return {}; len = uint64_t(e[0]) << 8 | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!readN(e, 8)) return {}; len = 0; for (uint8_t b : e) len = len << 8 | b; }
        std::vector<uint8_t> out(static_cast<size_t>(len), uint8_t(0));
        if (len && !readN(out.data(), size_t(len))) return {};
        if ((h[0] & 0x0F) == 8) { if (closed) *closed = true; return {}; }
        return out;
    }
};

std::pair<int, juce::String> httpGet(int port, const juce::String& path) {
    juce::StreamingSocket s;
    if (!s.connect("127.0.0.1", port, 2000)) return { 0, {} };
    const auto req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + juce::String(port) + "\r\nConnection: close\r\n\r\n";
    s.write(req.toRawUTF8(), int(req.getNumBytesAsUTF8()));
    juce::MemoryOutputStream out;
    char buf[4096];
    for (int n; s.waitUntilReady(true, 3000) == 1 && (n = s.read(buf, sizeof buf, false)) > 0;) out.write(buf, size_t(n));
    const auto text = out.toString();
    return { text.fromFirstOccurrenceOf(" ", false, false).getIntValue(), text.fromFirstOccurrenceOf("\r\n\r\n", false, false) };
}

void pump(int ms) {
    const auto end = juce::Time::getMillisecondCounter() + juce::uint32(ms);
    while (juce::Time::getMillisecondCounter() < end) {
        if (gAudio) gAudio(4);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
}

#ifdef _WIN32
// --test-app-audio <program.exe> [snapshot.png] [block]: runs the App Audio plug-in in real time for
// 3 s on that program (play something in it first) and checks that sound arrives.
int testAppAudio(const juce::String& exe, const juce::String& png, int block) {
    std::printf("programs with audio:");
    for (const auto& a : AppCapture::listAudioApps()) std::printf(" %s", a.exe.c_str());
    std::printf("\n");
    timeBeginPeriod(1);   // 1 ms sleeps: blocks arrive about as evenly as from an audio interface
    AppAudioProcessor p;
    p.setPlayConfigDetails(2, 2, 48000.0, block);   // an effect on a plain (silent) audio track
    p.prepareToPlay(48000.0, block);
    p.setApp(exe);
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    float peak = 0;
    int blocks = 0, loud = 0, counted = 0;
    const auto t0 = juce::Time::getMillisecondCounter();
    const int seconds = juce::jmax(3, juce::SystemStats::getEnvironmentVariable("HEARASIDE_TEST_SECONDS", "3").getIntValue());
    for (auto now = t0; now - t0 < juce::uint32(seconds * 1000); now = juce::Time::getMillisecondCounter()) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);   // the plug-in starts the capture from its timer
        while (blocks * block < int(now - t0) * 48) {   // the DAW's pace: one block every block / 48 ms
            buf.clear();
            p.processBlock(buf, midi);
            if (now - t0 > 1000) { peak = std::max(peak, buf.getMagnitude(0, block)); loud += buf.getMagnitude(0, block) > 1e-3f; ++counted; }
            ++blocks;
        }
        juce::Thread::sleep(1);
    }
    std::printf("state %d, peak %.3f, blocks with sound %d of %d, delay ~%.1f ms (block %.1f ms, packets %u frames%s)\n",
                int(p.captureState()), peak, loud, counted, p.latencyMs(), block / 48.0, p.capturePacketFrames(),
                p.captureShortPeriod() ? ", Windows low-latency" : "");
    if (png.isNotEmpty()) {
        std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
        pump(300);
        save(*ed, juce::File::getCurrentWorkingDirectory().getChildFile(png));
    }
    return peak > 1e-3f && loud == counted ? 0 : 1;
}
#endif

#ifdef _WIN32
// --test-sync <program.exe>: one-press auto sync with App Audio as the music (no Track on it).
// The program must play something broadband (real music); a pure tone repeats too often to time.
// The music track is App Audio capturing the program; the "mic" is a second capture of the same
// program delayed 23 ms (like the headphones heard by the mic). Real time, real capture.
int testSync(const juce::String& exe) {
    timeBeginPeriod(1);
    constexpr int B = 256, D = 1104;   // block, mic delay (23 ms)
    _putenv_s("HEARASIDE_DEFAULT_BUS", "SyncMic");   // the mic's own capture is not a source of the tested bus
    AppAudioProcessor micApp;
    _putenv_s("HEARASIDE_DEFAULT_BUS", "SyncTest");
    AppAudioProcessor music;
    TrackProcessor mic;
    HubProcessor hub;
    for (juce::AudioProcessor* p : std::initializer_list<juce::AudioProcessor*> { &micApp, &music, &mic, &hub }) {
        p->setPlayConfigDetails(2, 2, 48000.0, B);
        p->prepareToPlay(48000.0, B);
    }
    micApp.setApp(exe);
    music.setApp(exe);
    mic.setDisplayNameOverride(juce::String::fromUTF8("เสียงร้อง"));
    auto* monParam = mic.params().getParameter(trackparam::Mon);
    monParam->setValueNotifyingHost(0.0f);

    juce::AudioBuffer<float> m(2, B), v(2, B), line(2, 48000), master(2, B);
    juce::MidiBuffer midi;
    int lw = 0, blocks = 0;
    auto run = [&] {
        m.clear();
        music.processBlock(m, midi);
        v.clear();
        micApp.processBlock(v, midi);
        for (int c = 0; c < 2; ++c)   // the mic hears it 23 ms later
            for (int i = 0; i < B; ++i) {
                line.setSample(c, (lw + i) % 48000, v.getSample(c, i));
                v.setSample(c, i, line.getSample(c, (lw + i - D + 48000) % 48000));
            }
        lw = (lw + B) % 48000;
        mic.processBlock(v, midi);
        master.makeCopyOf(m);
        master.addFrom(0, 0, v, 0, 0, B);
        master.addFrom(1, 0, v, 1, 0, B);
        hub.processBlock(master, midi);
        ++blocks;
    };
    auto realtime = [&](int ms, std::function<bool()> until) {
        const auto t0 = juce::Time::getMillisecondCounter();
        const int b0 = blocks;
        for (auto now = t0; now - t0 < juce::uint32(ms); now = juce::Time::getMillisecondCounter()) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            while ((blocks - b0) * B < int(now - t0) * 48) run();
            if (until && until()) return;
            juce::Thread::sleep(1);
        }
    };
    realtime(6000, nullptr);   // captures start, buffers settle
    std::printf("music delay ~%.1f ms, mic capture delay ~%.1f ms\n", music.latencyMs(), micApp.latencyMs());
    hub.startAutoSync();
    using P = HubProcessor::SyncPhase;
    realtime(15000, [&] { const auto ph = hub.autoSync().phase; return ph == P::Done || ph == P::Failed; });
    realtime(500, nullptr);
    const auto st = hub.autoSync();
    const double expected = D / 48.0 + micApp.latencyMs() - music.latencyMs();
    const float got = music.params().getRawParameterValue(appparam::Delay)->load();
    std::printf("phase %d (error \"%s\"), mic late by %.2f ms, App Audio sync delay %.2f ms, expected ~%.2f ms\n",
                int(st.phase), tr(st.error).toRawUTF8(), st.deltaMs, got, expected);
    return st.phase == P::Done && std::abs(got - expected) < 1.5 ? 0 : 1;
}
#endif

// --test-directory: ShareDirectory (permanent links) against a mock share web site: register,
// heartbeat, retry after errors, unregister when sharing stops, and a quick shutdown while the
// site hangs (the DAW must never wait on the network). Exit 1 on a failure.
int testDirectory() {
    struct Mock {
        juce::StreamingSocket listener;
        std::thread thread;
        std::atomic<bool> run { true }, hang { false };
        std::atomic<int> status { 200 }, registers { 0 }, unregisters { 0 };
        std::mutex m;
        juce::String lastBody;
        int port = 0;
        Mock() {
            for (int p = 47870; p < 47890 && port == 0; ++p) if (listener.createListener(p, "127.0.0.1")) port = p;
            thread = std::thread([this] {
                while (run) {
                    std::unique_ptr<juce::StreamingSocket> s(listener.waitForNextConnection());
                    if (!s) continue;
                    std::string req;
                    char buf[4096];
                    while (req.find("\r\n\r\n") == std::string::npos && s->waitUntilReady(true, 2000) == 1) {
                        const int n = s->read(buf, sizeof buf, false);
                        if (n <= 0) break;
                        req.append(buf, size_t(n));
                    }
                    const auto head = req.substr(0, req.find("\r\n\r\n"));
                    const auto lenAt = juce::String(head).indexOfIgnoreCase("Content-Length:");
                    const int len = lenAt >= 0 ? juce::String(head).substring(lenAt + 15).getIntValue() : 0;
                    while (int(req.size() - head.size() - 4) < len && s->waitUntilReady(true, 2000) == 1) {
                        const int n = s->read(buf, sizeof buf, false);
                        if (n <= 0) break;
                        req.append(buf, size_t(n));
                    }
                    if (hang) { while (hang && run) juce::Thread::sleep(20); continue; }   // never answers
                    const bool reg = req.find("POST /api/register") == 0;
                    (reg ? registers : unregisters)++;
                    { const std::lock_guard<std::mutex> l(m); lastBody = juce::String(req.substr(head.size() + 4)); }
                    const auto body = juce::String(status == 200 ? "{\"ok\":true}" : "{\"error\":\"x\"}");
                    const auto resp = "HTTP/1.1 " + juce::String(status.load()) + " X\r\nContent-Type: application/json\r\nContent-Length: "
                                    + juce::String(body.length()) + "\r\nConnection: close\r\n\r\n" + body;
                    s->write(resp.toRawUTF8(), int(resp.getNumBytesAsUTF8()));
                }
            });
        }
        ~Mock() { run = false; hang = false; listener.close(); thread.join(); }
    } mock;
    int failures = 0;
    auto expect = [&](const char* what, bool ok) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += ok ? 0 : 1; };
    auto waitFor = [](std::function<bool()> f, int ms) {
        for (int t = 0; t < ms && !f(); t += 20) juce::Thread::sleep(20);
        return f();
    };
    const auto base = "http://127.0.0.1:" + juce::String(mock.port);
    const juce::String secret = juce::String::repeatedString("ab", 32);
    {
        ShareDirectory dir;
        dir.setTimings(300, 100);
        dir.update(base, "listentokenlisten000000000", "sendtokensendtoken00000000", secret, {});
        juce::Thread::sleep(300);
        expect("no tunnel yet: nothing sent", mock.registers == 0 && dir.state() == ShareDirectory::State::Off);
        dir.update(base, "listentokenlisten000000000", "sendtokensendtoken00000000", secret, "https://abc-def.trycloudflare.com");
        expect("tunnel ready: registered, online", waitFor([&] { return dir.state() == ShareDirectory::State::Online; }, 3000));
        {
            const std::lock_guard<std::mutex> l(mock.m);
            const auto j = juce::JSON::parse(mock.lastBody);
            expect("body: tunnel, both tokens, the secret", j["tunnel"] == "https://abc-def.trycloudflare.com" && j["tokens"]["l"] == "listentokenlisten000000000"
                                                            && j["tokens"]["s"] == "sendtokensendtoken00000000" && j["secret"] == secret);
        }
        expect("permanent link address", dir.listenUrl() == base + "/l/listentokenlisten000000000");
        const int before = mock.registers;
        expect("heartbeat keeps registering", waitFor([&] { return mock.registers >= before + 2; }, 3000));
        mock.status = 503;
        expect("site errors: unreachable", waitFor([&] { return dir.state() == ShareDirectory::State::Unreachable; }, 3000));
        mock.status = 200;
        expect("site back: online again (retry with backoff)", waitFor([&] { return dir.state() == ShareDirectory::State::Online; }, 5000));
        dir.stop();
        expect("sharing off: unregistered", waitFor([&] { return mock.unregisters == 1; }, 3000));
        expect("then off", waitFor([&] { return dir.state() == ShareDirectory::State::Off; }, 2000));
    }
    {
        auto dir = std::make_unique<ShareDirectory>();
        dir->update(base, "listentokenlisten000000000", {}, secret, "https://abc-def.trycloudflare.com");
        juce::Thread::sleep(100);
        mock.hang = true;   // the site takes the request and never answers
        dir->update(base, "listentokenlisten000000000", {}, secret, "https://other-name.trycloudflare.com");
        juce::Thread::sleep(300);
        const auto t0 = juce::Time::getMillisecondCounter();
        dir.reset();   // the DAW closes the song now
        const auto ms = juce::Time::getMillisecondCounter() - t0;
        std::printf("     shutdown with a hanging request took %u ms\n", ms);
        expect("closing never waits on the network (< 1 s)", ms < 1000);
        mock.hang = false;
    }
    std::printf("%s\n", failures == 0 ? "share directory: all passed" : "share directory: FAILED");
    return failures == 0 ? 0 : 1;
}

// --test-ui: the pure parts of the UI (values typed by people, names, share links). Exit 1 on a failure.
int testUi() {
    using namespace valuetext;
    int failures = 0;
    auto expect = [&](const char* what, bool ok) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += ok ? 0 : 1; };
    const auto minus = juce::String(juce::CharPointer_UTF8("\xe2\x88\x92"));
    auto near = [](std::optional<float> v, float x) { return v.has_value() && std::abs(*v - x) < 1.0e-4f; };
    // EditableValue: dB (prompt 3.2)
    expect("dB: -3", near(parseDb("-3", -30, 6), -3.0f));
    expect("dB: U+2212 3", near(parseDb(minus + "3", -30, 6), -3.0f));
    expect("dB: -3 dB", near(parseDb("-3 dB", -30, 6), -3.0f));
    expect("dB: +2", near(parseDb("+2", -30, 6), 2.0f));
    expect("dB: 2,5", near(parseDb("2,5", -30, 6), 2.5f));
    expect("dB: -inf = bottom", near(parseDb("-inf", -30, 6), -30.0f));
    expect("dB: infinity sign = bottom", near(parseDb(juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e")), -30, 6), -30.0f));
    expect("dB: out of range clamps", near(parseDb("+20", -30, 6), 6.0f) && near(parseDb("-99", -30, 6), -30.0f));
    expect("dB: nonsense keeps the old value", !parseDb("loud", -30, 6).has_value() && !parseDb("", -30, 6).has_value() && !parseDb("1.2.3", -30, 6).has_value());
    expect("dB: shown 0.0 dB", valuetext::formatDb(0.0f) == "0.0 dB");
    expect("dB: shown with U+2212", valuetext::formatDb(-3.0f) == minus + "3.0 dB");
    expect("dB: shown +2.0 dB", valuetext::formatDb(2.0f) == "+2.0 dB");
    expect("dB: shown -inf", valuetext::formatDb(-30.0f) == minus + juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e")) + " dB");
    expect("dB: field text", editDb(-30.0f) == "-inf" && editDb(2.0f) == "+2.0" && editDb(-3.0f) == "-3.0");
    // ms
    expect("ms: 40 ms", near(parseMs("40 ms", 0, 500), 40.0f) && formatMs(40.0f) == "40 ms");
    expect("ms: clamps", near(parseMs("900", 0, 500), 500.0f));
    // pan
    expect("pan: L40", near(parsePan("L40"), -40.0f));
    expect("pan: C", near(parsePan("C"), 0.0f));
    expect("pan: R25", near(parsePan("R25"), 25.0f));
    expect("pan: l 40", near(parsePan("l 40"), -40.0f));
    expect("pan: -40", near(parsePan("-40"), -40.0f));
    expect("pan: R250 clamps", near(parsePan("R250"), 100.0f));
    expect("pan: nonsense", !parsePan("left").has_value());
    expect("pan: shown", formatPan(-40.0f) == "L40" && formatPan(0.0f) == "C" && formatPan(25.0f) == "R25");
    // names: UTF-8 cut at a code point (63 bytes + NUL in shared memory)
    const auto thai = juce::String::fromUTF8("เสียงร้องประสานชุดที่สองแบบยาวมากจริงๆ");
    const auto cut = truncateUtf8(thai, 63);
    expect("name: Thai cut fits 63 bytes", int(cut.getNumBytesAsUTF8()) <= 63 && thai.startsWith(cut) && cut.length() == 21);
    const auto emoji = juce::String::fromUTF8("Mint \xf0\x9f\x8e\xa4\xf0\x9f\x8e\xa4\xf0\x9f\x8e\xa4");
    expect("name: emoji never split", truncateUtf8(emoji, 8) == juce::String::fromUTF8("Mint ") && truncateUtf8(emoji, 10) == juce::String::fromUTF8("Mint \xf0\x9f\x8e\xa4"));
    expect("name: short names untouched", truncateUtf8("Vocal", 63) == "Vocal");
    expect("initial of a Thai name skips the leading vowel", initialOf(juce::String::fromUTF8("เสียง")) == juce::String::fromUTF8("ส"));
    // share links (S7 parser)
    using links::Kind;
    expect("link: permanent /l/", links::parse("https://hearaside.vercel.app/l/7Kq2mW9fAbCd").kind == Kind::Listen);
    expect("link: tunnel /s/ with a slash", links::parse("https://abc-def.trycloudflare.com/s/7Kq2mW9fAbCd/").kind == Kind::Send);
    expect("link: Wi-Fi address", links::parse("http://192.168.1.5:47810/s/7Kq2mW9fAbCd").kind == Kind::Send);
    expect("link: token kept", links::parse("https://x.example/l/7Kq2mW9fAbCd?x=1").token == "7Kq2mW9fAbCd");
    expect("link: not a link", links::parse("hello").kind == Kind::None && links::parse("https://example.com/").kind == Kind::None
                               && links::parse("https://example.com/x/7Kq2mW9fAbCd").kind == Kind::None && links::parse("ftp://h/l/7Kq2mW9fAbCd").kind == Kind::None);
    expect("link: short token refused", links::parse("https://h/l/abc").kind == Kind::None);
    std::printf("%s\n", failures == 0 ? "ui: all passed" : "ui: FAILED");
    return failures == 0 ? 0 : 1;
}

// Every design-system component in one frame (prompt 4 U1: a demo page in ui-snapshot, both themes).
struct Gallery : juce::Component {
    LookAndFeel lnf;
    Overlay overlay;
    std::vector<std::unique_ptr<juce::Component>> owned;
    juce::Rectangle<int> area { 24, 24, 0, 0 };
    int x = 24, y = 24, rowH = 0;
    explicit Gallery(bool dark) {
        lnf.setDark(dark);
        setLookAndFeel(&lnf);
        setSize(1180, 860);
    }
    ~Gallery() override { owned.clear(); setLookAndFeel(nullptr); }
    template <typename T> T& add(std::unique_ptr<T> c, int w, int h) {
        if (x + w > getWidth() - 24) { x = 24; y += rowH + 16; rowH = 0; }
        c->setBounds(x, y, w, h);
        x += w + 16;
        rowH = std::max(rowH, h);
        auto& ref = *c;
        addAndMakeVisible(ref);
        owned.push_back(std::move(c));
        return ref;
    }
    void newLine() { x = 24; y += rowH + 22; rowH = 0; }
    void paint(juce::Graphics& g) override {
        const auto& p = lnf.pal();
        g.fillAll(p.paper);
        g.setColour(p.glass);
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(8.0f), 22.0f);
        drawWordmark(g, { 24.0f, 820.0f, 300.0f, 30.0f }, "COMPONENTS", 16.0f, p.ink);
        const auto& pal = p;
        float bx = 340.0f;
        for (auto d : { Dot::Ok, Dot::Warn, Dot::Rec, Dot::RecRing, Dot::Muted }) { drawStatusDot(g, { bx, 835.0f }, d, pal); bx += 18.0f; }
        drawBadge(g, { bx + 10.0f, 825.0f, badgeWidth("40 ms"), 20.0f }, "40 ms", BadgeStyle::Chip, pal);
        drawBadge(g, { bx + 70.0f, 825.0f, badgeWidth("Solo", BadgeStyle::Solid), 20.0f }, "Solo", BadgeStyle::Solid, pal);
        drawBadge(g, { bx + 120.0f, 825.0f, badgeWidth("Recording", BadgeStyle::Rec), 20.0f }, "Recording", BadgeStyle::Rec, pal);
        drawAvatar(g, { bx + 220.0f, 817.0f, 28.0f, 28.0f }, "Mint", true, pal);
        drawAvatar(g, { bx + 256.0f, 817.0f, 28.0f, 28.0f }, "Fah", false, pal);
    }
};

void buildGallery(Gallery& gl) {
    using std::make_unique;
    for (bool on : { true, false })
        for (auto side : { AudibleToggle::Side::You, AudibleToggle::Side::Viewers }) {
            auto& t = gl.add(make_unique<AudibleToggle>(side), 48, 38);
            t.setOn(on, false);
        }
    for (bool on : { true, false }) {
        auto& t = gl.add(make_unique<AudibleToggle>(AudibleToggle::Side::Viewers), 150, 42);
        t.setLabelShown(true);
        t.setOn(on, false);
    }
    gl.add(make_unique<AudibleToggle>(AudibleToggle::Side::You), 40, 32).setOn(true, false);
    {
        auto& s = gl.add(make_unique<Switch>(), 36, 20);
        s.setOn(true, false);
        gl.add(make_unique<Switch>(), 36, 20);
        gl.add(make_unique<Switch>(), 46, 26).setOn(true, false);
        auto& ls = gl.add(make_unique<LabelledSwitch>(), 100, 36);
        ls.setPill(true);
        ls.setOn(true, false);
    }
    gl.newLine();
    {
        auto& a = gl.add(make_unique<LevelSlider>(), 220, 20);
        a.setValue(-3.0);
        auto& b = gl.add(make_unique<LevelSlider>(), 220, 20);
        b.setValue(-6.0);
        b.setDim(true);
        auto& pf = gl.add(make_unique<PanSlider>(true), 260, PanSlider::kFullHeight);
        pf.setPan(-40.0f);
        auto& pc = gl.add(make_unique<PanSlider>(false), 160, 20);
        pc.setPan(25.0f);
    }
    gl.newLine();
    {
        auto& v1 = gl.add(make_unique<EditableValue>(EditableValue::Kind::Db, -30.0f, 6.0f), 72, 26);
        v1.setValue(-3.0f);
        auto& v2 = gl.add(make_unique<EditableValue>(EditableValue::Kind::Db, -30.0f, 6.0f), 72, 26);
        v2.setValue(-30.0f);
        auto& v3 = gl.add(make_unique<EditableValue>(EditableValue::Kind::Ms, 0.0f, 500.0f), 72, 26);
        v3.setValue(40.0f);
        auto& v4 = gl.add(make_unique<EditableValue>(EditableValue::Kind::Pan, -100.0f, 100.0f), 72, 26);
        v4.setValue(-40.0f);
        auto& v5 = gl.add(make_unique<EditableValue>(EditableValue::Kind::Db, -30.0f, 6.0f), 72, 26);
        v5.setValue(2.0f);
        v5.startEditing();
        auto& n1 = gl.add(make_unique<InlineName>(), 160, 26);
        n1.setName("Backing track", "Audio 03");
        auto& n2 = gl.add(make_unique<InlineName>(), 160, 26);
        n2.setName("Lead vocal", "Audio 04");
        n2.startEditing();
        auto& tf = gl.add(make_unique<TextField>(juce::String(juce::CharPointer_UTF8("https://\xe2\x80\xa6/l/\xe2\x80\xa6"))), 260, 38);
        juce::ignoreUnused(tf);
    }
    gl.newLine();
    {
        auto& dd = gl.add(make_unique<Dropdown>(), 200, 38);
        dd.setItems({ "None", "Stem 1", "Stem 2" });
        auto& sp = gl.add(make_unique<SourcePicker>(), 300, 48);
        sp.set(icons::Icon::Window, "Chrome");
        auto& ss = gl.add(make_unique<SourcePicker>(true), 220, 26);
        ss.set(icons::Icon::Monitor, "Whole computer (except the DAW)");
        auto& seg = gl.add(make_unique<SegmentedControl>(), 206, 32);
        seg.setSegments({ { "Headphones", icons::Icon::Headphones }, { "Viewers", icons::Icon::Broadcast } });
        seg.setSelected(1);
        auto& st = gl.add(make_unique<Stepper>(0, 2), 160, 34);
        st.setValue(2);
        st.setUnit("block");
    }
    gl.newLine();
    {
        auto& g1 = gl.add(make_unique<GhostButton>("Manage"), 110, 36);
        g1.setIcon(icons::Icon::Sliders);
        auto& g2 = gl.add(make_unique<GhostButton>("Add friend", GhostButton::Style::Solid), 130, 36);
        g2.setIcon(icons::Icon::Plus);
        gl.add(make_unique<GhostButton>("Reset", GhostButton::Style::Danger), 90, 36);
        auto& g4 = gl.add(make_unique<GhostButton>("Copy"), 70, 30);
        g4.setSmall(true);
        gl.add(make_unique<IconButton>(icons::Icon::Link), 40, 40);
        gl.add(make_unique<IconButton>(icons::Icon::Sliders), 36, 36).setOn(true);
        gl.add(make_unique<MoreButton>(), 32, 32);
        gl.add(make_unique<BackButton>(true), 90, 40);
        auto& m1 = gl.add(make_unique<MuteButton>(), 150, 40);
        juce::ignoreUnused(m1);
        auto& m2 = gl.add(make_unique<MuteButton>(), 160, 40);
        m2.setActive(true);
        auto& lk = gl.add(make_unique<LinkButton>("Manage"), 60, 18);
        juce::ignoreUnused(lk);
    }
    gl.newLine();
    {
        auto& p1 = gl.add(make_unique<PrimaryButton>(icons::Icon::Headphones), 260, 48);
        p1.setButtonText(tr(Str::PreviewOff));
        auto& p2 = gl.add(make_unique<PrimaryButton>(icons::Icon::Headphones), 260, 48);
        p2.setButtonText(tr(Str::PreviewOn));
        p2.setActive(true);
        auto& c1 = gl.add(make_unique<StatusChip>(36.0f), 250, 36);
        c1.set("OBS not connected", Dot::Warn, "5.3 ms", true);
        auto& c2 = gl.add(make_unique<StatusChip>(30.0f), 170, 30);
        c2.set("Connected to Hub", Dot::Ok);
        auto& d = gl.add(make_unique<Disclosure>("Other DAWs"), 220, 38);
        d.setOpen(true);
    }
    gl.newLine();
    {
        auto& b1 = gl.add(make_unique<Banner>(), 360, 58);
        b1.set(Banner::Style::Warning, icons::Icon::Warning, tr(Str::HubMissingBanner));
        auto& b2 = gl.add(make_unique<Banner>(), 360, 58);
        b2.set(Banner::Style::Dark, icons::Icon::Headphones, tr(Str::SyncViewersUntil));
        auto& steps = gl.add(make_unique<NumberedSteps>(), 360, 110);
        steps.setSteps({ tr(Str::StudioOneStep1), tr(Str::StudioOneStep2), tr(Str::StudioOneStep3) });
    }
    gl.newLine();
    {
        auto& row = gl.add(make_unique<ToggleRow>(icons::Icon::Headphones), 340, 60);
        row.setTexts(tr(Str::MonRowTitle), tr(Str::MonRowCaption));
        auto& row2 = gl.add(make_unique<ToggleRow>(icons::Icon::Broadcast), 340, 60);
        row2.setTexts(tr(Str::StrRowTitle), tr(Str::StrRowCaption));
        row2.setOn(true, false);
        auto& mb = gl.add(make_unique<MeterBar>(4.0f, true), 120, 8);
        mb.setLevel(0.7f);
    }
    // a menu and a toast on top
    gl.addAndMakeVisible(gl.overlay);
    gl.overlay.setBounds(gl.getLocalBounds());
    auto& anchor = gl.add(std::make_unique<MoreButton>(), 32, 32);
    gl.overlay.toast(trf(Str::RenamedToast, { "Lead vocal" }), 60000);
    Menu m(236);
    m.header("Backing track");
    m.toggle(tr(Str::StreamSolo), true, [] {});
    m.item(tr(Str::RenameDisplay), [] {}, tr(Str::RenameHint));
    m.separator();
    m.item(tr(Str::Pan), [] {}, tr(Str::Center));
    m.item(tr(Str::Delay), [] {}, "40 ms");
    m.item(tr(Str::Stem), [] {}, juce::String(juce::CharPointer_UTF8("None \xe2\x80\xba")));
    m.separator();
    m.item(tr(Str::FineSettingsEllipsis), [] {}, {}, true);
    gl.overlay.showMenu(std::move(m), anchor);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("HEARASIDE_SETTINGS_NAME", "settings-ui-snapshot");   // never the user's window sizes, flags, language
#else
    setenv("HEARASIDE_SETTINGS_NAME", "settings-ui-snapshot", 1);
#endif
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc > 1 && juce::String(argv[1]) == "--test-directory") return testDirectory();
    if (argc > 1 && juce::String(argv[1]) == "--test-ui") return testUi();
#ifdef _WIN32
    if (argc > 2 && juce::String(argv[1]) == "--test-sync") return testSync(argv[2]);
    if (argc > 2 && juce::String(argv[1]) == "--test-app-audio") return testAppAudio(argv[2], argc > 3 ? juce::String(argv[3]) : juce::String(), argc > 4 ? juce::String(argv[4]).getIntValue() : 480);
#endif
    // --audit [folder]: the UX audit matrix (sizes x language x theme x pages x states) + a layout
    // check; exits 1 when a layout problem is found
    const bool audit = argc > 1 && juce::String(argv[1]) == "--audit";
    const juce::String outArg = audit ? (argc > 2 ? juce::String(argv[2]) : juce::String("ui-snapshots/audit"))
                              : argc > 1 && !juce::String(argv[1]).startsWith("--") ? juce::String(argv[1]) : juce::String("ui-snapshots");
    const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile(outArg);
    dir.createDirectory();

    SharedSettings settings;
    const auto oldLang = settings->language();
    const auto oldTheme = settings->themeMode();

    // a Hub and seven Tracks on a private bus, like the mock-up (new instances must not touch a
    // running DAW's "Main" bus even before setBusName)
#ifdef _WIN32
    _putenv_s("HEARASIDE_DEFAULT_BUS", "Snapshot");
#endif
    HubProcessor hub;
    hub.setBusName("Snapshot");
    hub.prepareToPlay(48000, 256);
    struct T { const char* name; bool mon, str; float db, trim, pan; int stem; };
    const T demo[] = { { "Backing track", true, true, -3.0f, 0.0f, 0.0f, -1 }, { "Vocal", false, true, 0.0f, 0.0f, 0.0f, 1 },
                       { "Guitar", true, true, -2.0f, -6.0f, -30.0f, -1 }, { "Click / Guide", true, false, -6.0f, -10.0f, 0.0f, -1 },
                       { "Talk mic", false, false, 0.0f, 0.0f, 0.0f, -1 }, { "Bass", true, true, -1.5f, -4.0f, 0.0f, -1 },
                       { "Chorus", false, true, -4.0f, 0.0f, 25.0f, 1 } };
    std::vector<std::unique_ptr<TrackProcessor>> tracks;
    for (const auto& d : demo) {
        auto t = std::make_unique<TrackProcessor>();
        t->setBusName("Snapshot");
        t->setDisplayNameOverride(juce::String::fromUTF8(d.name));
        t->prepareToPlay(48000, 256);
        auto set = [&](const char* id, float v) { auto* p = t->params().getParameter(id); p->setValueNotifyingHost(p->convertTo0to1(v)); };
        set(trackparam::Mon, d.mon ? 1.0f : 0.0f);
        set(trackparam::Str, d.str ? 1.0f : 0.0f);
        set(trackparam::StrGain, d.db);
        set(trackparam::MonTrim, d.trim);
        set(trackparam::StrPan, d.pan);
        t->setStemIndex(d.stem);
        if (tracks.empty()) set(trackparam::StrDelay, 40.0f);
        tracks.push_back(std::move(t));
    }
#ifdef _WIN32
    // two App Audio instances on the same bus (they join HEARASIDE_DEFAULT_BUS)
    std::vector<std::unique_ptr<AppAudioProcessor>> apps;
    for (const char* exe : { "chrome.exe", AppAudioProcessor::kSystemAudio }) {
        auto a = std::make_unique<AppAudioProcessor>();
        a->setPlayConfigDetails(2, 2, 48000.0, 256);
        a->prepareToPlay(48000.0, 256);
        a->setApp(exe);
        apps.push_back(std::move(a));
    }
    apps[1]->setFollowRecord(true);
#endif
    // run a few audio blocks so meters, heartbeats and mirrors are live
    juce::AudioBuffer<float> buf(2, 256), master(2, 256);
    juce::MidiBuffer midi;
    int block = 0;
    gAudio = [&](int count) { for (int k = 0; k < count; ++k, ++block) {
        const int b = block;
        master.clear();
#ifdef _WIN32
        for (auto& a : apps) { buf.clear(); a->processBlock(buf, midi); }
#endif
        for (size_t i = 0; i < tracks.size(); ++i) {
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < 256; ++s)
                    buf.setSample(c, s, 0.3f / float(i + 1) * std::sin(float(b * 256 + s) * 0.03f * float(i + 1)));
            tracks[i]->processBlock(buf, midi);
            for (int c = 0; c < 2; ++c) master.addFrom(c, 0, buf, c, 0, 256, 0.3f);   // the tracks reach the master (what viewers hear)
        }
        hub.processBlock(master, midi);
    } };
    gAudio(200);

    if (audit) {
        const auto oldScale = settings->uiScale();
        settings->setUiScale(1.0f);   // a chosen 100 %: editors must not pick a size for the screen
        settings->setFlag("startHidden", true);
        // the friends room: three friends in the three states (singing, hasn't opened the link, left)
#ifdef _WIN32
        _putenv_s("HEARASIDE_NO_TUNNEL", "1");
#endif
        {
            const uint32_t mint = hub.addFriend("Mint"), beam = hub.addFriend("Beam"), fah = hub.addFriend("Fah");
            hub.setFriendPan(mint, -0.4f); hub.setFriendPan(beam, 0.25f);
            hub.setFriendVolumeDb(mint, -2.0f); hub.setFriendVolumeDb(beam, 1.5f);
            pump(400);
            if (auto* bus = hub.engine().bus()) {
                bus->friends[0].state.store(ssbus::kFriendLive);
                bus->friends[0].delayBits.store(ssbus::floatBits(180.0f));
                bus->friends[0].peakBits.store(ssbus::floatBits(0.3f));
                bus->friends[2].state.store(ssbus::kFriendOffline);
            }
            (void) fah;
            pump(150);
        }
        juce::String report;
        int problems = 0;
        auto check = [&](juce::Component& c, const juce::String& name) {
            const auto probs = layoutProblems(c);
            problems += probs.size();
            report << name << " (" << c.getWidth() << "x" << c.getHeight() << "): " << (probs.isEmpty() ? juce::String("ok") : juce::String(probs.size()) + " problem(s)") << "\n";
            for (const auto& pr : probs) report << "    " << pr << "\n";
        };
        // an editor at a content size (w, h); 0 = its default size. prep: put it in a state first.
        auto shot = [&](auto& p, const juce::String& name, int w, int h, std::function<void(juce::AudioProcessorEditor&)> prep = {}) {
            std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
            if (w > 0) ed->setSize(w, h);
            pump(150);
            if (prep) { prep(*ed); pump(250); }
            else pump(100);
            save(*ed, dir.getChildFile(name + ".png"), 1.0f);
            check(*ed, name);
        };
        auto page = [](HubEditor::Page pg, int slot = -1) {
            return [pg, slot](juce::AudioProcessorEditor& e) { if (auto* he = dynamic_cast<HubEditor*>(&e)) he->showPage(pg, slot); };
        };
        struct Size { const char* tag; int w, h; };
        const Size hubSizes[] = { { "default", 1040, 790 }, { "wide", 1600, 1000 }, { "laptop", 1000, 560 }, { "regular-edge", 780, 600 },
                                  { "compact", 420, 790 }, { "min", int(theme::layout::hubMinW), int(theme::layout::hubMinH) } };
        const std::pair<HubEditor::Page, const char*> pages[] = { { HubEditor::Page::Share, "share" }, { HubEditor::Page::Settings, "settings" },
                                                                 { HubEditor::Page::Track, "track" }, { HubEditor::Page::Sync, "sync" },
                                                                 { HubEditor::Page::Tracks, "tracks" }, { HubEditor::Page::Programs, "programs" },
                                                                 { HubEditor::Page::Setup, "setup" } };
        for (int lang = 0; lang < 2; ++lang) {
            for (int dark = 0; dark < 2; ++dark) {
                settings->setLanguage(lang == 0 ? Language::English : Language::Thai);
                settings->setThemeMode(dark ? ThemeMode::Dark : ThemeMode::Light);
                const juce::String sfx = juce::String(lang == 0 ? "en" : "th") + (dark ? "-dark" : "-light");
                for (const auto& s : hubSizes) shot(hub, "hub-" + juce::String(s.tag) + "-" + sfx, s.w, s.h);
                for (const auto& [pg, tag] : pages) {
                    shot(hub, "page-" + juce::String(tag) + "-" + sfx, 1040, 790, page(pg, hub.tracks().front().slot));
                    if (lang == 0 && !dark) shot(hub, "page-" + juce::String(tag) + "-compact-" + sfx, 420, 790, page(pg, hub.tracks().front().slot));
                }
                for (int sec = 1; sec < 4; ++sec)
                    shot(hub, "page-settings-" + juce::String(sec) + "-" + sfx, 1040, 790, [sec](juce::AudioProcessorEditor& e) {
                        if (auto* he = dynamic_cast<HubEditor*>(&e)) he->showSettings(HubEditor::SettingsSection(sec));
                    });
                shot(*tracks[1], "track-default-" + sfx, TrackEditor::kWidth, TrackEditor::kHeight);
                shot(*tracks[1], "track-short-" + sfx, 380, 520);
                shot(*tracks[1], "track-min-" + sfx, int(theme::layout::trackMinW), int(theme::layout::trackMinH));
                shot(*tracks[1], "track-fine-" + sfx, TrackEditor::kWidth, TrackEditor::kHeight, [](juce::AudioProcessorEditor& e) {
                    if (auto* te = dynamic_cast<TrackEditor*>(&e)) te->openFineSettings(true);
                });
#ifdef _WIN32
                shot(*apps[0], "app-default-" + sfx, 440, 600);
                shot(*apps[0], "app-short-" + sfx, 440, 480);
                shot(*apps[0], "app-min-" + sfx, int(theme::layout::appMinW), int(theme::layout::appMinH));
#endif
                {   // the component gallery (U1)
                    Gallery g(dark != 0);
                    buildGallery(g);
                    pump(200);
                    save(g, dir.getChildFile("gallery-" + sfx + ".png"), 1.0f);
                }
            }
        }
        // states (English + Thai, light)
        for (int lang = 0; lang < 2; ++lang) {
            settings->setLanguage(lang == 0 ? Language::English : Language::Thai);
            settings->setThemeMode(ThemeMode::Light);
            const juce::String sfx = lang == 0 ? "en" : "th";
            hub.setParam(hubparam::Preview, 1.0f);
            hub.setParam(hubparam::Panic, 1.0f);
            shot(hub, "state-preview-panic-default-" + sfx, 1040, 790);
            shot(hub, "state-preview-panic-compact-" + sfx, 420, 790);
            hub.setParam(hubparam::Preview, 0.0f);
            hub.setParam(hubparam::Panic, 0.0f);
            shot(hub, "state-menu-track-" + sfx, 1040, 790, [](juce::AudioProcessorEditor& e) { if (auto* he = dynamic_cast<HubEditor*>(&e)) he->openRowMenu(0); });
            shot(hub, "state-menu-program-" + sfx, 1040, 790, [](juce::AudioProcessorEditor& e) { if (auto* he = dynamic_cast<HubEditor*>(&e)) he->openRowMenu(7); });
            shot(hub, "state-obs-popover-" + sfx, 1040, 790, [](juce::AudioProcessorEditor& e) { if (auto* he = dynamic_cast<HubEditor*>(&e)) he->openObsPopover(); });
            shot(hub, "state-compact-levels-" + sfx, 420, 790, [](juce::AudioProcessorEditor& e) { if (auto* he = dynamic_cast<HubEditor*>(&e)) he->setCompactTab(1); });
            shot(hub, "state-compact-summary-" + sfx, 420, 790, [](juce::AudioProcessorEditor& e) { if (auto* he = dynamic_cast<HubEditor*>(&e)) he->setCompactTab(2); });
#ifdef _WIN32
            shot(*apps[0], "state-app-receive-" + sfx, 440, 600, [](juce::AudioProcessorEditor& e) { if (auto* ae = dynamic_cast<AppAudioEditor*>(&e)) ae->showReceiveLink(); });
            apps[1]->setOn(false);
            pump(200);
            shot(*apps[1], "state-app-off-" + sfx, 440, 600);
            apps[1]->setOn(true);
#endif
        }
        settings->setLanguage(Language::English);
        settings->setFlag("startHidden", false);
        {   // nothing on the bus yet: the empty state (getting started)
#ifdef _WIN32
            _putenv_s("HEARASIDE_DEFAULT_BUS", "AuditEmpty");
#endif
            HubProcessor empty;
            empty.setBusName("AuditEmpty");
            empty.prepareToPlay(48000, 256);
            juce::AudioBuffer<float> b(2, 256);
            for (int i = 0; i < 20; ++i) empty.processBlock(b, midi);
            shot(empty, "state-empty-default-en", 1040, 790);
            shot(empty, "state-empty-compact-en", 420, 790);
#ifdef _WIN32
            _putenv_s("HEARASIDE_DEFAULT_BUS", "Snapshot");
#endif
        }
        {   // 40+ tracks, one with a very long Thai name
            const size_t keep = tracks.size();
            for (int i = 0; i < 38; ++i) {
                auto t = std::make_unique<TrackProcessor>();
                t->setBusName("Snapshot");
                t->setDisplayNameOverride(i == 0 ? juce::String::fromUTF8("เสียงร้องประสานชุดที่สองแบบยาวมากจริงๆ (Backing Vocals Group B, double-tracked)")
                                                 : "Track " + juce::String(i + 8));
                t->prepareToPlay(48000, 256);
                tracks.push_back(std::move(t));
            }
            gAudio(40);
            settings->setLanguage(Language::Thai);
            shot(hub, "state-40tracks-default-th", 1040, 790);
            shot(hub, "state-40tracks-compact-th", 420, 790);
            settings->setLanguage(Language::English);
            shot(hub, "state-40tracks-laptop-en", 1000, 560);
            shot(hub, "state-40tracks-tracks-en", 1040, 790, page(HubEditor::Page::Tracks));
            gAudio = nullptr;   // the extra tracks go first
            tracks.resize(keep);
        }
        {   // an extra Hub on the same bus
            HubProcessor extra;
            extra.setBusName("Snapshot");
            shot(extra, "state-second-hub-en", 1040, 790);
            shot(extra, "state-second-hub-compact-en", 420, 790);
        }
        report = "HEARASIDE UI audit - " + juce::Time::getCurrentTime().toString(true, true) + "\nlayout problems: "
               + juce::String(problems) + "\n\n" + report;
        dir.getChildFile("layout-report.txt").replaceWithText(report);
        std::printf("%s", report.toRawUTF8());
        settings->setUiScaleAuto(oldScale);
        settings->setLanguage(oldLang);
        settings->setThemeMode(oldTheme);
        return problems == 0 ? 0 : 1;
    }

    // --test-rest: the REST API end to end (docs/rest-api.md) over a raw socket; exit 1 on a failure
    if (audit == false && argc > 1 && juce::String(argv[1]) == "--test-rest") {
        settings->setRestApi(true);
        settings->setRestApiKey("0123456789abcdef0123456789abcdef");
        for (int i = 0; i < 40 && !hub.control().running(); ++i) pump(50);
        if (!hub.control().running()) { std::printf("FAIL: the REST API did not start\n"); return 1; }
        const int port = hub.control().port();
        // one request on a raw socket: [status, body]
        auto call = [port](const juce::String& method, const juce::String& path, const juce::String& body, const juce::String& extra) {
            juce::StreamingSocket s;
            if (!s.connect("127.0.0.1", port, 2000)) return std::pair<int, juce::String>(0, {});
            const auto req = method + " " + path + " HTTP/1.1\r\n" + extra + "Content-Length: " + juce::String(body.getNumBytesAsUTF8())
                           + "\r\nConnection: close\r\n\r\n" + body;
            s.write(req.toRawUTF8(), int(req.getNumBytesAsUTF8()));
            juce::MemoryOutputStream out;
            char buf2[4096];
            for (int n; s.waitUntilReady(true, 3000) == 1 && (n = s.read(buf2, sizeof buf2, false)) > 0;) out.write(buf2, size_t(n));
            const auto text = out.toString();
            return std::pair<int, juce::String>(text.fromFirstOccurrenceOf(" ", false, false).getIntValue(), text.fromFirstOccurrenceOf("\r\n\r\n", false, false));
        };
        const juce::String host = "Host: 127.0.0.1:" + juce::String(port) + "\r\n", key = "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n";
        int failures = 0;
        auto expect = [&](const char* what, bool ok) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += ok ? 0 : 1; };
        // the requests block until the message thread answers: run them beside the message loop
        std::atomic<bool> finished { false };
        const int trackCount = int(tracks.size());
        std::thread client([&] {
            auto r = call("GET", "/api/v1/state", {}, host);
            expect("no key -> 401", r.first == 401);
            r = call("GET", "/api/v1/state", {}, host + "Authorization: Bearer wrongwrongwrongwrongwrongwrong00\r\n");
            expect("wrong key -> 401", r.first == 401);
            r = call("GET", "/api/v1/state", {}, host + key + "Origin: https://evil.example\r\n");
            expect("a web page (Origin) -> 403", r.first == 403);
            r = call("GET", "/api/v1/state", {}, "Host: evil.example:" + juce::String(port) + "\r\n" + key);
            expect("another host name (DNS rebinding) -> 403", r.first == 403);
            r = call("GET", "/api/v1/state", {}, host + key);
            const auto state = juce::JSON::parse(r.second);
            expect("GET /api/v1/state -> 200 with every track", r.first == 200 && state["tracks"].size() == trackCount);
            r = call("POST", "/api/v1/mute", "{\"on\": true}", host + key);
            expect("POST /api/v1/mute {on: true} -> 200, stream_muted", r.first == 200 && bool(juce::JSON::parse(r.second)["stream_muted"]));
            r = call("POST", "/api/v1/tracks/" + juce::URL::addEscapeChars("Vocal", false), "{\"you_hear\": true, \"viewers_db\": -6}", host + key);
            expect("POST /api/v1/tracks/<name> -> 200", r.first == 200);
            r = call("POST", "/api/v1/tracks/nope", "{}", host + key);
            expect("unknown track -> 404", r.first == 404);
            r = call("POST", "/api/v1/stream", "not json", host + key);
            expect("bad JSON -> 400", r.first == 400);
            finished = true;
        });
        for (int i = 0; i < 400 && !finished; ++i) pump(25);
        client.join();
        pump(300);   // the Track takes the command within a block, through its own parameters
        expect("the Hub parameter changed (host sees it)", hub.params().getRawParameterValue(hubparam::Panic)->load() > 0.5f);
        expect("the Track's \"you hear\" changed", tracks[1]->params().getRawParameterValue(trackparam::Mon)->load() > 0.5f);
        expect("the Track's viewers level changed", std::abs(tracks[1]->params().getRawParameterValue(trackparam::StrGain)->load() + 6.0f) < 0.01f);
        gAudio = nullptr;
        std::printf("%s\n", failures == 0 ? "REST API: all passed" : "REST API: FAILED");
        return failures == 0 ? 0 : 1;
    }


    // --test-friends: the friends room over real sockets (S2): one link per friend, microphone in, the
    // mix out with positions, delay measured from the echo, newest connection wins, a removed friend is cut off
    if (audit == false && argc > 1 && juce::String(argv[1]) == "--test-friends") {
        int failures = 0;
        auto expect = [&](const char* what, bool ok) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += ok ? 0 : 1; };
        expect("no friends: the share server is not running", !hub.share().running());
        const uint32_t mint = hub.addFriend("Mint");
        const uint32_t beam = hub.addFriend("Beam");
        expect("two friends added", mint != 0 && beam != 0 && hub.friendCount() == 2);
        for (int i = 0; i < 100 && !hub.share().running(); ++i) pump(50);
        expect("the share server starts for the friends even with the listen link off", hub.share().running() && !hub.sharing());
        const int port = hub.share().port();
        const auto tokenOf = [&](uint32_t id) { const auto link = hub.friendLink(id); return link.fromLastOccurrenceOf("/s/", false, false); };
        const auto tMint = tokenOf(mint), tBeam = tokenOf(beam);
        expect("links are 26 characters of secure random and differ", tMint.length() == 26 && tBeam.length() == 26 && tMint != tBeam);
        ssbus::BusLayout* bus = hub.engine().bus();
        std::atomic<bool> finished { false };
        std::atomic<int> state { 0 };
        std::thread client([&] {
            auto r = httpGet(port, "/s/" + tMint);
            expect("GET /s/<token> serves the page", r.first == 200 && r.second.contains("HEARASIDE"));
            r = httpGet(port, "/info/s/" + tMint);
            expect("GET /info/s/<token> names the friend", r.first == 200 && juce::JSON::parse(r.second)["name"].toString() == "Mint");
            r = httpGet(port, "/s/aaaaaaaaaaaaaaaaaaaaaaaaaa");
            expect("a link nobody has -> 404", r.first == 404);
            r = httpGet(port, "/l/" + juce::String("x"));
            expect("listen links are off while sharing is off -> 404", r.first == 404);
            WsClient evil;
            evil.open(port, "/ws/s/" + tMint, "https://evil.example");
            expect("a web page from another site may not connect -> 403", evil.status == 403);

            WsClient a;
            expect("the friend's WebSocket opens", a.open(port, "/ws/s/" + tMint));
            expect("the friend shows as singing", [&] { for (int i = 0; i < 40; ++i) { if (bus->friends[0].state.load() == ssbus::kFriendLive) return true; juce::Thread::sleep(25); } return false; }());
            // 20 ms microphone packets (HRA2), each saying "I was hearing the Stream Mix 100 ms ago"
            uint32_t downFrames = 0, downPackets = 0, rate = 0;
            uint64_t lastEnd = 0;
            bool contiguous = true, magicOk = true, sawHello = false;
            juce::String helloName;
            for (int i = 0; i < 60; ++i) {
                std::vector<uint8_t> pk(24 + 960 * 4);
                const uint32_t magic = 0x32415248, r48 = 48000, frames = 960;
                const uint16_t chans = 2, flags = 0;
                const uint64_t w = bus->friendMixWrite.load();
                const uint64_t echo = w > 4800 ? w - 4800 : 1;
                std::memcpy(pk.data(), &magic, 4); std::memcpy(pk.data() + 4, &r48, 4); std::memcpy(pk.data() + 8, &chans, 2);
                std::memcpy(pk.data() + 10, &flags, 2); std::memcpy(pk.data() + 12, &frames, 4); std::memcpy(pk.data() + 16, &echo, 8);
                auto* pcm = reinterpret_cast<int16_t*>(pk.data() + 24);
                for (int k = 0; k < 960; ++k) pcm[2 * k] = pcm[2 * k + 1] = int16_t(8000 * std::sin(0.05 * (i * 960 + k)));
                a.send(pk);
                // what comes back: the mix with its position
                for (int t = 0; t < 4; ++t) {
                    auto m = a.receive(5);
                    if (m.size() < 24) continue;
                    uint32_t mg = 0, fr = 0; uint64_t pos = 0;
                    std::memcpy(&mg, m.data(), 4); std::memcpy(&rate, m.data() + 4, 4); std::memcpy(&fr, m.data() + 12, 4); std::memcpy(&pos, m.data() + 16, 8);
                    if (mg != 0x32415248) {   // text: the hello ({"v":2,"name":...}) or the delay report
                        const juce::String text(reinterpret_cast<const char*>(m.data()), m.size());
                        if (juce::JSON::parse(text)["v"].toString() == "2") { sawHello = true; helloName = juce::JSON::parse(text)["name"].toString(); }
                        continue;
                    }
                    magicOk &= true;
                    if (lastEnd != 0 && pos != lastEnd) contiguous = false;
                    lastEnd = pos + fr;
                    downFrames += fr; ++downPackets;
                }
                juce::Thread::sleep(18);
            }
            expect("the friend hears the Stream Mix in HRA2 packets with positions", magicOk && downPackets > 10 && rate > 0 && downFrames > 4800);
            expect("the Hub greets the friend by name first", sawHello && helloName == "Mint");
            expect("those positions are contiguous (nothing lost, nothing repeated)", contiguous);
            expect("the friend's audio is in the bus", bus->friends[0].writePos.load() >= 40u * 960u);
            const float d = ssbus::bitsFloat(bus->friends[0].delayBits.load());
            std::printf("     measured delay %.1f ms (the echo said 100 ms heard, minus half a 20 ms packet = about 90)\n", d);
            expect("the delay was measured from the echo (about 90 ms)", d > 70.0f && d < 130.0f);

            WsClient b;
            expect("the same friend connects again (a second tab)", b.open(port, "/ws/s/" + tMint));
            bool closed = false;
            for (int i = 0; i < 80 && !closed; ++i) { a.receive(25, &closed); }
            expect("the newest connection wins: the first one is closed", closed);
            expect("the friend is still singing (on the new connection)", bus->friends[0].state.load() == ssbus::kFriendLive);

            WsClient other;
            expect("another friend has their own slot", other.open(port, "/ws/s/" + tBeam));
            expect("without closing the first friend", bus->friends[1].state.load() == ssbus::kFriendLive && bus->friends[0].state.load() == ssbus::kFriendLive);
            state = 1;
            for (int i = 0; i < 400 && state == 1; ++i) juce::Thread::sleep(10);   // the main thread removes Mint
            bool closedB = false;
            for (int i = 0; i < 100 && !closedB; ++i) { b.receive(25, &closedB); }
            expect("a removed friend is cut off", closedB);
            r = httpGet(port, "/s/" + tMint);
            expect("and their link no longer works -> 404", r.first == 404);
            expect("the other friend is not affected", bus->friends[1].state.load() == ssbus::kFriendLive);
            finished = true;
        });
        for (int i = 0; i < 2000 && state != 1 && !finished; ++i) pump(10);
        if (state == 1) { hub.removeFriend(mint); pump(50); state = 2; }
        for (int i = 0; i < 2000 && !finished; ++i) pump(10);
        client.join();
        expect("the removed friend's slot is free", bus->friends[0].id.load() == 0 && bus->friends[0].state.load() == ssbus::kFriendFree);
        // the room is saved with the project; the links survive a reload
        juce::MemoryBlock mb;
        hub.getStateInformation(mb);
        const auto savedLink = hub.friendLink(beam);
        hub.removeFriend(beam);
        expect("an empty room stops the server again", [&] { for (int i = 0; i < 60; ++i) { pump(50); if (!hub.share().running()) return true; } return false; }());
        hub.setStateInformation(mb.getData(), int(mb.getSize()));
        pump(300);
        expect("the project brings Beam back with the same link", hub.friendCount() == 1 && hub.friends()[0].name == "Beam" && [&] { for (int i = 0; i < 60 && !hub.share().running(); ++i) pump(50); return hub.friendLink(hub.friends()[0].id).fromLastOccurrenceOf("/s/", false, false) == savedLink.fromLastOccurrenceOf("/s/", false, false); }());
        const auto xml = mb.toString();
        gAudio = nullptr;
        std::printf("%s\n", failures == 0 ? "friends room: all passed" : "friends room: FAILED");
        return failures == 0 ? 0 : 1;
    }

    // --serve-friend <seconds>: a Hub with one friend, serving that friend's page; prints FRIEND_URL=... and, at
    // the end, what the Hub saw of the friend (for the browser test in tests/web-friend)
    if (audit == false && argc > 2 && juce::String(argv[1]) == "--serve-friend") {
        const int seconds = juce::String(argv[2]).getIntValue();
        const uint32_t id = hub.addFriend("Mint");
        for (int i = 0; i < 100 && !hub.share().running(); ++i) pump(50);
        std::printf("FRIEND_URL=http://127.0.0.1:%d/s/%s\n", hub.share().port(), hub.friendLink(id).fromLastOccurrenceOf("/s/", false, false).toRawUTF8());
        std::fflush(stdout);
        float maxDelay = -1.0f, maxPeak = 0.0f;
        bool wasLive = false;
        for (int t = 0; t < seconds * 10; ++t) {
            pump(100);
            const auto v = hub.friends();
            if (!v.empty()) {
                wasLive |= v[0].live();
                maxDelay = std::max(maxDelay, v[0].delayMs);
                maxPeak = std::max(maxPeak, v[0].peak);
            }
        }
        const auto v = hub.friends();
        std::printf("RESULT live=%d delay=%.1f peak=%.3f playing=%d writePos=%llu\n", wasLive ? 1 : 0, maxDelay, maxPeak,
                    (!v.empty() && v[0].playing) ? 1 : 0, (unsigned long long)hub.engine().bus()->friends[0].writePos.load());
        std::fflush(stdout);
        gAudio = nullptr;
        return wasLive ? 0 : 1;
    }


#ifdef _WIN32
    // --test-friend-app: HEARASIDE App Audio with a friend as its source (S5): the friend's microphone comes out
    // of the plug-in, the status says who, a take is moved back on the timeline by the friend's delay, and the
    // App Audio's own headphone slot stays silent (the Hub already plays the friend)
    if (audit == false && argc > 1 && juce::String(argv[1]) == "--test-friend-app") {
        int failures = 0;
        auto expect = [&](const char* what, bool ok) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += ok ? 0 : 1; };
        gAudio = nullptr;
        settings->setLanguage(Language::English);   // the checks read English text
        const uint32_t mint = hub.addFriend("Mint");
        for (int i = 0; i < 60; ++i) pump(25);
        auto* bus = hub.engine().bus();
        const int slot = hub.friends().front().slot;
        bus->friends[slot].sampleRate.store(48000);
        bus->friends[slot].delayBits.store(ssbus::floatBits(120.0f));
        bus->friends[slot].state.store(ssbus::kFriendLive);

        struct Head : juce::AudioPlayHead {
            int64_t time = 480000;
            juce::Optional<PositionInfo> getPosition() const override {
                PositionInfo p;
                p.setTimeInSamples(time);
                p.setIsPlaying(true);
                p.setIsRecording(false);
                return p;
            }
        } head;
        AppAudioProcessor app;
        app.setPlayConfigDetails(2, 2, 48000.0, 256);
        app.setPlayHead(&head);
        app.prepareToPlay(48000.0, 256);
        app.setApp(FriendDirectory::appFor(mint));
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        int64_t sentFrames = 0, blocks = 0;
        float peak = 0.0f;
        auto run = [&](double seconds) {
            const auto t0 = juce::Time::getMillisecondCounter();
            const auto end = t0 + juce::uint32(seconds * 1000);
            int64_t b0 = blocks;
            while (juce::Time::getMillisecondCounter() < end) {
                const auto now = juce::Time::getMillisecondCounter();
                // the friend's browser: 20 ms packets, as time passes
                while (sentFrames + 960 <= int64_t(now - t0) * 48 + int64_t(b0) * 256 + 0) {
                    std::vector<float> pk(960);
                    for (int i = 0; i < 960; ++i) pk[size_t(i)] = 0.4f * std::sin(float(sentFrames + i) * 0.058f);
                    const float* src[1] = { pk.data() };
                    ssbus::ringWrite(bus->friendAudio[slot], bus->friends[slot].writePos, src, 1, 960);
                    bus->friends[slot].heartbeatNs.store(ssbus::nowNs());
                    sentFrames += 960;
                }
                while ((blocks - b0) * 256 < int64_t(now - t0) * 48) {   // the DAW's pace
                    buf.clear();
                    app.processBlock(buf, midi);
                    head.time += 256;
                    ++blocks;
                    if (blocks - b0 > 190) peak = std::max(peak, buf.getMagnitude(0, 256));
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
                juce::Thread::sleep(1);
            }
        };
        run(2.0);
        std::printf("     App Audio output peak %.3f, status %d, latency %.1f ms\n", peak, int(app.captureState()), app.latencyMs());
        expect("the source is the friend", app.input() == AppAudioProcessor::Input::Friend);
        expect("the status is Running (the friend is singing)", app.captureState() == AppCapture::State::Running);
        expect("the friend's microphone comes out of the plug-in", peak > 0.1f);
        expect("the picker says \"Mint · friend\"", AppAudioProcessor::appLabel(app.app()) == juce::String(juce::CharPointer_UTF8("Mint \xc2\xb7 friend")));
        expect("a take's shift = the friend's delay + our buffer", std::abs(app.takeShiftMs() - (120.0 + app.latencyMs())) < 3.0);

        // the App Audio's own headphone slot stays silent: the Hub plays the friend
        bool monOff = false;
        for (const auto& t : hub.tracks()) (void) t;
        for (int i = 0; i < ssbus::kMaxSlots; ++i)
            if (bus->slots[i].state.load() == ssbus::kSlotActive && (bus->slots[i].flags.load() & ssbus::kFlagApp)) monOff = !(bus->slots[i].flags.load() & ssbus::kFlagMon);
        expect("its headphone slot is not in the headphones (no double)", monOff);

        // a take: its position on the timeline is moved back by the shift
        app.startRecording();
        const int64_t startTime = head.time;
        run(1.2);
        app.stopRecording();
        const auto take = app.recorder().lastTake();
        expect("a take was written", take.existsAsFile());
        if (take.existsAsFile()) {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> r(wav.createReaderFor(take.createInputStream().release(), true));
            const auto ref = r ? r->metadataValues.getValue(juce::WavAudioFormat::bwavTimeReference, "") : juce::String();
            const int64_t got = ref.getLargeIntValue();
            const int64_t want = startTime - int64_t(app.takeShiftMs() * 48.0 + 0.5);
            std::printf("     take position %lld samples, expected about %lld (started at %lld)\n", (long long) got, (long long) want, (long long) startTime);
            expect("the take sits where the friend meant it (moved back by the shift)", ref.isNotEmpty() && std::abs(got - want) < 48 * 15);
        }
        gAudio = nullptr;
        std::printf("%s\n", failures == 0 ? "friend as App Audio source: all passed" : "friend as App Audio source: FAILED");
        return failures == 0 ? 0 : 1;
    }
#endif

    // --demo <seconds>: keep the Hub and Tracks running (bus "Snapshot") so the OBS source
    // can be tried live
    for (int i = 1; i + 1 < argc; ++i) {
        if (juce::String(argv[i]) == "--demo") {
            // --share-base <url>: permanent links through that share web site (web/share-vercel)
            // --state <file>: the Hub's project state (its links) kept between runs, like a saved song
            juce::File stateFile;
            for (int k = 1; k + 1 < argc; ++k) {
                if (juce::String(argv[k]) == "--share-base") settings->setShareBase(argv[k + 1]);
                if (juce::String(argv[k]) == "--state") stateFile = juce::File::getCurrentWorkingDirectory().getChildFile(argv[k + 1]);
            }
            if (stateFile.existsAsFile()) {
                juce::MemoryBlock mb;
                stateFile.loadFileAsData(mb);
                hub.setStateInformation(mb.getData(), int(mb.getSize()));
            }
            const int seconds = juce::String(argv[i + 1]).getIntValue();
            hub.setSharing(true);   // try the share links (listen / send pages) while the demo runs
            pump(500);
            for (int k = 0; k < 60 && hub.share().tunnel() == ShareServer::Tunnel::Starting; ++k) pump(250);   // internet link (cloudflared)
            for (int k = 0; k < 40 && hub.permanentLinksSet() && hub.directory().state() != ShareDirectory::State::Online; ++k) pump(250);
            if (stateFile != juce::File()) {
                juce::MemoryBlock mb;
                hub.getStateInformation(mb);
                stateFile.replaceWithData(mb.getData(), mb.getSize());
            }
            std::printf("listen link %s%c" "send link %s%c", hub.share().listenUrl().toRawUTF8(), 10, hub.share().sendUrl().toRawUTF8(), 10);
            if (hub.permanentLinksSet())
                std::printf("permanent listen link %s (%s)%c", hub.directory().listenUrl().toRawUTF8(),
                            hub.directory().state() == ShareDirectory::State::Online ? "online" : "not online", 10);
            std::printf("demo running for %d s on bus \"Snapshot\"\n", seconds);
            std::fflush(stdout);
            pump(seconds * 1000);
            gAudio = nullptr;
            return 0;
        }
    }

    // default: the main screens of the three plug-ins, both languages and themes
    for (int lang = 0; lang < 2; ++lang) {
        for (int dark = 0; dark < 2; ++dark) {
            settings->setLanguage(lang == 0 ? Language::English : Language::Thai);
            settings->setThemeMode(dark ? ThemeMode::Dark : ThemeMode::Light);
            const juce::String suffix = juce::String(lang == 0 ? "en" : "th") + (dark ? "-dark" : "-light");
            {
                std::unique_ptr<juce::AudioProcessorEditor> ed(hub.createEditor());
                pump(400);
                save(*ed, dir.getChildFile("hub-" + suffix + ".png"));
            }
            {
                std::unique_ptr<juce::AudioProcessorEditor> ed(tracks[1]->createEditor());
                pump(400);
                save(*ed, dir.getChildFile("track-" + suffix + ".png"));
            }
#ifdef _WIN32
            {
                std::unique_ptr<juce::AudioProcessorEditor> ed(apps[0]->createEditor());
                pump(400);
                save(*ed, dir.getChildFile("app-" + suffix + ".png"));
            }
#endif
        }
    }
    gAudio = nullptr;
    settings->setLanguage(oldLang);
    settings->setThemeMode(oldTheme);
    return 0;
}
