// Remote control of the HEARASIDE Hub from inside OBS (control dock + hotkeys).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ssbus/bus.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace hearaside {

// Keeps one read/write mapping per bus name; thread safe.
class BusClient {
public:
    static BusClient& instance();

    // Runs fn with the bus layout if the bus exists (DAW running). Returns false otherwise.
    bool with(const std::string& busName, const std::function<void(ssbus::BusLayout&)>& fn);

    // JSON snapshot for the dock (tracks, Hub state, meters).
    std::string stateJson(const std::string& busName, const std::string& lang);

    // Sends a command to the Hub (target -1) or, through the Hub, to a Track slot.
    bool post(const std::string& busName, int target, uint32_t paramId, float value);

    // Hotkey helpers: toggle against the Hub's current state.
    void togglePanic(const std::string& busName);
    void togglePreview(const std::string& busName);
    void recallScene(const std::string& busName, int index);

    void releaseAll();

private:
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<ssbus::SharedMemory>> maps_;
    std::map<std::string, uint64_t> lastAttemptNs_;
};

} // namespace hearaside
