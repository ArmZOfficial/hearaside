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
                            const juce::String& secret, const juce::String& tunnel, const juce::StringArray& friends) {
    const Config c { base.trimCharactersAtEnd("/"), listenToken, sendToken, secret, tunnel, friends };
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

int ShareDirectory::post(const juce::String& path, const juce::String& json, juce::String* reply) {
    juce::String base;
    std::function<juce::String(const juce::String&)> bearer;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        base = wanted_.base.isNotEmpty() ? wanted_.base : registered_.base;
        bearer = bearer_;
    }
    juce::String headers = "Content-Type: application/json";
    if (const auto t = bearer ? bearer(base) : juce::String(); t.isNotEmpty()) headers << "\r\nAuthorization: Bearer " << t;
    juce::WebInputStream ws(juce::URL(base + path).withPOSTData(json), true);
    ws.withExtraHeaders(headers).withConnectionTimeout(5000).withNumRedirectsToFollow(0);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (threadShouldExit()) return 0;
        current_ = &ws;
    }
    int code = 0;
    if (ws.connect(nullptr)) {
        code = ws.getStatusCode();
        const auto text = ws.readEntireStreamAsString();   // small JSON; never logged (the request carries the secret)
        if (reply != nullptr) *reply = text;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    current_ = nullptr;
    return code;
}

static juce::String body(const juce::String& secret, const juce::String& listen, const juce::String& send, const juce::String& tunnel,
                         const juce::StringArray& friends) {
    auto* tokens = new juce::DynamicObject();
    if (listen.isNotEmpty()) tokens->setProperty("l", listen);
    if (send.isNotEmpty()) tokens->setProperty("s", send);
    if (!friends.isEmpty()) {
        juce::Array<juce::var> f;
        for (const auto& t : friends) f.add(t);
        tokens->setProperty("f", f);
    }
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
                         && (want.listen.isNotEmpty() || want.send.isNotEmpty() || !want.friends.isEmpty());
        if (!active) {
            if (unregister && registeredValid_) {   // sharing switched off: take the links offline now
                post("/api/unregister", body(registered_.secret, registered_.listen, registered_.send, {}, registered_.friends));
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
        juce::String reply;
        const int code = post("/api/register", body(want.secret, want.listen, want.send, want.tunnel, want.friends), &reply);
        if (threadShouldExit()) break;
        if (code == 200) {
            // links revoked on the account website: the Hub makes new ones (takeRevoked)
            if (const auto* r = juce::JSON::parse(reply)["revoked"].getArray(); r != nullptr && !r->isEmpty()) {
                const std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& t : *r) revoked_.addIfNotAlreadyThere(t.toString());
            }
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
