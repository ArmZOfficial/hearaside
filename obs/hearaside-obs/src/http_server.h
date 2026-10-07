// Minimal single-threaded HTTP/1.1 server for the OBS control dock. Listens on 127.0.0.1 only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace hearaside {

struct HttpRequest {
    std::string method, path, host;
    std::map<std::string, std::string> query;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    ~HttpServer() { stop(); }
    // Binds the first free port in [firstPort, firstPort + tries). Returns false if none.
    bool start(uint16_t firstPort, int tries, Handler handler);
    void stop();
    uint16_t port() const noexcept { return port_; }

private:
    void loop();
    void serve(std::uintptr_t client);

    Handler handler_;
    std::thread thread_;
    std::atomic<bool> running_ { false };
    std::uintptr_t listen_ = ~std::uintptr_t(0);
    uint16_t port_ = 0;
};

std::string urlDecode(const std::string& s);
std::string jsonEscape(const std::string& s);

} // namespace hearaside
