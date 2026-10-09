// SPDX-License-Identifier: GPL-2.0-or-later
#include "remote.h"

namespace hearaside {

using namespace ssbus;

BusClient& BusClient::instance() {
    static BusClient c;
    return c;
}

bool BusClient::with(const std::string& busName, const std::function<void(BusLayout&)>& fn) {
    const std::string bus = busName.empty() ? std::string("Main") : busName;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = maps_.find(bus);
    if (it == maps_.end() || !it->second) {
        // only retry opening once per second (DAW not running yet)
        const uint64_t now = nowNs();
        uint64_t& last = lastAttemptNs_[bus];
        if (last != 0 && now - last < 1000000000ull) return false;
        last = now;
        SharedMemory::Status st {};
        auto shm = SharedMemory::openExisting(bus, st);
        if (!shm) return false;
        it = maps_.insert_or_assign(bus, std::move(shm)).first;
    }
    fn(it->second->layout());
    return true;
}

void BusClient::togglePanic(const std::string& busName) {
    with(busName, [](BusLayout& L) {
        const bool on = (L.header.hubFlags.load(std::memory_order_relaxed) & kHubPanic) != 0;
        postRemote(L, RemoteParam::Panic, on ? 0.0f : 1.0f);
    });
}

void BusClient::togglePreview(const std::string& busName) {
    with(busName, [](BusLayout& L) {
        const bool on = (L.header.hubFlags.load(std::memory_order_relaxed) & kHubPreview) != 0;
        postRemote(L, RemoteParam::Preview, on ? 0.0f : 1.0f);
    });
}

void BusClient::releaseAll() {
    std::lock_guard<std::mutex> lk(mutex_);
    maps_.clear();
}

} // namespace hearaside
