// Cross-process test: a child process writes a known sample sequence into a slot FIFO while the
// parent reads it with its own cursor. Verifies every sample arrives bit-exact across processes,
// then kills the child and checks that the dead slot is reclaimed.
#include "ssbus/bus.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ssbus;

static float valueAt(uint64_t frame, int ch) { return float(int64_t(frame % 16000000)) * (ch == 0 ? 1.0f : -1.0f); }

static int writer(const std::string& bus, double seconds) {
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(bus, st);
    if (!shm) return 2;
    bool dup = false;
    const int idx = claimSlot(shm->layout(), "ipc-writer", 48000, 2, dup);
    if (idx < 0) return 3;
    auto& slot = shm->layout().slots[idx];
    std::vector<float> l(256), r(256);
    uint64_t frame = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        for (int i = 0; i < 256; ++i) { l[size_t(i)] = valueAt(frame + uint64_t(i), 0); r[size_t(i)] = valueAt(frame + uint64_t(i), 1); }
        const float* src[2] = { l.data(), r.data() };
        slot.heartbeatNs.store(nowNs());
        ringWrite(shm->layout().slotAudio[idx], slot.writePos, src, 2, 256);
        frame += 256;
        std::this_thread::sleep_for(std::chrono::microseconds(500));   // ~10x real time
    }
    // keep the slot claimed and exit without releasing (simulates a crash)
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--writer") == 0) return writer(argv[2], 2.0);

    const std::string bus = "ipc_" + std::to_string(currentPid());
    SharedMemory::Status st{};
    auto shm = SharedMemory::open(bus, st);
    if (!shm) { std::printf("cannot open bus: %s\n", statusText(st)); return 1; }
    auto& L = shm->layout();

#ifdef _WIN32
    std::string cmd = std::string("\"") + argv[0] + "\" --writer " + bus;
    STARTUPINFOA si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { std::printf("spawn failed\n"); return 1; }
#else
    pid_t child = fork();
    if (child == 0) { _exit(writer(bus, 2.0)); }
#endif

    // find the writer's slot
    int idx = -1;
    for (int tries = 0; tries < 2000 && idx < 0; ++tries) {
        for (int i = 0; i < kMaxSlots; ++i) {
            std::string n, u; uint32_t c;
            if (L.slots[i].state.load() == kSlotActive && readSlotIdentity(L.slots[i], n, u, c) && u == "ipc-writer") idx = i;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (idx < 0) { std::printf("writer slot not found\n"); return 1; }
    auto& slot = L.slots[idx];

    uint64_t cursor = slot.writePos.load();
    uint64_t checked = 0, bad = 0, overruns = 0;
    std::vector<float> l(4096), r(4096);
    float* dst[2] = { l.data(), r.data() };
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    while (std::chrono::steady_clock::now() < end) {
        const uint64_t w = slot.writePos.load(std::memory_order_acquire);
        const uint32_t n = uint32_t(std::min<uint64_t>(w - cursor, 4096));
        if (n == 0) { std::this_thread::yield(); continue; }
        const uint64_t start = cursor;
        const auto res = ringRead(L.slotAudio[idx], slot.writePos, cursor, dst, n);
        if (res == ReadResult::Overrun) { ++overruns; cursor = slot.writePos.load() - 1024; continue; }
        for (uint32_t i = 0; i < n; ++i) {
            if (l[i] != valueAt(start + i, 0) || r[i] != valueAt(start + i, 1)) ++bad;
            ++checked;
        }
    }
    std::printf("checked %llu frames across processes, %llu mismatches, %llu overruns\n",
                (unsigned long long)checked, (unsigned long long)bad, (unsigned long long)overruns);

    // kill the writer, then the slot must be reclaimable once its heartbeat is stale
#ifdef _WIN32
    TerminateProcess(pi.hProcess, 9);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
#endif
    slot.heartbeatNs.store(nowNs() - 10000000000ull);
    const int reclaimed = reclaimDeadSlots(L, 5000000000ull);
    std::printf("reclaimed %d dead slot(s)\n", reclaimed);

    const bool ok = checked > 100000 && bad == 0 && reclaimed == 1 && slot.state.load() == kSlotFree;
    std::printf(ok ? "IPC TEST PASSED\n" : "IPC TEST FAILED\n");
    return ok ? 0 : 1;
}
