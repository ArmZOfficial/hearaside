// SPDX-License-Identifier: MIT
#ifndef _WIN32

#include "ssbus/bus.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace ssbus {

uint64_t nowNs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

uint32_t currentPid() noexcept { return uint32_t(getpid()); }

bool processAlive(uint32_t pid) noexcept {
    if (pid == 0) return false;
    if (::kill(pid_t(pid), 0) == 0) return true;
    return errno == EPERM;   // exists, owned by someone else
}

static uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

std::string SharedMemory::segmentName(const std::string& busName) {
    // macOS limits POSIX shm names to ~31 characters, so hash the bus name.
    char buf[32];
    std::snprintf(buf, sizeof buf, "/hrsd%u_%08x", unsigned(kProtocolVersion), fnv1a(busName));
    return buf;
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& busName, Status& status) {
    return openImpl(busName, true, status);
}

std::unique_ptr<SharedMemory> SharedMemory::openExisting(const std::string& busName, Status& status) {
    return openImpl(busName, false, status);
}

std::unique_ptr<SharedMemory> SharedMemory::openImpl(const std::string& busName, bool create, Status& status) {
    const std::string name = segmentName(busName);
    const size_t size = sizeof(BusLayout);
    bool created = false;

    int fd = -1;
    if (create) {
        fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            created = true;
            fchmod(fd, 0666);
            if (ftruncate(fd, off_t(size)) != 0) { close(fd); shm_unlink(name.c_str()); status = Status::CreateFailed; return nullptr; }
        }
    }
    if (fd < 0) fd = shm_open(name.c_str(), O_RDWR, 0666);
    if (fd < 0) { status = Status::CreateFailed; return nullptr; }

    if (!created) {
        // wait until the creator has sized the segment
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        for (;;) {
            struct stat st{};
            if (fstat(fd, &st) == 0 && size_t(st.st_size) >= size) break;
            if (fstat(fd, &st) == 0 && st.st_size != 0 && size_t(st.st_size) != size) { close(fd); status = Status::VersionMismatch; return nullptr; }
            if (std::chrono::steady_clock::now() > deadline) { close(fd); status = Status::InitTimeout; return nullptr; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); status = Status::MapFailed; return nullptr; }

    std::unique_ptr<SharedMemory> shm(new SharedMemory());
    shm->base_ = base;
    shm->size_ = size;
    shm->fd_ = fd;
    shm->busName_ = busName;

    BusLayout& L = shm->layout();
    if (created) {
        L.header.version = kProtocolVersion;
        L.header.layoutSize = uint32_t(size);
        for (int i = 0; i < kMaxSlots; ++i) L.slots[i].stemIndex.store(-1, std::memory_order_relaxed);
        L.header.magic.store(kMagic, std::memory_order_release);
    } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (L.header.magic.load(std::memory_order_acquire) != kMagic) {
            if (std::chrono::steady_clock::now() > deadline) { status = Status::InitTimeout; return nullptr; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (L.header.version != kProtocolVersion || L.header.layoutSize != uint32_t(size)) {
            status = Status::VersionMismatch;
            return nullptr;
        }
    }
    status = Status::Ok;
    return shm;
}

SharedMemory::~SharedMemory() {
    // Never shm_unlink: other processes may still use the bus. Stale state is handled by heartbeats.
    if (base_) munmap(base_, size_);
    if (fd_ >= 0) close(fd_);
}

} // namespace ssbus

#endif // !_WIN32
