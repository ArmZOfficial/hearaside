// SPDX-License-Identifier: GPL-2.0-or-later
#include "remote.h"
#include "http_server.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace hearaside {

using namespace ssbus;

namespace {

std::string num(double v, int decimals = 1) {
    if (!std::isfinite(v)) v = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

std::string colourHex(uint32_t argb) {
    if ((argb >> 24) == 0) return "";
    char b[16];
    std::snprintf(b, sizeof b, "#%06x", unsigned(argb & 0xffffffu));
    return b;
}

} // namespace

BusClient& BusClient::instance() {
    static BusClient c;
    return c;
}

bool BusClient::with(const std::string& busName, const std::function<void(BusLayout&)>& fn) {
    const std::string bus = busName.empty() ? std::string("Main") : busName;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = maps_.find(bus);
    if (it == maps_.end() || !it->second) {
        // only retry opening once per second (DAW not running yet)
        const uint64_t now = nowNs();
        uint64_t& last = lastAttemptNs_[bus];
        if (last != 0 && now - last < 1000000000ull) return false;
        last = now;
        SharedMemory::Status st {};
        auto shm = SharedMemory::openExisting(bus, st);
        if (!shm) return false;
        it = maps_.insert_or_assign(bus, std::move(shm)).first;
    }
    fn(it->second->layout());
    return true;
}

bool BusClient::post(const std::string& busName, int target, uint32_t paramId, float value) {
    return with(busName, [&](BusLayout& L) { postRemote(L, target, paramId, value); });
}

void BusClient::togglePanic(const std::string& busName) {
    with(busName, [](BusLayout& L) {
        const bool on = (L.header.hubFlags.load(std::memory_order_relaxed) & kHubPanic) != 0;
        postRemote(L, RemoteParam::Panic, on ? 0.0f : 1.0f);
    });
}

void BusClient::togglePreview(const std::string& busName) {
    with(busName, [](BusLayout& L) {
        const bool on = (L.header.hubFlags.load(std::memory_order_relaxed) & kHubPreview) != 0;
        postRemote(L, RemoteParam::Preview, on ? 0.0f : 1.0f);
    });
}

void BusClient::recallScene(const std::string& busName, int index) {
    post(busName, -1, uint32_t(RemoteParam::RecallScene), float(index));
}

void BusClient::releaseAll() {
    std::lock_guard<std::mutex> lk(mutex_);
    maps_.clear();
}

std::string BusClient::stateJson(const std::string& busName, const std::string& lang) {
    std::string out;
    const bool found = with(busName, [&](BusLayout& L) {
        const auto& h = L.header;
        const uint64_t now = nowNs();
        const bool hub = hubAlive(L, 1000000000ull);
        const uint32_t flags = h.hubFlags.load(std::memory_order_relaxed);
        const uint32_t rate = h.hubSampleRate.load(std::memory_order_relaxed);
        HubStateView hs;
        const bool haveState = h.hubStateSeq.load(std::memory_order_acquire) != 0 && readHubState(L, hs);

        out = "{\"ok\":true,\"lang\":\"" + jsonEscape(lang) + "\",\"bus\":\"" + jsonEscape(busName) + "\"";
        out += ",\"hub\":" + std::string(hub ? "true" : "false");
        out += ",\"remote\":" + std::string(haveState ? "true" : "false");   // Hub supports remote control
        out += ",\"preview\":" + std::string((flags & kHubPreview) ? "true" : "false");
        out += ",\"panic\":" + std::string((flags & kHubPanic) ? "true" : "false");
        out += ",\"limiter\":" + std::string((flags & kHubLimiter) ? "true" : "false");
        out += ",\"bypassed\":" + std::string((flags & kHubBypassed) ? "true" : "false");
        out += ",\"rate\":" + std::to_string(rate);
        out += ",\"master\":" + num(hs.masterDb) + ",\"headphones\":" + num(hs.headphonesDb);
        out += ",\"scene\":" + std::to_string(hs.activeScene) + ",\"sceneMask\":" + std::to_string(hs.sceneMask);
        out += ",\"scenes\":[";
        for (int i = 0; i < kMaxScenes; ++i) out += (i ? ",\"" : "\"") + jsonEscape(hs.sceneNames[i]) + "\"";
        out += "]";
        const float lufs = bitsFloat(L.streamHeader.loudnessSBits.load(std::memory_order_relaxed));
        out += ",\"lufs\":" + num(lufs < -70.0f ? -200.0 : double(lufs));
        out += ",\"peakL\":" + num(bitsFloat(L.streamHeader.peakBits[0][0].load(std::memory_order_relaxed)), 4);
        out += ",\"peakR\":" + num(bitsFloat(L.streamHeader.peakBits[0][1].load(std::memory_order_relaxed)), 4);
        const double sr = rate > 0 ? double(rate) : 48000.0;
        const double latency = double(h.hubLatencyFrames.load(std::memory_order_relaxed) + h.hubBlockSize.load(std::memory_order_relaxed)) * 1000.0 / sr
                             + double(h.consumerBufferMs.load(std::memory_order_relaxed));
        out += ",\"latencyMs\":" + num(latency, 0);

        struct Row { uint32_t seq; std::string json; };
        std::vector<Row> rows;
        for (int i = 0; i < kMaxSlots; ++i) {
            const auto& s = L.slots[i];
            if (s.state.load(std::memory_order_acquire) != kSlotActive) continue;
            std::string name, uuid;
            uint32_t colour = 0;
            readSlotIdentity(s, name, uuid, colour);
            if (name.empty()) name = "Track " + std::to_string(i + 1);
            const uint32_t f = s.flags.load(std::memory_order_acquire);
            const uint64_t hb = s.heartbeatNs.load(std::memory_order_relaxed);
            const uint32_t st = s.hubStatus.load(std::memory_order_relaxed);
            std::string warn;
            if (st & kHubStatusRateMismatch) warn = "rate";
            else if (st & kHubStatusAhead) warn = "ahead";
            else if (f & kFlagBypassed) warn = "bypass";
            const float in = std::max(bitsFloat(s.peakInBits[0].load(std::memory_order_relaxed)), bitsFloat(s.peakInBits[1].load(std::memory_order_relaxed)));
            std::string r = "{\"slot\":" + std::to_string(i) + ",\"name\":\"" + jsonEscape(name) + "\",\"color\":\"" + colourHex(colour) + "\"";
            r += ",\"mon\":" + std::string((f & kFlagMon) ? "true" : "false");
            r += ",\"str\":" + std::string((f & kFlagStr) ? "true" : "false");
            r += ",\"solo\":" + std::string((f & kFlagSolo) ? "true" : "false");
            r += ",\"gain\":" + num(bitsFloat(s.strGainBits.load(std::memory_order_relaxed)));
            r += ",\"trim\":" + num(bitsFloat(s.monTrimBits.load(std::memory_order_relaxed)));
            r += ",\"delay\":" + num(bitsFloat(s.strDelayBits.load(std::memory_order_relaxed)), 0);
            r += ",\"pan\":" + num(bitsFloat(s.strPanBits.load(std::memory_order_relaxed)), 2);
            r += ",\"active\":" + std::string(hb != 0 && now - hb < 1000000000ull ? "true" : "false");
            r += ",\"in\":" + num(in, 4) + ",\"warn\":\"" + warn + "\"}";
            rows.push_back({ s.sequence.load(std::memory_order_relaxed), std::move(r) });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.seq < b.seq; });
        out += ",\"tracks\":[";
        for (size_t i = 0; i < rows.size(); ++i) out += (i ? "," : "") + rows[i].json;
        out += "]}";
    });
    if (!found) out = "{\"ok\":false,\"lang\":\"" + jsonEscape(lang) + "\",\"bus\":\"" + jsonEscape(busName) + "\"}";
    return out;
}

} // namespace hearaside
