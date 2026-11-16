/**
 * @file GpuCuller.hpp
 * @brief WebGPU Compute-shader-driven Frustum & Spatial Bounding Culling.
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "scene/Scene.hpp"
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

struct alignas(16) GpuBoundingBox {
    glm::vec4 minBound{0.0f};
    glm::vec4 maxBound{0.0f};
    uint32_t nodeIndex{0};
    uint32_t primitiveIndex{0};
    uint32_t instanceCount{1};
    uint32_t _pad{0};
};

static_assert(sizeof(GpuBoundingBox) == 48, "GpuBoundingBox must be 48 bytes");

struct DrawIndexedIndirectArgs {
    uint32_t indexCount{0};
    uint32_t instanceCount{0};
    uint32_t firstIndex{0};
    int32_t baseVertex{0};
    uint32_t firstInstance{0};
};

static_assert(sizeof(DrawIndexedIndirectArgs) == 20, "DrawIndexedIndirectArgs must be 20 bytes");

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
    core::Result<void> ReloadShader();
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
    [[nodiscard]] WGPUBuffer GetIndirectArgsBuffer() const noexcept { return indirectArgsBuffer_.Get(); }
    [[nodiscard]] WGPUBuffer GetVisibilityBuffer() const noexcept { return visibilityStorageBuffer_.Get(); }
    [[nodiscard]] WGPUBuffer GetAtomicCounterBuffer() const noexcept { return atomicCounterBuffer_.Get(); }

private:
    core::Result<void> CreateComputePipeline(WGPUShaderModule shaderModule);
    void MoveFrom(GpuCuller& other) noexcept;

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};

    ComputePipelineHandle computePipeline_{};
    PipelineLayoutHandle pipelineLayout_{};
    BindGroupLayoutHandle bindGroupLayout_{};
    BindGroupHandle bindGroup_{};

    BufferHandle uniformBuffer_{};
    BufferHandle boundsStorageBuffer_{};
    BufferHandle visibilityStorageBuffer_{};
    BufferHandle atomicCounterBuffer_{};
    BufferHandle indirectArgsBuffer_{};

    size_t primitiveCount_{0};
    size_t boundsBufferSize_{0};
    size_t visibilityBufferSize_{0};
    size_t indirectArgsBufferSize_{0};
    std::vector<GpuBoundingBox> cpuBounds_{};
    std::vector<DrawIndexedIndirectArgs> cpuIndirectArgs_{};
};

} // namespace bgl::gpu
