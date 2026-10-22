#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "ShareServer.h"

#include "BinaryData_web.h"
#include "ssbus/paced_send.h"
#include "ssnet/wire.h"
#include "ssnet/opus_codec.h"

#ifdef _WIN32
#include <winsock2.h>   // setsockopt, send, WSAPoll: after juce_core (as everywhere), before windows.h
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

#ifndef _WIN32
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <regex>

namespace hearaside {

namespace {

constexpr uint32_t kMagic = 0x31415248;   // "HRA1" little-endian: 16-byte header (magic, rate, channels, 0, frames)
constexpr uint32_t kMagic2 = 0x32415248;  // "HRA2": 24-byte header, plus a u64 position on the Stream Mix timeline (S3)
constexpr int kFirstPort = 47810;
constexpr size_t kMaxConns = 40;   // plenty for a few listeners
constexpr int kMaxListeners = int(kMaxConns) - ssbus::kMaxFriends - 2;   // listeners never take the friends' (and the send-in link's) places
constexpr long kMaxPerPeer = 6;    // one LAN device: a few pages + their WebSockets
constexpr long kMaxPending = 24;   // connections that have not sent a whole request head yet: the rest of the places stay free
// A slow line must not make the music later and later: what the socket cannot take is not queued, the Hub jumps ahead.
constexpr int kSendBufferBytes = 64 * 1024;   // the kernel's queue per connection (Windows would grow it to MBs; Linux doubles this). Less caps a 100 ms line below PCM stereo
constexpr size_t kMaxControlQueue = 1024;     // bytes of pongs that may wait behind a stuck line
constexpr uint32_t kFriendMaxLagMs = 150;     // friend: the page buffers ~60 ms itself, so no more than this waits at the Hub
constexpr uint32_t kListenMaxLagMs = 300;     // listener: the page may buffer up to 0.8 s itself

// ---- SHA-1 (WebSocket handshake only) ---------------------------------------------------------
std::array<uint8_t, 20> sha1(const std::string& msg) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::string m = msg;
    const uint64_t bits = uint64_t(msg.size()) * 8;
    m.push_back(static_cast<char>(-128));   // 0x80
    while (m.size() % 64 != 56) m += char(0);
    for (int i = 7; i >= 0; --i) m += char((bits >> (i * 8)) & 0xFF);
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(uint8_t(m[off + size_t(i) * 4])) << 24 | uint32_t(uint8_t(m[off + size_t(i) * 4 + 1])) << 16
                 | uint32_t(uint8_t(m[off + size_t(i) * 4 + 2])) << 8 | uint32_t(uint8_t(m[off + size_t(i) * 4 + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> out {};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[size_t(i * 4 + j)] = uint8_t(h[i] >> (24 - j * 8));
    return out;
}

// ---- socket helpers ---------------------------------------------------------------------------
// One send on the raw socket (JUCE's write cannot tell "full" from "gone", and the socket is non-blocking
// after the first read): bytes taken, 0 = full right now, < 0 = the connection is gone.
int trySend(juce::StreamingSocket& s, const void* data, size_t n) {
    const int h = s.getRawSocketHandle();
    if (h < 0) return -1;
    const int len = int(std::min<size_t>(n, 1 << 20));
#ifdef _WIN32
    const int w = ::send(static_cast<SOCKET>(h), static_cast<const char*>(data), len, 0);
    return w >= 0 ? w : (WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1);
#else
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const auto w = ::send(h, data, size_t(len), flags);
    return w >= 0 ? int(w) : (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1);
#endif
}

// Waits until the socket may take more (or the time is up). Own poll: JUCE's waitUntilReady holds the lock the
// reading thread of a friend lives in.
void waitWritable(juce::StreamingSocket& s, int ms) {
    const int h = s.getRawSocketHandle();
    if (h < 0) return;
#ifdef _WIN32
    WSAPOLLFD p { static_cast<SOCKET>(h), POLLWRNORM, 0 };
    WSAPoll(&p, 1, ms);
#else
    pollfd p { h, POLLOUT, 0 };
    ::poll(&p, 1, ms);
#endif
}

// Everything, or false after 5 s without the socket taking a byte (a page, a short frame: not for audio).
bool writeAll(juce::StreamingSocket& s, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    int stalled = 0;
    while (n > 0) {
        const int w = trySend(s, p, n);
        if (w < 0) return false;
        if (w == 0) { if (++stalled > 50) return false; waitWritable(s, 100); continue; }
        stalled = 0;
        p += w;
        n -= size_t(w);
    }
    return true;
}

// Reads exactly n bytes; gives up after timeoutMs without progress or when `alive` turns false.
bool readExact(juce::StreamingSocket& s, void* dst, size_t n, int timeoutMs, const std::atomic<bool>& alive) {
    char* p = static_cast<char*>(dst);
    int waited = 0;
    while (n > 0) {
        if (!alive.load()) return false;
        const int r = s.waitUntilReady(true, 100);
        if (r < 0) return false;
        if (r == 0) { if ((waited += 100) >= timeoutMs) return false; continue; }
        const int got = s.read(p, int(n), false);
        if (got <= 0) return false;
        p += got;
        n -= size_t(got);
        waited = 0;
    }
    return true;
}

// A WebSocket frame (server to browser: not masked) in one piece.
void frameInto(std::vector<uint8_t>& out, uint8_t opcode, const void* data, size_t n) {
    uint8_t h[10];
    size_t hn = 2;
    h[0] = uint8_t(0x80 | opcode);
    if (n < 126) h[1] = uint8_t(n);
    else if (n < 65536) { h[1] = 126; h[2] = uint8_t(n >> 8); h[3] = uint8_t(n); hn = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; ++i) h[2 + i] = uint8_t(uint64_t(n) >> (56 - i * 8)); hn = 10; }
    out.resize(hn + n);
    std::memcpy(out.data(), h, hn);
    if (n) std::memcpy(out.data() + hn, data, n);
}

// Header and payload leave in one write (two tiny segments per packet otherwise).
bool sendFrame(juce::StreamingSocket& s, uint8_t opcode, const void* data, size_t n) {
    thread_local std::vector<uint8_t> buf;
    frameInto(buf, opcode, data, n);
    return writeAll(s, buf.data(), buf.size());
}

// A small kernel queue per connection; a failure is not an error.
void limitSendBuffer(juce::StreamingSocket& s) {
    const int h = s.getRawSocketHandle();
    if (h < 0) return;
    const int bytes = kSendBufferBytes;
#ifdef _WIN32
    setsockopt(static_cast<SOCKET>(h), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), int(sizeof bytes));
#else
    setsockopt(h, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
#endif
}

// Reads one whole message (joins fragments). opcode 0 = timeout / error.
uint8_t readMessage(juce::StreamingSocket& s, std::vector<uint8_t>& out, int timeoutMs, const std::atomic<bool>& alive,
                    const std::function<void(const uint8_t*, size_t)>& onPing = {}) {
    out.clear();
    uint8_t first = 0;
    for (;;) {
        uint8_t h[2];
        if (!readExact(s, h, 2, timeoutMs, alive)) return 0;
        const uint8_t op = h[0] & 0x0F;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) { uint8_t e[2]; if (!readExact(s, e, 2, timeoutMs, alive)) return 0; len = uint64_t(e[0]) << 8 | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!readExact(s, e, 8, timeoutMs, alive)) return 0; len = 0; for (uint8_t b : e) len = len << 8 | b; }
        if (op >= 8 && len > 125) return 0;   // RFC 6455 5.5: a control frame is 125 bytes at most (a 4 MB ping must not be answered, again and again)
        if (len > (1u << 22) || out.size() + len > (1u << 22)) return 0;   // whole message, not per fragment: endless fragments must not fill the DAW's memory
        uint8_t mask[4] = { 0, 0, 0, 0 };
        const bool masked = (h[1] & 0x80) != 0;
        if (masked && !readExact(s, mask, 4, timeoutMs, alive)) return 0;
        const size_t at = out.size();
        out.resize(at + size_t(len));
        if (len > 0 && !readExact(s, out.data() + at, size_t(len), timeoutMs, alive)) return 0;
        if (masked) for (size_t i = 0; i < size_t(len); ++i) out[at + i] ^= mask[i & 3];
        if (op >= 8) {   // control frames may arrive between fragments
            if (op == 9) { if (onPing) onPing(out.data() + at, size_t(len)); else sendFrame(s, 0xA, out.data() + at, size_t(len)); }   // onPing: this socket has a writer thread
            if (op == 8) return 8;
            out.resize(at);
            continue;
        }
        if (op != 0) first = op;
        if (h[0] & 0x80) return first;
    }
}

void respond(juce::StreamingSocket& s, int code, const char* type, const void* body, size_t n) {
    const auto head = juce::String("HTTP/1.1 ") + juce::String(code) + (code == 200 ? " OK" : code == 403 ? " Forbidden" : code == 503 ? " Service Unavailable" : " Not Found")
                    + "\r\nContent-Type: " + type + "\r\nContent-Length: " + juce::String(int64_t(n))
                    + "\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    writeAll(s, head.toRawUTF8(), head.getNumBytesAsUTF8());
    if (n) writeAll(s, body, n);
}

} // namespace

// cloudflared runs inside a job object that is closed (and the process killed) when the DAW's
// process ends for any reason, so a crashed DAW never leaves a tunnel running.
// It inherits only its output pipe: with every inheritable handle of the DAW it would also hold
// our listening socket, closing the listener would then not wake accept(), and stop() would hang
// the DAW's message thread (seen in Studio One while closing a song).
struct ShareServer::Tunnelled {
#ifdef _WIN32
    HANDLE job = nullptr, process = nullptr, out = nullptr;

    bool start(const juce::String& exe, int port) {
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim {};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) return false;
        SECURITY_ATTRIBUTES sa { sizeof sa, nullptr, TRUE };
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&out, &writeEnd, &sa, 0)) return false;
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<char> attrMem(attrSize);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrMem.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) { CloseHandle(writeEnd); return false; }
        HANDLE inherit[] = { writeEnd };
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
        STARTUPINFOEXW si {};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = writeEnd;
        si.StartupInfo.hStdInput = nullptr;
        si.lpAttributeList = attrs;
        PROCESS_INFORMATION pi {};
        const juce::String cmd = "\"" + exe + "\" tunnel --no-autoupdate --url http://127.0.0.1:" + juce::String(port);
        std::wstring line(cmd.toWideCharPointer());
        const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                       nullptr, nullptr, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(writeEnd);
        if (!ok) return false;
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        return true;
    }
    int read(char* buf, int n) {
        DWORD got = 0;
        return ReadFile(out, buf, DWORD(n), &got, nullptr) ? int(got) : 0;
    }
    void kill() { if (process) TerminateProcess(process, 0); }
    ~Tunnelled() {
        kill();
        for (HANDLE h : { process, out, job }) if (h) CloseHandle(h);
    }
#else
    juce::ChildProcess proc;
    bool start(const juce::String& exe, int port) {
        return proc.start(juce::StringArray { exe, "tunnel", "--no-autoupdate", "--url", "http://127.0.0.1:" + juce::String(port) });
    }
    int read(char* buf, int n) { return proc.readProcessOutput(buf, n); }
    void kill() { proc.kill(); }
#endif
};

struct ShareServer::Conn {
    std::unique_ptr<juce::StreamingSocket> sock;
    std::thread thread;
    std::atomic<bool> done { false };
    juce::String peer;   // the client's address (127.0.0.1 for everything that comes through the tunnel)
    juce::String client; // who is really asking: Cloudflare's Cf-Connecting-Ip behind the tunnel, else peer
    std::atomic<bool> headDone { false };
};

ShareServer::ShareServer() = default;

ShareServer::~ShareServer() { stop(); }

bool ShareServer::secureRandom(void* dst, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(dst), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
    arc4random_buf(dst, n);
    return true;
#else
    return getrandom(dst, n, 0) == ssize_t(n);
#endif
}

juce::String ShareServer::newToken() {
    // links live on the internet for months now (permanent links): 26 x 5 bits, never juce::Random
    uint8_t bytes[26];
    if (!secureRandom(bytes, sizeof bytes)) return {};
    const char* abc = "abcdefghijkmnpqrstuvwxyz23456789";   // 32 letters, no 0/o/1/l
    juce::String t;
    for (uint8_t b : bytes) t << juce::String::charToString(juce::juce_wchar(abc[b & 31]));
    return t;
}

juce::String ShareServer::newSecret() {
    uint8_t bytes[32];
    if (!secureRandom(bytes, sizeof bytes)) return {};
    return juce::String::toHexString(bytes, int(sizeof bytes), 0);
}

void ShareServer::setAllowedOrigin(const juce::String& origin) {
    const std::lock_guard<std::mutex> lock(urlMutex_);
    allowedOrigin_ = origin.toLowerCase();
}

bool ShareServer::originAllowed(const juce::String& origin) const {
    const auto o = origin.trim().toLowerCase();
    if (o.isEmpty()) return true;   // not a browser page (App Audio receiving a link, tools)
    const auto port = ":" + juce::String(port_);
    if (o == "http://127.0.0.1" + port || o == "http://localhost" + port || o == lanBase().toLowerCase()) return true;
    const std::lock_guard<std::mutex> lock(urlMutex_);
    return (publicBase_.isNotEmpty() && o == publicBase_.toLowerCase()) || (allowedOrigin_.isNotEmpty() && o == allowedOrigin_);
}

juce::File ShareServer::findCloudflared() {
    juce::StringArray dirs;
    // the copy the installer puts next to the plug-ins comes first: links work with nothing else installed
    for (const char* v : { "ProgramFiles", "ProgramW6432" })
        dirs.add(juce::SystemStats::getEnvironmentVariable(v, {}) + "\\HEARASIDE\\cloudflared");
    dirs.addTokens(juce::SystemStats::getEnvironmentVariable("PATH", {}), ";", "\"");
    for (const char* v : { "ProgramFiles", "ProgramFiles(x86)" })
        dirs.add(juce::SystemStats::getEnvironmentVariable(v, {}) + "\\cloudflared");
    const auto local = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});
    dirs.add(local + "\\Microsoft\\WinGet\\Links");
    dirs.add(local + "\\HEARASIDE");
    for (const auto& d : dirs) {
        if (!juce::File::isAbsolutePath(d.trim())) continue;   // empty or relative PATH entries (".")
        const juce::File f = juce::File(d.trim()).getChildFile("cloudflared.exe");
        if (f.existsAsFile()) return f;
    }
    return {};
}

void ShareServer::ignoreBrokenPipes() {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);   // a failed write then returns an error instead
#endif
}

bool ShareServer::start(ssbus::BusLayout* bus, const juce::String& listenToken, const juce::String& sendToken) {
    stop();
    ignoreBrokenPipes();
    if (bus == nullptr) return false;
    bus_ = bus;
    setListenToken(listenToken);
    sendToken_ = sendToken;
    listener_ = std::make_unique<juce::StreamingSocket>();
    port_ = 0;
    for (int p = kFirstPort; p < kFirstPort + 20 && port_ == 0; ++p)
        if (listener_->createListener(p)) port_ = p;
    if (port_ == 0) { listener_.reset(); return false; }
#ifdef _WIN32
    // a process the DAW starts must not keep the listener open after we close it
    SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<intptr_t>(listener_->getRawSocketHandle())), HANDLE_FLAG_INHERIT, 0);
#endif
    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });

    if (juce::SystemStats::getEnvironmentVariable("HEARASIDE_NO_TUNNEL", {}).isNotEmpty()) {
        tunnel_.store(Tunnel::Off);   // tests: local links only
    } else if (const auto exe = findCloudflared(); !exe.existsAsFile()) {
        tunnel_.store(Tunnel::Missing);
    } else {
        cloudflared_ = std::make_unique<Tunnelled>();
        if (cloudflared_->start(exe.getFullPathName(), port_)) {
            tunnelStartMs_.store(juce::Time::getMillisecondCounter());
            tunnel_.store(Tunnel::Starting);
            tunnelThread_ = std::thread([this] { tunnelLoop(); });
        } else {
            cloudflared_.reset();
            tunnel_.store(Tunnel::Failed);
        }
    }
    return true;
}

void ShareServer::stop() {
    if (!running_.exchange(false)) return;
    if (cloudflared_) cloudflared_->kill();   // first: nothing below may wait on the tunnel
    if (listener_) listener_->close();
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        const std::lock_guard<std::mutex> lock(connsMutex_);
        for (auto& c : conns_) c->sock->close();
    }
    for (auto& c : conns_) if (c->thread.joinable()) c->thread.join();
    conns_.clear();
    listener_.reset();
    if (tunnelThread_.joinable()) tunnelThread_.join();
    cloudflared_.reset();
    {
        const std::lock_guard<std::mutex> lock(urlMutex_);
        publicBase_ = {};
    }
    tunnel_.store(Tunnel::Off);
    if (bus_) {
        bus_->linkIn.active.store(0);
        for (auto& f : bus_->friends) if (f.state.load() == ssbus::kFriendLive) f.state.store(ssbus::kFriendOffline);
    }
    listeners_.store(0);
    senders_.store(0);
    friendsLive_.store(0);
}

juce::String ShareServer::lanBase() const {
    juce::String best = "127.0.0.1";
    for (const auto& a : juce::IPAddress::getAllAddresses(false)) {
        const auto s = a.toString();
        if (s.startsWith("192.168.") || s.startsWith("10.") || s.startsWith("172.")) { best = s; break; }
        if (!s.startsWith("127.") && !s.startsWith("169.254.") && best == "127.0.0.1") best = s;
    }
    return "http://" + best + ":" + juce::String(port_);
}

juce::String ShareServer::publicBase() const {
    const std::lock_guard<std::mutex> lock(urlMutex_);
    return publicBase_;
}

void ShareServer::setListenToken(const juce::String& token) {
    const std::lock_guard<std::mutex> lock(urlMutex_);
    listenToken_ = token;
}

juce::String ShareServer::listenToken() const {
    const std::lock_guard<std::mutex> lock(urlMutex_);
    return listenToken_;
}

juce::String ShareServer::linkBase() const {
    const auto p = publicBase();
    if (p.isNotEmpty()) return p;
    if (tunnel_.load() == Tunnel::Starting && juce::Time::getMillisecondCounter() - tunnelStartMs_.load() < kTunnelWaitMs) return {};
    return lanBase();
}

juce::String ShareServer::listenUrl() const {
    const auto b = linkBase();
    return b.isEmpty() ? juce::String() : b + "/l/" + listenToken();
}

juce::String ShareServer::localListenUrl() const {
    return "http://127.0.0.1:" + juce::String(port_) + "/l/" + listenToken();
}

juce::String ShareServer::sendUrl() const {
    const auto b = linkBase();
    return b.isEmpty() ? juce::String() : b + "/s/" + sendToken_;
}

juce::String ShareServer::friendUrl(const juce::String& token) const {
    const auto b = linkBase();
    return b.isEmpty() ? juce::String() : b + "/s/" + token;
}

void ShareServer::setFriendLinks(std::vector<FriendLink> links) {
    const std::lock_guard<std::mutex> lock(friendsMutex_);
    friendLinks_ = std::move(links);
}

void ShareServer::kickFriend(int slot) {
    if (slot >= 0 && slot < ssbus::kMaxFriends) friendKick_[size_t(slot)].store(true);
}

void ShareServer::remeasureFriend(int slot) {
    if (slot >= 0 && slot < ssbus::kMaxFriends) friendRemeasure_[size_t(slot)].store(true);
}

ShareServer::FriendStats ShareServer::friendStats(int slot) const {
    FriendStats r;
    if (slot < 0 || slot >= ssbus::kMaxFriends) return r;
    const auto& n = friendNet_[size_t(slot)];
    r.rttMs = n.rtt.load(std::memory_order_relaxed);
    r.upKbps = n.up.load(std::memory_order_relaxed);
    r.downKbps = n.down.load(std::memory_order_relaxed);
    r.micGapMaxMs = n.micGap.load(std::memory_order_relaxed);
    r.downLagMaxMs = n.lag.load(std::memory_order_relaxed);
    r.downSkips = n.skips.load(std::memory_order_relaxed);
    return r;
}

int ShareServer::friendSlotFor(const juce::String& token, uint32_t* id, juce::String* name, juce::String* host) const {
    if (token.length() < 20) return -1;
    const std::lock_guard<std::mutex> lock(friendsMutex_);
    for (const auto& f : friendLinks_) {
        if (f.token != token) continue;
        if (id) *id = f.id;
        if (name) *name = f.name;
        if (host) *host = f.host;
        return f.slot;
    }
    return -1;
}

void ShareServer::reap() {
    const std::lock_guard<std::mutex> lock(connsMutex_);
    for (auto it = conns_.begin(); it != conns_.end();) {
        if ((*it)->done.load()) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

void ShareServer::acceptLoop() {
    while (running_.load()) {
        std::unique_ptr<juce::StreamingSocket> s(listener_->waitForNextConnection());
        if (!s) { if (!running_.load()) break; juce::Thread::sleep(20); continue; }
        reap();
        auto c = std::make_unique<Conn>();
        c->sock = std::move(s);
        c->peer = c->sock->getHostName();
        Conn* raw = c.get();
        const std::lock_guard<std::mutex> lock(connsMutex_);
        if (conns_.size() >= kMaxConns) { raw->sock->close(); continue; }
        // one LAN device may not take every place; the tunnel's connections all come from 127.0.0.1
        const bool viaTunnel = raw->peer.startsWith("127.") || raw->peer == "::1";
        if (!viaTunnel && std::count_if(conns_.begin(), conns_.end(), [raw](const auto& o) { return !o->done.load() && o->peer == raw->peer; }) >= kMaxPerPeer) {
            raw->sock->close();
            continue;
        }
        // endless half-sent requests must not use up every place (the tunnel hides who sends them)
        if (std::count_if(conns_.begin(), conns_.end(), [](const auto& o) { return !o->done.load() && !o->headDone.load(); }) >= kMaxPending) {
            raw->sock->close();
            continue;
        }
        raw->client = raw->peer;
        conns_.push_back(std::move(c));
        raw->thread = std::thread([this, raw] { serve(*raw); raw->sock->close(); raw->done.store(true); });
    }
}

void ShareServer::serve(Conn& c) {
    auto& s = *c.sock;
    // request head
    std::string head;
    char ch = 0;
    const auto headStart = juce::Time::getMillisecondCounter();
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
        // 5 s for the whole head: one byte now and then must not hold a connection for hours
        if (juce::Time::getMillisecondCounter() - headStart > 5000 || !readExact(s, &ch, 1, 5000, running_)) return;
        head += ch;
    }
    const auto lines = juce::StringArray::fromLines(juce::String::fromUTF8(head.c_str()));
    const auto first = juce::StringArray::fromTokens(lines[0], " ", "");
    if (first.size() < 2 || first[0] != "GET") { respond(s, 404, "text/plain", "", 0); return; }
    const auto path = first[1].upToFirstOccurrenceOf("?", false, false);
    juce::String key, origin, via;
    for (const auto& l : lines) {
        if (l.startsWithIgnoreCase("Cf-Connecting-Ip:")) via = l.fromFirstOccurrenceOf(":", false, false).trim();
        if (l.startsWithIgnoreCase("Sec-WebSocket-Key:")) key = l.fromFirstOccurrenceOf(":", false, false).trim();
        if (l.startsWithIgnoreCase("Origin:")) origin = l.fromFirstOccurrenceOf(":", false, false).trim();
    }
    c.headDone.store(true);
    if (c.peer.startsWith("127.") || c.peer == "::1") {   // through the tunnel: one visitor may not take every place either
        if (via.isNotEmpty() && via.length() <= 45) {
            const std::lock_guard<std::mutex> lock(connsMutex_);
            c.client = via;
            if (std::count_if(conns_.begin(), conns_.end(), [&c](const auto& o) { return !o->done.load() && o->headDone.load() && o->client == c.client; }) > kMaxPerPeer) {
                static const char busy[] = "HEARASIDE: too many connections from your address.";
                respond(s, 503, "text/plain; charset=utf-8", busy, sizeof(busy) - 1);
                return;
            }
        }
    }
    // a page from another site must not open the audio (or the mic input) behind the user's back
    if (key.isNotEmpty() && !originAllowed(origin)) {
        static const char forbidden[] = "HEARASIDE: this page may not connect.";
        respond(s, 403, "text/plain; charset=utf-8", forbidden, sizeof(forbidden) - 1);
        return;
    }

    auto upgrade = [&] {
        const auto digest = sha1(key.toStdString() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        const auto accept = juce::Base64::toBase64(digest.data(), digest.size());
        const auto resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                        + accept + "\r\n\r\n";
        return writeAll(s, resp.toRawUTF8(), resp.getNumBytesAsUTF8());
    };

    // friends: one link each (and a tiny JSON for the page: the friend's name, who invited them)
    if (path.startsWith("/s/") || path.startsWith("/ws/s/") || path.startsWith("/info/s/")) {
        const auto tok = path.fromLastOccurrenceOf("/", false, false);
        if (tok != sendToken_) {
            uint32_t fid = 0;
            juce::String fname, fhost;
            const int slot = friendSlotFor(tok, &fid, &fname, &fhost);
            if (slot >= 0) {
                if (path.startsWith("/s/")) { respond(s, 200, "text/html; charset=utf-8", HearasideWeb::send_html, size_t(HearasideWeb::send_htmlSize)); return; }
                if (path.startsWith("/info/s/")) {
                    auto* o = new juce::DynamicObject();
                    o->setProperty("name", fname);
                    o->setProperty("host", fhost);
                    const auto json = juce::JSON::toString(juce::var(o), true);
                    respond(s, 200, "application/json; charset=utf-8", json.toRawUTF8(), json.getNumBytesAsUTF8());
                    return;
                }
                if (key.isNotEmpty()) { if (upgrade()) friendSession(c, slot, fid, tok); return; }
            }
        }
    }
    if (!listenEnabled_.load() && (path.startsWith("/l/") || path.startsWith("/ws/l/"))) {
        static const char off[] = "HEARASIDE: this link is not valid (any more).";
        respond(s, 404, "text/plain; charset=utf-8", off, sizeof(off) - 1);
        return;
    }
    const auto lt = listenToken();
    if (path == "/l/" + lt) { respond(s, 200, "text/html; charset=utf-8", HearasideWeb::listen_html, size_t(HearasideWeb::listen_htmlSize)); return; }
    if (path == "/s/" + sendToken_)   { respond(s, 200, "text/html; charset=utf-8", HearasideWeb::send_html, size_t(HearasideWeb::send_htmlSize)); return; }
    if (path == "/ws/l/" + lt && key.isNotEmpty()) {
        if (listeners_.load() >= kMaxListeners) {
            static const char busy[] = "HEARASIDE: too many listeners right now, try again later.";
            respond(s, 503, "text/plain; charset=utf-8", busy, sizeof(busy) - 1);
            return;
        }
        if (upgrade()) streamTo(c);
        return;
    }
    if (path == "/ws/s/" + sendToken_ && key.isNotEmpty())   { if (upgrade()) receiveFrom(c); return; }
    static const char notFound[] = "HEARASIDE: this link is not valid (any more).";
    respond(s, 404, "text/plain; charset=utf-8", notFound, sizeof(notFound) - 1);
}

// Sends the Stream Mix (Hub output 0) as it is written, in ~10 ms packets. A line that cannot keep up gets
// no queue: the packet that waits is the only one, and the newest audio is sent when it is out (see SendQueue).
void ShareServer::streamTo(Conn& c) {
    auto& s = *c.sock;
    auto& sh = bus_->streamHeader;
    ++listeners_;
    limitSendBuffer(s);
    std::vector<float> l(4096), r(4096);
    std::vector<uint8_t> packet, incoming;
    ssbus::SendQueue out;
    const auto sendNow = [&s](const uint8_t* p, size_t n) { return trySend(s, p, n); };
    uint64_t cursor = sh.writePos[0].load();
    int idle = 0;

    static const char kListenHello[] = "{\"v\":3,\"codecs\":[\"opus\",\"pcm16\"]}";
    frameInto(packet, 1, kListenHello, sizeof(kListenHello) - 1);
    out.append(packet.data(), packet.size());

    uint8_t codec = ssnet::CodecPcm16;
    int bitrate = 128000;
    ssnet::OpusEnc opusEnc;
    bool opusReady = false;
    uint32_t seq = 0;

    while (running_.load()) {
        if (s.waitUntilReady(true, out.idle() ? 10 : 2) == 1) {   // the browser closed or pinged
            const uint8_t op = readMessage(s, incoming, 1000, running_, [&out](const uint8_t* p, size_t n) {
                std::vector<uint8_t> pong;
                frameInto(pong, 0xA, p, n);
                out.appendIfRoom(pong.data(), pong.size(), kMaxControlQueue);
            });
            if (op == 8 || op == 0) break;
            if (op == 1) {
                const juce::String text(reinterpret_cast<const char*>(incoming.data()), incoming.size());
                const auto j = juce::JSON::parse(text);
                if (j.hasProperty("codec")) {
                    const auto cName = j["codec"].toString();
                    if (cName == "opus") {
                        codec = ssnet::CodecOpus;
                        if (j.hasProperty("bitrate")) {
                            bitrate = std::clamp(int(j["bitrate"]), 32000, 320000);
                            if (opusReady) opusEnc.setBitrate(bitrate);
                        }
                    } else if (cName == "pcm16") {
                        codec = ssnet::CodecPcm16;
                    }
                }
            }
        }
        if (!out.flush(sendNow)) break;
        const uint32_t rate = std::max<uint32_t>(8000, sh.sampleRate.load());
        const uint64_t w = sh.writePos[0].load(std::memory_order_acquire);
        if (sh.active.load() == 0 || w == cursor) {
            cursor = w;
            if (++idle % 100 == 0) {   // keep the connection (and the tunnel) alive while the DAW is quiet
                uint8_t hdr[16] = {};
                std::memcpy(hdr, &kMagic, 4);
                std::memcpy(hdr + 4, &rate, 4);
                frameInto(packet, 2, hdr, sizeof hdr);
                out.offer(packet.data(), packet.size());
            }
            continue;
        }
        idle = 0;
        if (!out.idle()) {   // the line is slower than the music: do not build what cannot go out now
            cursor = ssbus::catchUp(cursor, w, rate, kListenMaxLagMs, 100);
            continue;
        }
        if (w - cursor > uint64_t(rate) * 2) cursor = w - rate / 10;   // fell far behind: jump to now

        if (codec == ssnet::CodecOpus && !opusReady) {
            if (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) {
                if (opusEnc.init(int(rate), 2, ssnet::OpusEnc::App::Audio)) {
                    opusEnc.setBitrate(bitrate);
                    opusEnc.setComplexity(5);
                    opusReady = true;
                }
            }
        }
        const bool useOpus = (codec == ssnet::CodecOpus && opusReady);
        const uint32_t step = rate / 100;
        const uint32_t n = useOpus ? step : uint32_t(std::min<uint64_t>(w - cursor, l.size()));
        if (!useOpus && n < rate / 200) continue;   // wait for ~5 ms of audio
        if (useOpus && w - cursor < step) continue;

        float* dst[2] = { l.data(), r.data() };
        if (ssbus::ringReadAt(bus_->streamAudio[0], sh.writePos[0], cursor, dst, n) == ssbus::ReadResult::Overrun) {
            cursor = sh.writePos[0].load() - rate / 10;
            continue;
        }
        cursor += n;

        if (useOpus) {
            const float* const encCh[2] = { l.data(), r.data() };
            uint8_t opusPayload[1275];
            const int encodedBytes = opusEnc.encode(encCh, int(n), opusPayload, sizeof(opusPayload));
            if (encodedBytes > 0) {
                packet.resize(sizeof(ssnet::Hra3Header) + size_t(encodedBytes));
                ssnet::Hra3Header h3{};
                h3.magic = ssnet::kMagic3;
                h3.codec = ssnet::CodecOpus;
                h3.chans = 2;
                h3.flags = 0;
                h3.seq = seq++;
                h3.pos = cursor;
                h3.frames = uint16_t(n);
                ssnet::writeHra3(packet.data(), h3, opusPayload, size_t(encodedBytes));
            } else {
                packet.resize(16 + size_t(n) * 4);
                const uint16_t chans = 2;
                std::memcpy(packet.data(), &kMagic, 4);
                std::memcpy(packet.data() + 4, &rate, 4);
                std::memcpy(packet.data() + 8, &chans, 2);
                std::memset(packet.data() + 10, 0, 2);
                std::memcpy(packet.data() + 12, &n, 4);
                auto* pcm = reinterpret_cast<int16_t*>(packet.data() + 16);
                for (uint32_t i = 0; i < n; ++i) {
                    pcm[2 * i]     = int16_t(std::lround(std::clamp(l[i], -1.0f, 1.0f) * 32767.0f));
                    pcm[2 * i + 1] = int16_t(std::lround(std::clamp(r[i], -1.0f, 1.0f) * 32767.0f));
                }
            }
        } else {
            packet.resize(16 + size_t(n) * 4);
            const uint16_t chans = 2;
            std::memcpy(packet.data(), &kMagic, 4);
            std::memcpy(packet.data() + 4, &rate, 4);
            std::memcpy(packet.data() + 8, &chans, 2);
            std::memset(packet.data() + 10, 0, 2);
            std::memcpy(packet.data() + 12, &n, 4);
            auto* pcm = reinterpret_cast<int16_t*>(packet.data() + 16);
            for (uint32_t i = 0; i < n; ++i) {
                pcm[2 * i]     = int16_t(std::lround(std::clamp(l[i], -1.0f, 1.0f) * 32767.0f));
                pcm[2 * i + 1] = int16_t(std::lround(std::clamp(r[i], -1.0f, 1.0f) * 32767.0f));
            }
        }
        thread_local std::vector<uint8_t> framed;
        frameInto(framed, 2, packet.data(), packet.size());
        out.offer(framed.data(), framed.size());
        if (!out.flush(sendNow)) break;
    }
    --listeners_;
}

// Writes the sender's microphone into the bus for App Audio ("*link*"). The newest sender wins.
void ShareServer::receiveFrom(Conn& c) {
    auto& s = *c.sock;
    const uint32_t gen = ++senderGen_;
    ++senders_;
    auto& li = bus_->linkIn;
    std::vector<uint8_t> msg;
    std::vector<float> l, r;
    while (running_.load() && senderGen_.load() == gen) {
        const uint8_t op = readMessage(s, msg, 5000, running_);
        if (op == 0 || op == 8) break;
        if (op != 2 || msg.size() < 16) continue;
        uint32_t magic = 0, rate = 0, frames = 0;
        uint16_t chans = 0;
        std::memcpy(&magic, msg.data(), 4);
        std::memcpy(&rate, msg.data() + 4, 4);
        std::memcpy(&chans, msg.data() + 8, 2);
        std::memcpy(&frames, msg.data() + 12, 4);
        if (magic != kMagic || chans < 1 || chans > 2 || rate < 8000 || rate > 192000) continue;
        frames = std::min<uint32_t>(frames, uint32_t((msg.size() - 16) / (2u * chans)));
        const auto* pcm = reinterpret_cast<const int16_t*>(msg.data() + 16);
        l.resize(frames);
        r.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            l[i] = pcm[i * chans] / 32768.0f;
            r[i] = pcm[i * chans + chans - 1] / 32768.0f;
        }
        const std::lock_guard<std::mutex> lock(writeMutex_);
        if (senderGen_.load() != gen) break;
        const float* src[2] = { l.data(), r.data() };
        li.sampleRate.store(rate, std::memory_order_relaxed);
        ssbus::ringWrite(bus_->linkInAudio, li.writePos, src, 2, frames);
        li.heartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
        li.active.store(1, std::memory_order_release);
    }
    if (senderGen_.load() == gen) li.active.store(0, std::memory_order_release);
    --senders_;
}

void ShareServer::setFriendAecMode(int slot, ssnet::AecMode mode) {
    if (slot >= 0 && slot < ssbus::kMaxFriends) {
        if (!friendEcho_[size_t(slot)]) friendEcho_[size_t(slot)] = std::make_unique<ssnet::FriendEcho>();
        friendEcho_[size_t(slot)]->setMode(mode);
    }
}

ssnet::AecMode ShareServer::friendAecMode(int slot) const {
    if (slot >= 0 && slot < ssbus::kMaxFriends && friendEcho_[size_t(slot)])
        return friendEcho_[size_t(slot)]->mode();
    return ssnet::AecMode::Auto;
}

void ShareServer::setFriendLeakDetected(int slot, bool leak) {
    if (slot >= 0 && slot < ssbus::kMaxFriends) {
        if (!friendEcho_[size_t(slot)]) friendEcho_[size_t(slot)] = std::make_unique<ssnet::FriendEcho>();
        friendEcho_[size_t(slot)]->setLeakDetected(leak);
    }
}

bool ShareServer::friendLeakDetected(int slot) const {
    if (slot >= 0 && slot < ssbus::kMaxFriends && friendEcho_[size_t(slot)])
        return friendEcho_[size_t(slot)]->leakDetected();
    return false;
}

float ShareServer::friendErleDb(int slot) const {
    if (slot >= 0 && slot < ssbus::kMaxFriends && friendEcho_[size_t(slot)])
        return friendEcho_[size_t(slot)]->erleDb();
    return 0.0f;
}

// What leaves for one friend: the writer thread fills and empties it, the reader thread adds pongs.
struct ShareServer::FriendOut {
    std::mutex lock;
    ssbus::SendQueue q;
    std::atomic<bool> over { false };   // either thread: the session is finished
    std::atomic<uint8_t> codec { ssnet::CodecPcm16 };   // negotiated codec: CodecOpus or CodecPcm16
    std::atomic<bool> hearMyself { false };             // (3.5) "Hear myself from Hub"
};

// A friend's browser: the microphone comes in (HRA1 or HRA2), the Stream Mix as the DAW makes it goes out
// (HRA2, never delayed by Line up). HRA2 microphone packets say which Stream Mix position the friend was
// hearing when the packet's first frame was captured; with that the Hub knows how late the voice is.
// Two threads: this one only reads (a slow line down must never make the microphone wait), friendSend writes.
void ShareServer::friendSession(Conn& c, int slot, uint32_t id, const juce::String& token) {
    auto& s = *c.sock;
    auto& h = bus_->friends[slot];
    const auto idx = size_t(slot);
    auto& net = friendNet_[idx];
    const uint32_t gen = ++friendGen_[idx];   // the newest connection of this friend wins
    friendKick_[idx].store(false);
    friendRemeasure_[idx].store(false);
    h.sampleRate.store(48000, std::memory_order_relaxed);
    h.delayBits.store(ssbus::floatBits(-1.0f), std::memory_order_relaxed);   // not measured yet
    h.heartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
    h.state.store(ssbus::kFriendLive, std::memory_order_release);
    ++friendsLive_;
    net.rtt.store(-1.0f); net.up.store(0.0f); net.down.store(0.0f); net.micGap.store(0.0f); net.lag.store(0.0f);
    net.skips.store(0); net.upBytes.store(0); net.downBytes.store(0);
    limitSendBuffer(s);

    if (!friendEcho_[idx]) friendEcho_[idx] = std::make_unique<ssnet::FriendEcho>();
    friendEcho_[idx]->init(48000, 480, 200);

    std::vector<uint8_t> msg;
    std::vector<float> l, r;
    std::vector<double> history;                 // recent delay readings (ms), for the median
    std::vector<float> rtts;                     // recent round trips (ms)
    double smooth = -1.0;
    uint32_t lastCheck = juce::Time::getMillisecondCounter(), lastMic = 0;
    FriendOut out;
    bool closing = false;
    ssnet::OpusDec opusDec;
    bool opusDecInited = false;
    uint32_t lastSeq = 0;
    bool lastSeqValid = false;
    {   // hello v3: tells the friend who they are and which codecs are supported
        juce::String name, host;
        uint32_t ignore = 0;
        friendSlotFor(token, &ignore, &name, &host);
        auto* o = new juce::DynamicObject();
        o->setProperty("v", 3);
        o->setProperty("name", name);
        o->setProperty("host", host);
        juce::Array<juce::var> codecs;
        codecs.add("opus");
        codecs.add("pcm16");
        o->setProperty("codecs", codecs);
        const auto json = juce::JSON::toString(juce::var(o), true);
        if (!sendFrame(s, 1, json.toRawUTF8(), json.getNumBytesAsUTF8())) closing = true;
    }
    std::thread writer([this, &s, slot, gen, &out] { friendSend(s, slot, gen, out); });
    const auto onPing = [&out](const uint8_t* p, size_t n) {   // the browser's own ping: the answer goes out with the rest
        std::vector<uint8_t> pong;
        frameInto(pong, 0xA, p, n);
        const std::lock_guard<std::mutex> lock(out.lock);
        out.q.appendIfRoom(pong.data(), pong.size(), kMaxControlQueue);
    };
    while (!closing && !out.over.load() && running_.load() && friendGen_[idx].load() == gen && !friendKick_[idx].load()) {
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastCheck >= 250) {   // now and then: is this link still this friend's?
            lastCheck = now;
            uint32_t cur = 0;
            if (friendSlotFor(token, &cur) != slot || cur != id) break;
        }
        if (friendRemeasure_[idx].exchange(false)) { history.clear(); smooth = -1.0; h.delayBits.store(ssbus::floatBits(-1.0f), std::memory_order_relaxed); }

        // ---- microphone ------------------------------------------------------------------
        const int ready = s.waitUntilReady(true, 20);
        if (ready < 0) break;
        if (ready == 1) {
            const uint8_t op = readMessage(s, msg, 5000, running_, onPing);
            if (op == 0 || op == 8) break;
            net.upBytes.fetch_add(msg.size(), std::memory_order_relaxed);
            if (op == 1 && msg.size() > 8 && msg.size() < 40 && std::memcmp(msg.data(), "{\"pong\":", 8) == 0) {   // the page answers the writer's ping
                const std::string t(msg.begin() + 8, msg.end());
                const uint32_t rtt = juce::Time::getMillisecondCounter() - uint32_t(std::strtoull(t.c_str(), nullptr, 10));
                if (rtt < 10000) {
                    rtts.push_back(float(rtt));
                    if (rtts.size() > 5) rtts.erase(rtts.begin());
                    auto sorted = rtts;
                    std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
                    net.rtt.store(sorted[sorted.size() / 2], std::memory_order_relaxed);
                }
            } else if (op == 1) {   // client control messages e.g. {"codec":"opus"}, {"hearMyself":true}, {"aec":"on"}
                const juce::String text(reinterpret_cast<const char*>(msg.data()), msg.size());
                const auto j = juce::JSON::parse(text);
                if (j.hasProperty("codec")) {
                    const auto codecName = j["codec"].toString();
                    if (codecName == "opus") out.codec.store(ssnet::CodecOpus);
                    else if (codecName == "pcm16") out.codec.store(ssnet::CodecPcm16);
                }
                if (j.hasProperty("hearMyself")) {
                    out.hearMyself.store(bool(j["hearMyself"]), std::memory_order_relaxed);
                }
                if (j.hasProperty("speakerMode")) {
                    if (bool(j["speakerMode"])) setFriendAecMode(slot, ssnet::AecMode::On);
                }
                if (j.hasProperty("aec")) {
                    const auto mode = j["aec"].toString();
                    if (mode == "on") setFriendAecMode(slot, ssnet::AecMode::On);
                    else if (mode == "off") setFriendAecMode(slot, ssnet::AecMode::Off);
                    else if (mode == "auto") setFriendAecMode(slot, ssnet::AecMode::Auto);
                }
            }
            ssnet::Hra3Header h3{};
            const uint8_t* h3Payload = nullptr;
            size_t h3PayloadSize = 0;
            if (op == 2 && ssnet::parseHra3(msg.data(), msg.size(), h3, h3Payload, h3PayloadSize)) {
                out.codec.store(h3.codec);
                if (h3.codec == ssnet::CodecOpus) {
                    if (!opusDecInited || opusDec.channels() != h3.chans) {
                        opusDec.init(48000, h3.chans);
                        opusDecInited = true;
                        lastSeq = h3.seq;
                        lastSeqValid = true;
                    } else if (lastSeqValid && h3.seq > lastSeq + 1) {
                        const uint32_t missed = std::min<uint32_t>(h3.seq - lastSeq - 1, 10);
                        l.resize(h3.frames);
                        r.resize(h3.frames);
                        for (uint32_t m = 0; m < missed; ++m) {
                            float* const plcCh[2] = { l.data(), r.data() };
                            opusDec.decodePlc(plcCh, h3.frames);
                            const std::lock_guard<std::mutex> lock(friendWrite_[idx]);
                            if (friendGen_[idx].load() != gen) break;
                            const float* src[2] = { l.data(), h3.chans == 2 ? r.data() : l.data() };
                            ssbus::ringWrite(bus_->friendAudio[slot], h.writePos, src, 2, h3.frames);
                        }
                    }
                    lastSeq = h3.seq;
                    lastSeqValid = true;

                    l.resize(h3.frames);
                    r.resize(h3.frames);
                    float* const decCh[2] = { l.data(), r.data() };
                    const int decFrames = opusDec.decode(h3Payload, int(h3PayloadSize), decCh, h3.frames);
                    if (decFrames > 0) {
                        float dMs = ssbus::bitsFloat(h.delayBits.load(std::memory_order_relaxed));
                        if (dMs < 0.0f) dMs = net.rtt.load(std::memory_order_relaxed);
                        if (dMs < 0.0f) dMs = 80.0f;
                        const int delaySamples = int(dMs * 48.0f);
                        if (friendEcho_[idx]) {
                            int rem = decFrames, off = 0;
                            while (rem >= 480) {
                                friendEcho_[idx]->process(l.data() + off, l.data() + off, 480, delaySamples);
                                if (h3.chans == 2) {
                                    std::memcpy(r.data() + off, l.data() + off, 480 * sizeof(float));
                                }
                                off += 480;
                                rem -= 480;
                            }
                        }
                        const auto t = juce::Time::getMillisecondCounter();
                        if (lastMic != 0 && float(t - lastMic) > net.micGap.load(std::memory_order_relaxed))
                            net.micGap.store(float(t - lastMic), std::memory_order_relaxed);
                        lastMic = t;
                        float pk = 0.0f;
                        for (int i = 0; i < decFrames; ++i) {
                            pk = std::max(pk, std::abs(l[size_t(i)]));
                            if (h3.chans == 2) pk = std::max(pk, std::abs(r[size_t(i)]));
                        }
                        const uint64_t W = bus_->friendMixWrite.load(std::memory_order_acquire);
                        const uint32_t hubRate = std::max<uint32_t>(8000, bus_->streamHeader.sampleRate.load(std::memory_order_relaxed));
                        {
                            const std::lock_guard<std::mutex> lock(friendWrite_[idx]);
                            if (friendGen_[idx].load() != gen) break;
                            const float* src[2] = { l.data(), h3.chans == 2 ? r.data() : l.data() };
                            h.sampleRate.store(48000, std::memory_order_relaxed);
                            ssbus::ringWrite(bus_->friendAudio[slot], h.writePos, src, 2, uint32_t(decFrames));
                            h.heartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
                            h.peakBits.store(ssbus::floatBits(pk), std::memory_order_relaxed);
                        }
                        if (h3.pos != 0 && W >= h3.pos && decFrames > 0) {
                            const double ms = double(W - h3.pos) * 1000.0 / double(hubRate) - double(decFrames) * 500.0 / 48000.0;
                            if (ms >= 0.0 && ms < 3000.0) {
                                history.push_back(ms);
                                if (history.size() > 41) history.erase(history.begin());
                                if (history.size() >= 8) {
                                    auto sorted = history;
                                    std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
                                    const double med = sorted[sorted.size() / 2];
                                    smooth = smooth < 0.0 ? med : smooth + (med - smooth) * 0.15;
                                    h.delayBits.store(ssbus::floatBits(float(smooth)), std::memory_order_relaxed);
                                }
                            }
                        }
                    }
                }
            } else if (op == 2 && msg.size() >= (msg.size() >= 4 && std::memcmp(msg.data(), &kMagic2, 4) == 0 ? 24 : 16)) {
                const size_t hdr = msg.size() >= 4 && std::memcmp(msg.data(), &kMagic2, 4) == 0 ? 24 : 16;
                uint32_t magic = 0, rate = 0, frames = 0;
                uint16_t chans = 0;
                uint64_t echo = 0;
                std::memcpy(&magic, msg.data(), 4);
                std::memcpy(&rate, msg.data() + 4, 4);
                std::memcpy(&chans, msg.data() + 8, 2);
                std::memcpy(&frames, msg.data() + 12, 4);
                if (hdr == 24) std::memcpy(&echo, msg.data() + 16, 8);
                if ((magic == kMagic || magic == kMagic2) && chans >= 1 && chans <= 2 && rate >= 8000 && rate <= 192000) {
                    const auto t = juce::Time::getMillisecondCounter();
                    if (lastMic != 0 && float(t - lastMic) > net.micGap.load(std::memory_order_relaxed)) net.micGap.store(float(t - lastMic), std::memory_order_relaxed);
                    lastMic = t;
                    frames = std::min<uint32_t>(frames, uint32_t((msg.size() - hdr) / (2u * chans)));
                    const auto* pcm = reinterpret_cast<const int16_t*>(msg.data() + hdr);
                    l.resize(frames);
                    r.resize(frames);
                    for (uint32_t i = 0; i < frames; ++i) {
                        l[i] = pcm[i * chans] / 32768.0f;
                        r[i] = pcm[i * chans + chans - 1] / 32768.0f;
                    }
                    float dMs = ssbus::bitsFloat(h.delayBits.load(std::memory_order_relaxed));
                    if (dMs < 0.0f) dMs = net.rtt.load(std::memory_order_relaxed);
                    if (dMs < 0.0f) dMs = 80.0f;
                    const int delaySamples = int(dMs * (float(rate) / 1000.0f));
                    if (friendEcho_[idx]) {
                        int rem = int(frames), off = 0;
                        while (rem >= 480) {
                            friendEcho_[idx]->process(l.data() + off, l.data() + off, 480, delaySamples);
                            if (chans == 2) {
                                std::memcpy(r.data() + off, l.data() + off, 480 * sizeof(float));
                            }
                            off += 480;
                            rem -= 480;
                        }
                    }
                    float pk = 0.0f;
                    for (uint32_t i = 0; i < frames; ++i) {
                        pk = std::max(pk, std::max(std::abs(l[i]), std::abs(r[i])));
                    }
                    // how late is the average sample of this packet when it arrives? (Stream Mix frames are the Hub's)
                    const uint64_t W = bus_->friendMixWrite.load(std::memory_order_acquire);
                    const uint32_t hubRate = std::max<uint32_t>(8000, bus_->streamHeader.sampleRate.load(std::memory_order_relaxed));
                    {
                        const std::lock_guard<std::mutex> lock(friendWrite_[idx]);
                        if (friendGen_[idx].load() != gen) break;
                        const float* src[2] = { l.data(), r.data() };
                        h.sampleRate.store(rate, std::memory_order_relaxed);
                        ssbus::ringWrite(bus_->friendAudio[slot], h.writePos, src, 2, frames);
                        h.heartbeatNs.store(ssbus::nowNs(), std::memory_order_relaxed);
                        h.peakBits.store(ssbus::floatBits(pk), std::memory_order_relaxed);
                    }
                    if (echo != 0 && W >= echo && frames > 0) {
                        const double ms = double(W - echo) * 1000.0 / double(hubRate) - double(frames) * 500.0 / double(rate);
                        if (ms >= 0.0 && ms < 3000.0) {
                            history.push_back(ms);
                            if (history.size() > 41) history.erase(history.begin());
                            if (history.size() >= 8) {   // the median ignores a late packet now and then
                                auto sorted = history;
                                std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
                                const double med = sorted[sorted.size() / 2];
                                smooth = smooth < 0.0 ? med : smooth + (med - smooth) * 0.15;
                                h.delayBits.store(ssbus::floatBits(float(smooth)), std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
        }
    }
    out.over.store(true);
    writer.join();
    --friendsLive_;
    const std::lock_guard<std::mutex> lock(friendWrite_[idx]);
    if (friendGen_[idx].load() == gen || friendKick_[idx].load()) {   // not taken over by a newer connection
        if (h.id.load() == id && h.state.load() == ssbus::kFriendLive) h.state.store(ssbus::kFriendOffline, std::memory_order_release);
        h.peakBits.store(0, std::memory_order_relaxed);
    }
}

// What the friend hears: the Stream Mix as the DAW makes it + every other friend (like Jamulus: each voice as
// soon as it arrives; never their own). Never waits for the line: what the socket does not take is not queued,
// the Hub jumps ahead instead (SendQueue, catchUp). Also: the delay for the friend's screen and the ping.
void ShareServer::friendSend(juce::StreamingSocket& s, int slot, uint32_t gen, FriendOut& out) {
    const auto idx = size_t(slot);
    auto& h = bus_->friends[slot];
    auto& net = friendNet_[idx];
    std::vector<float> ml(4096), mr(4096);
    std::vector<float> voices[ssbus::kMaxFriends];   // the other friends (friendVoiceAudio), same positions as ml / mr
    for (auto& v : voices) v.assign(ml.size(), 0.0f);
    std::vector<uint8_t> packet, framed;
    ssnet::OpusEnc opusEnc;
    bool opusReady = false;
    uint32_t downSeq = 0;
    uint64_t mixCursor = bus_->friendMixWrite.load() > 2400 ? bus_->friendMixWrite.load() - 960 : 0;
    const auto sendNow = [&s, &net](const uint8_t* p, size_t n) {
        const int w = trySend(s, p, n);
        if (w > 0) net.downBytes.fetch_add(uint64_t(w), std::memory_order_relaxed);
        return w;
    };
    auto lastStat = juce::Time::getMillisecondCounter(), lastInfo = lastStat;
    uint64_t prevUp = 0, prevDown = 0;
    bool controlDue = false;   // delay + ping wait for an empty queue, and are only counted as sent when they were queued
    bool blocked = false;   // the socket still holds a packet
    while (!out.over.load() && running_.load() && friendGen_[idx].load() == gen && !friendKick_[idx].load()) {
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastStat >= 1000) {   // once a second: kbps
            const float secs = float(now - lastStat) / 1000.0f;
            lastStat = now;
            const uint64_t up = net.upBytes.load(std::memory_order_relaxed), down = net.downBytes.load(std::memory_order_relaxed);
            net.up.store(float(up - prevUp) * 8.0f / 1000.0f / secs, std::memory_order_relaxed);
            net.down.store(float(down - prevDown) * 8.0f / 1000.0f / secs, std::memory_order_relaxed);
            prevUp = up;
            prevDown = down;
        }
        if (now - lastInfo >= 1000) controlDue = true;   // how late the friend is (their screen) and the ping (round trip)
        {
            const std::lock_guard<std::mutex> lock(out.lock);
            if (!out.q.flush(sendNow)) break;
            if (controlDue && out.q.idle()) {   // as soon as the queue is empty, before the next audio packet: a bad line needs these most
                const auto delay = "{\"delay\":" + juce::String(ssbus::bitsFloat(h.delayBits.load(std::memory_order_relaxed)), 1) + "}";
                const auto ping = "{\"ping\":" + juce::String(now) + "}";
                frameInto(framed, 1, delay.toRawUTF8(), delay.getNumBytesAsUTF8());
                out.q.append(framed.data(), framed.size());
                frameInto(framed, 1, ping.toRawUTF8(), ping.getNumBytesAsUTF8());
                out.q.append(framed.data(), framed.size());
                controlDue = false;
                lastInfo = now;
                if (!out.q.flush(sendNow)) break;
            }
            blocked = !out.q.idle();
        }

        const uint32_t hubRate = std::max<uint32_t>(8000, bus_->streamHeader.sampleRate.load(std::memory_order_relaxed));
        const uint64_t mw = bus_->friendMixWrite.load(std::memory_order_acquire);
        if (mw < mixCursor) mixCursor = mw;   // Hub restarted
        const uint64_t was = mixCursor;
        mixCursor = blocked ? ssbus::catchUp(mixCursor, mw, hubRate, kFriendMaxLagMs, 30)
                            : ssbus::catchUp(mixCursor, mw, hubRate, 2000, 100);   // fell far behind: jump to now
        if (blocked && mixCursor != was) net.skips.fetch_add(1, std::memory_order_relaxed);
        const float lagMs = float(mw - mixCursor) * 1000.0f / float(hubRate);
        if (lagMs > net.lag.load(std::memory_order_relaxed)) net.lag.store(lagMs, std::memory_order_relaxed);

        const uint8_t curCodec = out.codec.load(std::memory_order_relaxed);
        if (curCodec == ssnet::CodecOpus && !opusReady) {
            if (hubRate == 48000 || hubRate == 24000 || hubRate == 16000 || hubRate == 12000 || hubRate == 8000) {
                if (opusEnc.init(int(hubRate), 2, ssnet::OpusEnc::App::Audio)) {
                    opusEnc.setBitrate(128000);   // 128 kbps stereo
                    opusEnc.setComplexity(5);
                    opusEnc.setInbandFec(true);
                    opusEnc.setPacketLossPerc(5);
                    opusReady = true;
                }
            }
        }

        while (!blocked && mw - mixCursor >= hubRate / 100 && running_.load()) {
            const bool useOpus = (curCodec == ssnet::CodecOpus && opusReady);
            const uint32_t n = useOpus ? (hubRate / 100) : uint32_t(std::min<uint64_t>(mw - mixCursor, ml.size()));
            float* dst[2] = { ml.data(), mr.data() };
            if (ssbus::ringReadAt(bus_->friendMixAudio, bus_->friendMixWrite, mixCursor, dst, n) == ssbus::ReadResult::Overrun) { mixCursor = mw - hubRate / 10; break; }
            for (int k = 0; k < ssbus::kMaxFriends / 2; ++k) {
                float* vd[2] = { voices[size_t(2 * k)].data(), voices[size_t(2 * k + 1)].data() };
                ssbus::ringReadAt(bus_->friendVoiceAudio[k], bus_->friendVoiceWrite[k], mixCursor, vd, n);   // a gap = silence
            }
            for (int j = 0; j < ssbus::kMaxFriends; ++j) {
                if (j == slot && !out.hearMyself.load(std::memory_order_relaxed)) continue;   // nobody hears themselves come back unless opted in
                const float* v = voices[size_t(j)].data();
                for (uint32_t i = 0; i < n; ++i) { ml[i] += v[i]; mr[i] += v[i]; }
            }
            if (slot >= 0 && slot < ssbus::kMaxFriends && friendEcho_[size_t(slot)]) {
                std::vector<float> refMono(n);
                for (uint32_t i = 0; i < n; ++i) refMono[i] = 0.5f * (ml[i] + mr[i]);
                friendEcho_[size_t(slot)]->pushReference(refMono.data(), n);
            }
            if (useOpus) {
                const float* const encCh[2] = { ml.data(), mr.data() };
                uint8_t opusPayload[1275];
                const int encodedBytes = opusEnc.encode(encCh, int(n), opusPayload, sizeof(opusPayload));
                if (encodedBytes > 0) {
                    packet.resize(sizeof(ssnet::Hra3Header) + size_t(encodedBytes));
                    ssnet::Hra3Header h3{};
                    h3.magic = ssnet::kMagic3;
                    h3.codec = ssnet::CodecOpus;
                    h3.chans = 2;
                    h3.flags = ssnet::FlagFec;
                    h3.seq = downSeq++;
                    h3.pos = mixCursor;
                    h3.frames = uint16_t(n);
                    ssnet::writeHra3(packet.data(), h3, opusPayload, size_t(encodedBytes));
                } else {
                    // Fallback to PCM16 on encode error
                    packet.resize(24 + size_t(n) * 4);
                    const uint16_t chans = 2, flags = 0;
                    std::memcpy(packet.data(), &kMagic2, 4);
                    std::memcpy(packet.data() + 4, &hubRate, 4);
                    std::memcpy(packet.data() + 8, &chans, 2);
                    std::memcpy(packet.data() + 10, &flags, 2);
                    std::memcpy(packet.data() + 12, &n, 4);
                    std::memcpy(packet.data() + 16, &mixCursor, 8);
                    auto* pcm = reinterpret_cast<int16_t*>(packet.data() + 24);
                    for (uint32_t i = 0; i < n; ++i) {
                        pcm[2 * i]     = int16_t(std::lround(std::clamp(ml[i], -1.0f, 1.0f) * 32767.0f));
                        pcm[2 * i + 1] = int16_t(std::lround(std::clamp(mr[i], -1.0f, 1.0f) * 32767.0f));
                    }
                }
            } else {
                packet.resize(24 + size_t(n) * 4);
                const uint16_t chans = 2, flags = 0;
                std::memcpy(packet.data(), &kMagic2, 4);
                std::memcpy(packet.data() + 4, &hubRate, 4);
                std::memcpy(packet.data() + 8, &chans, 2);
                std::memcpy(packet.data() + 10, &flags, 2);
                std::memcpy(packet.data() + 12, &n, 4);
                std::memcpy(packet.data() + 16, &mixCursor, 8);
                auto* pcm = reinterpret_cast<int16_t*>(packet.data() + 24);
                for (uint32_t i = 0; i < n; ++i) {
                    pcm[2 * i]     = int16_t(std::lround(std::clamp(ml[i], -1.0f, 1.0f) * 32767.0f));
                    pcm[2 * i + 1] = int16_t(std::lround(std::clamp(mr[i], -1.0f, 1.0f) * 32767.0f));
                }
            }
            frameInto(framed, 2, packet.data(), packet.size());
            const std::lock_guard<std::mutex> lock(out.lock);
            if (!out.q.offer(framed.data(), framed.size())) break;   // the browser's ping answer is still going out: next round
            mixCursor += n;
            if (!out.q.flush(sendNow)) { out.over.store(true); break; }
            blocked = !out.q.idle();
        }
        if (blocked) waitWritable(s, 5);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    out.over.store(true);
}

// cloudflared quick tunnel: prints "https://<random>.trycloudflare.com" once it is reachable.
void ShareServer::tunnelLoop() {
    const std::regex url(R"(https://[a-z0-9-]+\.trycloudflare\.com)");
    std::string text;
    char buf[1024];
    while (running_.load()) {
        const int n = cloudflared_->read(buf, sizeof buf);
        if (n <= 0) break;   // exited (or killed by stop())
        if (tunnel_.load() == Tunnel::Ready) continue;   // keep draining the pipe
        text.append(buf, size_t(n));
        std::smatch m;
        if (std::regex_search(text, m, url)) {
            {
                const std::lock_guard<std::mutex> lock(urlMutex_);
                publicBase_ = juce::String(m.str());
            }
            tunnel_.store(Tunnel::Ready);
        }
        if (text.size() > 65536) text.erase(0, text.size() - 4096);
    }
    if (running_.load()) {
        const std::lock_guard<std::mutex> lock(urlMutex_);
        publicBase_ = {};
        tunnel_.store(Tunnel::Failed);
    }
}

} // namespace hearaside
