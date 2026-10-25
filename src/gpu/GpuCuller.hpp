/**
 * @file GpuCuller.hpp
 * @brief WebGPU Compute-shader-driven Frustum & Spatial Bounding Culling.
 */
#pragma once

#include <vector>
#include <span>
#include <webgpu/webgpu.h>
#include <glm/glm.hpp>
#include "core/Types.hpp"
#include "scene/Scene.hpp"

namespace bgl::gpu {

struct alignas(16) GpuBoundingBox {
    glm::vec4 minBound{0.0f};
    glm::vec4 maxBound{0.0f};
    uint32_t nodeIndex{0};
    uint32_t primitiveIndex{0};
    uint32_t _pad[2]{0, 0};
};

static_assert(sizeof(GpuBoundingBox) == 48, "GpuBoundingBox must be 48 bytes");

struct alignas(16) GpuCullUniforms {
    glm::vec4 frustumPlanes[6]; // 6 analytical plane equations (xyz = normal, w = distance)
    uint32_t primitiveCount{0};
    uint32_t _pad[3]{0, 0, 0};
};

static_assert(sizeof(GpuCullUniforms) == 112, "GpuCullUniforms must be 112 bytes");

/**
 * @class GpuCuller
 * @brief Evaluates view frustum and spatial bounding tests in parallel on the GPU via compute shaders.
 */
class GpuCuller {
public:
    GpuCuller() = default;
    ~GpuCuller();

    GpuCuller(const GpuCuller&) = delete;
    GpuCuller& operator=(const GpuCuller&) = delete;
    GpuCuller(GpuCuller&& other) noexcept;
    GpuCuller& operator=(GpuCuller&& other) noexcept;

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue);
    void Shutdown() noexcept;

    /**
     * @brief Uploads or refreshes scene primitive bounding boxes in the GPU storage buffer.
     */
    void UploadSceneBounds(const scene::Scene& scene);

    /**
     * @brief Executes GPU compute frustum culling pass.
     * @param encoder Active command encoder.
     * @param frustum Current camera viewing frustum.
     */
    void Dispatch(WGPUCommandEncoder encoder, const core::Frustum& frustum);

    [[nodiscard]] size_t GetPrimitiveCount() const noexcept { return primitiveCount_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return computePipeline_ != nullptr; }

private:
    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};

    WGPUComputePipeline computePipeline_{nullptr};
    WGPUBindGroupLayout bindGroupLayout_{nullptr};
    WGPUBindGroup bindGroup_{nullptr};

    WGPUBuffer uniformBuffer_{nullptr};
    WGPUBuffer boundsStorageBuffer_{nullptr};
    WGPUBuffer visibilityStorageBuffer_{nullptr};
    WGPUBuffer atomicCounterBuffer_{nullptr};

    size_t primitiveCount_{0};
    size_t boundsBufferSize_{0};
    std::vector<GpuBoundingBox> cpuBounds_{};
};

} // namespace bgl::gpu
