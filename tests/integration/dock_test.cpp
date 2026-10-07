// Tests the OBS control-dock plumbing without OBS: state JSON from a live bus, commands into the
// remote queue, and the localhost HTTP server.
#include "http_server.h"
#include "remote.h"
#include "ssengine/engine.h"

#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++failures;
}

static std::string httpGet(uint16_t port, const std::string& target, const std::string& host) {
    std::string out;
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { closesocket(s); return out; }
    const std::string req = "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    send(s, req.data(), int(req.size()), 0);
    char buf[4096];
    for (int n; (n = recv(s, buf, sizeof buf, 0)) > 0;) out.append(buf, size_t(n));
    closesocket(s);
#endif
    return out;
}

int main() {
    using namespace hearaside;
    const std::string bus = "docktest_" + std::to_string(ssbus::currentPid());

    ssengine::HubEngine hub;
    hub.connect(bus);
    hub.prepare(48000, 256);
    hub.maintain();
    ssengine::TrackPublisher track;
    track.connect(bus, "", 48000, 2);
    track.setIdentity("เสียงร้อง \"Lead\"", 0xff3366ccu);
    track.mirror({ false, true, false, -3.0f, 0.0f, 40.0f, -6.0f, -1 });
    std::vector<float> l(256, 0.25f), r(256, 0.25f);
    float* io[2] = { l.data(), r.data() };
    const float* in[2] = { l.data(), r.data() };
    for (int i = 0; i < 20; ++i) {
        track.process(in, 2, 256, int64_t(i) * 256, true, false, false);
        hub.process(io, 2, 256, ssengine::HubParams {}, int64_t(i) * 256, true, false);
    }
    ssbus::HubStateView hs;
    hs.masterDb = -1.0f;
    hs.sceneMask = 7;
    hs.sceneNames[0] = "ร้องเพลง";
    ssbus::publishHubState(*hub.bus(), hs);

    auto& client = BusClient::instance();
    const std::string json = client.stateJson(bus, "th");
    check(json.find("\"ok\":true") != std::string::npos, "state JSON: bus found");
    check(json.find("\"hub\":true") != std::string::npos, "state JSON: Hub alive");
    check(json.find("\"remote\":true") != std::string::npos, "state JSON: Hub supports remote control");
    check(json.find("เสียงร้อง \\\"Lead\\\"") != std::string::npos, "state JSON: Thai track name with escaped quotes");
    check(json.find("\"mon\":false,\"str\":true") != std::string::npos, "state JSON: track switches");
    check(json.find("\"delay\":40") != std::string::npos && json.find("\"trim\":-6.0") != std::string::npos, "state JSON: delay and headphone level");
    check(json.find("\"color\":\"#3366cc\"") != std::string::npos, "state JSON: host colour");
    check(client.stateJson("no_such_bus_xyz", "en").find("\"ok\":false") != std::string::npos, "state JSON: missing bus reports ok=false");

    uint32_t cursor = hub.bus()->header.remoteReserve.load();
    check(client.post(bus, 0, uint32_t(ssbus::ParamId::Mon), 1.0f), "post: command accepted");
    client.togglePanic(bus);
    int got = 0;
    bool sawMon = false, sawPanic = false;
    ssbus::pollRemote(*hub.bus(), cursor, [&](int t, uint32_t p, float v) {
        ++got;
        if (t == 0 && p == uint32_t(ssbus::ParamId::Mon) && v == 1.0f) sawMon = true;
        if (t == -1 && p == uint32_t(ssbus::RemoteParam::Panic) && v == 1.0f) sawPanic = true;
    });
    check(got == 2 && sawMon && sawPanic, "post / togglePanic arrive in the Hub's remote queue");

    HttpServer server;
    const bool started = server.start(47690, 10, [&](const HttpRequest& req) -> HttpResponse {
        if (req.host != "127.0.0.1:" + std::to_string(server.port())) return { 403, "text/plain", "forbidden" };
        if (req.path == "/api/state") return { 200, "application/json; charset=utf-8", client.stateJson(req.query.at("bus"), "th") };
        return { 404, "text/plain", "not found" };
    });
    check(started, "HTTP server listens on 127.0.0.1");
    if (started) {
        const std::string host = "127.0.0.1:" + std::to_string(server.port());
        const std::string ok = httpGet(server.port(), "/api/state?bus=" + bus, host);
        check(ok.rfind("HTTP/1.1 200", 0) == 0 && ok.find("\"hub\":true") != std::string::npos, "HTTP: state served");
        const std::string bad = httpGet(server.port(), "/api/state?bus=" + bus, "evil.example:80");
        check(bad.rfind("HTTP/1.1 403", 0) == 0, "HTTP: foreign Host header rejected (DNS rebinding)");
        const std::string nf = httpGet(server.port(), "/nope", host);
        check(nf.rfind("HTTP/1.1 404", 0) == 0, "HTTP: unknown path 404");
        server.stop();
    }
    client.releaseAll();
    std::printf("%s\n", failures == 0 ? "DOCK TEST PASSED" : "DOCK TEST FAILED");
    return failures == 0 ? 0 : 1;
}
