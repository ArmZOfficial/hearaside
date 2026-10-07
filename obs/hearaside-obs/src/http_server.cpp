// SPDX-License-Identifier: GPL-2.0-or-later
#include "http_server.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
using sock_t = SOCKET;
static void closeSock(std::uintptr_t s) { closesocket(SOCKET(s)); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_type = socklen_t;
using sock_t = int;
static void closeSock(std::uintptr_t s) { ::close(int(s)); }
#endif

namespace hearaside {

namespace {
constexpr std::uintptr_t kInvalid = ~std::uintptr_t(0);

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        default:  return "Error";
    }
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            const std::string hex = s.substr(i + 1, 2);
            out += char(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += char(c);
        }
    }
    return out;
}

bool HttpServer::start(uint16_t firstPort, int tries, Handler handler) {
    stop();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    for (int i = 0; i < tries; ++i) {
        const uint16_t p = uint16_t(firstPort + i);
        auto s = std::uintptr_t(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (s == kInvalid) continue;
#ifdef _WIN32
        BOOL excl = TRUE;   // never share the port with another process
        setsockopt(SOCKET(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#endif
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(p);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost only
        if (::bind(sock_t(s), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && ::listen(sock_t(s), 8) == 0) {
            listen_ = s;
            port_ = p;
            handler_ = std::move(handler);
            running_ = true;
            thread_ = std::thread([this] { loop(); });
            return true;
        }
        closeSock(s);
    }
    return false;
}

void HttpServer::stop() {
    if (running_.exchange(false) && thread_.joinable()) thread_.join();
    if (listen_ != kInvalid) { closeSock(listen_); listen_ = kInvalid; }
    port_ = 0;
}

void HttpServer::loop() {
    while (running_) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sock_t(listen_), &set);
        timeval tv { 0, 200000 };
        const int r = ::select(int(listen_ + 1), &set, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        sockaddr_in from {};
        socklen_type len = sizeof from;
        const auto c = std::uintptr_t(::accept(sock_t(listen_), reinterpret_cast<sockaddr*>(&from), &len));
        if (c == kInvalid) continue;
        serve(c);
        closeSock(c);
    }
}

void HttpServer::serve(std::uintptr_t c) {
#ifdef _WIN32
    DWORD timeout = 1000;
    setsockopt(SOCKET(c), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
#else
    timeval timeout { 1, 0 };
    setsockopt(sock_t(c), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#endif
    std::string raw;
    char buf[2048];
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
        const int n = int(::recv(sock_t(c), buf, sizeof buf, 0));
        if (n <= 0) return;
        raw.append(buf, size_t(n));
    }

    HttpRequest req;
    std::istringstream in(raw);
    std::string target, version, line;
    in >> req.method >> target >> version;
    std::getline(in, line);
    while (std::getline(in, line) && line != "\r" && !line.empty()) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto key = lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ')) value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        if (key == "host") req.host = value;
    }
    const auto q = target.find('?');
    req.path = urlDecode(target.substr(0, q));
    if (q != std::string::npos) {
        std::istringstream qs(target.substr(q + 1));
        std::string kv;
        while (std::getline(qs, kv, '&')) {
            const auto eq = kv.find('=');
            if (eq == std::string::npos) req.query[urlDecode(kv)] = "";
            else req.query[urlDecode(kv.substr(0, eq))] = urlDecode(kv.substr(eq + 1));
        }
    }

    HttpResponse res = handler_ ? handler_(req) : HttpResponse { 404, "text/plain", "not found" };
    std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\n"
                     + "Content-Type: " + res.contentType + "\r\n"
                     + "Content-Length: " + std::to_string(res.body.size()) + "\r\n"
                     + "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
    head += res.body;
    size_t sent = 0;
    while (sent < head.size()) {
        const int n = int(::send(sock_t(c), head.data() + sent, int(head.size() - sent), 0));
        if (n <= 0) break;
        sent += size_t(n);
    }
}

} // namespace hearaside
