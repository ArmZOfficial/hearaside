#include "ShareDirectory.h"

namespace hearaside {

ShareDirectory::ShareDirectory() : juce::Thread("HEARASIDE share directory") {}

ShareDirectory::~ShareDirectory() {
    signalThreadShouldExit();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (current_ != nullptr) current_->cancel();   // never keep the DAW waiting on the network
    }
    notify();
    stopThread(3000);
}

void ShareDirectory::update(const juce::String& base, const juce::String& listenToken, const juce::String& sendToken,
                            const juce::String& secret, const juce::String& tunnel) {
    const Config c { base.trimCharactersAtEnd("/"), listenToken, sendToken, secret, tunnel };
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (c == wanted_ && !unregister_) return;
        wanted_ = c;
        unregister_ = false;
    }
    if (!isThreadRunning()) startThread(juce::Thread::Priority::low);
    notify();
}

void ShareDirectory::stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (wanted_.tunnel.isEmpty() && !registeredValid_) return;
        wanted_.tunnel = {};
        unregister_ = true;
    }
    notify();
}

juce::String ShareDirectory::listenUrl() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return wanted_.base.isEmpty() || wanted_.listen.isEmpty() ? juce::String() : wanted_.base + "/l/" + wanted_.listen;
}

juce::String ShareDirectory::sendUrl() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return wanted_.base.isEmpty() || wanted_.send.isEmpty() ? juce::String() : wanted_.base + "/s/" + wanted_.send;
}

juce::String ShareDirectory::originOf(const juce::String& url) {
    const auto scheme = url.upToFirstOccurrenceOf("://", false, false).toLowerCase();
    if (scheme.isEmpty() || !url.contains("://")) return {};
    const auto rest = url.fromFirstOccurrenceOf("://", false, false).upToFirstOccurrenceOf("/", false, false).toLowerCase();
    return rest.isEmpty() ? juce::String() : scheme + "://" + rest;
}

juce::String ShareDirectory::origin() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return originOf(wanted_.base);
}

int ShareDirectory::post(const juce::String& path, const juce::String& json) {
    juce::String base;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        base = wanted_.base.isNotEmpty() ? wanted_.base : registered_.base;
    }
    juce::WebInputStream ws(juce::URL(base + path).withPOSTData(json), true);
    ws.withExtraHeaders("Content-Type: application/json").withConnectionTimeout(5000).withNumRedirectsToFollow(0);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (threadShouldExit()) return 0;
        current_ = &ws;
    }
    int code = 0;
    if (ws.connect(nullptr)) {
        code = ws.getStatusCode();
        ws.readEntireStreamAsString();   // small JSON; never logged (the request carries the secret)
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    current_ = nullptr;
    return code;
}

static juce::String body(const juce::String& secret, const juce::String& listen, const juce::String& send, const juce::String& tunnel) {
    auto* tokens = new juce::DynamicObject();
    if (listen.isNotEmpty()) tokens->setProperty("l", listen);
    if (send.isNotEmpty()) tokens->setProperty("s", send);
    auto* o = new juce::DynamicObject();
    o->setProperty("secret", secret);
    o->setProperty("tokens", juce::var(tokens));
    if (tunnel.isNotEmpty()) o->setProperty("tunnel", tunnel);
    return juce::JSON::toString(juce::var(o), true);
}

void ShareDirectory::run() {
    int backoff = retryMs_.load();
    while (!threadShouldExit()) {
        Config want;
        bool unregister;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            want = wanted_;
            unregister = unregister_;
        }
        const bool active = want.base.isNotEmpty() && want.tunnel.isNotEmpty() && want.secret.isNotEmpty()
                         && (want.listen.isNotEmpty() || want.send.isNotEmpty());
        if (!active) {
            if (unregister && registeredValid_) {   // sharing switched off: take the links offline now
                post("/api/unregister", body(registered_.secret, registered_.listen, registered_.send, {}));
                const std::lock_guard<std::mutex> lock(mutex_);
                if (unregister_) unregister_ = false;
            }
            registeredValid_ = false;
            state_.store(State::Off);
            wait(-1);
            continue;
        }
        if (!registeredValid_ || !(registered_ == want)) state_.store(State::Registering);
        // registering again is also the heartbeat (the site forgets an address after 90 s)
        const int code = post("/api/register", body(want.secret, want.listen, want.send, want.tunnel));
        if (threadShouldExit()) break;
        if (code == 200) {
            registered_ = want;
            registeredValid_ = true;
            state_.store(State::Online);
            backoff = retryMs_.load();
            wait(heartbeatMs_.load());
        } else {
            registeredValid_ = registeredValid_ && registered_ == want;
            state_.store(State::Unreachable);
            // 400 / 403 will not fix themselves quickly: retry slowly
            wait(code == 400 || code == 403 ? juce::jmax(backoff, 60000) : backoff);
            backoff = juce::jmin(backoff * 2, 60000);
        }
    }
}

} // namespace hearaside
