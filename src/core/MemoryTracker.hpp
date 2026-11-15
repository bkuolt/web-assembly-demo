/**
 * @file MemoryTracker.hpp
 * @brief System RAM (resident set size) and WebGPU VRAM allocation telemetry tracker.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <string>

namespace bgl::core {

/**
 * @struct MemoryTelemetry
 * @brief Live memory consumption telemetry in bytes and megabytes.
 */
struct MemoryTelemetry {
    size_t residentRamBytes{0};
    size_t virtualRamBytes{0};
    size_t gpuBufferBytes{0};
    size_t gpuTextureBytes{0};

    [[nodiscard]] size_t TotalGpuBytes() const noexcept { return gpuBufferBytes + gpuTextureBytes; }

    [[nodiscard]] float RamMb() const noexcept { return static_cast<float>(residentRamBytes) / (1024.0f * 1024.0f); }

    [[nodiscard]] float VramMb() const noexcept { return static_cast<float>(TotalGpuBytes()) / (1024.0f * 1024.0f); }
};

/**
 * @class MemoryTracker
 * @brief Thread-safe telemetry tracker for process RAM and WebGPU VRAM resources.
 */
class MemoryTracker {
public:
    static MemoryTracker& Instance() noexcept;

    /**
     * @brief Queries both resident RAM and virtual memory allocation in a single OS read.
     */
    void QueryProcessMemory(size_t& residentBytesOut, size_t& virtualBytesOut) const noexcept;

    /**
     * @brief Queries current process resident RAM consumption via OS kernel APIs.
     */
    [[nodiscard]] size_t GetResidentRamBytes() const noexcept;

    /**
     * @brief Queries current process virtual memory allocation.
     */
    [[nodiscard]] size_t GetVirtualRamBytes() const noexcept;

    // GPU VRAM Tracking
    void TrackBufferAlloc(size_t bytes) noexcept { gpuBufferBytes_.fetch_add(bytes, std::memory_order_relaxed); }
    void TrackBufferFree(size_t bytes) noexcept { SubtractSaturated(gpuBufferBytes_, bytes); }

    void TrackTextureAlloc(size_t bytes) noexcept { gpuTextureBytes_.fetch_add(bytes, std::memory_order_relaxed); }
    void TrackTextureFree(size_t bytes) noexcept { SubtractSaturated(gpuTextureBytes_, bytes); }

    [[nodiscard]] size_t GetGpuBufferBytes() const noexcept { return gpuBufferBytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t GetGpuTextureBytes() const noexcept {
        return gpuTextureBytes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] MemoryTelemetry Query() const noexcept {
        size_t resBytes{0};
        size_t virtBytes{0};
        QueryProcessMemory(resBytes, virtBytes);
        return MemoryTelemetry{.residentRamBytes = resBytes,
                               .virtualRamBytes = virtBytes,
                               .gpuBufferBytes = GetGpuBufferBytes(),
                               .gpuTextureBytes = GetGpuTextureBytes()};
    }

private:
    MemoryTracker() = default;

    static void SubtractSaturated(std::atomic<size_t>& target, size_t bytes) noexcept {
        size_t expected = target.load(std::memory_order_relaxed);
        while (expected != 0) {
            const size_t desired = (expected >= bytes) ? (expected - bytes) : 0;
            if (target.compare_exchange_weak(expected, desired, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    std::atomic<size_t> gpuBufferBytes_{0};
    std::atomic<size_t> gpuTextureBytes_{0};
};

} // namespace bgl::core
