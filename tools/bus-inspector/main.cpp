// bus-inspector: live view of a HEARASIDE bus (slot table, heartbeats, fill levels).
//   bus-inspector [BusName] [--once]
#include "ssbus/bus.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace ssbus;

static double ageMs(uint64_t ns) {
    const uint64_t now = nowNs();
    return ns == 0 ? -1.0 : double(now > ns ? now - ns : 0) / 1.0e6;
}
static double db(float lin) { return lin <= 1e-9f ? -120.0 : 20.0 * std::log10(lin); }

int main(int argc, char** argv) {
    std::string bus = "Main";
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--once") == 0) once = true;
        else if (std::strcmp(argv[i], "--help") == 0) { std::printf("usage: bus-inspector [BusName] [--once]\n"); return 0; }
        else bus = argv[i];
    }

    SharedMemory::Status st{};
    std::unique_ptr<SharedMemory> shm;
    for (;;) {
        if (!shm) shm = SharedMemory::openExisting(bus, st);
        if (!once) std::printf("\x1b[2J\x1b[H");
        std::printf("HEARASIDE bus \"%s\"  (%s, protocol v%u, %.1f MB)\n", bus.c_str(),
                    SharedMemory::segmentName(bus).c_str(), kProtocolVersion, double(kLayoutSize) / 1048576.0);
        if (!shm) {
            std::printf("  not available: %s\n", st == SharedMemory::Status::CreateFailed ? "no plug-in has opened this bus yet" : statusText(st));
        } else {
            auto& L = shm->layout();
            const auto& h = L.header;
            const uint32_t f = h.hubFlags.load();
            std::printf("Hub: %s  pid=%u  %u Hz  block=%u  heartbeat=%.0f ms  flags=%s%s%s%s%s\n",
                        hubAlive(L, 1000000000ull) ? "ALIVE" : "none ", h.hubOwnerPid.load(), h.hubSampleRate.load(),
                        h.hubBlockSize.load(), ageMs(h.hubHeartbeatNs.load()),
                        (f & kHubPreview) ? "preview " : "", (f & kHubPanic) ? "PANIC " : "", (f & kHubBypassed) ? "bypassed " : "",
                        (f & kHubOffline) ? "offline " : "", (f & kHubLimiter) ? "limiter" : "");
            std::printf("Consumer (OBS/Bridge): heartbeat=%.0f ms  buffer=%u ms\n", ageMs(h.consumerHeartbeatNs.load()), h.consumerBufferMs.load());
            const auto& sh = L.streamHeader;
            std::printf("Stream Mix: active=%u  %u Hz  writePos=%llu  peak L %.1f / R %.1f dBFS  LUFS-M %.1f  LUFS-S %.1f\n\n",
                        sh.active.load(), sh.sampleRate.load(), (unsigned long long)sh.writePos[0].load(),
                        db(bitsFloat(sh.peakBits[0][0].load())), db(bitsFloat(sh.peakBits[0][1].load())),
                        bitsFloat(sh.loudnessMBits.load()), bitsFloat(sh.loudnessSBits.load()));
            std::printf(" # state  pid    name                 rate   MON STR SOLO  gain   pan  delay stem  in dB  str dB   lead  hb ms  status\n");
            for (int i = 0; i < kMaxSlots; ++i) {
                const auto& s = L.slots[i];
                const uint32_t state = s.state.load();
                if (state == kSlotFree) continue;
                std::string name, uuid; uint32_t col = 0;
                readSlotIdentity(s, name, uuid, col);
                const uint32_t fl = s.flags.load();
                const uint32_t hs = s.hubStatus.load();
                std::printf("%2d %-6s %-6u %-20.20s %-6u  %s   %s   %s  %+5.1f %+5.2f %5.1f  %3d  %6.1f  %6.1f %6u %6.0f  %s%s%s%s%s%s\n",
                            i, state == kSlotActive ? "active" : "claim", s.ownerPid.load(), name.empty() ? "(unnamed)" : name.c_str(),
                            s.sampleRate.load(), (fl & kFlagMon) ? "on " : "off", (fl & kFlagStr) ? "on " : "off", (fl & kFlagSolo) ? "on " : "-  ",
                            bitsFloat(s.strGainBits.load()), bitsFloat(s.strPanBits.load()), bitsFloat(s.strDelayBits.load()), s.stemIndex.load(),
                            db(bitsFloat(s.peakInBits[0].load())), db(bitsFloat(s.peakStreamBits[0].load())),
                            s.hubLeadFrames.load(), ageMs(s.heartbeatNs.load()),
                            (hs & kHubStatusOk) ? "ok " : "", (hs & kHubStatusRateMismatch) ? "RATE " : "", (hs & kHubStatusAhead) ? "AHEAD " : "",
                            (hs & kHubStatusUnderrun) ? "underrun " : "", (fl & kFlagBypassed) ? "BYPASSED " : "", (fl & kFlagOffline) ? "offline" : "");
            }
            std::printf("\nApp Audio sources\n # pid    name                 app                  capture  level  peak dB  latency  delay  slot  hb ms\n");
            for (int i = 0; i < kMaxSources; ++i) {
                const auto& s = L.sources[i];
                if (s.state.load() == kSlotFree) continue;
                std::string name, app; uint32_t col = 0;
                readSourceIdentity(s, name, app, col);
                std::printf("%2d %-6u %-20.20s %-20.20s %u        %+5.1f  %6.1f   %5.1f  %5.1f  %4d  %6.0f\n", i, s.ownerPid.load(),
                            name.empty() ? "(unnamed)" : name.c_str(), app.c_str(), s.capture.load(), bitsFloat(s.levelBits.load()),
                            db(bitsFloat(s.peakBits.load())), bitsFloat(s.latencyBits.load()), bitsFloat(s.delayBits.load()),
                            s.trackSlot.load(), ageMs(s.heartbeatNs.load()));
            }
        }
        if (once) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return 0;
}
