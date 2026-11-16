/**
 * @file AabbRenderer.hpp
 * @brief Batched 3D wireframe bounding box renderer for Scene and Quadtree spatial partitions.
 */
#ifndef BGL_GPU_AABBRENDERER_HPP
#define BGL_GPU_AABBRENDERER_HPP

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "scene/Quadtree.hpp"
#include "scene/Scene.hpp"
#include <array>
#include <glm/glm.hpp>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

struct alignas(16) AabbUniforms {
    glm::mat4 viewProj{1.0f};
};

static_assert(sizeof(AabbUniforms) == 64, "AabbUniforms must be 64 bytes");

struct alignas(16) AabbVertex {
    glm::vec3 pos{0.0f};
    float _pad{0.0f};
    glm::vec4 color{1.0f};
};

static_assert(sizeof(AabbVertex) == 32, "AabbVertex must be 32 bytes");

/**
 * @class AabbRenderer
 * @brief Manages batched wireframe line rendering for AABBs and Quadtree cells in a single draw call.
 */
class AabbRenderer {
public:
    AabbRenderer() = default;
    ~AabbRenderer();

    AabbRenderer(const AabbRenderer&) = delete;
    AabbRenderer& operator=(const AabbRenderer&) = delete;
    AabbRenderer(AabbRenderer&& other) noexcept;
    AabbRenderer& operator=(AabbRenderer&& other) noexcept;

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat colorFormat,
                                  uint32_t sampleCount, WGPUTextureFormat depthFormat = WGPUTextureFormat_Depth24Plus);
    core::Result<void> ReloadShader();
    void Shutdown() noexcept;

    void AppendAABBLines(const core::AABB& aabb, const glm::vec4& color);
    void BatchSceneAABBs(const scene::Scene& scene, bool showSceneAABB = true, bool showPrimitiveAABBs = true);
    void BatchQuadtreeBounds(const scene::Quadtree& quadtree);
    void FlushAABBLines(WGPURenderPassEncoder pass, const glm::mat4& viewProj);
    void RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj,
                    const glm::vec4& color);

    void ResetFrameOffset() noexcept { aabbFrameByteOffset_ = 0; }
    [[nodiscard]] bool IsInitialized() const noexcept { return aabbPipeline_ != nullptr; }

private:
    core::Result<void> CreateRenderPipeline(WGPUShaderModule shaderModule);

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat colorFormat_{WGPUTextureFormat_RGBA16Float};
    uint32_t sampleCount_{4};
    WGPUTextureFormat depthFormat_{WGPUTextureFormat_Depth24Plus};

    PipelineLayoutHandle aabbPipelineLayout_{};
    RenderPipelineHandle aabbPipeline_{};
    BindGroupLayoutHandle aabbBindGroupLayout_{};
    BufferHandle aabbVertexBuffer_{};
    BufferHandle aabbUniformBuffer_{};
    BindGroupHandle aabbBindGroup_{};
    size_t aabbVertexBufferSize_{0};
    size_t aabbFrameByteOffset_{0};

    std::vector<AabbVertex> batchedAabbVertices_{};
    std::vector<AabbVertex> cachedSceneAabbVertices_{};
};

} // namespace bgl::gpu

#endif // BGL_GPU_AABBRENDERER_HPP
