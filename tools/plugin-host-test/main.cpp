// plugin-host-test: loads the built HEARASIDE Track / Hub (and App Audio) binaries the way a DAW
// does (VST3 or VST2 via JUCE hosting) and checks the behaviour end to end through shared memory.
//   plugin-host-test <Track plugin> <Hub plugin> [App Audio plugin]
// Exit code 0 = all checks passed.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "ssbus/bus.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++failures;
}

void pump(int ms) {
    const auto end = juce::Time::getMillisecondCounter() + juce::uint32(ms);
    while (juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

juce::String bundlePath(const juce::String& file) {
    // VST3 on Windows: <name>.vst3/Contents/x86_64-win/<name>.vst3 -> load the bundle folder
    juce::File f(file);
    if (f.getFileExtension() == ".vst3" && f.getParentDirectory().getFileName() == "x86_64-win")
        return f.getParentDirectory().getParentDirectory().getParentDirectory().getFullPathName();
    return f.getFullPathName();
}

std::unique_ptr<juce::AudioPluginInstance> load(juce::AudioPluginFormatManager& fm, const juce::String& path) {
    for (auto* format : fm.getFormats()) {
        if (!format->fileMightContainThisPluginType(path)) continue;
        juce::OwnedArray<juce::PluginDescription> types;
        format->findAllTypesForFile(types, path);
        if (types.isEmpty()) continue;
        juce::String err;
        if (auto inst = fm.createPluginInstance(*types[0], 48000.0, 256, err)) {
            std::printf("loaded %s (%s)\n", inst->getName().toRawUTF8(), format->getName().toRawUTF8());
            return inst;
        }
        std::printf("could not instantiate %s: %s\n", path.toRawUTF8(), err.toRawUTF8());
    }
    return nullptr;
}

// Track parameter order (TrackProcessor::createLayout)
enum TrackParam { kMon = 0, kStr, kStrGain, kStrPan, kStrDelay, kMonTrim, kStrSolo };

juce::AudioProcessorParameter* P(juce::AudioPluginInstance& p, int index) { return p.getParameters()[index]; }
void setParam(juce::AudioPluginInstance& p, int index, float norm) { P(p, index)->setValueNotifyingHost(norm); }

struct Rig {
    std::unique_ptr<juce::AudioPluginInstance> a, b, hub;
    juce::AudioBuffer<float> ba { 2, 256 }, bb { 2, 256 }, bh { 2, 256 };
    juce::MidiBuffer midi;
    int64_t t = 0;
    float outPeakA = 0, phonesPeak = 0;   // Track A's DAW output (= viewers), Hub output (= headphones)
    float masterFx = 1.0f;                // gain of a plug-in on the master bus above the Hub

    void block(float ampA, float ampB, int delayB = 0) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) {
                const double ph = double(t + i) * 2.0 * juce::MathConstants<double>::pi * 480.0 / 48000.0;
                const double phB = double(t + i - delayB) * 2.0 * juce::MathConstants<double>::pi * 480.0 / 48000.0;
                ba.setSample(c, i, ampA * float(std::sin(ph)));
                bb.setSample(c, i, ampB * float(std::sin(phB)));
            }
        a->processBlock(ba, midi);
        b->processBlock(bb, midi);
        outPeakA = ba.getMagnitude(0, 0, 256);
        bh.clear();
        bh.addFrom(0, 0, ba, 0, 0, 256);
        bh.addFrom(1, 0, ba, 1, 0, 256);
        bh.addFrom(0, 0, bb, 0, 0, 256);
        bh.addFrom(1, 0, bb, 1, 0, 256);
        bh.applyGain(masterFx);
        hub->processBlock(bh, midi);
        phonesPeak = bh.getMagnitude(0, 256);
        t += 256;
    }
};

float streamPeak(ssbus::BusLayout& L, uint32_t frames);

// Auto sync: A plays "music" (noise), B is a microphone that hears it from the headphones 23 ms
// later and inverted. After the Hub measured and set the Viewers Delay, A + B must cancel in the
// stream, which only happens when they line up to the sample.
void testAutoSync(Rig& rig, ssbus::BusLayout& L, int slotA, int slotB) {
    constexpr int D = 1104;   // 23.0 ms at 48 kHz
    std::vector<float> noise(48000 * 40);
    uint32_t seed = 12345;
    for (auto& v : noise) { seed = seed * 1664525u + 1013904223u; v = 0.4f * (float(seed >> 8) / 8388608.0f - 1.0f); }
    int64_t pos = 50000;
    auto block = [&] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) {
                rig.ba.setSample(c, i, noise[size_t(pos + i)]);
                rig.bb.setSample(c, i, -noise[size_t(pos + i - D)]);
            }
        rig.a->processBlock(rig.ba, rig.midi);
        rig.b->processBlock(rig.bb, rig.midi);
        rig.bh.makeCopyOf(rig.ba);
        rig.bh.addFrom(0, 0, rig.bb, 0, 0, 256);
        rig.bh.addFrom(1, 0, rig.bb, 1, 0, 256);
        rig.hub->processBlock(rig.bh, rig.midi);
        pos += 256;
    };
    setParam(*rig.a, kStrDelay, 0.0f);
    setParam(*rig.b, kStrDelay, 0.0f);
    pump(100);
    for (int i = 0; i < 100; ++i) block();
    const float before = streamPeak(L, 4096);

    ssbus::postRemote(L, slotB, uint32_t(ssbus::RemoteParam::AutoSync), float(slotA));
    // about 4x real time, with the message thread running in between (Hub + Track timers)
    for (int k = 0; k < 300 && P(*rig.a, kStrDelay)->getValue() == 0.0f; ++k) {
        for (int i = 0; i < 8; ++i) block();
        pump(10);
    }
    pump(200);
    for (int i = 0; i < 200; ++i) block();
    const float after = streamPeak(L, 4096);
    const float ms = P(*rig.a, kStrDelay)->getValue() * 500.0f;
    std::printf("  auto sync: music delay %.1f ms (mic was 23.0 ms late), stream %.1f dBFS -> %.1f dBFS\n", ms,
                20.0 * std::log10(std::max(1e-12f, before)), 20.0 * std::log10(std::max(1e-12f, after)));
    check(std::abs(ms - 23.0f) < 0.6f, "auto sync measures the mic 23 ms late and delays the music track for the viewers");
    check(P(*rig.b, kStrDelay)->getValue() < 0.001f, "...and leaves the mic track undelayed");
    check(after < before * 0.01f, "...so the voice lines up with the music in the stream (inverted copy cancels, > 40 dB)");
    check(L.slots[slotA].hubMute.load() == 0 && L.slots[slotB].hubMute.load() == 0, "...and no track stays muted afterwards");
    setParam(*rig.a, kStrDelay, 0.0f);
    pump(50);
}

// Auto sync with App Audio as the music and no HEARASIDE Track on its track: this test plays the
// App Audio's part on the bus (its signal ring, delay and measuring mute, like the plug-in does).
// B is the mic hearing the music 23 ms late and inverted; after the sync the App's own delay must
// line the music up so the two cancel in the stream. D < 0: the mic hears the music before App
// Audio delivers it (the program plays straight to Windows, App Audio buffers its capture), so the
// mic track gets the delay instead.
void testAutoSyncApp(Rig& rig, ssbus::BusLayout& L, int slotB, int D = 1104) {
    const int idx = ssbus::claimSource(L);
    check(idx >= 0, "simulated App Audio joins the bus");
    if (idx < 0) return;
    auto& src = L.sources[idx];
    ssbus::setSourceIdentity(src, "music", "brave.exe", 0);
    src.flags.store(ssbus::kSrcOn);
    src.capture.store(2);
    uint32_t cursor = src.cmdWrite.load();
    float delayMs = 0.0f;
    std::vector<float> noise(48000 * 40), line(48000, 0.0f), out(256);
    uint32_t seed = 777;
    for (auto& v : noise) { seed = seed * 1664525u + 1013904223u; v = 0.4f * (float(seed >> 8) / 8388608.0f - 1.0f); }
    int64_t pos = 60000;
    int lw = 0;
    auto block = [&] {
        ssbus::pollSourceCommands(src, cursor, [&](ssbus::SourceParam id, float v) { if (id == ssbus::SourceParam::DelayMs) delayMs = v; });
        src.delayBits.store(ssbus::floatBits(delayMs));
        src.heartbeatNs.store(ssbus::nowNs());
        const float* sig[1] = { noise.data() + pos };
        ssbus::ringWrite(L.sourceAudio[idx], src.writePos, sig, 1, 256);   // what App Audio adds, before its delay
        const int d = int(std::lround(delayMs * 48.0f));
        for (int i = 0; i < 256; ++i) {
            line[size_t((lw + i) % 48000)] = noise[size_t(pos + i)];
            out[size_t(i)] = src.hubMute.load() ? 0.0f : line[size_t((lw + i - d + 48000) % 48000)];
        }
        lw = (lw + 256) % 48000;
        rig.ba.clear();
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) rig.bb.setSample(c, i, -noise[size_t(pos + i - D)]);
        rig.a->processBlock(rig.ba, rig.midi);
        rig.b->processBlock(rig.bb, rig.midi);
        for (int c = 0; c < 2; ++c) {
            rig.bh.copyFrom(c, 0, out.data(), 256);
            rig.bh.addFrom(c, 0, rig.bb, c, 0, 256);
        }
        rig.hub->processBlock(rig.bh, rig.midi);
        pos += 256;
    };
    setParam(*rig.b, kStrDelay, 0.0f);
    pump(100);
    for (int i = 0; i < 100; ++i) block();
    const float before = streamPeak(L, 4096);
    ssbus::postRemote(L, slotB, uint32_t(ssbus::RemoteParam::AutoSync), float(-idx - 2));
    for (int k = 0; k < 300 && delayMs == 0.0f && P(*rig.b, kStrDelay)->getValue() == 0.0f; ++k) {
        for (int i = 0; i < 8; ++i) block();
        pump(10);
    }
    pump(200);
    for (int i = 0; i < 200; ++i) block();
    const float after = streamPeak(L, 4096);
    const float micMs = P(*rig.b, kStrDelay)->getValue() * 500.0f, want = std::abs(float(D)) / 48.0f;
    std::printf("  auto sync (App Audio): App delay %.1f ms, mic delay %.1f ms (mic was %.1f ms %s), stream %.1f dBFS -> %.1f dBFS\n",
                delayMs, micMs, want, D > 0 ? "late" : "early",
                20.0 * std::log10(std::max(1e-12f, before)), 20.0 * std::log10(std::max(1e-12f, after)));
    if (D > 0) check(std::abs(delayMs - want) < 0.1f && micMs < 0.01f, "auto sync with App Audio as the music: no Track needed on its track, App delay = 23 ms");
    else check(delayMs < 0.01f && std::abs(micMs - want) < 0.6f, "auto sync, mic hears the program before App Audio delivers it: the mic track is delayed instead");
    check(after < before * 0.01f, "...and the voice lines up with the music in the stream (> 40 dB null)");
    check(src.hubMute.load() == 0 && L.slots[slotB].hubMute.load() == 0, "...nothing stays muted");
    setParam(*rig.b, kStrDelay, 0.0f);
    ssbus::releaseSource(L, idx);
}

// ---- share links (like LISTENTO) --------------------------------------------------------------
struct Http {
    juce::StreamingSocket s;
    bool open(int port) { return s.connect("127.0.0.1", port, 2000); }
    std::string request(const juce::String& path, const juce::String& extra = {}) {
        const auto req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra + "\r\n";
        s.write(req.toRawUTF8(), int(req.getNumBytesAsUTF8()));
        std::string head;
        char c;
        while (head.find("\r\n\r\n") == std::string::npos && s.waitUntilReady(true, 2000) == 1 && s.read(&c, 1, true) == 1) head += c;
        return head;
    }
    std::string body() {
        std::string b;
        char buf[4096];
        while (s.waitUntilReady(true, 500) == 1) { const int n = s.read(buf, sizeof buf, false); if (n <= 0) break; b.append(buf, size_t(n)); }
        return b;
    }
    // one server frame (unmasked)
    bool frame(std::vector<uint8_t>& out, int ms = 2000) {
        uint8_t h[2];
        auto readN = [&](void* d, int n) { return s.waitUntilReady(true, ms) == 1 && s.read(d, n, true) == n; };
        if (!readN(h, 2)) return false;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) { uint8_t e[2]; if (!readN(e, 2)) return false; len = uint64_t(e[0]) << 8 | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!readN(e, 8)) return false; len = 0; for (uint8_t b : e) len = len << 8 | b; }
        out.resize(size_t(len));
        return len == 0 || readN(out.data(), int(len));
    }
    // one client frame (masked, as browsers send)
    void send(const std::vector<uint8_t>& p) {
        std::vector<uint8_t> f { 0x82 };
        if (p.size() < 126) f.push_back(uint8_t(0x80 | p.size()));
        else { f.push_back(0x80 | 126); f.push_back(uint8_t(p.size() >> 8)); f.push_back(uint8_t(p.size())); }
        const uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
        f.insert(f.end(), mask, mask + 4);
        for (size_t i = 0; i < p.size(); ++i) f.push_back(uint8_t(p[i] ^ mask[i & 3]));
        s.write(f.data(), int(f.size()));
    }
};

std::vector<uint8_t> pcmPacket(int frames, int64_t t0, float amp) {
    std::vector<uint8_t> p(16 + size_t(frames) * 4);
    const uint32_t magic = 0x31415248, rate = 48000, n = uint32_t(frames);
    const uint16_t ch = 2;
    std::memcpy(p.data(), &magic, 4); std::memcpy(p.data() + 4, &rate, 4); std::memcpy(p.data() + 8, &ch, 2); std::memcpy(p.data() + 12, &n, 4);
    auto* s = reinterpret_cast<int16_t*>(p.data() + 16);
    for (int i = 0; i < frames; ++i) s[2 * i] = s[2 * i + 1] = int16_t(std::lround(amp * 32767.0 * std::sin(double(t0 + i) * 0.05)));
    return p;
}

void testShare(Rig& rig, ssbus::BusLayout& L, juce::AudioPluginFormatManager& fm, const char* appPath) {
    ssbus::postRemote(L, ssbus::RemoteParam::Share, 1.0f);
    pump(400);
    juce::MemoryBlock st;
    rig.hub->getStateInformation(st);
    // the host wraps the plug-in's XML in its own container: find the attributes in the raw bytes
    std::string raw(static_cast<const char*>(st.getData()), st.getSize());
    if (const auto wrap = juce::AudioProcessor::getXmlFromBinary(st.getData(), int(st.getSize())))   // VST3: base64 component state
        if (const auto* comp = wrap->getChildByName("IComponent")) {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding(comp->getAllSubText().trim())) raw = std::string(static_cast<const char*>(mb.getData()), mb.getSize());
        }
    auto attr = [&raw](const std::string& name) {
        const auto at = raw.find(name + "=\"");
        if (at == std::string::npos) return juce::String();
        const auto from = at + name.size() + 2, end = raw.find('"', from);
        return juce::String(raw.substr(from, end - from));
    };
    const juce::String lt = attr("listenToken"), sendTok = attr("sendToken");
    int port = 0;
    for (int p = 47810; p < 47830 && port == 0 && lt.isNotEmpty(); ++p) {
        Http h;
        if (h.open(p) && h.request("/l/" + lt).find(" 200 ") != std::string::npos && h.body().find("HEARASIDE") != std::string::npos) port = p;
    }
    if (port == 0) {
        std::printf("  tokens '%s' '%s', state %d bytes\n", lt.toRawUTF8(), sendTok.toRawUTF8(), int(st.getSize()));
        for (int p = 47810; p < 47813; ++p) {
            Http h;
            const bool ok = h.open(p);
            const auto head = ok ? h.request("/l/" + lt) : std::string();
            std::printf("  port %d open %d head [%s]\n", p, int(ok), head.substr(0, 40).c_str());
        }
    }
    check(port != 0, "share: the Hub serves the listen page");
    if (port == 0) return;
    check(lt.length() == 26 && sendTok.length() == 26 && lt.containsOnly("abcdefghijkmnpqrstuvwxyz23456789"),
          "share: links use 26-character tokens from the OS's secure random source");
    {   // a page from another site may not open the audio
        Http h;
        const auto r = h.open(port) ? h.request("/ws/l/" + lt, "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                                              "Sec-WebSocket-Version: 13\r\nOrigin: https://evil.example\r\n") : std::string();
        check(r.find(" 403 ") != std::string::npos, "share: a WebSocket from another site's page is refused (Origin)");
    }
    {
        Http h;
        check(h.open(port) && h.request("/l/wrong-token").find(" 404 ") != std::string::npos, "...and refuses links with a wrong token");
    }
    // listener: RFC 6455 sample key -> the server's SHA-1 / base64 must give the RFC's answer
    Http lis;
    const auto head = lis.open(port) ? lis.request("/ws/l/" + lt, "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n") : std::string();
    check(head.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos, "share: WebSocket handshake (RFC 6455 test vector)");
    float got = 0.0f;
    uint32_t rate = 0;
    for (int k = 0; k < 60; ++k) {
        for (int i = 0; i < 4; ++i) rig.block(0.5f, 0.0f);
        std::vector<uint8_t> f;
        while (lis.s.waitUntilReady(true, 15) == 1 && lis.frame(f) && f.size() >= 16) {
            std::memcpy(&rate, f.data() + 4, 4);
            const auto* s = reinterpret_cast<const int16_t*>(f.data() + 16);
            for (size_t i = 0; i < (f.size() - 16) / 2; ++i) got = std::max(got, std::abs(s[i]) / 32768.0f);
        }
    }
    std::printf("  listener got %.3f peak at %u Hz (stream %.3f)\n", got, rate, streamPeak(L, 4096));
    check(rate == 48000 && std::abs(got - streamPeak(L, 4096)) < 0.02f && got > 0.1f, "share: a browser listener receives the Stream Mix at its level");

    // sender: masked frames like a browser -> bus link input, sample for sample
    Http snd;
    const auto sh = snd.open(port) ? snd.request("/ws/s/" + sendTok, "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n") : std::string();
    check(sh.find(" 101 ") != std::string::npos, "share: the send link accepts a sender");
    const uint64_t w0 = L.linkIn.writePos.load();
    snd.send(pcmPacket(960, 0, 0.25f));
    for (int i = 0; i < 50 && L.linkIn.writePos.load() < w0 + 960; ++i) juce::Thread::sleep(10);
    std::vector<float> l(960), r(960);
    float* d[2] = { l.data(), r.data() };
    ssbus::ringReadAt(L.linkInAudio, L.linkIn.writePos, w0, d, 960);
    float err = 0.0f;
    for (int i = 0; i < 960; ++i) err = std::max(err, std::abs(l[size_t(i)] - float(std::lround(0.25 * 32767.0 * std::sin(i * 0.05))) / 32768.0f));
    check(L.linkIn.writePos.load() == w0 + 960 && L.linkIn.active.load() == 1 && err < 1e-6f, "...and its audio reaches the bus exactly");

    if (appPath != nullptr) {
        // App Audio playing what the friend sends ("*link*")
        auto app = load(fm, bundlePath(appPath));
        if (app) {
            app->enableAllBuses();
            app->setPlayConfigDetails(2, 2, 48000.0, 256);
            app->prepareToPlay(48000.0, 256);
            pump(300);
            int idx = -1;
            for (int i = 0; i < ssbus::kMaxSources; ++i) if (L.sources[i].state.load() == ssbus::kSlotActive) idx = i;
            if (idx >= 0) ssbus::requestSourceApp(L.sources[idx], "*link*");
            pump(300);
            juce::AudioBuffer<float> b(2, 256);
            juce::MidiBuffer midi;
            float peak = 0.0f;
            int64_t sent = 960;
            const auto t0 = juce::Time::getMillisecondCounter();
            for (int k = 0; juce::Time::getMillisecondCounter() - t0 < 2500; ++k) {
                while (sent < int64_t(juce::Time::getMillisecondCounter() - t0) * 48 + 4800) { snd.send(pcmPacket(480, sent, 0.25f)); sent += 480; }
                b.clear();
                app->processBlock(b, midi);
                if (juce::Time::getMillisecondCounter() - t0 > 1500) peak = std::max(peak, b.getMagnitude(0, 256));
                juce::Thread::sleep(5);
                if (k % 20 == 0) pump(1);
            }
            std::printf("  App Audio (*link*) output peak %.3f (sent 0.25)\n", peak);
            check(std::abs(peak - 0.25f) < 0.03f, "App Audio plays what a friend sends through the send link");

            // App Audio receiving a listen link (DAW to DAW, like LISTENTO Receiver): our own Hub's link
            if (idx >= 0) ssbus::requestSourceApp(L.sources[idx], ("http://127.0.0.1:" + juce::String(port) + "/l/" + lt).toStdString());
            pump(300);
            peak = 0.0f;
            const auto t1 = juce::Time::getMillisecondCounter();
            int blocks = 0;
            while (juce::Time::getMillisecondCounter() - t1 < 3000) {
                while (blocks * 256 < int(juce::Time::getMillisecondCounter() - t1) * 48) {
                    rig.block(0.5f, 0.0f);   // the DAW keeps playing: the Hub writes the Stream Mix
                    b.clear();
                    app->processBlock(b, midi);
                    if (juce::Time::getMillisecondCounter() - t1 > 2000) peak = std::max(peak, b.getMagnitude(0, 256));
                    ++blocks;
                }
                pump(2);
            }
            std::printf("  App Audio (listen link) output peak %.3f (stream %.3f)\n", peak, streamPeak(L, 4096));
            check(std::abs(peak - streamPeak(L, 4096)) < 0.03f, "App Audio receives another Hub's listen link (DAW to DAW)");
            app->releaseResources();
            app.reset();
        }
    }
    ssbus::postRemote(L, ssbus::RemoteParam::Share, 0.0f);
    pump(300);
    Http after;
    check(!after.open(port) || after.request("/l/" + lt).empty(), "turning sharing off closes the links");
}

// Transport the host test drives (App Audio follows the DAW's record button).
struct Head : juce::AudioPlayHead {
    int64_t time = 0;
    bool playing = false, recording = false;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p;
        p.setTimeInSamples(time);
        p.setIsPlaying(playing);
        p.setIsRecording(recording);
        return p;
    }
};

// HEARASIDE App Audio as an insert on a plain audio track, remote-controlled through the bus.
void testAppAudio(juce::AudioPluginFormatManager& fm, const juce::String& path, ssbus::BusLayout& L) {
    auto app = load(fm, bundlePath(path));
    check(app != nullptr, "App Audio loads in a host");
    if (!app) return;
    check(!app->getPluginDescription().isInstrument, "App Audio is an effect (goes on a normal audio track)");
    app->enableAllBuses();
    app->setPlayConfigDetails(2, 2, 48000.0, 256);
    app->prepareToPlay(48000.0, 256);
    check(app->getTotalNumInputChannels() == 2 && app->getTotalNumOutputChannels() == 2, "...with a stereo input and output");
    Head head;
    app->setPlayHead(&head);
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    auto run = [&](int blocks, float amp) {
        float worst = 0.0f;
        for (int k = 0; k < blocks; ++k) {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 256; ++i) buf.setSample(c, i, amp * std::sin(float(head.time + i) * 0.0628f));
            juce::AudioBuffer<float> dry(buf);
            app->processBlock(buf, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 256; ++i) worst = std::max(worst, std::abs(buf.getSample(c, i) - dry.getSample(c, i)));
            head.time += 256;
        }
        return worst;
    };
    pump(200);
    check(run(50, 0.5f) == 0.0f, "no program chosen: the track's own sound passes untouched");

    int idx = -1;
    for (int i = 0; i < ssbus::kMaxSources; ++i)
        if (L.sources[i].state.load() == ssbus::kSlotActive) idx = i;
    check(idx >= 0, "App Audio registers itself on the bus for the Hub");
    if (idx < 0) return;
    auto& src = L.sources[idx];
    check(ssbus::nowNs() - src.heartbeatNs.load() < 500000000ull, "...and reports that the DAW processes it");

    ssbus::postSourceCommand(src, ssbus::SourceParam::On, 0.0f);
    ssbus::postSourceCommand(src, ssbus::SourceParam::LevelDb, -6.0f);
    pump(300);
    check(app->getParameters()[0]->getValue() < 0.5f, "Hub switches capture off (host sees the parameter)");
    check(std::abs(app->getParameters()[1]->getValue() - (-6.0f + 60.0f) / 72.0f) < 0.002f, "Hub sets the program level");
    check((src.flags.load() & ssbus::kSrcOn) == 0, "...and the bus mirrors the switch");
    ssbus::postSourceCommand(src, ssbus::SourceParam::DelayMs, 12.5f);
    pump(300);
    check(std::abs(app->getParameters()[2]->getValue() * 500.0f - 12.5f) < 0.06f, "Hub sets the App Audio sync delay (auto sync)");
    ssbus::requestSourceApp(src, "*system*");
    pump(300);
    check((src.flags.load() & ssbus::kSrcSystem) != 0, "Hub picks the program (whole computer)");
    ssbus::postSourceCommand(src, ssbus::SourceParam::On, 1.0f);
    pump(200);

    // print with the DAW: record from timeline 48000 for 100 blocks
    ssbus::postSourceCommand(src, ssbus::SourceParam::FollowRecord, 1.0f);
    pump(300);
    head.time = 48000;
    head.playing = true;
    run(8, 0.0f);   // playing, not recording yet
    head.recording = true;
    for (int k = 0; k < 100; ++k) {
        run(1, 0.0f);
        if (k % 10 == 0) pump(20);   // the message thread opens the take while the DAW records
    }
    const int64_t stopAt = head.time;
    head.recording = false;
    run(4, 0.0f);
    pump(400);
    const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HEARASIDE").getChildFile("Recordings");
    juce::File take;
    for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.wav"))
        if (take == juce::File() || f.getLastModificationTime() > take.getLastModificationTime()) take = f;
    check(take.existsAsFile(), "a take is printed while the DAW records");
    if (take.existsAsFile()) {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> r(wav.createReaderFor(take.createInputStream().release(), true));
        const auto ref = r ? r->metadataValues.getValue(juce::WavAudioFormat::bwavTimeReference, "-1").getLargeIntValue() : -1;
        const auto len = r ? int64_t(r->lengthInSamples) : 0;
        std::printf("  take %s: starts at %lld, %lld frames, DAW stopped recording at %lld\n", take.getFileName().toRawUTF8(),
                    (long long) ref, (long long) len, (long long) stopAt);
        check(r != nullptr && r->numChannels == 2 && r->sampleRate == 48000.0, "...as a 48 kHz stereo WAV");
        check(ref == 48000 + 8 * 256, "...starting with the DAW's first recorded block");
        check(ref + len == stopAt, "...and its BWF position lines it up with the DAW timeline");
        r.reset();
        take.deleteFile();
    }
    app->releaseResources();
    app.reset();
    pump(100);
    check(src.state.load() == ssbus::kSlotFree, "removing App Audio frees its bus entry");
}

// Peak of the Stream Mix over the last `frames` frames.
float streamPeak(ssbus::BusLayout& L, uint32_t frames) {
    std::vector<float> l(frames), r(frames);
    float* d[2] = { l.data(), r.data() };
    const uint64_t w = L.streamHeader.writePos[0].load();
    ssbus::ringReadAt(L.streamAudio[0], L.streamHeader.writePos[0], w - frames, d, frames);
    float pk = 0;
    for (uint32_t i = 0; i < frames; ++i) pk = std::max({ pk, std::abs(l[i]), std::abs(r[i]) });
    return pk;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: plugin-host-test <Track plugin> <Hub plugin> [App Audio plugin]\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager(fm);

    // isolate from any real session: the plug-ins use this bus when nothing else is configured
    const juce::String bus = "HostTest" + juce::String(juce::Random::getSystemRandom().nextInt(100000));
#if JUCE_WINDOWS
    _putenv_s("HEARASIDE_DEFAULT_BUS", bus.toRawUTF8());
    _putenv_s("HEARASIDE_NO_TUNNEL", "1");   // share tests stay on this machine
#else
    setenv("HEARASIDE_DEFAULT_BUS", bus.toRawUTF8(), 1);
#endif
    Rig rig;
    rig.a = load(fm, bundlePath(argv[1]));
    rig.b = load(fm, bundlePath(argv[1]));
    rig.hub = load(fm, bundlePath(argv[2]));
    check(rig.a && rig.b && rig.hub, "plug-ins load in a host");
    if (!(rig.a && rig.b && rig.hub)) return 1;

    for (auto* p : { rig.a.get(), rig.b.get(), rig.hub.get() }) {
        p->enableAllBuses();
        p->setPlayConfigDetails(2, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
    }
    pump(300);

    ssbus::SharedMemory::Status st {};
    auto shm = ssbus::SharedMemory::openExisting(bus.toStdString(), st);
    check(shm != nullptr, "bus segment exists");
    if (!shm) return 1;
    auto& L = shm->layout();
    int active = 0;
    for (auto& s : L.slots) active += s.state.load() == ssbus::kSlotActive;
    check(active == 2, "two tracks registered on the bus");

    // ---- null test: B = -A, both STR on -> stream must be silent ---------------------------------
    for (int i = 0; i < 200; ++i) rig.block(0.5f, -0.5f);
    const float nullPk = streamPeak(L, 4096);
    std::printf("  null test residual %.1f dBFS\n", 20.0 * std::log10(std::max(1e-12f, nullPk)));
    check(nullPk < std::pow(10.0f, -90.0f / 20.0f), "null test: inverted copies cancel in the Stream Mix (< -90 dBFS)");

    // ---- one track alone reaches the stream at unity --------------------------------------------
    for (int i = 0; i < 100; ++i) rig.block(0.5f, 0.0f);
    const float alone = streamPeak(L, 4096);
    check(std::abs(alone - 0.5f) < 0.01f, "single track reaches the stream at unity gain");

    // ---- headphone level (monTrim) changes only the DAW output ------------------------------------
    setParam(*rig.a, kMonTrim, (-20.0f + 60.0f) / 66.0f);   // -60..+6 dB range
    pump(50);
    for (int i = 0; i < 100; ++i) rig.block(0.5f, 0.0f);
    std::printf("  headphones %.3f, stream %.3f\n", rig.phonesPeak, streamPeak(L, 4096));
    check(std::abs(rig.phonesPeak - 0.05f) < 0.003f, "Headphone Level -20 dB lowers the track in the headphones");
    check(std::abs(streamPeak(L, 4096) - 0.5f) < 0.01f, "...while viewers still get full level");

    // ---- You Hear off: DAW output silent, stream unchanged -----------------------------------------
    setParam(*rig.a, kMon, 0.0f);
    pump(50);
    for (int i = 0; i < 100; ++i) rig.block(0.5f, 0.0f);
    check(rig.phonesPeak < 1e-6f, "You Hear off silences the track in the headphones");
    check(std::abs(streamPeak(L, 4096) - 0.5f) < 0.01f, "...and viewers still hear the track");

    // ---- viewers delay: B is the same sine 40 ms late, A delayed 40 ms + inverted -> null ---------
    setParam(*rig.a, kStrDelay, 40.0f / 500.0f);
    pump(50);
    for (int i = 0; i < 300; ++i) rig.block(0.5f, -0.5f, 1920);
    const float delayPk = streamPeak(L, 4096);
    std::printf("  delay residual %.1f dBFS\n", 20.0 * std::log10(std::max(1e-12f, delayPk)));
    check(delayPk < 0.001f, "Viewers Delay 40 ms lines a track up with a 1920-sample later copy");

    // ---- remote control from the Hub (mailbox -> host parameter) ----------------------------------
    int slotA = -1;
    for (int i = 0; i < ssbus::kMaxSlots; ++i)
        if (L.slots[i].state.load() == ssbus::kSlotActive && !(L.slots[i].flags.load() & ssbus::kFlagMon)) slotA = i;
    check(slotA >= 0, "track A mirrors You Hear = off to the bus");
    if (slotA >= 0) {
        ssbus::postCommand(L.slots[slotA], ssbus::ParamId::Mon, 1.0f);
        pump(300);   // the Track applies commands on its message-thread timer
        rig.block(0.5f, 0.0f);
        check((L.slots[slotA].flags.load() & ssbus::kFlagMon) != 0, "Hub command turns You Hear back on");
        check(P(*rig.a, kMon)->getValue() > 0.5f, "...and the host sees the parameter change");
    }

    // ---- remote control from OBS hotkeys: remote queue -> Hub -------------------
    {
        ssbus::postRemote(L, ssbus::RemoteParam::Panic, 1.0f);
        pump(300);
        for (int i = 0; i < 40; ++i) rig.block(0.5f, 0.0f);
        check((L.header.hubFlags.load() & ssbus::kHubPanic) != 0, "OBS command: mute stream reaches the Hub");
        check(streamPeak(L, 2048) < 1e-6f, "...and the stream goes silent");
        ssbus::postRemote(L, ssbus::RemoteParam::Panic, 0.0f);
        pump(300);
        rig.block(0.5f, 0.0f);
    }

    // ---- a plug-in on the master bus above the Hub masters only the stream ------------------------
    for (int i = 0; i < 20; ++i) rig.block(0.5f, 0.0f);
    const float phonesBefore = rig.phonesPeak;
    rig.masterFx = 0.5f;
    for (int i = 0; i < 100; ++i) rig.block(0.5f, 0.0f);
    std::printf("  master plug-in -6 dB: stream %.3f, headphones %.3f\n", streamPeak(L, 4096), rig.phonesPeak);
    check(std::abs(streamPeak(L, 4096) - 0.25f) < 0.01f, "a plug-in on the master bus above the Hub reaches the viewers");
    check(phonesBefore > 0.01f && std::abs(rig.phonesPeak - phonesBefore) < 0.003f, "...but not the headphones");
    rig.masterFx = 1.0f;

    // ---- state round trip --------------------------------------------------------------------------
    juce::MemoryBlock saved;
    rig.a->getStateInformation(saved);
    setParam(*rig.a, kStrDelay, 0.0f);
    rig.a->setStateInformation(saved.getData(), int(saved.getSize()));
    pump(50);
    check(std::abs(P(*rig.a, kStrDelay)->getValue() - 40.0f / 500.0f) < 0.002f, "state save / restore keeps the delay");

    {
        int slotB = -1;
        for (int i = 0; i < ssbus::kMaxSlots; ++i)
            if (L.slots[i].state.load() == ssbus::kSlotActive && i != slotA) slotB = i;
        if (slotA >= 0 && slotB >= 0) testAutoSync(rig, L, slotA, slotB);
        if (slotB >= 0) testAutoSyncApp(rig, L, slotB);
        if (slotB >= 0) testAutoSyncApp(rig, L, slotB, -1488);   // 31 ms early (the Track delay has 1 ms steps)
    }
    testShare(rig, L, fm, argc > 3 ? argv[3] : nullptr);
    if (argc > 3) testAppAudio(fm, argv[3], L);

    for (auto* p : { rig.a.get(), rig.b.get(), rig.hub.get() }) p->releaseResources();
    rig.a.reset(); rig.b.reset(); rig.hub.reset();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "PLUGIN HOST TEST PASSED" : "PLUGIN HOST TEST FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
