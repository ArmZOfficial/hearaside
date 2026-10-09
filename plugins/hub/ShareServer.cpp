#include "ShareServer.h"

#include "BinaryData_web.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <regex>

namespace hearaside {

namespace {

constexpr uint32_t kMagic = 0x31415248;   // "HRA1" little-endian
constexpr int kFirstPort = 47810;

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
bool writeAll(juce::StreamingSocket& s, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
        const int w = s.write(p, int(std::min<size_t>(n, 1 << 20)));
        if (w <= 0) return false;
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

bool sendFrame(juce::StreamingSocket& s, uint8_t opcode, const void* data, size_t n) {
    uint8_t h[10];
    size_t hn = 2;
    h[0] = uint8_t(0x80 | opcode);
    if (n < 126) h[1] = uint8_t(n);
    else if (n < 65536) { h[1] = 126; h[2] = uint8_t(n >> 8); h[3] = uint8_t(n); hn = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; ++i) h[2 + i] = uint8_t(uint64_t(n) >> (56 - i * 8)); hn = 10; }
    return writeAll(s, h, hn) && (n == 0 || writeAll(s, data, n));
}

// Reads one whole message (joins fragments). opcode 0 = timeout / error.
uint8_t readMessage(juce::StreamingSocket& s, std::vector<uint8_t>& out, int timeoutMs, const std::atomic<bool>& alive) {
    out.clear();
    uint8_t first = 0;
    for (;;) {
        uint8_t h[2];
        if (!readExact(s, h, 2, timeoutMs, alive)) return 0;
        const uint8_t op = h[0] & 0x0F;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) { uint8_t e[2]; if (!readExact(s, e, 2, timeoutMs, alive)) return 0; len = uint64_t(e[0]) << 8 | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!readExact(s, e, 8, timeoutMs, alive)) return 0; len = 0; for (uint8_t b : e) len = len << 8 | b; }
        if (len > (1u << 22)) return 0;
        uint8_t mask[4] = { 0, 0, 0, 0 };
        const bool masked = (h[1] & 0x80) != 0;
        if (masked && !readExact(s, mask, 4, timeoutMs, alive)) return 0;
        const size_t at = out.size();
        out.resize(at + size_t(len));
        if (len > 0 && !readExact(s, out.data() + at, size_t(len), timeoutMs, alive)) return 0;
        if (masked) for (size_t i = 0; i < size_t(len); ++i) out[at + i] ^= mask[i & 3];
        if (op >= 8) {   // control frames may arrive between fragments
            if (op == 9) sendFrame(s, 0xA, out.data() + at, size_t(len));
            if (op == 8) return 8;
            out.resize(at);
            continue;
        }
        if (op != 0) first = op;
        if (h[0] & 0x80) return first;
    }
}

void respond(juce::StreamingSocket& s, int code, const char* type, const void* body, size_t n) {
    const auto head = juce::String("HTTP/1.1 ") + juce::String(code) + (code == 200 ? " OK" : code == 403 ? " Forbidden" : " Not Found")
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
        if (d.trim().isEmpty()) continue;
        const juce::File f = juce::File(d.trim()).getChildFile("cloudflared.exe");
        if (f.existsAsFile()) return f;
    }
    return {};
}

bool ShareServer::start(ssbus::BusLayout* bus, const juce::String& listenToken, const juce::String& sendToken) {
    stop();
    if (bus == nullptr) return false;
    bus_ = bus;
    listenToken_ = listenToken;
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
    if (bus_) bus_->linkIn.active.store(0);
    listeners_.store(0);
    senders_.store(0);
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

juce::String ShareServer::listenUrl() const {
    const auto p = publicBase();
    return (p.isNotEmpty() ? p : lanBase()) + "/l/" + listenToken_;
}

juce::String ShareServer::localListenUrl() const {
    return "http://127.0.0.1:" + juce::String(port_) + "/l/" + listenToken_;
}

juce::String ShareServer::sendUrl() const {
    const auto p = publicBase();
    return (p.isNotEmpty() ? p : lanBase()) + "/s/" + sendToken_;
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
        Conn* raw = c.get();
        const std::lock_guard<std::mutex> lock(connsMutex_);
        if (conns_.size() >= 40) { raw->sock->close(); continue; }   // plenty for a few listeners
        conns_.push_back(std::move(c));
        raw->thread = std::thread([this, raw] { serve(*raw); raw->sock->close(); raw->done.store(true); });
    }
}

void ShareServer::serve(Conn& c) {
    auto& s = *c.sock;
    // request head
    std::string head;
    char ch = 0;
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
        if (!readExact(s, &ch, 1, 5000, running_)) return;
        head += ch;
    }
    const auto lines = juce::StringArray::fromLines(juce::String::fromUTF8(head.c_str()));
    const auto first = juce::StringArray::fromTokens(lines[0], " ", "");
    if (first.size() < 2 || first[0] != "GET") { respond(s, 404, "text/plain", "", 0); return; }
    const auto path = first[1].upToFirstOccurrenceOf("?", false, false);
    juce::String key, origin;
    for (const auto& l : lines) {
        if (l.startsWithIgnoreCase("Sec-WebSocket-Key:")) key = l.fromFirstOccurrenceOf(":", false, false).trim();
        if (l.startsWithIgnoreCase("Origin:")) origin = l.fromFirstOccurrenceOf(":", false, false).trim();
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

    if (path == "/l/" + listenToken_) { respond(s, 200, "text/html; charset=utf-8", HearasideWeb::listen_html, size_t(HearasideWeb::listen_htmlSize)); return; }
    if (path == "/s/" + sendToken_)   { respond(s, 200, "text/html; charset=utf-8", HearasideWeb::send_html, size_t(HearasideWeb::send_htmlSize)); return; }
    if (path == "/ws/l/" + listenToken_ && key.isNotEmpty()) { if (upgrade()) streamTo(c); return; }
    if (path == "/ws/s/" + sendToken_ && key.isNotEmpty())   { if (upgrade()) receiveFrom(c); return; }
    static const char notFound[] = "HEARASIDE: this link is not valid (any more).";
    respond(s, 404, "text/plain; charset=utf-8", notFound, sizeof(notFound) - 1);
}

// Sends the Stream Mix (Hub output 0) as it is written, in ~10 ms packets.
void ShareServer::streamTo(Conn& c) {
    auto& s = *c.sock;
    auto& sh = bus_->streamHeader;
    ++listeners_;
    std::vector<float> l(4096), r(4096);
    std::vector<uint8_t> packet, incoming;
    uint64_t cursor = sh.writePos[0].load();
    int idle = 0;
    while (running_.load()) {
        if (s.waitUntilReady(true, 10) == 1) {   // the browser closed or pinged
            const uint8_t op = readMessage(s, incoming, 1000, running_);
            if (op == 8 || op == 0) break;
        }
        const uint32_t rate = std::max<uint32_t>(8000, sh.sampleRate.load());
        const uint64_t w = sh.writePos[0].load(std::memory_order_acquire);
        if (sh.active.load() == 0 || w == cursor) {
            cursor = w;
            if (++idle % 100 == 0) {   // keep the connection (and the tunnel) alive while the DAW is quiet
                uint8_t hdr[16] = {};
                std::memcpy(hdr, &kMagic, 4);
                std::memcpy(hdr + 4, &rate, 4);
                if (!sendFrame(s, 2, hdr, sizeof hdr)) break;
            }
            continue;
        }
        idle = 0;
        if (w - cursor > uint64_t(rate) * 2) cursor = w - rate / 10;   // fell far behind: jump to now
        const uint32_t n = uint32_t(std::min<uint64_t>(w - cursor, l.size()));
        if (n < rate / 200) continue;   // wait for ~5 ms of audio
        float* dst[2] = { l.data(), r.data() };
        if (ssbus::ringReadAt(bus_->streamAudio[0], sh.writePos[0], cursor, dst, n) == ssbus::ReadResult::Overrun) {
            cursor = sh.writePos[0].load() - rate / 10;
            continue;
        }
        cursor += n;
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
        if (!sendFrame(s, 2, packet.data(), packet.size())) break;
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
