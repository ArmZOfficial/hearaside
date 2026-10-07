// HEARASIDE OBS Source - delivers the HEARASIDE Stream Mix (or a stem) to OBS without any
// virtual audio cable. SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <util/platform.h>

#include "http_server.h"
#include "remote.h"
#include "ssbus/bus.h"
#include "ssdsp/consumer.h"

#include "dock_page.inc"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("HEARASIDE")

namespace {

bool thai() {
    const char* loc = obs_get_locale();
    return loc != nullptr && std::strncmp(loc, "th", 2) == 0;
}
const char* T(const char* en, const char* th) { return thai() ? th : en; }

// ---- control dock server (one per OBS process) ------------------------------------------------

constexpr uint16_t kDockPort = 47621;
hearaside::HttpServer gServer;
std::string gToken;

std::string makeToken() {
    std::random_device rd;
    std::string t;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) t += hex[rd() & 15u];
    return t;
}

std::string dockUrl(const std::string& bus) {
    if (gServer.port() == 0) return {};
    std::string url = "http://127.0.0.1:" + std::to_string(gServer.port()) + "/";
    if (!bus.empty() && bus != "Main") url += "?bus=" + bus;
    return url;
}

hearaside::HttpResponse handleRequest(const hearaside::HttpRequest& req) {
    // DNS-rebinding guard: only answer requests addressed to the loopback name we serve
    const std::string port = std::to_string(gServer.port());
    if (req.host != "127.0.0.1:" + port && req.host != "localhost:" + port) return { 403, "text/plain", "forbidden" };

    if (req.path == "/" || req.path == "/dock") {
        std::string page(reinterpret_cast<const char*>(hearaside::kDockPage), hearaside::kDockPageSize);
        auto replace = [&](const std::string& key, const std::string& value) {
            for (size_t pos = page.find(key); pos != std::string::npos; pos = page.find(key, pos + value.size()))
                page.replace(pos, key.size(), value);
        };
        replace("%TOKEN%", gToken);
        replace("%LANG%", thai() ? "th" : "en");
        return { 200, "text/html; charset=utf-8", page };
    }
    // The API needs the per-session token that is only embedded in the page above, so other web
    // pages open in a browser on this machine cannot drive the Hub (they cannot read our page).
    const auto tok = req.query.find("token");
    if (tok == req.query.end() || tok->second != gToken) return { 403, "text/plain", "forbidden" };
    const auto busIt = req.query.find("bus");
    const std::string bus = busIt != req.query.end() && !busIt->second.empty() ? busIt->second : "Main";

    if (req.path == "/api/state")
        return { 200, "application/json; charset=utf-8", hearaside::BusClient::instance().stateJson(bus, thai() ? "th" : "en") };
    if (req.path == "/api/cmd" && req.method == "POST") {
        const auto t = req.query.find("t"), p = req.query.find("p"), v = req.query.find("v");
        if (t == req.query.end() || p == req.query.end() || v == req.query.end()) return { 400, "text/plain", "missing t/p/v" };
        const int target = std::atoi(t->second.c_str());
        const long pid = std::atol(p->second.c_str());
        const float value = float(std::atof(v->second.c_str()));
        if (target < -1 || target >= ssbus::kMaxSlots || pid < 1 || pid > 32 || !std::isfinite(value))
            return { 400, "text/plain", "bad command" };
        const bool ok = hearaside::BusClient::instance().post(bus, target, uint32_t(pid), value);
        return { ok ? 204 : 404, "text/plain", "" };
    }
    return { 404, "text/plain", "not found" };
}

struct Config {
    std::string bus = "Main";
    int output = 0;
    int bufferMs = 30;
};

struct Source {
    obs_source_t* source = nullptr;
    std::thread thread;
    std::atomic<bool> running { true };

    std::mutex cfgMutex;
    Config cfg;
    bool cfgDirty = true;

    std::mutex statusMutex;
    ssdsp::ConsumerStatus status;

    struct SceneHotkey { Source* owner; int index; };
    std::vector<obs_hotkey_id> hotkeys;
    std::vector<std::unique_ptr<SceneHotkey>> sceneKeys;

    std::string bus() {
        std::lock_guard<std::mutex> lk(cfgMutex);
        return cfg.bus;
    }

    void run() {
        ssdsp::StreamConsumer consumer;
        const uint32_t rate = audio_output_get_sample_rate(obs_get_audio());
        const uint32_t frames = rate / 100;   // 10 ms per tick
        std::vector<float> left(frames), right(frames);
        const uint64_t tick = 10000000ull;
        uint64_t next = os_gettime_ns();
        uint64_t last = next;
        uint64_t ts = next;

        while (running.load(std::memory_order_relaxed)) {
            next += tick;
            if (!os_sleepto_ns(next)) {
                // we are late (system stall): don't try to catch up a long backlog
                const uint64_t now = os_gettime_ns();
                if (now > next + 200000000ull) next = now;
            }
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                if (cfgDirty) {
                    consumer.configure(cfg.bus, cfg.output, double(cfg.bufferMs), rate);
                    cfgDirty = false;
                }
            }
            const uint64_t now = os_gettime_ns();
            const double dt = double(now - last) * 1.0e-9;
            last = now;

            float* out[2] = { left.data(), right.data() };
            consumer.pull(out, frames, dt > 0.0 && dt < 1.0 ? dt : 0.010);

            obs_source_audio a {};
            a.data[0] = reinterpret_cast<const uint8_t*>(left.data());
            a.data[1] = reinterpret_cast<const uint8_t*>(right.data());
            a.frames = frames;
            a.speakers = SPEAKERS_STEREO;
            a.format = AUDIO_FORMAT_FLOAT_PLANAR;
            a.samples_per_sec = rate;
            // continuous timestamps (frame-accurate); re-anchor if we drift from the clock
            const int64_t diff = int64_t(ts) - int64_t(now);
            if (diff > 70000000 || diff < -70000000) ts = now;
            a.timestamp = ts;
            ts += uint64_t(frames) * 1000000000ull / rate;
            obs_source_output_audio(source, &a);

            std::lock_guard<std::mutex> lk(statusMutex);
            status = consumer.status();
        }
    }
};

std::string statusText(const ssdsp::ConsumerStatus& s) {
    char buf[256];
    if (!s.connected)
        return T("Waiting for HEARASIDE Hub (open your DAW project with the Hub on the master bus)",
                 "รอ HEARASIDE Hub (เปิดโปรเจกต์ DAW ที่มี Hub อยู่ที่ Master)");
    if (!s.hubAlive) {
        if (s.hubFlags & ssbus::kHubBypassed) return T("Hub is bypassed - viewers hear nothing", "Hub ถูก bypass อยู่ คนดูจะไม่ได้ยินอะไร");
        return T("Connected, but the Hub is not running (DAW stopped audio?)", "เชื่อมแล้ว แต่ Hub ไม่ทำงาน (DAW ปิดเสียงอยู่หรือเปล่า)");
    }
    std::snprintf(buf, sizeof buf, "%s · DAW %.1f kHz · buffer %.1f ms · drift %+.0f ppm · underrun %llu%s",
                  T("Connected", "เชื่อมแล้ว"), s.dawRate / 1000.0, s.bufferMs, s.driftPpm,
                  (unsigned long long) s.underruns,
                  (s.hubFlags & ssbus::kHubPanic) ? T(" · stream muted in Hub", " · ตัดเสียงคนดูอยู่ใน Hub") : "");
    return buf;
}

// ---- obs_source_info callbacks -------------------------------------------------------------

const char* ss_get_name(void*) { return "HEARASIDE"; }

void ss_update(void* data, obs_data_t* settings) {
    auto* s = static_cast<Source*>(data);
    std::lock_guard<std::mutex> lk(s->cfgMutex);
    const char* bus = obs_data_get_string(settings, "bus");
    s->cfg.bus = (bus && *bus) ? bus : "Main";
    s->cfg.output = int(obs_data_get_int(settings, "output"));
    s->cfg.bufferMs = int(obs_data_get_int(settings, "buffer_ms"));
    if (s->cfg.bufferMs < 5) s->cfg.bufferMs = 30;
    s->cfgDirty = true;
}

void hk_panic(void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) hearaside::BusClient::instance().togglePanic(static_cast<Source*>(data)->bus());
}
void hk_preview(void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) hearaside::BusClient::instance().togglePreview(static_cast<Source*>(data)->bus());
}
void hk_scene(void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (!pressed) return;
    auto* k = static_cast<Source::SceneHotkey*>(data);
    hearaside::BusClient::instance().recallScene(k->owner->bus(), k->index);
}

void* ss_create(obs_data_t* settings, obs_source_t* source) {
    auto* s = new Source();
    s->source = source;
    ss_update(s, settings);
    s->thread = std::thread([s] { s->run(); });

    // hotkeys (Settings > Hotkeys, also usable from Stream Deck); OBS saves them with the source
    s->hotkeys.push_back(obs_hotkey_register_source(source, "hearaside.mute_stream",
        T("HEARASIDE: Mute / unmute stream", "HEARASIDE: ตัด / คืนเสียงคนดู"), hk_panic, s));
    s->hotkeys.push_back(obs_hotkey_register_source(source, "hearaside.preview",
        T("HEARASIDE: Hear viewers' mix on / off", "HEARASIDE: ฟังแบบคนดู เปิด / ปิด"), hk_preview, s));
    for (int i = 0; i < ssbus::kMaxScenes; ++i) {
        s->sceneKeys.push_back(std::make_unique<Source::SceneHotkey>(Source::SceneHotkey { s, i }));
        const std::string name = "hearaside.scene" + std::to_string(i + 1);
        const std::string desc = std::string(T("HEARASIDE: Scene ", "HEARASIDE: ซีน ")) + std::to_string(i + 1);
        s->hotkeys.push_back(obs_hotkey_register_source(source, name.c_str(), desc.c_str(), hk_scene, s->sceneKeys.back().get()));
    }
    return s;
}

void ss_destroy(void* data) {
    auto* s = static_cast<Source*>(data);
    for (auto id : s->hotkeys) obs_hotkey_unregister(id);
    s->running.store(false);
    if (s->thread.joinable()) s->thread.join();
    delete s;
}

void ss_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "bus", "Main");
    obs_data_set_default_int(settings, "output", 0);
    obs_data_set_default_int(settings, "buffer_ms", 30);
}

bool ss_refresh(obs_properties_t*, obs_property_t*, void*) { return true; }   // re-creates the properties (status)

bool ss_open_dock(obs_properties_t*, obs_property_t*, void* data) {
    const std::string url = dockUrl(data ? static_cast<Source*>(data)->bus() : std::string());
#ifdef _WIN32
    if (!url.empty()) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    return false;
}

obs_properties_t* ss_properties(void* data) {
    auto* s = static_cast<Source*>(data);
    obs_properties_t* props = obs_properties_create();

    std::string bus = "Main";
    ssdsp::ConsumerStatus st;
    if (s) {
        { std::lock_guard<std::mutex> lk(s->cfgMutex); bus = s->cfg.bus; }
        { std::lock_guard<std::mutex> lk(s->statusMutex); st = s->status; }
    }

    obs_properties_add_text(props, "bus", T("Bus name", "ชื่อ Bus"), OBS_TEXT_DEFAULT);

    obs_property_t* out = obs_properties_add_list(props, "output", T("Output", "เสียงที่ส่ง"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    // read the stem names the Hub published (best effort; works only while the DAW runs)
    std::vector<std::string> names(ssbus::kNumStreamOuts);
    ssbus::SharedMemory::Status shmStatus {};
    if (auto shm = ssbus::SharedMemory::openExisting(bus, shmStatus)) {
        auto& sh = shm->layout().streamHeader;
        for (int i = 0; i < ssbus::kNumStreamOuts; ++i) {
            char n[ssbus::kNameBytes];
            std::memcpy(n, sh.names[i], sizeof n);
            n[sizeof n - 1] = 0;
            names[size_t(i)] = n;
        }
    }
    obs_property_list_add_int(out, T("Stream Mix (what viewers hear)", "Stream Mix (เสียงที่คนดูได้ยิน)"), 0);
    for (int i = 1; i < ssbus::kNumStreamOuts; ++i) {
        std::string label = "Stem " + std::to_string(i);
        if (!names[size_t(i)].empty() && names[size_t(i)] != label) label += " - " + names[size_t(i)];
        obs_property_list_add_int(out, label.c_str(), i);
    }

    obs_property_t* buf = obs_properties_add_list(props, "buffer_ms", T("Buffer", "บัฟเฟอร์"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(buf, T("Low (15 ms)", "ต่ำ (15 ms)"), 15);
    obs_property_list_add_int(buf, T("Normal (30 ms)", "ปกติ (30 ms)"), 30);
    obs_property_list_add_int(buf, T("Safe (60 ms)", "ปลอดภัย (60 ms)"), 60);

    const std::string status = statusText(st);
    obs_property_t* info = obs_properties_add_text(props, "status_info", status.c_str(), OBS_TEXT_INFO);
    obs_property_set_long_description(info, T("Set this source's Audio Monitoring to \"Monitor Off\" - you already hear your DAW. "
                                              "If the DAW also plays through Desktop Audio, mute Desktop Audio or viewers hear everything twice.",
                                              "ตั้ง Audio Monitoring ของ source นี้เป็น \"Monitor Off\" เพราะคุณฟังจาก DAW อยู่แล้ว "
                                              "และถ้า DAW เล่นเสียงผ่าน Desktop Audio ด้วย ให้ปิด Desktop Audio ไม่งั้นคนดูจะได้ยินซ้ำ 2 ชั้น"));
    obs_properties_add_button(props, "refresh", T("Refresh status", "อัปเดตสถานะ"), ss_refresh);

    // control panel for the Hub inside OBS
    const std::string url = dockUrl(bus);
    if (!url.empty()) {
        const std::string text = std::string(T("Hub control panel: ", "หน้าควบคุม Hub: ")) + url;
        obs_property_t* dock = obs_properties_add_text(props, "dock_info", text.c_str(), OBS_TEXT_INFO);
        obs_property_set_long_description(dock,
            T("Docks > Custom Browser Docks... > add this URL to control the Hub (tracks, scenes, mute stream, preview, levels) "
              "without leaving OBS. Hotkeys: Settings > Hotkeys > HEARASIDE.",
              "เมนู Docks > Custom Browser Docks... > ใส่ URL นี้ เพื่อคุม Hub (แทร็ก ซีน ตัดเสียงคนดู ฟังแบบคนดู ระดับเสียง) ได้ใน OBS "
              "และตั้ง hotkey ได้ที่ Settings > Hotkeys > HEARASIDE"));
        obs_properties_add_button(props, "open_dock", T("Open control panel in browser", "เปิดหน้าควบคุมในเบราว์เซอร์"), ss_open_dock);
    }
    return props;
}

} // namespace

bool obs_module_load(void) {
    obs_source_info info {};
    info.id = "hearaside_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = ss_get_name;
    info.create = ss_create;
    info.destroy = ss_destroy;
    info.update = ss_update;
    info.get_defaults = ss_defaults;
    info.get_properties = ss_properties;
    info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
    obs_register_source(&info);

    gToken = makeToken();
    if (gServer.start(kDockPort, 10, handleRequest))
        blog(LOG_INFO, "[HEARASIDE] control dock at http://127.0.0.1:%u/", unsigned(gServer.port()));
    else
        blog(LOG_WARNING, "[HEARASIDE] no free local port for the control dock (%u-%u)", unsigned(kDockPort), unsigned(kDockPort + 9));
    blog(LOG_INFO, "[HEARASIDE] OBS source loaded (version %s)", HEARASIDE_VERSION);
    return true;
}

void obs_module_unload(void) {
    gServer.stop();
    hearaside::BusClient::instance().releaseAll();
}

const char* obs_module_name(void) { return "HEARASIDE"; }
const char* obs_module_description(void) { return "Receives the HEARASIDE viewers' mix from your DAW."; }
