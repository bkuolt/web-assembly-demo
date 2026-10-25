/**
 * @file MemoryTracker.cpp
 * @brief Implementation of OS resident memory querying and VRAM tracking.
 */
#include "core/MemoryTracker.hpp"
#include <cstdio>

#if defined(BGL_PLATFORM_NATIVE)
#include <unistd.h>
#include <sys/resource.h>
#elif defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#endif

namespace bgl::core {

MemoryTracker& MemoryTracker::Instance() noexcept {
    static MemoryTracker tracker;
    return tracker;
}

size_t MemoryTracker::GetResidentRamBytes() const noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    long residentPages = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        long totalPages = 0;
        if (std::fscanf(f, "%ld %ld", &totalPages, &residentPages) == 2) {
            std::fclose(f);
            const long pageSize = sysconf(_SC_PAGESIZE);
            return static_cast<size_t>(residentPages * pageSize);
        }
        std::fclose(f);
    }

    // Fallback: getrusage (ru_maxrss in kilobytes on Linux)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<size_t>(usage.ru_maxrss * 1024L);
    }
    return 0;
#elif defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
    return static_cast<size_t>(emscripten_get_heap_size());
#else
    return 0;
#endif
}

size_t MemoryTracker::GetVirtualRamBytes() const noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    long totalPages = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        if (std::fscanf(f, "%ld", &totalPages) == 1) {
            std::fclose(f);
            const long pageSize = sysconf(_SC_PAGESIZE);
            return static_cast<size_t>(totalPages * pageSize);
        }
        std::fclose(f);
    }
    return 0;
#elif defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
    return static_cast<size_t>(emscripten_get_heap_size());
#else
    return 0;
#endif
}

} // namespace bgl::core
