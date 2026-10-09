#include "FriendRoom.h"

#include <algorithm>

namespace hearaside {

using namespace ssbus;

const FriendRoom::Entry* FriendRoom::find(uint32_t id) const noexcept {
    for (const auto& e : entries_) if (e.id == id) return &e;
    return nullptr;
}

FriendRoom::Entry* FriendRoom::get(uint32_t id) {
    for (auto& e : entries_) if (e.id == id) return &e;
    return nullptr;
}

int FriendRoom::freeSlot() const {
    for (int s = 0; s < kMax; ++s) {
        bool used = false;
        for (const auto& e : entries_) used |= e.slot == s;
        if (!used) return s;
    }
    return -1;
}

int FriendRoom::slotOf(uint32_t id) const {
    const auto* e = find(id);
    return e ? e->slot : -1;
}

juce::String FriendRoom::token(uint32_t id) const {
    const auto* e = find(id);
    return e ? e->token : juce::String();
}

uint32_t FriendRoom::add(const juce::String& name) {
    const int slot = freeSlot();
    if (slot < 0) return 0;
    const auto token = ShareServer::newToken();
    if (token.length() < 24) return 0;   // no secure random: no link rather than a guessable one
    Entry e;
    e.id = nextId_++;
    e.slot = slot;
    e.token = token;
    e.name = name.trim().isNotEmpty() ? name.trim() : defaultName(entries_.size() + 1);
    entries_.add(e);
    touch();
    return e.id;
}

bool FriendRoom::remove(uint32_t id) {
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_.getReference(i).id != id) continue;
        entries_.remove(i);
        touch();
        return true;
    }
    return false;
}

juce::String FriendRoom::newLink(uint32_t id) {
    auto* e = get(id);
    if (!e) return {};
    const auto t = ShareServer::newToken();
    if (t.length() < 24) return {};
    e->token = t;
    touch();
    return t;
}

void FriendRoom::rename(uint32_t id, const juce::String& name) {
    if (auto* e = get(id)) {
        const auto n = name.trim().isNotEmpty() ? name.trim() : defaultName(e->slot + 1);
        if (e->name != n) { e->name = n; touch(); }
    }
}

void FriendRoom::setVolumeDb(uint32_t id, float db) { if (auto* e = get(id)) e->volumeDb = juce::jlimit(-60.0f, 6.0f, db); }
void FriendRoom::setPan(uint32_t id, float pan) { if (auto* e = get(id)) e->pan = juce::jlimit(-1.0f, 1.0f, pan); }
void FriendRoom::setMon(uint32_t id, bool on) { if (auto* e = get(id)) e->mon = on; }
void FriendRoom::setStr(uint32_t id, bool on) { if (auto* e = get(id)) e->str = on; }
void FriendRoom::setSolo(uint32_t id, bool on) { if (auto* e = get(id)) e->solo = on; }

void FriendRoom::spreadOut() {
    const int n = entries_.size();
    if (n < 2) return;
    for (int i = 0; i < n; ++i) entries_.getReference(i).pan = -0.6f + 1.2f * float(i) / float(n - 1);
}

bool FriendRoom::changed() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

// ---- project state: ids, names, tokens and levels. The tokens ARE the links: they live in the Hub's
// state (the project) and nowhere else (never in a log).
juce::ValueTree FriendRoom::toState() const {
    juce::ValueTree t("FRIENDS");
    t.setProperty("nextId", int(nextId_), nullptr);
    for (const auto& e : entries_) {
        juce::ValueTree f("FRIEND");
        f.setProperty("id", int(e.id), nullptr);
        f.setProperty("name", e.name, nullptr);
        f.setProperty("token", e.token, nullptr);
        f.setProperty("vol", e.volumeDb, nullptr);
        f.setProperty("pan", e.pan, nullptr);
        f.setProperty("mon", e.mon, nullptr);
        f.setProperty("str", e.str, nullptr);
        f.setProperty("solo", e.solo, nullptr);
        t.appendChild(f, nullptr);
    }
    return t;
}

void FriendRoom::fromState(const juce::ValueTree& t) {
    entries_.clear();
    nextId_ = uint32_t(juce::jmax(100, int(t.getProperty("nextId", 100))));
    for (int i = 0; i < t.getNumChildren() && entries_.size() < kMax; ++i) {
        const auto f = t.getChild(i);
        Entry e;
        e.id = uint32_t(int(f.getProperty("id", 0)));
        e.token = f.getProperty("token", "").toString();
        if (e.id == 0 || e.token.length() < 24) continue;   // a damaged entry: skip it rather than make up a link
        bool dup = false;
        for (const auto& o : entries_) dup |= o.id == e.id;
        if (dup) continue;
        e.slot = freeSlot();
        e.name = f.getProperty("name", defaultName(entries_.size() + 1)).toString();
        e.volumeDb = float(double(f.getProperty("vol", 0.0)));
        e.pan = float(double(f.getProperty("pan", 0.0)));
        e.mon = bool(f.getProperty("mon", true));
        e.str = bool(f.getProperty("str", true));
        e.solo = bool(f.getProperty("solo", false));
        nextId_ = juce::jmax(nextId_, e.id + 1);
        entries_.add(e);
    }
    touch();
}

// ---- every tick -----------------------------------------------------------------------------------
void FriendRoom::service(BusLayout* bus, ssengine::HubEngine& engine, ShareServer& share, const juce::String& host) {
    if (tableDirty_ || host != publishedHost_) {
        tableDirty_ = false;
        std::vector<ShareServer::FriendLink> links;
        for (const auto& e : entries_) links.push_back({ e.slot, e.id, e.token, e.name, host });
        share.setFriendLinks(std::move(links));
        publishedHost_ = host;
    }
    // a friend who left the room: end their connection (the table above no longer knows the token)
    for (int s = 0; s < kMax; ++s) {
        bool used = false;
        for (const auto& e : entries_) used |= e.slot == s;
        if (!used && bus != nullptr && bus->friends[s].id.load() != 0) share.kickFriend(s);
    }
    for (const auto& e : entries_) {
        auto& c = engine.friendControl(e.slot);
        c.flags.store((e.mon ? ssengine::HubEngine::kFriendMon : 0u) | (e.str ? ssengine::HubEngine::kFriendStr : 0u)
                      | (e.solo ? ssengine::HubEngine::kFriendSolo : 0u), std::memory_order_relaxed);
        c.gainBits.store(floatBits(e.volumeDb), std::memory_order_relaxed);
        c.panBits.store(floatBits(e.pan), std::memory_order_relaxed);
    }
    if (bus == nullptr) { published_ = nullptr; return; }
    if (bus == published_ && !pushAll_) return;
    // ids and names into shared memory (App Audio / HEARASIDE Track list them); the share server owns the live state
    for (int s = 0; s < kMax; ++s) {
        FriendHeader& h = bus->friends[s];
        const Entry* e = nullptr;
        for (const auto& x : entries_) if (x.slot == s) e = &x;
        if (e == nullptr) {
            if (h.id.load() != 0) {
                h.state.store(kFriendFree, std::memory_order_release);
                h.id.store(0, std::memory_order_release);
                h.route.store(kRouteDirect, std::memory_order_relaxed);
                h.feeder.store(-1, std::memory_order_relaxed);
                h.outSlot.store(-1, std::memory_order_relaxed);
                h.peakBits.store(0, std::memory_order_relaxed);
                setFriendName(h, {});
            }
            continue;
        }
        setFriendName(h, e->name.toStdString());
        if (h.id.load() != e->id) {   // a new friend in this slot
            h.state.store(kFriendWaiting, std::memory_order_release);
            h.id.store(e->id, std::memory_order_release);
            h.route.store(kRouteDirect, std::memory_order_relaxed);
            h.feeder.store(-1, std::memory_order_relaxed);
            h.outSlot.store(-1, std::memory_order_relaxed);
            h.delayBits.store(floatBits(-1.0f), std::memory_order_relaxed);
        } else if (h.state.load() == kFriendFree) {
            h.state.store(kFriendWaiting, std::memory_order_release);
        }
    }
    published_ = bus;
    pushAll_ = false;
}

std::vector<FriendView> FriendRoom::views(const BusLayout* bus, const ssengine::HubEngine& engine) const {
    std::vector<FriendView> out;
    for (const auto& e : entries_) {
        FriendView v;
        v.id = e.id; v.slot = e.slot; v.name = e.name;
        v.volumeDb = e.volumeDb; v.pan = e.pan; v.mon = e.mon; v.str = e.str; v.solo = e.solo;
        if (bus != nullptr && bus->friends[e.slot].id.load() == e.id) {
            const FriendHeader& h = bus->friends[e.slot];
            switch (h.state.load(std::memory_order_acquire)) {
                case kFriendLive:    v.state = FriendView::State::Live; break;
                case kFriendOffline: v.state = FriendView::State::Offline; break;
                default:             v.state = FriendView::State::Waiting; break;
            }
            v.delayMs = bitsFloat(h.delayBits.load(std::memory_order_relaxed));
            v.peak = bitsFloat(h.peakBits.load(std::memory_order_relaxed));
            v.inDaw = h.route.load(std::memory_order_relaxed) == kRouteDaw;
            v.feeder = h.feeder.load(std::memory_order_relaxed);
            v.outSlot = h.outSlot.load(std::memory_order_relaxed);
            const auto& info = engine.friendInfo(e.slot);
            v.playing = info.playing.load(std::memory_order_relaxed) != 0;
            v.totalMs = bitsFloat(info.totalMsBits.load(std::memory_order_relaxed));
            v.lineMs = bitsFloat(info.lineMsBits.load(std::memory_order_relaxed));
            v.overLimit = info.overLimit.load(std::memory_order_relaxed) != 0;
        }
        out.push_back(v);
    }
    return out;
}

} // namespace hearaside
