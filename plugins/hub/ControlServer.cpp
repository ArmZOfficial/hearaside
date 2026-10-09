#include "ControlServer.h"
#include "ShareServer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hearaside {

namespace {

bool writeAll(juce::StreamingSocket& s, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
        const int w = s.write(p, int(std::min<size_t>(n, 1 << 16)));
        if (w <= 0) return false;
        p += w;
        n -= size_t(w);
    }
    return true;
}

bool readSome(juce::StreamingSocket& s, std::string& into, size_t want, int timeoutMs, const std::atomic<bool>& alive) {
    char buf[2048];
    int waited = 0;
    while (into.size() < want) {
        if (!alive.load()) return false;
        const int r = s.waitUntilReady(true, 100);
        if (r < 0) return false;
        if (r == 0) { if ((waited += 100) >= timeoutMs) return false; continue; }
        const int got = s.read(buf, int(std::min<size_t>(sizeof buf, want - into.size())), false);
        if (got <= 0) return false;
        into.append(buf, size_t(got));
        waited = 0;
    }
    return true;
}

void respond(juce::StreamingSocket& s, int status, const juce::var& body) {
    const juce::String text = juce::JSON::toString(body, true) + "\n";
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 413 ? "Payload Too Large" : "Service Unavailable";
    const auto head = "HTTP/1.1 " + juce::String(status) + " " + reason
                    + "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + juce::String(int64_t(text.getNumBytesAsUTF8()))
                    + "\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    writeAll(s, head.toRawUTF8(), head.getNumBytesAsUTF8());
    writeAll(s, text.toRawUTF8(), text.getNumBytesAsUTF8());
}

juce::var error(const juce::String& message) {
    auto* o = new juce::DynamicObject();
    o->setProperty("error", message);
    return juce::var(o);
}

bool sameSecret(const juce::String& a, const juce::String& b) {   // constant time
    if (a.length() != b.length() || a.isEmpty()) return false;
    int diff = 0;
    for (int i = 0; i < a.length(); ++i) diff |= int(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace

struct ControlServer::Conn {
    std::unique_ptr<juce::StreamingSocket> sock;
    std::thread thread;
    std::atomic<bool> done { false };
};

ControlServer::ControlServer() = default;
ControlServer::~ControlServer() { stop(); }

juce::String ControlServer::newKey() {
    uint8_t bytes[16];
    return ShareServer::secureRandom(bytes, sizeof bytes) ? juce::String::toHexString(bytes, int(sizeof bytes), 0) : juce::String();
}

bool ControlServer::start(const juce::String& key) {
    stop();
    if (key.length() < 16) return false;
    key_ = key;
    listener_ = std::make_unique<juce::StreamingSocket>();
    port_ = 0;
    for (int p = kFirstPort; p < kFirstPort + 10 && port_ == 0; ++p)
        if (listener_->createListener(p, "127.0.0.1")) port_ = p;   // this computer only
    if (port_ == 0) { listener_.reset(); return false; }
#ifdef _WIN32
    // a process the DAW starts must not keep the listener open after we close it (see ShareServer)
    SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<intptr_t>(listener_->getRawSocketHandle())), HANDLE_FLAG_INHERIT, 0);
#endif
    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void ControlServer::stop() {
    if (!running_.exchange(false)) return;
    if (listener_) listener_->close();
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        const std::lock_guard<std::mutex> lock(jobsMutex_);   // nobody will answer these any more
        for (auto& j : jobs_) j->done.set_value({ 503, error("the Hub is closing") });
        jobs_.clear();
    }
    {
        const std::lock_guard<std::mutex> lock(connsMutex_);
        for (auto& c : conns_) c->sock->close();
    }
    for (auto& c : conns_) if (c->thread.joinable()) c->thread.join();
    conns_.clear();
    listener_.reset();
    port_ = 0;
}

void ControlServer::service(const Handler& handler) {
    std::deque<std::shared_ptr<Job>> jobs;
    {
        const std::lock_guard<std::mutex> lock(jobsMutex_);
        jobs.swap(jobs_);
    }
    for (auto& j : jobs) j->done.set_value(handler(j->request));
}

void ControlServer::acceptLoop() {
    while (running_.load()) {
        std::unique_ptr<juce::StreamingSocket> s(listener_->waitForNextConnection());
        if (!s) { if (!running_.load()) break; juce::Thread::sleep(20); continue; }
        const std::lock_guard<std::mutex> lock(connsMutex_);
        for (auto it = conns_.begin(); it != conns_.end();) {   // reap finished connections
            if ((*it)->done.load()) { if ((*it)->thread.joinable()) (*it)->thread.join(); it = conns_.erase(it); }
            else ++it;
        }
        if (conns_.size() >= 8) { s->close(); continue; }
        auto c = std::make_unique<Conn>();
        c->sock = std::move(s);
        Conn* raw = c.get();
        conns_.push_back(std::move(c));
        raw->thread = std::thread([this, raw] { serve(*raw); raw->sock->close(); raw->done.store(true); });
    }
}

void ControlServer::serve(Conn& c) {
    auto& s = *c.sock;
    std::string data;
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        if (data.size() > 8192 || !readSome(s, data, data.size() + 1, 3000, running_)) return;
        headEnd = data.find("\r\n\r\n");
    }
    const auto lines = juce::StringArray::fromLines(juce::String::fromUTF8(data.c_str(), int(headEnd)));
    const auto first = juce::StringArray::fromTokens(lines[0], " ", "");
    if (first.size() < 2) { respond(s, 400, error("bad request")); return; }
    juce::String host, origin, auth;
    int64_t length = 0;
    for (const auto& l : lines) {
        const auto value = l.fromFirstOccurrenceOf(":", false, false).trim();
        if (l.startsWithIgnoreCase("Host:")) host = value.toLowerCase();
        else if (l.startsWithIgnoreCase("Origin:")) origin = value;
        else if (l.startsWithIgnoreCase("Authorization:")) auth = value;
        else if (l.startsWithIgnoreCase("Content-Length:")) length = value.getLargeIntValue();
    }
    // only programs on this computer: a web page (Origin, CORS preflight) or another host name
    // (DNS rebinding) never gets in
    const auto port = ":" + juce::String(port_);
    if ((host != "127.0.0.1" + port && host != "localhost" + port) || origin.isNotEmpty() || first[0] == "OPTIONS") {
        respond(s, 403, error("only programs on this computer may use the HEARASIDE API"));
        return;
    }
    if (!sameSecret(auth.fromFirstOccurrenceOf("Bearer ", false, true).trim(), key_)) {
        respond(s, 401, error("missing or wrong API key (Hub settings > REST API)"));
        return;
    }
    if (length < 0 || length > 16384) { respond(s, 413, error("request too large")); return; }
    std::string body = data.substr(headEnd + 4);
    if (int64_t(body.size()) < length && !readSome(s, body, size_t(length), 3000, running_)) return;

    auto job = std::make_shared<Job>();
    job->request.method = first[0].toUpperCase();
    job->request.path = first[1].upToFirstOccurrenceOf("?", false, false);
    if (!body.empty()) {
        job->request.body = juce::JSON::parse(juce::String::fromUTF8(body.data(), int(body.size())));
        if (job->request.body.isVoid()) { respond(s, 400, error("the body is not JSON")); return; }
    }
    auto answer = job->done.get_future();
    {
        const std::lock_guard<std::mutex> lock(jobsMutex_);
        if (!running_.load()) return;
        jobs_.push_back(job);
    }
    if (answer.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        respond(s, 503, error("the DAW did not answer (busy?)"));
        return;
    }
    const auto r = answer.get();
    respond(s, r.status, r.body);
}

} // namespace hearaside
