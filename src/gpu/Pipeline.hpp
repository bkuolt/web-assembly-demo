/**
 * @file Pipeline.hpp
 * @brief Hardware WebGPU RenderPipeline and glTF mesh rendering.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <glm/glm.hpp>
#include "core/Types.hpp"

namespace bgl::scene {
struct Scene;
class SceneNode;
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
}

struct alignas(16) PrimitiveUniforms {
    // 0..127: Matrices
    glm::mat4 viewProj{1.0f};
    glm::mat4 model{1.0f};

    // 128..159: Camera & Light
    glm::vec4 cameraPos{0.0f};
    glm::vec4 lightDir{0.577f, 0.577f, 0.577f, 0.0f};

    // 160..191: Base Color & PBR Core
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;

    // 192..223: Emissive & Alpha & Flags
    glm::vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f}; // xyz = rgb, w = strength
    float alphaCutoff = 0.5f;
    uint32_t alphaMode = 0; // 0=Opaque, 1=Mask, 2=Blend
    uint32_t flags = 0;
    float ior = 1.5f;

    // 224..255: Clearcoat, Transmission, Sheen
    float clearcoatFactor = 0.0f;
    float clearcoatRoughness = 0.0f;
    float transmissionFactor = 0.0f;
    float specularFactor = 1.0f;
    glm::vec4 sheenColorFactor{0.0f, 0.0f, 0.0f, 0.0f}; // xyz = color, w = roughness

    // 256..271: Specular Color
    glm::vec4 specularColorFactor{1.0f, 1.0f, 1.0f, 0.0f};
};
static_assert(sizeof(PrimitiveUniforms) == 272, "PrimitiveUniforms must be exactly 272 bytes (16-byte aligned)");

class PipelineManager {
public:
    PipelineManager() = default;
    ~PipelineManager();

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat);

    void PrepareScene(scene::Scene& scene);
    void RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe = false);

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    void RenderNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, const glm::vec3& cameraPos, bool wireframe);

    bool initialized_ = false;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUTextureFormat surfaceFormat_ = WGPUTextureFormat_BGRA8Unorm;

    WGPURenderPipeline opaquePipeline_ = nullptr;
    WGPURenderPipeline wireframePipeline_ = nullptr;
    WGPUBindGroupLayout bindGroupLayout_ = nullptr;

    // Default 1x1 Fallback Textures
    WGPUTexture defaultWhiteTexture_ = nullptr;
    WGPUTextureView defaultWhiteTextureView_ = nullptr;

    WGPUTexture defaultNormalTexture_ = nullptr;
    WGPUTextureView defaultNormalTextureView_ = nullptr;

    WGPUTexture defaultBlackTexture_ = nullptr;
    WGPUTextureView defaultBlackTextureView_ = nullptr;

    WGPUSampler defaultSampler_ = nullptr;
};

} // namespace bgl::gpu
