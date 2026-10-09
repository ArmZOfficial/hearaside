// HEARASIDE REST API (docs/rest-api.md): control the Hub from Stream Deck, scripts or bots.
//   http://127.0.0.1:47800/api/v1/...   JSON in / out, "Authorization: Bearer <key>"
// Listens on 127.0.0.1 only and checks the Host header, so neither the network nor a web page in
// a browser can reach it. Requests are parsed on socket threads but run on the message thread
// (HubProcessor's timer calls service()), where every change goes through the host's parameters:
// undo, automation and saving keep working.
#pragma once

#include <juce_core/juce_core.h>

#include <deque>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

namespace hearaside {

class ControlServer {
public:
    struct Request {
        juce::String method, path;   // "GET", "/api/v1/state"
        juce::var body;              // parsed JSON (undefined when there is none)
    };
    struct Response {
        int status = 200;
        juce::var body;
    };
    using Handler = std::function<Response(const Request&)>;

    static constexpr int kFirstPort = 47800;

    ControlServer();
    ~ControlServer();

    bool start(const juce::String& key);   // message thread; false = no free port
    void stop();
    bool running() const noexcept { return running_.load(); }
    int port() const noexcept { return port_; }
    juce::String baseUrl() const { return "http://127.0.0.1:" + juce::String(port_); }

    // message thread: answers the queued requests with `handler`
    void service(const Handler& handler);

    static juce::String newKey();   // 32 hex characters from the OS's secure random source

private:
    struct Conn;
    struct Job {
        Request request;
        std::promise<Response> done;
    };
    void acceptLoop();
    void serve(Conn&);

    std::atomic<bool> running_ { false };
    std::unique_ptr<juce::StreamingSocket> listener_;
    std::thread acceptThread_;
    std::list<std::unique_ptr<Conn>> conns_;
    std::mutex connsMutex_, jobsMutex_;
    std::deque<std::shared_ptr<Job>> jobs_;
    juce::String key_;
    int port_ = 0;
};

} // namespace hearaside
