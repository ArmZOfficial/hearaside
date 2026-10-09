// HEARASIDE REST API routes (docs/rest-api.md). Runs on the message thread (ControlServer::service
// from HubProcessor's timer). Every change goes through the Hub's parameters or the Tracks'
// mailboxes, exactly like a click in the Hub: the DAW can undo / automate / save it.
#include "HubProcessor.h"

namespace hearaside {

namespace {

using Response = ControlServer::Response;

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> fields) {
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : fields) o->setProperty(k, v);
    return juce::var(o);
}

Response fail(int status, const juce::String& message) { return { status, obj({ { "error", message } }) }; }

// {"on": true/false}; no "on" = toggle
bool wantOn(const juce::var& body, bool current) {
    const auto v = body.getProperty("on", juce::var());
    return v.isBool() || v.isInt() || v.isDouble() ? bool(v) : !current;
}

bool number(const juce::var& body, const char* key, double& out) {
    const auto v = body.getProperty(key, juce::var());
    if (!(v.isInt() || v.isInt64() || v.isDouble())) return false;
    out = double(v);
    return std::isfinite(out);
}

} // namespace

ControlServer::Response HubProcessor::handleControl(const ControlServer::Request& req) {
    const auto parts = juce::StringArray::fromTokens(req.path.fromFirstOccurrenceOf("/api/v1", false, false), "/", "");
    juce::StringArray seg;
    for (const auto& p : parts) if (p.isNotEmpty()) seg.add(juce::URL::removeEscapeChars(p));
    if (!req.path.startsWith("/api/v1")) return fail(404, "unknown path (try GET /api/v1)");
    const bool get = req.method == "GET", post = req.method == "POST";
    const auto& body = req.body;
    auto on = [this](const char* id) { return apvts_.getRawParameterValue(id)->load() > 0.5f; };

    auto trackJson = [](const TrackView& v) {
        return obj({ { "slot", v.slot }, { "name", v.name }, { "active", v.active }, { "you_hear", v.mon }, { "viewers_hear", v.str },
                     { "headphone_db", v.trimDb }, { "viewers_db", v.gainDb }, { "delay_ms", v.delayMs }, { "solo", v.solo },
                     { "bypassed", v.bypassed } });
    };
    auto sourceJson = [](const SourceView& s) {
        return obj({ { "index", s.index }, { "name", s.name }, { "program", s.app }, { "on", s.on() }, { "active", s.active },
                     { "you_hear", s.mon }, { "viewers_hear", s.str }, { "headphone_db", s.trimDb }, { "viewers_db", s.gainDb },
                     { "delay_ms", s.delayMs }, { "recording", s.recording() } });
    };
    auto state = [&] {
        juce::Array<juce::var> tracks, sources;
        for (const auto& v : this->tracks()) tracks.add(trackJson(v));
        for (const auto& s : this->sources()) sources.add(sourceJson(s));
        double lufs = -100.0;
        if (auto* bus = engine_.bus()) lufs = ssbus::bitsFloat(bus->streamHeader.loudnessSBits.load(std::memory_order_relaxed));
        const auto li = latency();
        const auto hub = obj({ { "stream_muted", on(hubparam::Panic) }, { "hearing_viewers_mix", on(hubparam::Preview) },
                               { "stream_db", double(apvts_.getRawParameterValue(hubparam::Master)->load()) },
                               { "headphones_db", double(apvts_.getRawParameterValue(hubparam::Headphones)->load()) },
                               { "peak_protection", on(hubparam::LimiterOn) },
                               { "loudness_lufs", lufs < -70.0 ? juce::var() : juce::var(std::round(lufs * 10.0) / 10.0) },
                               { "obs_connected", obsConnected() }, { "viewers_silent", viewersSilent_ }, { "latency_to_obs_ms", li.obs ? juce::var(li.total()) : juce::var() },
                               { "daw_buffer", li.block }, { "sharing", sharing() },
                               { "listen_link", sharing() ? (permanentLinksSet() ? permanentUrl(true) : share_.listenUrl()) : juce::String() } });
        return obj({ { "hub", hub }, { "tracks", tracks }, { "sources", sources } });
    };
    auto findTrack = [&](const juce::String& key) -> std::optional<TrackView> {
        for (const auto& v : this->tracks())
            if ((key.containsOnly("0123456789") && v.slot == key.getIntValue()) || v.name.equalsIgnoreCase(key)) return v;
        return std::nullopt;
    };
    auto findSource = [&](const juce::String& key) -> std::optional<SourceView> {
        for (const auto& s : this->sources())
            if ((key.containsOnly("0123456789") && s.index == key.getIntValue()) || s.name.equalsIgnoreCase(key)) return s;
        return std::nullopt;
    };
    // the per-row settings shared by Tracks and App Audio (through their headphone slot)
    auto applyRow = [&](int slot, bool mon, bool str) -> juce::String {
        if (slot < 0) return "this App Audio is too old for remote levels";
        double v = 0.0;
        if (body.hasProperty("you_hear")) send(slot, ssbus::ParamId::Mon, wantOn(obj({ { "on", body["you_hear"] } }), mon) ? 1.0f : 0.0f);
        if (body.hasProperty("viewers_hear")) send(slot, ssbus::ParamId::Str, wantOn(obj({ { "on", body["viewers_hear"] } }), str) ? 1.0f : 0.0f);
        if (number(body, "headphone_db", v)) send(slot, ssbus::ParamId::MonTrimDb, float(juce::jlimit(-60.0, 6.0, v)));
        if (number(body, "viewers_db", v)) send(slot, ssbus::ParamId::StrGainDb, float(juce::jlimit(-60.0, 6.0, v)));
        return {};
    };

    if (seg.isEmpty() && get)
        return { 200, obj({ { "name", "HEARASIDE Hub" }, { "version", HEARASIDE_VERSION }, { "docs", "docs/rest-api.md" },
                            { "endpoints", juce::StringArray { "GET  /api/v1/state", "POST /api/v1/mute {on?}", "POST /api/v1/preview {on?}",
                                                               "POST /api/v1/limiter {on?}", "POST /api/v1/stream {db}", "POST /api/v1/headphones {db}",
                                                               "GET  /api/v1/tracks", "POST /api/v1/tracks/<slot|name> {you_hear?, viewers_hear?, headphone_db?, viewers_db?, delay_ms?, solo?}",
                                                               "GET  /api/v1/sources", "POST /api/v1/sources/<index|name> {on?, you_hear?, viewers_hear?, headphone_db?, viewers_db?, record?}",
                                                               "POST /api/v1/sync {action: start|cancel}", "POST /api/v1/share {on?}" } } }) };
    if (seg.isEmpty()) return fail(405, "GET only");
    const auto what = seg[0];
    if (what == "state") return get ? Response { 200, state() } : fail(405, "GET only");

    if (what == "tracks" && seg.size() == 1 && get) return { 200, state()["tracks"] };
    if (what == "sources" && seg.size() == 1 && get) return { 200, state()["sources"] };
    if (!post) return fail(405, "POST only");
    if (!body.isVoid() && !body.isObject()) return fail(400, "the body must be a JSON object");

    // the switches that change what the viewers get are applied at once, like the buttons
    if (what == "mute") { setParam(hubparam::Panic, wantOn(body, on(hubparam::Panic)) ? 1.0f : 0.0f); return { 200, state()["hub"] }; }
    if (what == "preview") { setParam(hubparam::Preview, wantOn(body, on(hubparam::Preview)) ? 1.0f : 0.0f); return { 200, state()["hub"] }; }
    if (what == "limiter") { setParam(hubparam::LimiterOn, wantOn(body, on(hubparam::LimiterOn)) ? 1.0f : 0.0f); return { 200, state()["hub"] }; }
    if (what == "stream" || what == "headphones") {
        double db = 0.0;
        if (!number(body, "db", db)) return fail(400, "needs {\"db\": number}");
        setParam(what == "stream" ? hubparam::Master : hubparam::Headphones, float(db));   // the parameter clamps it
        return { 200, state()["hub"] };
    }
    if (what == "tracks" && seg.size() == 2) {
        const auto t = findTrack(seg[1]);
        if (!t) return fail(404, "no track \"" + seg[1] + "\" (GET /api/v1/tracks)");
        if (const auto err = applyRow(t->slot, t->mon, t->str); err.isNotEmpty()) return fail(400, err);
        double v = 0.0;
        if (number(body, "delay_ms", v)) send(t->slot, ssbus::ParamId::StrDelayMs, float(juce::jlimit(0.0, 500.0, v)));
        if (body.hasProperty("solo")) send(t->slot, ssbus::ParamId::StrSolo, wantOn(obj({ { "on", body["solo"] } }), t->solo) ? 1.0f : 0.0f);
        return { 200, obj({ { "ok", true }, { "slot", t->slot }, { "name", t->name } }) };   // the Track applies it within one block
    }
    if (what == "sources" && seg.size() == 2) {
        const auto s = findSource(seg[1]);
        if (!s) return fail(404, "no App Audio \"" + seg[1] + "\" (GET /api/v1/sources)");
        if (body.hasProperty("you_hear") || body.hasProperty("viewers_hear") || body.hasProperty("headphone_db") || body.hasProperty("viewers_db"))
            if (const auto err = applyRow(s->slot, s->mon, s->str); err.isNotEmpty()) return fail(400, err);
        if (body.hasProperty("on")) sendSource(s->index, ssbus::SourceParam::On, wantOn(body, s->on()) ? 1.0f : 0.0f);
        if (body.hasProperty("record")) sendSource(s->index, ssbus::SourceParam::Record, wantOn(obj({ { "on", body["record"] } }), s->recording()) ? 1.0f : 0.0f);
        return { 200, obj({ { "ok", true }, { "index", s->index }, { "name", s->name } }) };
    }
    if (what == "sync") {
        const auto action = body.getProperty("action", "start").toString();
        if (action == "cancel") cancelAutoSync();
        else if (action == "start") startAutoSync();
        else return fail(400, "action is start or cancel");
        return { 200, obj({ { "ok", true } }) };
    }
    if (what == "share") { setSharing(wantOn(body, sharing())); return { 200, state()["hub"] }; }
    return fail(404, "unknown path (try GET /api/v1)");
}

void HubProcessor::serviceControl() {
    // only the Hub that owns the bus answers (a second Hub only passes audio through)
    const bool want = settings_->restApi() && engine_.role() == ssengine::HubEngine::Role::Owner;
    if (want && settings_->restApiKey().isEmpty()) settings_->setRestApiKey(ControlServer::newKey());
    const auto now = juce::Time::getMillisecondCounter();
    if (want && (!control_.running() || controlKey_ != settings_->restApiKey()) && now - controlTry_ > 5000) {
        controlTry_ = now;   // (a busy port is tried again every 5 s, not 20 times a second)
        controlKey_ = settings_->restApiKey();
        control_.start(controlKey_);
    }
    if (!want && control_.running()) control_.stop();
    if (control_.running()) control_.service([this](const ControlServer::Request& r) { return handleControl(r); });
}

} // namespace hearaside
