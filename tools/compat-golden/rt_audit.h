// SPDX-License-Identifier: MIT
// Real-time audit for the golden generators: counts heap allocations and blocking mutex
// locks made while a thread is inside an rt::Scope (an audio-thread call). Include it in
// exactly one translation unit per executable; it replaces the global allocators.
//
// glibc: malloc/calloc/realloc/aligned allocation and pthread_mutex_lock are interposed,
// which also covers operator new and std::mutex::lock. Elsewhere only the plain and
// nothrow forms of operator new/new[] are counted; locks are not detected.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace rt {
struct Site {
    const char* label;
    std::atomic<uint64_t> allocations{0}, locks{0};
};
constexpr int kMaxSites = 64;
inline Site g_sites[kMaxSites];
inline std::atomic<int> g_siteCount{0};
inline thread_local Site* t_site = nullptr;
#if defined(__GLIBC__)
constexpr bool kDetectsLocks = true;
#else
constexpr bool kDetectsLocks = false;
#endif

inline Site* site(const char* label) {   // labels are string literals; strcmp never allocates
    const int n = g_siteCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) if (std::strcmp(g_sites[i].label, label) == 0) return &g_sites[i];
    const int at = g_siteCount.fetch_add(1, std::memory_order_acq_rel);
    if (at >= kMaxSites) std::abort();
    g_sites[at].label = label;
    return &g_sites[at];
}
inline void noteAllocation() noexcept {
    if (t_site) t_site->allocations.fetch_add(1, std::memory_order_relaxed);
}
inline void noteLock() noexcept {
    if (t_site) t_site->locks.fetch_add(1, std::memory_order_relaxed);
}
inline void reset() noexcept {
    for (int i = 0; i < g_siteCount.load(); ++i) { g_sites[i].allocations = 0; g_sites[i].locks = 0; }
}

// Marks the calls a host, OBS or the network makes on the audio thread. Nesting keeps the outer label.
struct Scope {
    Site* previous;
    explicit Scope(const char* label) : previous(t_site) { if (!previous) t_site = site(label); }
    ~Scope() { t_site = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace rt

#if defined(__GLIBC__)
#include <dlfcn.h>
#include <pthread.h>

extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);

void* malloc(size_t size) noexcept { rt::noteAllocation(); return __libc_malloc(size); }
void* calloc(size_t count, size_t size) noexcept { rt::noteAllocation(); return __libc_calloc(count, size); }
void* realloc(void* p, size_t size) noexcept { rt::noteAllocation(); return __libc_realloc(p, size); }
void* aligned_alloc(size_t alignment, size_t size) noexcept { rt::noteAllocation(); return __libc_memalign(alignment, size); }
void* memalign(size_t alignment, size_t size) noexcept { rt::noteAllocation(); return __libc_memalign(alignment, size); }
int posix_memalign(void** out, size_t alignment, size_t size) noexcept {
    rt::noteAllocation();
    if (alignment % sizeof(void*) != 0 || (alignment & (alignment - 1)) != 0) return 22; // EINVAL
    *out = __libc_memalign(alignment, size);
    return *out ? 0 : 12; // ENOMEM
}

}
namespace rt {
using MutexLock = int (*)(pthread_mutex_t*);
inline MutexLock g_realMutexLock = nullptr;   // no function-local static: its guard could lock
inline MutexLock realMutexLock() noexcept {
    if (!g_realMutexLock) g_realMutexLock = reinterpret_cast<MutexLock>(dlsym(RTLD_NEXT, "pthread_mutex_lock"));
    if (!g_realMutexLock) std::abort();
    return g_realMutexLock;
}
// Resolve before any audio scope so dlsym's own allocation is never attributed to one.
inline const bool g_mutexResolved = realMutexLock() != nullptr;
} // namespace rt
extern "C" int pthread_mutex_lock(pthread_mutex_t* mutex) noexcept {
    rt::noteLock();
    return rt::realMutexLock()(mutex);
}
#else
inline void* countedMalloc(size_t size) noexcept { rt::noteAllocation(); return std::malloc(size ? size : 1); }
void* operator new(size_t size) {
    if (void* p = countedMalloc(size)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t size) {
    if (void* p = countedMalloc(size)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t size, const std::nothrow_t&) noexcept { return countedMalloc(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return countedMalloc(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif

namespace rt {
// Proves the detectors fire before an audit trusts a zero count.
template <class Lockable>
inline bool selfTest(Lockable& mutex) {
    static const char* const label = "rt-audit self-test";
    {
        Scope scope(label);
        int* volatile probe = new int(1);
        delete probe;
        if constexpr (kDetectsLocks) { mutex.lock(); mutex.unlock(); }
    }
    Site* s = site(label);
    const bool ok = s->allocations.load() >= 1 && (!kDetectsLocks || s->locks.load() >= 1);
    s->allocations = 0; s->locks = 0;
    return ok;
}
} // namespace rt
