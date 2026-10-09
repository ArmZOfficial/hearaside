// The friends of the bus this process is on, for labels and pickers (HEARASIDE App Audio, HEARASIDE
// Track, the Hub). Whoever is connected to the bus calls update() from its message-thread timer; the
// labels ("Mint · friend") are then available anywhere in the process without touching shared memory.
// A friend is chosen by id, never by token: the link itself never leaves the Hub.
#pragma once

#include "ssbus/bus.h"

#include <juce_core/juce_core.h>

#include <mutex>
#include <vector>

namespace hearaside {

class FriendDirectory {
public:
    struct Info {
        uint32_t id = 0;
        int slot = -1;
        juce::String name;
        uint32_t state = ssbus::kFriendFree;   // ssbus::FriendState
        float delayMs = -1.0f;                 // how late the voice arrives (-1 = not measured)
        bool inDaw = false;                    // a DAW track carries them (route = Daw)
        int feeder = -1, outSlot = -1;
        bool live() const noexcept { return state == ssbus::kFriendLive; }
    };

    static constexpr const char* kPrefix = "*friend:";   // an App Audio source string: "*friend:101*"

    static juce::String appFor(uint32_t id) { return juce::String(kPrefix) + juce::String(id) + "*"; }
    static bool isFriendApp(const juce::String& app, uint32_t* id = nullptr) {
        if (!app.startsWith(kPrefix) || !app.endsWith("*")) return false;
        const auto digits = app.substring(int(std::strlen(kPrefix)), app.length() - 1);
        if (digits.isEmpty() || !digits.containsOnly("0123456789")) return false;
        if (id) *id = uint32_t(digits.getLargeIntValue());
        return true;
    }

    // message thread, any bus (nullptr = no bus: the list empties)
    static void update(const ssbus::BusLayout* bus) {
        std::vector<Info> out;
        if (bus != nullptr) {
            for (int s = 0; s < ssbus::kMaxFriends; ++s) {
                const auto& h = bus->friends[s];
                const uint32_t id = h.id.load(std::memory_order_acquire);
                if (id == 0) continue;
                Info i;
                i.id = id;
                i.slot = s;
                i.name = juce::String::fromUTF8(ssbus::readFriendName(h).c_str());
                i.state = h.state.load(std::memory_order_acquire);
                i.delayMs = ssbus::bitsFloat(h.delayBits.load(std::memory_order_relaxed));
                i.inDaw = h.route.load(std::memory_order_relaxed) == ssbus::kRouteDaw;
                i.feeder = h.feeder.load(std::memory_order_relaxed);
                i.outSlot = h.outSlot.load(std::memory_order_relaxed);
                out.push_back(std::move(i));
            }
        }
        const std::lock_guard<std::mutex> lock(mutex());
        list() = std::move(out);
    }
    static std::vector<Info> all() {
        const std::lock_guard<std::mutex> lock(mutex());
        return list();
    }
    static bool find(uint32_t id, Info& out) {
        const std::lock_guard<std::mutex> lock(mutex());
        for (const auto& i : list()) if (i.id == id) { out = i; return true; }
        return false;
    }
    static juce::String nameOf(uint32_t id) {
        Info i;
        return find(id, i) ? i.name : juce::String();
    }

private:
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::vector<Info>& list() { static std::vector<Info> l; return l; }
};

} // namespace hearaside
