// Remote control of the HEARASIDE Hub from OBS hotkeys.
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

    // Toggle against the Hub's current state.
    void togglePanic(const std::string& busName);
    void togglePreview(const std::string& busName);

    void releaseAll();

private:
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<ssbus::SharedMemory>> maps_;
    std::map<std::string, uint64_t> lastAttemptNs_;
};

} // namespace hearaside
