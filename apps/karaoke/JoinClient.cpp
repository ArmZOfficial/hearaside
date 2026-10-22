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
#include <algorithm>

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

    // Initialise Opus encoder (VoIP mode mono @ 96 kbps with FEC for singing clarity)
    opusEnc_.init(48000, 1, ssnet::OpusEnc::App::Voip);
    opusEnc_.setBitrate(96000);
    opusEnc_.setComplexity(5);
    opusEnc_.setInbandFec(true);
    opusEnc_.setPacketLossPerc(5);

    // Initialise Opus decoder (stereo)
    opusDec_.init(48000, 2);

    // Initialise AEC (48 kHz, 10 ms frame, 200 ms room tail)
    {
        std::lock_guard<std::mutex> lk(aecMutex_);
        aec_.init(48000, 480, 200);
        aec_.setMode(speakerMode_.load(std::memory_order_relaxed) ? ssnet::AecMode::On : ssnet::AecMode::Off);
    }
}

void JoinClient::connect(const juce::String& urlOrToken) {
    disconnect();
    quit_.store(false);
    setState(ConnectionState::Connecting);
    workerThread_ = std::thread([this, urlOrToken] { run(urlOrToken); });
}

void JoinClient::disconnect() {
    quit_.store(true);
    {
        std::lock_guard<std::mutex> lk(rtcMutex_);
        if (rtcPeer_) {
            rtcPeer_->close();
            rtcPeer_.reset();
        }
    }
    isUdp_.store(false, std::memory_order_relaxed);
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
    lastDownSeqValid_ = false;
    upSeq_ = 0;
}

RoomInfo JoinClient::roomInfo() const {
    RoomInfo info;
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        info = roomInfo_;
    }

    // Populate real-time jitter buffer & AEC metrics (read non-allocating atomics/methods)
    info.bufferMs = float(friendReader_.bufferMs());
    info.jitterP95Ms = float(friendReader_.jitterP95Ms());
    info.underruns = friendReader_.underruns();
    info.concealedMs = float(friendReader_.concealedMs());
    info.codec = activeCodec_;
    info.speakerMode = speakerMode_.load(std::memory_order_relaxed);
    info.erleDb = aec_.erleDb();
    info.leakDetected = aec_.leakDetected();
    info.isUdp = isUdp_.load(std::memory_order_relaxed);
    info.transport = info.isUdp ? "WebRTC UDP" : (info.codec.equalsIgnoreCase("opus") ? "WebSocket Opus" : "WebSocket PCM");

    return info;
}

void JoinClient::setSpeakerMode(bool enabled) {
    speakerMode_.store(enabled, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(aecMutex_);
        aec_.setMode(enabled ? ssnet::AecMode::On : ssnet::AecMode::Off);
    }

#ifdef _WIN32
    void* h = wsHandle_.load(std::memory_order_relaxed);
    if (h != nullptr && isLive()) {
        const std::string ctrlJson = "{\"speakerMode\":" + std::string(enabled ? "true" : "false") +
                                     ",\"aec\":\"" + std::string(enabled ? "on" : "off") + "\"}";
        WinHttpWebSocketSend(static_cast<HINTERNET>(h), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)ctrlJson.data(), DWORD(ctrlJson.size()));
    }
#endif
}

void JoinClient::setLocalLatencyMs(double totalLatencyMs) {
    // Add estimated 3 ms acoustic flight time (speaker -> mic ~1 meter)
    const int delay = std::clamp(int((totalLatencyMs + 3.0) * 48.0), 64, 9600);
    localDelaySamples_.store(delay, std::memory_order_relaxed);
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

        // K5: Instantiate WebRTC peer for low-latency UDP DataChannel audio
        ssnet::RtcPeerConfig rtcConfig;
        rtcConfig.iceServers.push_back({ "stun:stun.cloudflare.com:3478", "", "" });
        rtcConfig.iceServers.push_back({ "stun:stun.l.google.com:19302", "", "" });
        auto rtc = std::make_shared<ssnet::RtcPeer>(rtcConfig);
        {
            std::lock_guard<std::mutex> lk(rtcMutex_);
            rtcPeer_ = rtc;
        }

        std::atomic<bool> sessionOver{ false };
        std::mutex wsSendMutex;

        auto sendWsText = [&](const std::string& txt) -> bool {
            std::lock_guard<std::mutex> lk(wsSendMutex);
            if (sessionOver.load() || wsHandle_.load() != rawWs) return false;
            const DWORD err = WinHttpWebSocketSend(rawWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                                   (PVOID)txt.data(), DWORD(txt.size()));
            return err == NO_ERROR;
        };

        auto sendWsBinary = [&](const uint8_t* data, size_t size) -> bool {
            std::lock_guard<std::mutex> lk(wsSendMutex);
            if (sessionOver.load() || wsHandle_.load() != rawWs) return false;
            const DWORD err = WinHttpWebSocketSend(rawWs, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                                   (PVOID)data, DWORD(size));
            return err == NO_ERROR;
        };

        std::mutex downAudioMutex;
        std::vector<float> pcmL, pcmR;

        auto handleAudioPacket = [&](const uint8_t* pData, size_t pSize) {
            std::lock_guard<std::mutex> lk(downAudioMutex);
            if (sessionOver.load()) return;

            if (pSize >= 22) {
                ssnet::Hra3Header h3{};
                const uint8_t* payload = nullptr;
                size_t payloadSize = 0;

                if (ssnet::parseHra3(pData, pSize, h3, payload, payloadSize)) {
                    if (h3.codec == ssnet::CodecOpus) {
                        if (lastDownSeqValid_ && h3.seq > lastDownSeq_ + 1) {
                            const uint32_t missed = std::min<uint32_t>(h3.seq - lastDownSeq_ - 1, 10u);
                            pcmL.resize(h3.frames);
                            pcmR.resize(h3.frames);
                            float* plcCh[2] = { pcmL.data(), pcmR.data() };
                            for (uint32_t m = 0; m < missed; ++m) {
                                opusDec_.decodePlc(plcCh, h3.frames);
                                const float* src[2] = { pcmL.data(), pcmR.data() };
                                ssbus::ringWrite(*downRing_, downWritePos_, src, 2, h3.frames);
                            }
                        }

                        pcmL.resize(h3.frames);
                        pcmR.resize(h3.frames);
                        float* decCh[2] = { pcmL.data(), pcmR.data() };
                        const int decFrames = opusDec_.decode(payload, int(payloadSize), decCh, h3.frames);

                        if (decFrames > 0) {
                            const float* src[2] = { pcmL.data(), h3.chans == 2 ? pcmR.data() : pcmL.data() };
                            downSampleRate_.store(48000, std::memory_order_relaxed);
                            lastTimelinePos_.store(h3.pos, std::memory_order_relaxed);
                            ssbus::ringWrite(*downRing_, downWritePos_, src, 2, uint32_t(decFrames));

                            std::vector<float> refMono;
                            refMono.resize(size_t(decFrames));
                            for (size_t i = 0; i < size_t(decFrames); ++i) {
                                const float l = pcmL[i];
                                const float r = (h3.chans == 2) ? pcmR[i] : l;
                                refMono[i] = 0.5f * (l + r);
                            }
                            {
                                std::lock_guard<std::mutex> aecLk(aecMutex_);
                                aec_.pushReference(refMono.data(), size_t(decFrames));
                            }

                            lastDownSeq_ = h3.seq;
                            lastDownSeqValid_ = true;
                            activeCodec_ = "opus";
                        }
                        return;
                    } else if (h3.codec == ssnet::CodecPcm16) {
                        const uint32_t frames = std::min<uint32_t>(h3.frames, uint32_t(payloadSize / (2u * h3.chans)));
                        const auto* pcm = reinterpret_cast<const int16_t*>(payload);

                        pcmL.resize(frames);
                        pcmR.resize(frames);
                        for (uint32_t i = 0; i < frames; ++i) {
                            pcmL[i] = pcm[i * h3.chans] / 32768.0f;
                            pcmR[i] = pcm[i * h3.chans + (h3.chans > 1 ? 1 : 0)] / 32768.0f;
                        }
                        downSampleRate_.store(48000, std::memory_order_relaxed);
                        lastTimelinePos_.store(h3.pos, std::memory_order_relaxed);

                        const float* src[2] = { pcmL.data(), pcmR.data() };
                        ssbus::ringWrite(*downRing_, downWritePos_, src, 2, frames);

                        std::vector<float> refMono(frames);
                        for (uint32_t i = 0; i < frames; ++i) refMono[i] = 0.5f * (pcmL[i] + pcmR[i]);
                        {
                            std::lock_guard<std::mutex> aecLk(aecMutex_);
                            aec_.pushReference(refMono.data(), frames);
                        }
                        activeCodec_ = "pcm16";
                        return;
                    }
                }
            }

            if (pSize >= 24) {
                // Fallback: HRA2 PCM16 from Hub 1.0.4
                uint32_t magic = 0, rate = 0, frames = 0;
                uint16_t chans = 0;
                uint64_t pos = 0;

                std::memcpy(&magic, pData, 4);
                std::memcpy(&rate, pData + 4, 4);
                std::memcpy(&chans, pData + 8, 2);
                std::memcpy(&frames, pData + 12, 4);
                std::memcpy(&pos, pData + 16, 8);

                if (magic == 0x32415248 && chans >= 1 && chans <= 2 && frames > 0) {
                    frames = std::min<uint32_t>(frames, uint32_t((pSize - 24) / (2u * chans)));
                    const auto* pcm = reinterpret_cast<const int16_t*>(pData + 24);

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

                    std::vector<float> refMono(frames);
                    for (uint32_t i = 0; i < frames; ++i) refMono[i] = 0.5f * (pcmL[i] + pcmR[i]);
                    {
                        std::lock_guard<std::mutex> aecLk(aecMutex_);
                        aec_.pushReference(refMono.data(), frames);
                    }
                    activeCodec_ = "pcm16";
                }
            }
        };

        // Hook up WebRTC signaling & DataChannel events
        rtc->onLocalDescription([&sendWsText, &sessionOver](const std::string& /* type */, const std::string& sdp) {
            if (sessionOver.load()) return;
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty("type", "rtc_answer");
            o->setProperty("sdp", juce::String(sdp));
            const auto json = juce::JSON::toString(juce::var(o), true);
            sendWsText(json.toStdString());
        });

        rtc->onLocalCandidate([&sendWsText, &sessionOver](const std::string& candidate, const std::string& mid) {
            if (sessionOver.load()) return;
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty("type", "rtc_candidate");
            o->setProperty("candidate", juce::String(candidate));
            o->setProperty("mid", juce::String(mid));
            const auto json = juce::JSON::toString(juce::var(o), true);
            sendWsText(json.toStdString());
        });

        rtc->onDataChannelOpen([this, &sessionOver]() {
            if (sessionOver.load()) return;
            isUdp_.store(true, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(infoMutex_);
                roomInfo_.isUdp = true;
                roomInfo_.transport = "WebRTC UDP";
            }
            if (onStateChanged) {
                RoomInfo info = roomInfo();
                juce::MessageManager::callAsync([this, info] {
                    if (onStateChanged) onStateChanged(state(), info);
                });
            }
        });

        rtc->onDataChannelClosed([this, &sessionOver]() {
            if (sessionOver.load()) return;
            isUdp_.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(infoMutex_);
                roomInfo_.isUdp = false;
                roomInfo_.transport = isOpus_.load() ? "WebSocket Opus" : "WebSocket PCM";
            }
            if (onStateChanged) {
                RoomInfo info = roomInfo();
                juce::MessageManager::callAsync([this, info] {
                    if (onStateChanged) onStateChanged(state(), info);
                });
            }
        });

        rtc->onMessage([&handleAudioPacket, &sessionOver](const uint8_t* pData, size_t pSize) {
            if (sessionOver.load()) return;
            handleAudioPacket(pData, pSize);
        });

        // Send initial handshake JSON advertising Opus, speakerMode, and WebRTC
        const std::string initJson = "{\"codec\":\"opus\",\"speakerMode\":" +
                                     std::string(speakerMode_.load() ? "true" : "false") +
                                     ",\"webrtc\":true,\"hearMyself\":false}";
        sendWsText(initJson);

        std::vector<uint8_t> rxBuffer(128 * 1024);

        // Reset sequence and read pointers
        upReadPos_ = upWritePos_.load(std::memory_order_relaxed);
        upSeq_ = 0;
        lastDownSeqValid_ = false;

        // Dedicated sender thread for upstream microphone audio
        std::thread sendThread([this, rawWs, rtc, &sendWsBinary] {
            std::vector<float> floatBuf(480);
            std::vector<float> aecCleanBuf(480);
            std::vector<uint8_t> opusPayload(1275);
            std::vector<int16_t> txPcm(480);
            std::vector<uint8_t> txPacket;

            while (!quit_.load() && wsHandle_.load() == rawWs) {
                const uint64_t upW = upWritePos_.load(std::memory_order_relaxed);
                if (upW >= upReadPos_ + 480) {
                    for (int i = 0; i < 480; ++i) {
                        floatBuf[size_t(i)] = upRing_->ch[0][(upReadPos_ + i) & (ssbus::kRingMask)];
                    }
                    upReadPos_ += 480;

                    const float* toSend = floatBuf.data();

                    // Run local AEC when in speaker mode
                    if (speakerMode_.load(std::memory_order_relaxed)) {
                        const int d = localDelaySamples_.load(std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lk(aecMutex_);
                        aec_.process(floatBuf.data(), aecCleanBuf.data(), 480, d);
                        toSend = aecCleanBuf.data();
                    }

                    const uint64_t pos = lastEchoPos_.load(std::memory_order_relaxed);

                    if (isOpus_.load(std::memory_order_relaxed)) {
                        // K2.2 Opus HRA3 transmission
                        const float* const encCh[1] = { toSend };
                        const int encBytes = opusEnc_.encode(encCh, 480, opusPayload.data(), int(opusPayload.size()));

                        if (encBytes > 0) {
                            ssnet::Hra3Header h3{};
                            h3.magic = ssnet::kMagic3;
                            h3.codec = ssnet::CodecOpus;
                            h3.chans = 1;
                            h3.flags = ssnet::FlagFec;
                            h3.seq = upSeq_++;
                            h3.pos = pos;
                            h3.frames = 480;

                            txPacket.resize(sizeof(ssnet::Hra3Header) + size_t(encBytes));
                            ssnet::writeHra3(txPacket.data(), h3, opusPayload.data(), size_t(encBytes));

                            // K5: Prefer WebRTC DataChannel (UDP), fallback to WebSocket
                            bool sentUdp = false;
                            if (rtc && rtc->isOpen()) {
                                sentUdp = rtc->send(txPacket.data(), txPacket.size());
                            }
                            if (!sentUdp) {
                                if (!sendWsBinary(txPacket.data(), txPacket.size())) break;
                            }
                        }
                    } else {
                        // Fallback: HRA2 PCM16 transmission (compatible with Hub 1.0.4)
                        const uint32_t magic = 0x32415248; // "HRA2"
                        const uint32_t rate = 48000;
                        const uint16_t chans = 1;
                        const uint16_t latencyKnown = 1;
                        const uint32_t frames = 480;

                        txPacket.resize(24 + 480 * sizeof(int16_t));
                        std::memcpy(txPacket.data(), &magic, 4);
                        std::memcpy(txPacket.data() + 4, &rate, 4);
                        std::memcpy(txPacket.data() + 8, &chans, 2);
                        std::memcpy(txPacket.data() + 10, &latencyKnown, 2);
                        std::memcpy(txPacket.data() + 12, &frames, 4);
                        std::memcpy(txPacket.data() + 16, &pos, 8);

                        for (int i = 0; i < 480; ++i) {
                            const float s = std::clamp(toSend[i], -1.0f, 1.0f);
                            txPcm[size_t(i)] = int16_t(std::round(s * 32767.0f));
                        }
                        std::memcpy(txPacket.data() + 24, txPcm.data(), 480 * sizeof(int16_t));

                        bool sentUdp = false;
                        if (rtc && rtc->isOpen()) {
                            sentUdp = rtc->send(txPacket.data(), txPacket.size());
                        }
                        if (!sentUdp) {
                            if (!sendWsBinary(txPacket.data(), txPacket.size())) break;
                        }
                    }
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
                        // K5: WebRTC signaling handling (offer / candidate)
                        if (obj->hasProperty("type")) {
                            const juce::String msgType = obj->getProperty("type").toString();
                            if (msgType == "rtc_offer" && rtc && obj->hasProperty("sdp")) {
                                const std::string sdp = obj->getProperty("sdp").toString().toStdString();
                                rtc->setRemoteDescription("offer", sdp);
                            } else if (msgType == "rtc_candidate" && rtc && obj->hasProperty("candidate")) {
                                const std::string cand = obj->getProperty("candidate").toString().toStdString();
                                const std::string mid = obj->hasProperty("mid") ? obj->getProperty("mid").toString().toStdString() : "";
                                rtc->addRemoteCandidate(cand, mid);
                            }
                        }

                        if (obj->hasProperty("v") && int(obj->getProperty("v")) >= 2) {
                            // Codec negotiation with Hub
                            bool supportsOpus = false;
                            if (obj->hasProperty("codecs")) {
                                if (auto* arr = obj->getProperty("codecs").getArray()) {
                                    for (const auto& c : *arr) {
                                        if (c.toString().equalsIgnoreCase("opus")) {
                                            supportsOpus = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            isOpus_.store(supportsOpus, std::memory_order_relaxed);

                            {
                                std::lock_guard<std::mutex> lock(infoMutex_);
                                roomInfo_.name = obj->getProperty("name").toString();
                                roomInfo_.host = obj->getProperty("host").toString();
                                activeCodec_ = supportsOpus ? "opus" : "pcm16";
                                roomInfo_.codec = activeCodec_;
                                roomInfo_.isUdp = isUdp_.load(std::memory_order_relaxed);
                                roomInfo_.transport = roomInfo_.isUdp ? "WebRTC UDP" : (supportsOpus ? "WebSocket Opus" : "WebSocket PCM");
                            }
                            setState(ConnectionState::Live);
                        }

                        if (obj->hasProperty("ping")) {
                            const int pingVal = int(obj->getProperty("ping"));
                            const std::string pongJson = "{\"pong\":" + std::to_string(pingVal) + "}";
                            sendWsText(pongJson);
                        }
                        if (obj->hasProperty("delay")) {
                            std::lock_guard<std::mutex> lock(infoMutex_);
                            roomInfo_.delayMs = float(double(obj->getProperty("delay")));
                        }
                    }
                }
                have = 0;
            } else if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && have >= 22) {
                // Downstream audio via WebSocket fallback
                handleAudioPacket(rxBuffer.data(), have);
                have = 0;
            }
        }

        sessionOver.store(true);
        if (rtc) {
            rtc->close();
        }
        {
            std::lock_guard<std::mutex> lk(rtcMutex_);
            if (rtcPeer_ == rtc) rtcPeer_.reset();
        }
        isUdp_.store(false, std::memory_order_relaxed);

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
