/**
 * @file MemoryTracker.cpp
 * @brief Implementation of OS resident memory querying and VRAM tracking.
 */
#include "core/MemoryTracker.hpp"
#include <cstdio>

#if defined(BGL_PLATFORM_NATIVE)
#include <sys/resource.h>
#include <unistd.h>
#elif defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#endif

namespace bgl::core {

MemoryTracker& MemoryTracker::Instance() noexcept {
    static MemoryTracker tracker;
    return tracker;
}

void MemoryTracker::QueryProcessMemory(size_t& residentBytesOut, size_t& virtualBytesOut) const noexcept {
    residentBytesOut = 0;
    virtualBytesOut = 0;
#if defined(BGL_PLATFORM_NATIVE)
    long totalPages = 0;
    long residentPages = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        if (std::fscanf(f, "%ld %ld", &totalPages, &residentPages) == 2) {
            std::fclose(f);
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize > 0) {
                virtualBytesOut = static_cast<size_t>(totalPages * pageSize);
                residentBytesOut = static_cast<size_t>(residentPages * pageSize);
                return;
            }
        } else {
            std::fclose(f);
        }
    }

    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        residentBytesOut = static_cast<size_t>(usage.ru_maxrss * 1024L);
    }
#elif defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
    const size_t heapBytes = static_cast<size_t>(emscripten_get_heap_size());
    residentBytesOut = heapBytes;
    virtualBytesOut = heapBytes;
#endif
}

size_t MemoryTracker::GetResidentRamBytes() const noexcept {
    size_t resBytes{0};
    size_t virtBytes{0};
    QueryProcessMemory(resBytes, virtBytes);
    return resBytes;
}

size_t MemoryTracker::GetVirtualRamBytes() const noexcept {
    size_t resBytes{0};
    size_t virtBytes{0};
    QueryProcessMemory(resBytes, virtBytes);
    return virtBytes;
}

} // namespace bgl::core
