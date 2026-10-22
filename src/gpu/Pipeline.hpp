/**
 * @file Pipeline.hpp
 * @brief Hardware WebGPU RenderPipeline, Shadow Mapping, Environment Reflections,
 *        Bump/Normal Mapping, Trilinear Mipmapping, and glTF mesh rendering.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <glm/glm.hpp>
#include <vector>
#include <span>
#include "core/Types.hpp"
#include "gpu/Handle.hpp"

namespace bgl::scene {
struct Scene;
class SceneNode;
struct MeshPrimitive;
class Quadtree;
}

namespace bgl::gpu {

namespace MaterialFlags {
    constexpr uint32_t HasBaseColorTex         = 1u << 0;
    constexpr uint32_t HasMetallicRoughnessTex = 1u << 1;
    constexpr uint32_t HasNormalTex            = 1u << 2;
    constexpr uint32_t HasEmissiveTex          = 1u << 3;
    constexpr uint32_t HasOcclusionTex         = 1u << 4;
    constexpr uint32_t HasClearcoatTex         = 1u << 5;
    constexpr uint32_t HasTransmissionTex      = 1u << 6;
    constexpr uint32_t HasSheenTex             = 1u << 7;
    constexpr uint32_t Unlit                   = 1u << 8;
    constexpr uint32_t DoubleSided             = 1u << 9;
    constexpr uint32_t EnableBumpMapping       = 1u << 10;
    constexpr uint32_t EnableMipmapping        = 1u << 11;
    constexpr uint32_t EnableShadows           = 1u << 12;
    constexpr uint32_t EnableReflections       = 1u << 13;
}

struct alignas(16) PrimitiveUniforms {
    // 0..191: Matrices (ViewProj, Model, LightViewProj)
    glm::mat4 viewProj{1.0f};
    glm::mat4 model{1.0f};
    glm::mat4 lightViewProj{1.0f};

    // 192..223: Camera & Light
    glm::vec4 cameraPos{0.0f};
    glm::vec4 lightDir{0.577f, 0.577f, 0.577f, 0.0f};

    // 224..255: Base Color & PBR Core
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    float normalScale{1.0f};
    float occlusionStrength{1.0f};

    // 256..287: Emissive & Alpha & Flags
    glm::vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f}; // xyz = rgb, w = strength
    float alphaCutoff{0.5f};
    uint32_t alphaMode{0}; // 0=Opaque, 1=Mask, 2=Blend
    uint32_t flags{0};
    float ior{1.5f};

    // 288..319: Clearcoat, Transmission, Sheen
    float clearcoatFactor{0.0f};
    float clearcoatRoughness{0.0f};
    float transmissionFactor{0.0f};
    float specularFactor{1.0f};
    glm::vec4 sheenColorFactor{0.0f, 0.0f, 0.0f, 0.0f}; // xyz = color, w = roughness

    // 320..335: Specular Color
    glm::vec4 specularColorFactor{1.0f, 1.0f, 1.0f, 0.0f};

    // 336..351: Bump, Reflection, Shadow & Environment Mip Parameters
    float bumpScale{1.0f};
    float reflectionIntensity{1.0f};
    float shadowBias{0.0015f};
    float envMipCount{7.0f};
};
static_assert(sizeof(PrimitiveUniforms) == 352, "PrimitiveUniforms must be exactly 352 bytes (16-byte aligned)");

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

class PipelineManager {
public:
    static constexpr uint32_t kShadowMapSize{2048};
    static constexpr uint32_t kEnvCubemapSize{64};

    PipelineManager() = default;
    ~PipelineManager();

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat, uint32_t sampleCount = 4);
    void Shutdown() noexcept;

    void PrepareScene(scene::Scene& scene);
    void RebuildMaterialBindGroups(scene::Scene& scene);

    void RenderShadowPass(WGPUCommandEncoder encoder, scene::Scene& scene, const glm::vec3& lightDir, const scene::Quadtree* spatialIndex = nullptr);
    void RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe = false, const scene::Quadtree* spatialIndex = nullptr);
    void RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj, const glm::vec4& color);
    void BatchSceneAABBs(const scene::Scene& scene, bool showSceneAABB = true, bool showPrimitiveAABBs = true);
    void BatchQuadtreeBounds(const scene::Quadtree& quadtree);
    void RenderSceneAABBs(WGPURenderPassEncoder pass, const scene::Scene& scene, const glm::mat4& viewProj, bool showSceneAABB = true, bool showPrimitiveAABBs = true);
    void RenderQuadtreeBounds(WGPURenderPassEncoder pass, const scene::Quadtree& quadtree, const glm::mat4& viewProj);
    void AppendAABBLines(const core::AABB& aabb, const glm::vec4& color);
    void FlushAABBLines(WGPURenderPassEncoder pass, const glm::mat4& viewProj);

    // Feature Toggles (Bump Mapping, Mipmapping, Shadow Mapping, Environment Reflections)
    void SetBumpMappingEnabled(bool enabled) noexcept { bumpMappingEnabled_ = enabled; }
    void ToggleBumpMapping() noexcept { bumpMappingEnabled_ = !bumpMappingEnabled_; }
    [[nodiscard]] bool IsBumpMappingEnabled() const noexcept { return bumpMappingEnabled_; }

    void SetMipmappingEnabled(bool enabled, scene::Scene* activeScene = nullptr);
    void ToggleMipmapping(scene::Scene* activeScene = nullptr) { SetMipmappingEnabled(!mipmappingEnabled_, activeScene); }
    [[nodiscard]] bool IsMipmappingEnabled() const noexcept { return mipmappingEnabled_; }

    void SetShadowsEnabled(bool enabled) noexcept {
        if (shadowsEnabled_ != enabled) {
            shadowsEnabled_ = enabled;
            shadowMapDirty_ = true;
        }
    }
    void ToggleShadows() noexcept { SetShadowsEnabled(!shadowsEnabled_); }
    [[nodiscard]] bool IsShadowsEnabled() const noexcept { return shadowsEnabled_; }

    void SetReflectionsEnabled(bool enabled) noexcept { reflectionsEnabled_ = enabled; }
    void ToggleReflections() noexcept { reflectionsEnabled_ = !reflectionsEnabled_; }
    [[nodiscard]] bool IsReflectionsEnabled() const noexcept { return reflectionsEnabled_; }

    void SetOcclusionCullingEnabled(bool enabled) noexcept { occlusionCullingEnabled_ = enabled; }
    void ToggleOcclusionCulling() noexcept { occlusionCullingEnabled_ = !occlusionCullingEnabled_; }
    [[nodiscard]] bool IsOcclusionCullingEnabled() const noexcept { return occlusionCullingEnabled_; }

    [[nodiscard]] size_t GetVisiblePrimitiveCount() const noexcept { return lastVisiblePrimitiveCount_; }
    [[nodiscard]] size_t GetOccludedPrimitiveCount() const noexcept { return lastOccludedPrimitiveCount_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

    // Pure CPU helpers for Mipmap Generation, Shadow Matrices, and Cubemap Prefiltering (also used by unit tests)
    [[nodiscard]] static uint32_t CalculateMipLevels(uint32_t width, uint32_t height) noexcept;
    [[nodiscard]] static std::vector<uint8_t> GenerateNextMipLevel(std::span<const uint8_t> src, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, bool isNormalMap = false);
    [[nodiscard]] static std::vector<uint8_t> GenerateNextMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, bool isNormalMap = false);
    [[nodiscard]] static glm::mat4 ComputeDirectionalLightMatrix(const core::AABB& sceneBounds, const glm::vec3& lightDir) noexcept;
    [[nodiscard]] static std::vector<uint8_t> GenerateEnvCubemapFaceMip(uint32_t faceIndex, uint32_t faceSize, float roughness, const glm::vec3& sunDir);

private:
    void RenderPrimitive(WGPURenderPassEncoder pass, scene::Scene& scene, scene::MeshPrimitive& prim, const glm::mat4& worldMat, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe, WGPURenderPipeline& currentBoundPipeline);
    void RenderNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, const glm::vec3& cameraPos, bool wireframe, const core::Frustum& frustum, WGPURenderPipeline& currentBoundPipeline);
    void RenderShadowNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& parentTransform);

    bool initialized_{false};
    bool bumpMappingEnabled_{true};
    bool mipmappingEnabled_{true};
    bool shadowsEnabled_{true};
    bool reflectionsEnabled_{true};
    bool occlusionCullingEnabled_{true};
    bool shadowMapDirty_{true};

    uint32_t sampleCount_{4};
    uint32_t envMipLevels_{7};
    size_t lastVisiblePrimitiveCount_{0};
    size_t lastOccludedPrimitiveCount_{0};
    const scene::Scene* lastShadowScene_{nullptr};
    glm::vec3 lastShadowLightDir_{0.0f};
    glm::mat4 currentLightViewProj_{1.0f};
    glm::vec3 currentLightDir_{0.35f, 1.0f, 0.25f};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};

    RenderPipelineHandle opaquePipeline_{};
    RenderPipelineHandle opaqueBackCulledPipeline_{};
    RenderPipelineHandle wireframePipeline_{};
    BindGroupLayoutHandle bindGroupLayout_{};

    // Directional Shadow Mapping Resources
    RenderPipelineHandle shadowPipeline_{};
    BindGroupLayoutHandle shadowBindGroupLayout_{};
    TextureHandle shadowDepthTexture_{};
    TextureViewHandle shadowDepthView_{};
    SamplerHandle shadowSampler_{};

    // Pre-filtered Environment Cubemap Resources
    TextureHandle envCubemapTexture_{};
    TextureViewHandle envCubemapView_{};

    // AABB Wireframe Resources
    RenderPipelineHandle aabbPipeline_{};
    BindGroupLayoutHandle aabbBindGroupLayout_{};
    BufferHandle aabbVertexBuffer_{};
    BufferHandle aabbUniformBuffer_{};
    BindGroupHandle aabbBindGroup_{};
    size_t aabbVertexBufferSize_{0};
    size_t aabbFrameByteOffset_{0};
    std::vector<AabbVertex> batchedAabbVertices_{};
    std::vector<AabbVertex> cachedSceneAabbVertices_{};

    // Default 1x1 Fallback Textures
    TextureHandle defaultWhiteTexture_{};
    TextureViewHandle defaultWhiteTextureView_{};

    TextureHandle defaultNormalTexture_{};
    TextureViewHandle defaultNormalTextureView_{};

    TextureHandle defaultBlackTexture_{};
    TextureViewHandle defaultBlackTextureView_{};

    // Samplers (Trilinear 16x Aniso vs Nearest Mip-0)
    SamplerHandle defaultSampler_{};
    SamplerHandle noMipSampler_{};
};

} // namespace bgl::gpu
