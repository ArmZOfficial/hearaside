// The friends room of a Hub (S2): up to eight friends, each with a send-in link of their own.
// Message thread only. It owns the list (names, tokens, levels), saves it in the Hub's project
// state, publishes ids and names into shared memory (so App Audio and HEARASIDE Track can list
// them), pushes the levels to the engine, and hands the share server its table of links.
// Audio never goes through here.
#pragma once

#include "ShareServer.h"
#include "ssengine/engine.h"

#include <juce_data_structures/juce_data_structures.h>

namespace hearaside {

// What the screens show for one friend.
struct FriendView {
    uint32_t id = 0;
    int slot = -1;
    juce::String name;
    enum class State { Waiting, Live, Offline } state = State::Waiting;   // never opened the link / singing / left
    float volumeDb = 0.0f, pan = 0.0f;
    bool mon = true, str = true, solo = false;
    float delayMs = -1.0f;      // how late the voice arrives (network + browser), -1 = not measured yet
    float totalMs = 0.0f;       // + the Hub's small jitter buffer: what Line up works with
    float lineMs = 0.0f;        // extra delay the Hub gives this friend so they land on the beat
    float peak = 0.0f;          // microphone level (linear)
    bool playing = false;       // the Hub is playing the audio right now
    bool overLimit = false;     // slower than the Line-up limit: not waited for
    bool inDaw = false;         // a DAW track carries this friend (S7), not the Hub
    int feeder = -1;            // that track's feeder index
    int outSlot = -1;           // the Track at the end of that channel (-1 = not paired yet)
    bool live() const noexcept { return state == State::Live; }
};

class FriendRoom {
public:
    static constexpr int kMax = ssbus::kMaxFriends;

    struct Entry {
        uint32_t id = 0;
        int slot = -1;
        juce::String name, token;
        float volumeDb = 0.0f, pan = 0.0f;
        bool mon = true, str = true, solo = false;
    };

    int count() const noexcept { return entries_.size(); }
    bool full() const noexcept { return entries_.size() >= kMax; }
    const juce::Array<Entry>& entries() const noexcept { return entries_; }
    const Entry* find(uint32_t id) const noexcept;

    // 0 = the room is full (or the OS gave no secure random). The name defaults to "Friend N".
    uint32_t add(const juce::String& name = {});
    bool remove(uint32_t id);                       // revokes the token: the friend's link stops working
    juce::String newLink(uint32_t id);              // "New link": a new token, the old link stops working ("" = failed)
    void rename(uint32_t id, const juce::String& name);
    void setVolumeDb(uint32_t id, float db);
    void setPan(uint32_t id, float pan);            // -1 .. +1, viewers only
    void setMon(uint32_t id, bool on);
    void setStr(uint32_t id, bool on);
    void setSolo(uint32_t id, bool on);
    void spreadOut();                               // L60 .. R60, equally apart (needs two friends)
    juce::String token(uint32_t id) const;          // the secret in the send-in link; never log it
    int  slotOf(uint32_t id) const;

    // project state
    juce::ValueTree toState() const;
    void fromState(const juce::ValueTree&);
    bool changed();                                 // the list changed since the last call (links / names): refresh the share table

    // each tick: publish to the bus, push levels to the engine, keep the share server's table current
    void service(ssbus::BusLayout* bus, ssengine::HubEngine& engine, ShareServer& share, const juce::String& host);
    std::vector<FriendView> views(const ssbus::BusLayout* bus, const ssengine::HubEngine& engine) const;

    // "Mint · friend" in lists: the name shown to people (also read by App Audio / Track from shared memory)
    static juce::String defaultName(int n) { return "Friend " + juce::String(n); }

private:
    int freeSlot() const;
    Entry* get(uint32_t id);
    void touch() { dirty_ = true; pushAll_ = true; tableDirty_ = true; }

    juce::Array<Entry> entries_;
    uint32_t nextId_ = 100;
    bool dirty_ = true, pushAll_ = true, tableDirty_ = true;
    ssbus::BusLayout* published_ = nullptr;   // the bus the headers were last written to
    juce::String publishedHost_;
};

} // namespace hearaside
