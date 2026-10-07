// SPDX-License-Identifier: MIT
#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>

#include "ssbus/bus.h"

#include <chrono>
#include <thread>

#pragma comment(lib, "advapi32.lib")

namespace ssbus {

uint64_t nowNs() noexcept {
    static const LONGLONG freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // split to avoid overflow of c * 1e9
    const uint64_t sec = uint64_t(c.QuadPart / freq);
    const uint64_t rem = uint64_t(c.QuadPart % freq);
    return sec * 1000000000ull + rem * 1000000000ull / uint64_t(freq);
}

uint32_t currentPid() noexcept { return uint32_t(GetCurrentProcessId()); }

bool processAlive(uint32_t pid) noexcept {
    if (pid == 0) return false;
    if (pid == GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
        // Access denied means the process exists but runs at a higher integrity level / other user.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

std::string SharedMemory::segmentName(const std::string& busName) {
    return "Local\\StreamSplit_v" + std::to_string(kProtocolVersion) + "_" + busName;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& busName, Status& status) {
    return openImpl(busName, true, status);
}

std::unique_ptr<SharedMemory> SharedMemory::openExisting(const std::string& busName, Status& status) {
    return openImpl(busName, false, status);
}

std::unique_ptr<SharedMemory> SharedMemory::openImpl(const std::string& busName, bool create, Status& status) {
    const std::wstring name = widen(segmentName(busName));
    const uint64_t size = sizeof(BusLayout);

    HANDLE h = nullptr;
    bool created = false;
    if (create) {
        // Everyone + anonymous get full access; mandatory label "low, no-write-up" so that a
        // segment created by an elevated DAW/OBS can still be written by a normal process and
        // vice versa.
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;WD)(A;;GA;;;AN)(A;;GA;;;SY)(A;;GA;;;BA)S:(ML;;NW;;;LW)",
                SDDL_REVISION_1, &sd, nullptr)) {
            sa.lpSecurityDescriptor = sd;
        }
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE,
                               DWORD(size >> 32), DWORD(size & 0xFFFFFFFFu), name.c_str());
        const DWORD err = GetLastError();
        if (sd) LocalFree(sd);
        if (h == nullptr) {
            // Could exist but be inaccessible for create with these attributes - try plain open.
            h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        } else {
            created = (err != ERROR_ALREADY_EXISTS);
        }
    } else {
        h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    }
    if (h == nullptr) { status = Status::CreateFailed; return nullptr; }

    void* base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size_t(size));
    if (base == nullptr) { CloseHandle(h); status = Status::MapFailed; return nullptr; }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(base, &mbi, sizeof mbi) == 0 || mbi.RegionSize < size) {
        UnmapViewOfFile(base); CloseHandle(h); status = Status::VersionMismatch; return nullptr;
    }

    std::unique_ptr<SharedMemory> shm(new SharedMemory());
    shm->base_ = base;
    shm->size_ = size_t(size);
    shm->handle_ = h;
    shm->busName_ = busName;

    BusLayout& L = shm->layout();
    if (created) {
        // Fresh pagefile-backed mappings are zero filled; zero bits are valid for all atomics.
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
    if (base_) UnmapViewOfFile(base_);
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

} // namespace ssbus

#endif // _WIN32
