/**
 * @file GpuRayPicker.hpp
 * @brief WebGPU Compute-shader-driven parallel Ray-Bounding & Geometry Picking.
 */
#ifndef BGL_GPU_GPURAYPICKER_HPP
#define BGL_GPU_GPURAYPICKER_HPP

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "scene/Scene.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

struct alignas(16) GpuRayPickUniforms {
    glm::vec4 rayOrigin{0.0f};    // xyz = origin, w = maxDistance
    glm::vec4 rayDirection{0.0f}; // xyz = direction, w = primitiveCount
};

static_assert(sizeof(GpuRayPickUniforms) == 32, "GpuRayPickUniforms must be 32 bytes");

struct alignas(16) GpuRayPickResult {
    uint32_t hitNodeIndex{0xFFFFFFFF};
    uint32_t hitPrimitiveIndex{0xFFFFFFFF};
    uint32_t hitDistanceBits{0x7F7FFFFF}; // float stored as bits for atomic min
    uint32_t hitFound{0};
};

static_assert(sizeof(GpuRayPickResult) == 16, "GpuRayPickResult must be 16 bytes");

/**
 * @class GpuRayPicker
 * @brief Evaluates ray-bounding intersections across all scene primitives on the GPU in parallel.
 */
class GpuRayPicker {
public:
    GpuRayPicker() = default;
    ~GpuRayPicker();

    GpuRayPicker(const GpuRayPicker&) = delete;
    GpuRayPicker& operator=(const GpuRayPicker&) = delete;
    GpuRayPicker(GpuRayPicker&& other) noexcept;
    GpuRayPicker& operator=(GpuRayPicker&& other) noexcept;

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue);
    core::Result<void> ReloadShader();
    void Shutdown() noexcept;

    /**
     * @brief Dispatches parallel GPU ray intersection test.
     */
    void Dispatch(WGPUCommandEncoder encoder, WGPUBuffer boundsBuffer, uint32_t primitiveCount, const core::Ray& ray);

    [[nodiscard]] bool IsInitialized() const noexcept {
        return computePipeline_ != nullptr && resolvePipeline_ != nullptr;
    }

private:
    void MoveFrom(GpuRayPicker& other) noexcept;
    core::Result<void> CreateComputePipelines(WGPUShaderModule shaderModule);

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};

    ComputePipelineHandle computePipeline_{};
    ComputePipelineHandle resolvePipeline_{};
    PipelineLayoutHandle pipelineLayout_{};
    BindGroupLayoutHandle bindGroupLayout_{};
    BindGroupHandle bindGroup_{};

    BufferHandle uniformBuffer_{};
    BufferHandle resultBuffer_{};
    WGPUBuffer cachedBoundsBuffer_{nullptr};
    uint32_t cachedPrimitiveCount_{0};
};

} // namespace bgl::gpu

#endif // BGL_GPU_GPURAYPICKER_HPP
