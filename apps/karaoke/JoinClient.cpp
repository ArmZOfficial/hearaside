#include "JoinClient.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <cmath>
#include <cstring>
#include <vector>

namespace hearaside::karaoke {

#ifdef _WIN32
namespace {

struct ScopedHandle {
    HINTERNET h = nullptr;
    ~ScopedHandle() { if (h) WinHttpCloseHandle(h); }
};

struct Target {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool crackUrl(const std::wstring& url, Target& t) {
    URL_COMPONENTS uc { sizeof(uc) };
    wchar_t host[512] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    t.host = host;
    t.path = path;
    t.port = uc.nPort;
    t.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

int resolvePermanentLink(const Target& site, const std::wstring& token, std::wstring& wsResolved) {
    ScopedHandle session, connect, request;
    const DWORD accessType = (site.host == L"127.0.0.1" || site.host == L"localhost") ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    session.h = WinHttpOpen(L"HAS Karaoke/1.0", accessType, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return -1;
    WinHttpSetTimeouts(session.h, 2000, 2000, 2000, 2000);
    connect.h = WinHttpConnect(session.h, site.host.c_str(), site.port, 0);
    const std::wstring path = L"/api/resolve/" + token;
    if (connect.h) {
        request.h = WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, nullptr, nullptr, site.secure ? WINHTTP_FLAG_SECURE : 0);
    }
    if (!request.h || !WinHttpSendRequest(request.h, nullptr, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) {
        return -1;
    }
    std::string body;
    char buf[1024];
    DWORD got = 0;
    while (body.size() < 16384 && WinHttpReadData(request.h, buf, sizeof(buf), &got) && got > 0) {
        body.append(buf, got);
    }
    if (body.find("\"online\"") == std::string::npos) return -1;
    if (body.find("\"online\":true") == std::string::npos) return 0;
    const auto at = body.find("\"ws\":\"");
    if (at == std::string::npos) return 0;
    const auto end = body.find('"', at + 6);
    const std::string wsUrl = body.substr(at + 6, end - at - 6);
    wsResolved.assign(wsUrl.begin(), wsUrl.end());
    if (wsResolved.rfind(L"wss://", 0) == 0) wsResolved.replace(0, 3, L"https");
    else if (wsResolved.rfind(L"ws://", 0) == 0) wsResolved.replace(0, 2, L"http");
    return 1;
}

} // namespace
#endif

JoinClient::JoinClient()
    : downRing_(std::make_unique<ssbus::ChannelRing>()),
      upRing_(std::make_unique<ssbus::ChannelRing>()) {}

JoinClient::~JoinClient() {
    disconnect();
}

void JoinClient::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    friendReader_.prepare(sampleRate_, maxBlockSize);
}

void JoinClient::connect(const juce::String& urlOrToken) {
    disconnect();
    quit_.store(false);
    setState(ConnectionState::Connecting);
    workerThread_ = std::thread([this, urlOrToken] { run(urlOrToken); });
}

void JoinClient::disconnect() {
    quit_.store(true);
#ifdef _WIN32
    if (void* h = wsHandle_.exchange(nullptr)) {
        WinHttpCloseHandle(static_cast<HINTERNET>(h));
    }
#endif
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    quit_.store(false);
    setState(ConnectionState::Disconnected);
    friendReader_.reset();
}

RoomInfo JoinClient::roomInfo() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return roomInfo_;
}

void JoinClient::setState(ConnectionState s) {
    state_.store(s, std::memory_order_relaxed);
    if (onStateChanged) {
        RoomInfo info = roomInfo();
        juce::MessageManager::callAsync([this, s, info] {
            if (onStateChanged) onStateChanged(s, info);
        });
    }
}

bool JoinClient::pullDownstream(float* outL, float* outR, int numSamples) noexcept {
    if (outL == nullptr || outR == nullptr || numSamples <= 0) return false;
    float* channels[2] = { outL, outR };
    return friendReader_.pull(*downRing_, downWritePos_, downSampleRate_.load(std::memory_order_relaxed), channels, numSamples);
}

void JoinClient::pushUpstream(const float* micSamples, int numSamples, uint64_t playbackPos) noexcept {
    if (micSamples == nullptr || numSamples <= 0) return;
    const float* src[1] = { micSamples };
    ssbus::ringWrite(*upRing_, upWritePos_, src, 1, numSamples);
    lastEchoPos_.store(playbackPos, std::memory_order_relaxed);
}

void JoinClient::run(juce::String inputUrl) {
#ifdef _WIN32
    juce::String clean = inputUrl.trim();
    if (clean.startsWithIgnoreCase("has://join?u=")) {
        clean = juce::URL::removeEscapeChars(clean.substring(13));
    }

    juce::String token = clean;
    if (token.containsChar('/')) token = token.fromLastOccurrenceOf("/", false, false);
    if (token.containsChar('?')) token = token.upToFirstOccurrenceOf("?", false, false);

    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        roomInfo_.token = token;
    }

    if (!clean.startsWithIgnoreCase("http://") && !clean.startsWithIgnoreCase("https://") &&
        !clean.startsWithIgnoreCase("ws://") && !clean.startsWithIgnoreCase("wss://")) {
        clean = "https://trycloudflare.com/s/" + clean;
    }

    std::wstring wUrl = clean.toWideCharPointer();
    if (wUrl.rfind(L"ws://", 0) == 0) wUrl.replace(0, 2, L"http");
    else if (wUrl.rfind(L"wss://", 0) == 0) wUrl.replace(0, 3, L"https");

    Target site;
    if (!crackUrl(wUrl, site)) {
        setState(ConnectionState::Failed);
        return;
    }

    const auto at = site.path.find(L"/s/");
    const std::wstring tokenW = at == std::wstring::npos ? std::wstring() : site.path.substr(at + 3);
    Target direct = site;
    if (at != std::wstring::npos) direct.path.replace(at, 3, L"/ws/s/");

    bool isDirectory = site.secure && (site.host.find(L"trycloudflare.com") == std::wstring::npos) && (site.host.find(L"127.0.0.1") == std::wstring::npos);

    while (!quit_.load()) {
        Target t = direct;
        if (isDirectory) {
            setState(ConnectionState::Resolving);
            std::wstring resolved;
            const int r = resolvePermanentLink(site, tokenW, resolved);
            if (r == 1 && crackUrl(resolved, t)) {
                // resolved successfully
            } else if (r == -1) {
                isDirectory = false; // hub's own direct address
            } else {
                setState(ConnectionState::Reconnecting);
                for (int i = 0; i < 40 && !quit_.load(); ++i) Sleep(100);
                continue;
            }
        }

        setState(ConnectionState::Connecting);
        ScopedHandle session, connectHandle, requestHandle;
        const DWORD accessType = (t.host == L"127.0.0.1" || t.host == L"localhost") ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
        session.h = WinHttpOpen(L"HAS Karaoke/1.0", accessType, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session.h) {
            WinHttpSetTimeouts(session.h, 2000, 2000, 2000, 2000);
            connectHandle.h = WinHttpConnect(session.h, t.host.c_str(), t.port, 0);
        }
        if (connectHandle.h) {
            requestHandle.h = WinHttpOpenRequest(connectHandle.h, L"GET", t.path.c_str(), nullptr, nullptr, nullptr, t.secure ? WINHTTP_FLAG_SECURE : 0);
        }

        bool ok = requestHandle.h && WinHttpSetOption(requestHandle.h, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
               && WinHttpSendRequest(requestHandle.h, nullptr, 0, nullptr, 0, 0, 0)
               && WinHttpReceiveResponse(requestHandle.h, nullptr);

        HINTERNET rawWs = nullptr;
        if (ok) rawWs = WinHttpWebSocketCompleteUpgrade(requestHandle.h, 0);
        requestHandle.h = nullptr; // Handed off / consumed by upgrade

        if (!rawWs) {
            setState(ConnectionState::Reconnecting);
            for (int i = 0; i < 30 && !quit_.load(); ++i) Sleep(100);
            continue;
        }

        wsHandle_.store(rawWs);
        setState(ConnectionState::Handshaking);

        // Send initial handshake JSON
        const std::string initJson = "{\"speakerMode\":false,\"hearMyself\":false}";
        WinHttpWebSocketSend(rawWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)initJson.data(), DWORD(initJson.size()));

        std::vector<uint8_t> rxBuffer(128 * 1024);
        std::vector<float> pcmL, pcmR;

        // Initialise upReadPos to current writePos
        upReadPos_ = upWritePos_.load(std::memory_order_relaxed);

        // Dedicated sender thread for upstream microphone audio
        std::thread sendThread([this, rawWs] {
            std::vector<int16_t> txPcm(480);
            std::vector<uint8_t> txPacket(24 + 480 * sizeof(int16_t));
            while (!quit_.load() && wsHandle_.load() == rawWs) {
                const uint64_t upW = upWritePos_.load(std::memory_order_relaxed);
                if (upW >= upReadPos_ + 480) {
                    float floatBuf[480];
                    for (int i = 0; i < 480; ++i) {
                        floatBuf[i] = upRing_->ch[0][(upReadPos_ + i) & (ssbus::kRingMask)];
                    }
                    upReadPos_ += 480;

                    const uint32_t magic = 0x32415248; // "HRA2"
                    const uint32_t rate = 48000;
                    const uint16_t chans = 1;
                    const uint16_t latencyKnown = 1;
                    const uint32_t frames = 480;
                    const uint64_t pos = lastEchoPos_.load(std::memory_order_relaxed);

                    std::memcpy(txPacket.data(), &magic, 4);
                    std::memcpy(txPacket.data() + 4, &rate, 4);
                    std::memcpy(txPacket.data() + 8, &chans, 2);
                    std::memcpy(txPacket.data() + 10, &latencyKnown, 2);
                    std::memcpy(txPacket.data() + 12, &frames, 4);
                    std::memcpy(txPacket.data() + 16, &pos, 8);

                    for (int i = 0; i < 480; ++i) {
                        const float s = std::clamp(floatBuf[i], -1.0f, 1.0f);
                        txPcm[i] = int16_t(std::round(s * 32767.0f));
                    }
                    std::memcpy(txPacket.data() + 24, txPcm.data(), 480 * sizeof(int16_t));

                    const DWORD sendErr = WinHttpWebSocketSend(rawWs, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                                               txPacket.data(), DWORD(txPacket.size()));
                    if (sendErr != NO_ERROR) break;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }
        });

        size_t have = 0;

        while (!quit_.load()) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type {};
            if (have >= rxBuffer.size()) have = 0;

            const DWORD err = WinHttpWebSocketReceive(rawWs, rxBuffer.data() + have, DWORD(rxBuffer.size() - have), &got, &type);
            if (err != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;

            have += got;
            if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
                continue;
            }

            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && have > 0) {
                const juce::String text = juce::String::fromUTF8((const char*)rxBuffer.data(), (int)have);
                const auto var = juce::JSON::parse(text);
                if (var.isObject()) {
                    auto* obj = var.getDynamicObject();
                    if (obj != nullptr) {
                        if (obj->hasProperty("v") && int(obj->getProperty("v")) >= 2) {
                            std::lock_guard<std::mutex> lock(infoMutex_);
                            roomInfo_.name = obj->getProperty("name").toString();
                            roomInfo_.host = obj->getProperty("host").toString();
                            setState(ConnectionState::Live);
                        }
                        if (obj->hasProperty("ping")) {
                            const int pingVal = int(obj->getProperty("ping"));
                            const std::string pongJson = "{\"pong\":" + std::to_string(pingVal) + "}";
                            WinHttpWebSocketSend(rawWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)pongJson.data(), DWORD(pongJson.size()));
                        }
                        if (obj->hasProperty("delay")) {
                            std::lock_guard<std::mutex> lock(infoMutex_);
                            roomInfo_.delayMs = float(double(obj->getProperty("delay")));
                        }
                    }
                }
                have = 0;
            } else if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && have >= 24) {
                uint32_t magic = 0, rate = 0, frames = 0;
                uint16_t chans = 0;
                uint64_t pos = 0;

                std::memcpy(&magic, rxBuffer.data(), 4);
                std::memcpy(&rate, rxBuffer.data() + 4, 4);
                std::memcpy(&chans, rxBuffer.data() + 8, 2);
                std::memcpy(&frames, rxBuffer.data() + 12, 4);
                std::memcpy(&pos, rxBuffer.data() + 16, 8);

                if (magic == 0x32415248 && chans >= 1 && chans <= 2 && frames > 0) {
                    frames = std::min<uint32_t>(frames, uint32_t((have - 24) / (2u * chans)));
                    const auto* pcm = reinterpret_cast<const int16_t*>(rxBuffer.data() + 24);

                    pcmL.resize(frames);
                    pcmR.resize(frames);
                    for (uint32_t i = 0; i < frames; ++i) {
                        pcmL[i] = pcm[i * chans] / 32768.0f;
                        pcmR[i] = pcm[i * chans + (chans > 1 ? 1 : 0)] / 32768.0f;
                    }
                    downSampleRate_.store(rate, std::memory_order_relaxed);
                    lastTimelinePos_.store(pos, std::memory_order_relaxed);

                    const float* src[2] = { pcmL.data(), pcmR.data() };
                    ssbus::ringWrite(*downRing_, downWritePos_, src, 2, frames);
                }
                have = 0;
            }
        }

        if (void* h = wsHandle_.exchange(nullptr)) {
            WinHttpCloseHandle(static_cast<HINTERNET>(h));
        }
        if (sendThread.joinable()) {
            sendThread.join();
        }

        if (!quit_.load()) {
            setState(ConnectionState::Reconnecting);
            for (int i = 0; i < 30 && !quit_.load(); ++i) Sleep(100);
        }
    }
#else
    setState(ConnectionState::Failed);
#endif
}

} // namespace hearaside::karaoke
