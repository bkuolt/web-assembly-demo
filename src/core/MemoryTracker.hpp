/**
 * @file MemoryTracker.hpp
 * @brief System RAM (resident set size) and WebGPU VRAM allocation telemetry tracker.
 */
#pragma once

#include <cstddef>
#include <atomic>
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

    [[nodiscard]] size_t TotalGpuBytes() const noexcept {
        return gpuBufferBytes + gpuTextureBytes;
    }

    [[nodiscard]] float RamMb() const noexcept {
        return static_cast<float>(residentRamBytes) / (1024.0f * 1024.0f);
    }

    [[nodiscard]] float VramMb() const noexcept {
        return static_cast<float>(TotalGpuBytes()) / (1024.0f * 1024.0f);
    }
};

/**
 * @class MemoryTracker
 * @brief Thread-safe telemetry tracker for process RAM and WebGPU VRAM resources.
 */
class MemoryTracker {
public:
    static MemoryTracker& Instance() noexcept;

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
    void TrackBufferFree(size_t bytes) noexcept { gpuBufferBytes_.fetch_sub(bytes, std::memory_order_relaxed); }

    void TrackTextureAlloc(size_t bytes) noexcept { gpuTextureBytes_.fetch_add(bytes, std::memory_order_relaxed); }
    void TrackTextureFree(size_t bytes) noexcept { gpuTextureBytes_.fetch_sub(bytes, std::memory_order_relaxed); }

    [[nodiscard]] size_t GetGpuBufferBytes() const noexcept { return gpuBufferBytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t GetGpuTextureBytes() const noexcept { return gpuTextureBytes_.load(std::memory_order_relaxed); }

    [[nodiscard]] MemoryTelemetry Query() const noexcept {
        return MemoryTelemetry{
            .residentRamBytes = GetResidentRamBytes(),
            .virtualRamBytes = GetVirtualRamBytes(),
            .gpuBufferBytes = GetGpuBufferBytes(),
            .gpuTextureBytes = GetGpuTextureBytes()
        };
    }

private:
    MemoryTracker() = default;
    std::atomic<size_t> gpuBufferBytes_{0};
    std::atomic<size_t> gpuTextureBytes_{0};
};

} // namespace bgl::core
