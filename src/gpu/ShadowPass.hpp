/**
 * @file ShadowPass.hpp
 * @brief Directional 2048x2048 PCF shadow map depth pre-pass with alpha testing.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <glm/glm.hpp>
#include "core/Types.hpp"
#include "scene/Scene.hpp"
#include "scene/Quadtree.hpp"

namespace bgl::gpu {

struct alignas(16) ShadowUniforms {
    glm::mat4 viewProj{1.0f};
    glm::mat4 model{1.0f};
    glm::mat4 lightViewProj{1.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 lightDir{0.577f, 0.577f, 0.577f, 0.0f};
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    float normalScale{1.0f};
    float occlusionStrength{1.0f};
    glm::vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f};
    float alphaCutoff{0.5f};
    uint32_t alphaMode{0};
    uint32_t flags{0};
    float ior{1.5f};
    float clearcoatFactor{0.0f};
    float clearcoatRoughness{0.0f};
    float transmissionFactor{0.0f};
    float specularFactor{1.0f};
    glm::vec4 sheenColorFactor{0.0f};
    glm::vec4 specularColorFactor{1.0f};
    float bumpScale{1.0f};
    float reflectionIntensity{1.0f};
    float shadowBias{0.0015f};
    float envMipCount{8.0f};
    float thicknessFactor{0.0f};
    float attenuationDistance{1e9f};
    float transmissionRoughness{0.0f};
    float paddingVol1{0.0f};
    glm::vec4 attenuationColor{1.0f};
};

static_assert(sizeof(ShadowUniforms) == 384, "ShadowUniforms must be 384 bytes");

class ShadowPass {
public:
    static constexpr uint32_t kShadowMapSize{2048};

    ShadowPass() = default;
    ~ShadowPass();

    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;
    ShadowPass(ShadowPass&& other) noexcept;
    ShadowPass& operator=(ShadowPass&& other) noexcept;

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUSampler defaultSampler, WGPUTextureView defaultWhiteView);
    void Shutdown() noexcept;

    /**
     * @brief Computes scene-enclosing directional light-space projection matrix.
     */
    [[nodiscard]] static glm::mat4 ComputeDirectionalLightMatrix(const core::AABB& sceneBounds, const glm::vec3& lightDir) noexcept;

    /**
     * @brief Records directional shadow map depth pre-pass.
     */
    void Render(WGPUCommandEncoder encoder,
                scene::Scene& scene,
                const glm::vec3& lightDir,
                const scene::Quadtree* spatialIndex = nullptr);

    [[nodiscard]] WGPUTextureView GetShadowMapView() const noexcept { return shadowTextureView_; }
    [[nodiscard]] WGPUSampler GetShadowSampler() const noexcept { return shadowSampler_; }
    [[nodiscard]] glm::mat4 GetLightViewProjMatrix() const noexcept { return lightViewProj_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return shadowPipeline_ != nullptr; }

private:
    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};

    WGPURenderPipeline shadowPipeline_{nullptr};
    WGPUBindGroupLayout shadowBindGroupLayout_{nullptr};
    WGPUTexture shadowTexture_{nullptr};
    WGPUTextureView shadowTextureView_{nullptr};
    WGPUSampler shadowSampler_{nullptr};

    WGPUSampler defaultSampler_{nullptr};
    WGPUTextureView defaultWhiteView_{nullptr};

    glm::mat4 lightViewProj_{1.0f};
};

} // namespace bgl::gpu
