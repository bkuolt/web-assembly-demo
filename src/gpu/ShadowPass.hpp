/**
 * @file ShadowPass.hpp
 * @brief Directional 2048x2048 PCF shadow map depth pre-pass with alpha testing.
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/Uniforms.hpp"
#include "scene/Quadtree.hpp"
#include "scene/Scene.hpp"
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

class ShadowPass {
public:
    static constexpr uint32_t kShadowMapSize{2048};

    ShadowPass() = default;
    ~ShadowPass();

    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;
    ShadowPass(ShadowPass&& other) noexcept;
    ShadowPass& operator=(ShadowPass&& other) noexcept;

    /**
     * @brief Initializes shadow depth target, PCF sampler, shared 3-group pipeline layout, and dynamic UBO ring.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUBindGroupLayout perDrawBgl,
                                  WGPUBindGroupLayout materialBgl, WGPUBindGroupLayout skinBgl,
                                  WGPUBindGroup dummySkinBindGroup);
    core::Result<void> ReloadShader();
    void Shutdown() noexcept;

    /**
     * @brief Computes scene-enclosing directional light-space projection matrix.
     */
    [[nodiscard]] static glm::mat4 ComputeDirectionalLightMatrix(const core::AABB& sceneBounds,
                                                                 const glm::vec3& lightDir) noexcept;

    /**
     * @brief Marks the cached shadow map dirty so the next Render() call re-renders depth.
     */
    void MarkDirty() noexcept { dirty_ = true; }

    /**
     * @brief Clears the shadow depth map to 1.0 (unshadowed) if not already cleared.
     */
    void Clear(WGPUCommandEncoder encoder);

    /**
     * @brief Records directional shadow map depth pre-pass (cached for static scenes).
     */
    void Render(WGPUCommandEncoder encoder, scene::Scene& scene, const glm::vec3& lightDir,
                const scene::Quadtree* spatialIndex = nullptr, uint64_t skinFrameId = 0);

    [[nodiscard]] WGPUTextureView GetShadowMapView() const noexcept { return shadowTextureView_; }
    [[nodiscard]] WGPUSampler GetShadowSampler() const noexcept { return shadowSampler_; }
    [[nodiscard]] glm::mat4 GetLightViewProjMatrix() const noexcept { return lightViewProj_; }
    [[nodiscard]] bool IsInitialized() const noexcept {
        return shadowPipeline_ != nullptr && instanceBuffer_.GetBuffer() != nullptr;
    }

private:
    struct ShadowDrawItem {
        scene::MeshPrimitive* prim{nullptr};
        WGPUBindGroup skinBindGroup{nullptr};
        uint32_t dynamicOffset{0};
        uint32_t instanceStart{0};
        uint32_t instanceCount{1};
    };

    core::Result<void> CreatePipeline(bool validateScope);
    void MoveFrom(ShadowPass& other) noexcept;

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};

    WGPURenderPipeline shadowPipeline_{nullptr};
    WGPUPipelineLayout pipelineLayout_{nullptr};
    WGPUTexture shadowTexture_{nullptr};
    WGPUTextureView shadowTextureView_{nullptr};
    WGPUSampler shadowSampler_{nullptr};
    WGPUBindGroup dummySkinBindGroup_{nullptr};

    DynamicUniformRing<PerDrawUniforms> perDrawRing_{};
    DynamicInstanceBuffer instanceBuffer_{};
    std::vector<scene::QuadtreeItem> visibleItemsScratch_{};
    std::vector<ShadowDrawItem> drawItemsScratch_{};
    std::vector<PerDrawUniforms> perDrawScratch_{};
    std::vector<glm::mat4> instanceTransformsScratch_{};
    std::unordered_map<const scene::SceneNode*, uint32_t> nodeInstanceStartScratch_{};

    glm::mat4 lightViewProj_{1.0f};
    bool dirty_{true};
    bool cleared_{false};
    const scene::Scene* lastScene_{nullptr};
    uint64_t lastTransformVersion_{std::numeric_limits<uint64_t>::max()};
    glm::vec3 lastLightDir_{0.0f};
};

} // namespace bgl::gpu
