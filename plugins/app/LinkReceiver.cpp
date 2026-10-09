#include "LinkReceiver.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace hearaside {

LinkReceiver::LinkReceiver() : ring_(std::make_unique<ssbus::ChannelRing>()) {}

LinkReceiver::~LinkReceiver() { stop(); }

bool LinkReceiver::isLink(const std::string& t) {
    return (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0) && t.find("/l/") != std::string::npos;
}

void LinkReceiver::start(const std::string& url) {
    stop();
    state_.store(AppCapture::State::Starting);
    thread_ = std::thread([this, url] { run(url); });
}

void LinkReceiver::stop() {
    quit_.store(true);
#ifdef _WIN32
    if (void* h = request_.exchange(nullptr)) WinHttpCloseHandle(static_cast<HINTERNET>(h));   // unblocks a pending receive
#endif
    if (thread_.joinable()) thread_.join();
    quit_.store(false);
    state_.store(AppCapture::State::Idle);
}

#ifdef _WIN32

namespace {
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

struct Target { std::wstring host, path; INTERNET_PORT port = 0; bool secure = false; };

bool crack(const std::wstring& url, Target& t) {
    URL_COMPONENTS uc { sizeof(uc) };
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    t = { host, path, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS };
    return true;
}

// Permanent links: GET /api/resolve/<token> on the share site. 1 = online (ws filled in),
// 0 = a directory that says "not shared right now", -1 = no directory there (a Hub's own link).
int resolveLink(const Target& site, const std::wstring& token, std::wstring& ws) {
    Handle session, connect, request;
    session.h = WinHttpOpen(L"HEARASIDE App Audio", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session.h) return -1;
    WinHttpSetTimeouts(session.h, 3000, 3000, 3000, 3000);   // stop() joins this thread: keep any wait short
    connect.h = WinHttpConnect(session.h, site.host.c_str(), site.port, 0);
    const std::wstring path = L"/api/resolve/" + token;
    if (connect.h) request.h = WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, nullptr, nullptr, site.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h || !WinHttpSendRequest(request.h, nullptr, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) return -1;
    std::string body;
    char buf[1024];
    DWORD got = 0;
    while (body.size() < 8192 && WinHttpReadData(request.h, buf, sizeof buf, &got) && got > 0) body.append(buf, got);
    if (body.find("\"online\"") == std::string::npos) return -1;
    if (body.find("\"online\":true") == std::string::npos) return 0;
    const auto at = body.find("\"ws\":\"");
    if (at == std::string::npos) return 0;
    const auto end = body.find('"', at + 6);
    const std::string url = body.substr(at + 6, end - at - 6);
    if (url.rfind("wss://", 0) != 0) return 0;   // the directory only hands out secure tunnels
    ws.assign(url.begin(), url.end());
    ws.replace(0, 3, L"https");   // WinHttpCrackUrl knows http(s); the upgrade makes it a WebSocket
    return 1;
}
} // namespace

void LinkReceiver::run(std::string url) {
    // the listen page /l/<token> streams from /ws/l/<token>
    const std::wstring w(url.begin(), url.end());
    Target site;
    if (!crack(w, site)) { state_.store(AppCapture::State::Failed); return; }
    const auto at = site.path.find(L"/l/");
    const std::wstring token = at == std::wstring::npos ? std::wstring() : site.path.substr(at + 3);
    Target direct = site;
    if (at != std::wstring::npos) direct.path.replace(at, 3, L"/ws/l/");
    bool isDirectory = site.secure;   // only an https site can be a share directory

    std::vector<uint8_t> msg(1 << 20);
    std::vector<float> l, r;
    while (!quit_.load()) {
        Target t = direct;
        if (isDirectory) {   // where is that Hub right now? (its tunnel changes when it restarts)
            std::wstring ws;
            const int found = resolveLink(site, token, ws);
            if (found == 1 && crack(ws, t)) {
            } else if (found == -1) {
                isDirectory = false;   // the Hub's own (tunnel) link: connect straight
            } else {
                state_.store(AppCapture::State::NotRunning);   // not shared right now
                for (int i = 0; i < 50 && !quit_.load(); ++i) Sleep(100);
                continue;
            }
        }
        Handle session, connect, request;
        session.h = WinHttpOpen(L"HEARASIDE App Audio", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
        if (session.h) connect.h = WinHttpConnect(session.h, t.host.c_str(), t.port, 0);
        if (connect.h) request.h = WinHttpOpenRequest(connect.h, L"GET", t.path.c_str(), nullptr, nullptr, nullptr,
                                                      t.secure ? WINHTTP_FLAG_SECURE : 0);
        bool ok = request.h && WinHttpSetOption(request.h, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
               && WinHttpSendRequest(request.h, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request.h, nullptr);
        Handle ws;
        if (ok) ws.h = WinHttpWebSocketCompleteUpgrade(request.h, 0);
        if (!ws.h) {
            state_.store(AppCapture::State::NotRunning);   // the link is offline (Hub stopped sharing, tunnel down)
            for (int i = 0; i < 30 && !quit_.load(); ++i) Sleep(100);
            continue;
        }
        request_.store(ws.h);
        state_.store(AppCapture::State::Running);
        size_t have = 0;
        while (!quit_.load()) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type {};
            if (have >= msg.size()) have = 0;   // oversized message: drop it
            if (WinHttpWebSocketReceive(ws.h, msg.data() + have, DWORD(msg.size() - have), &got, &type) != NO_ERROR) break;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            have += got;
            if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
            if (type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || have < 16) { have = 0; continue; }
            uint32_t magic = 0, rate = 0, frames = 0;
            uint16_t chans = 0;
            std::memcpy(&magic, msg.data(), 4);
            std::memcpy(&rate, msg.data() + 4, 4);
            std::memcpy(&chans, msg.data() + 8, 2);
            std::memcpy(&frames, msg.data() + 12, 4);
            if (magic == 0x31415248 && chans >= 1 && chans <= 2 && frames > 0) {
                frames = std::min<uint32_t>(frames, uint32_t((have - 16) / (2u * chans)));
                const auto* pcm = reinterpret_cast<const int16_t*>(msg.data() + 16);
                l.resize(frames);
                r.resize(frames);
                for (uint32_t i = 0; i < frames; ++i) {
                    l[i] = pcm[i * chans] / 32768.0f;
                    r[i] = pcm[i * chans + chans - 1] / 32768.0f;
                }
                rate_.store(rate, std::memory_order_relaxed);
                const float* src[2] = { l.data(), r.data() };
                ssbus::ringWrite(*ring_, writePos_, src, 2, frames);
            }
            have = 0;
        }
        if (void* h = request_.exchange(nullptr)) { (void) h; } else { ws.h = nullptr; }   // stop() closed it already
        if (!quit_.load()) state_.store(AppCapture::State::NotRunning);
    }
}

#else

void LinkReceiver::run(std::string) { state_.store(AppCapture::State::Failed); }

#endif

} // namespace hearaside
