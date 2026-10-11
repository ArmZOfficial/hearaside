// Permanent share links (docs/ux-roadmap.md, 8): tells the share web site (Vercel) where this
// Hub's cloudflared tunnel is, so https://<site>/l/<token> keeps working when the tunnel address
// changes. Only the address is sent - the audio still goes straight from the Hub to the listener.
// All network calls run on its own thread; nothing here ever waits on the message thread.
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <mutex>

namespace hearaside {

class ShareDirectory : private juce::Thread {
public:
    enum class State { Off, Registering, Online, Unreachable };

    ShareDirectory();
    ~ShareDirectory() override;   // quick: cancels a request in flight, does not unregister

    // message thread. base: "https://hearaside.vercel.app" ("" = no permanent links).
    // tunnel: the Hub's https tunnel ("" = none: the links go offline).
    // friends: every friend's send-in token (the friends room), registered like the send token.
    void update(const juce::String& base, const juce::String& listenToken, const juce::String& sendToken,
                const juce::String& secret, const juce::String& tunnel, const juce::StringArray& friends = {});
    // The account's access token for this base (thread; "" = none): the site then lists the links
    // in Account › Links and friends. Only ever given for the account's own server.
    void setBearer(std::function<juce::String(const juce::String& base)> f) { const std::lock_guard<std::mutex> l(mutex_); bearer_ = std::move(f); }
    // Tokens the site said were revoked on the account website (message thread takes them).
    juce::StringArray takeRevoked() { const std::lock_guard<std::mutex> l(mutex_); auto r = revoked_; revoked_.clear(); return r; }
    void stop();                  // sharing switched off: unregister (best effort, on the thread)

    State state() const noexcept { return state_.load(); }
    juce::String listenUrl() const;   // "" unless a base is set
    juce::String sendUrl() const;
    juce::String origin() const;      // scheme://host of the base, for the WebSocket Origin check

    // tests: heartbeat interval and first retry delay (ms)
    void setTimings(int heartbeatMs, int retryMs) { heartbeatMs_ = heartbeatMs; retryMs_ = retryMs; }

    static juce::String originOf(const juce::String& url);

private:
    void run() override;
    int post(const juce::String& path, const juce::String& json, juce::String* reply = nullptr);   // HTTP status, 0 = no answer

    struct Config {
        juce::String base, listen, send, secret, tunnel;
        juce::StringArray friends;
        bool operator==(const Config&) const = default;
    };
    mutable std::mutex mutex_;
    Config wanted_, registered_;      // registered_: what the site has (thread only)
    std::atomic<bool> registeredValid_{ false };   // written by run(), read by stop() on the message thread
    bool unregister_ = false;                   // stop() asked to take the links offline (guarded by mutex_)
    std::atomic<State> state_ { State::Off };
    std::atomic<int> heartbeatMs_ { 30000 }, retryMs_ { 2000 };
    juce::WebInputStream* current_ = nullptr;   // the request in flight (guarded by mutex_), for cancel()
    std::function<juce::String(const juce::String&)> bearer_;   // guarded by mutex_
    juce::StringArray revoked_;                                  // guarded by mutex_
};

} // namespace hearaside
