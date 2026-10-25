/**
 * @file Pipeline.hpp
 * @brief Hardware WebGPU RenderPipeline, HDR RGBA16Float Post-Processing Tonemapping
 *        (Khronos PBR Neutral, AgX, ACES), True HDR IBL (L2 Spherical Harmonics,
 *        Compute GGX Prefiltered Cubemap, Compute Split-Sum BRDF LUT),
 *        Clustered Forward+ KHR_lights_punctual, and Native GPU Compressed Textures (BC7/ASTC/ETC2).
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/AabbRenderer.hpp"
#include "gpu/GpuCuller.hpp"
#include "gpu/GpuRayPicker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/PostProcessPass.hpp"
#include "gpu/ShadowPass.hpp"
#include <array>
#include <glm/glm.hpp>
#include <span>
#include <string_view>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::scene {
struct Scene;
class SceneNode;
struct MeshPrimitive;
class Quadtree;
struct DecodedHdrImage;
} // namespace bgl::scene

namespace bgl::gpu {

enum class EnvironmentPreset : uint32_t {
    MediterraneanCourtyard = 0,
    SunsetStudio = 1,
    OvercastAtelier = 2,
    NightNeon = 3,
    CustomHdr = 4
};

namespace MaterialFlags {
constexpr uint32_t HasBaseColorTex = 1u << 0;
constexpr uint32_t HasMetallicRoughnessTex = 1u << 1;
constexpr uint32_t HasNormalTex = 1u << 2;
constexpr uint32_t HasEmissiveTex = 1u << 3;
constexpr uint32_t HasOcclusionTex = 1u << 4;
constexpr uint32_t HasClearcoatTex = 1u << 5;
constexpr uint32_t HasTransmissionTex = 1u << 6;
constexpr uint32_t HasSheenTex = 1u << 7;
constexpr uint32_t Unlit = 1u << 8;
constexpr uint32_t DoubleSided = 1u << 9;
constexpr uint32_t EnableBumpMapping = 1u << 10;
constexpr uint32_t EnableMipmapping = 1u << 11;
constexpr uint32_t EnableShadows = 1u << 12;
constexpr uint32_t EnableReflections = 1u << 13;
constexpr uint32_t EnableAmbientOcclusion = 1u << 14;
constexpr uint32_t EnableParallaxOcclusion = 1u << 15;
constexpr uint32_t HasThicknessTex = 1u << 16;
constexpr uint32_t HasSkinning = 1u << 17;
constexpr uint32_t HasMorphTargets = 1u << 18;
} // namespace MaterialFlags

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

    // 352..383: Volume & Attenuation (KHR_materials_volume) & UV Scale
    float thicknessFactor{0.0f};
    float attenuationDistance{1e9f};
    glm::vec2 uvScale{1.0f, 1.0f};
    glm::vec4 attenuationColor{1.0f, 1.0f, 1.0f, 1.0f};

    // 384..399: Animation & Skinning counts & UV Offset
    uint32_t jointCount{0};
    uint32_t morphTargetCount{0};
    glm::vec2 uvOffset{0.0f, 0.0f};

    // 400..415: Morph Weights (first 4 channels)
    glm::vec4 morphWeights{0.0f};

    // 416..8607: Joint Matrices for Skeletal Animation (up to 128 joints)
    glm::mat4 jointMatrices[128]{};
};
static_assert(sizeof(PrimitiveUniforms) == 8608, "PrimitiveUniforms must be exactly 8608 bytes (16-byte aligned)");

struct alignas(16) GpuPunctualLight {
    glm::vec4 posOrDir{
        0.0f}; // xyz = position (Point/Spot) or directionToLight (Directional), w = type (0=Dir, 1=Point, 2=Spot)
    glm::vec4 colorInt{0.0f};                      // xyz = color * intensity, w = range (0 = infinite)
    glm::vec4 spotParams{0.0f, -1.0f, 0.0f, 0.0f}; // xyz = spotDirection (light-to-target), w = reserved
};
static_assert(sizeof(GpuPunctualLight) == 48, "GpuPunctualLight must be 48 bytes");

struct alignas(16) SceneLightingUniforms {
    std::array<glm::vec4, 9> shIrradiance{};
    std::array<GpuPunctualLight, 16> lights{};
    std::array<glm::vec4, 16> spotAngles{};     // x = lightAngleScale, y = lightAngleOffset, z = castsShadow, w = pad
    std::array<glm::uvec4, 256> clusterMasks{}; // 1024 uint32_t bitmasks for 16 x 8 x 8 Forward+ frustum clusters
    glm::vec4 gridParams{1280.0f, 720.0f, 0.1f, 500.0f}; // x = viewportW, y = viewportH, z = zNear, w = zFar
    glm::vec4 envParams{0.0f, 0.0f, 1.0f,
                        1.0f}; // x = envRotation, y = punctualLightCount, z = useClusteredCulling, w = hasPrimarySun
    glm::mat4 viewMatrix{1.0f};
};
static_assert(sizeof(SceneLightingUniforms) == 5360, "SceneLightingUniforms must be 5360 bytes");

struct alignas(16) PrefilterComputeParams {
    float roughness{0.0f};
    uint32_t faceSize{64};
    float sourceSize{64.0f};
    uint32_t sampleCount{64};
};
static_assert(sizeof(PrefilterComputeParams) == 16, "PrefilterComputeParams must be 16 bytes");

/**
 * @class PipelineManager
 * @brief High-level orchestrator for WebGPU render pipelines, HDR post-processing,
 *        Forward+ clustered lighting, IBL environment computation, and shadow passes.
 */
class PipelineManager {
public:
    static constexpr uint32_t kShadowMapSize{2048};
    static constexpr uint32_t kEnvCubemapSize{128};
    static constexpr uint32_t kBrdfLutSize{128};
    static constexpr uint32_t kClusterCountX{16};
    static constexpr uint32_t kClusterCountY{8};
    static constexpr uint32_t kClusterCountZ{8};
    static constexpr uint32_t kTotalClusters{kClusterCountX * kClusterCountY * kClusterCountZ};

    PipelineManager() = default;
    ~PipelineManager();

    /**
     * @brief Initializes shaders, compute pipelines, bind group layouts, and default GPU textures.
     * @param device WebGPU device handle.
     * @param queue WebGPU default queue handle.
     * @param surfaceFormat Final swapchain presentation surface texture format.
     * @param sampleCount Multisample count (e.g. 4 for 4x MSAA).
     * @return Result indicating success or error string.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount = 4);

    /**
     * @brief Releases all GPU pipeline layouts, pipelines, uniform buffers, and textures.
     */
    void Shutdown() noexcept;

    /**
     * @brief Prepares and uploads GPU buffers, textures, samplers, and bind groups for a scene.
     * @param scene Scene whose primitives and textures will be registered with WebGPU.
     */
    void PrepareScene(scene::Scene& scene);

    /**
     * @brief Rebuilds texture and sampler bind groups following dynamic material changes.
     * @param scene Target scene whose material bind groups need updating.
     */
    void RebuildMaterialBindGroups(scene::Scene& scene);

    /**
     * @brief Executes the directional shadow map depth pre-pass.
     * @param encoder Command encoder to record the render pass into.
     * @param scene Scene containing geometry to cast shadows.
     * @param lightDir Normalized vector pointing towards the light source.
     * @param spatialIndex Optional spatial Quadtree for accelerated shadow frustum culling.
     */
    void RenderShadowPass(WGPUCommandEncoder encoder, scene::Scene& scene, const glm::vec3& lightDir,
                          const scene::Quadtree* spatialIndex = nullptr);

    /**
     * @brief Executes the primary HDR forward+ clustered scene render pass.
     * @param pass Active WebGPU render pass encoder targeting the HDR color/depth attachments.
     * @param scene Scene to render.
     * @param viewProj Combined view-projection matrix.
     * @param cameraPos World-space camera eye position.
     * @param wireframe Render in wireframe mode if true.
     * @param spatialIndex Optional spatial Quadtree for view frustum and occlusion culling.
     */
    void RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj,
                     const glm::vec3& cameraPos, bool wireframe = false, const scene::Quadtree* spatialIndex = nullptr,
                     const glm::mat4& viewMatrix = glm::mat4{1.0f}, const glm::mat4& projMatrix = glm::mat4{1.0f},
                     uint32_t viewportW = 1280, uint32_t viewportH = 720, float zNear = 0.1f, float zFar = 500.0f);

    /**
     * @brief Executes the fullscreen tonemapping and color space conversion pass onto the swapchain.
     * @param swapchainPass Render pass encoder targeting the final presentation surface.
     * @param hdrResolveView Resolved RGBA16Float HDR input texture view.
     * @param viewProj Optional view-projection matrix for lens flare position calculation.
     * @param cameraPos Optional world camera position.
     * @param lightDir Optional directional sun vector.
     * @param envRotation Optional environment rotation angle.
     * @param viewportW Viewport width in pixels.
     * @param viewportH Viewport height in pixels.
     */
    void RenderTonemapPass(WGPURenderPassEncoder swapchainPass, WGPUTextureView hdrResolveView,
                           const glm::mat4& viewProj = glm::mat4{1.0f}, const glm::vec3& cameraPos = glm::vec3{0.0f},
                           const glm::vec3& lightDir = glm::vec3{0.577f}, float envRotation = 0.0f,
                           uint32_t viewportW = 1280, uint32_t viewportH = 720);

    /**
     * @brief Renders a single 3D wireframe Axis-Aligned Bounding Box.
     */
    void RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj,
                    const glm::vec4& color);

    /**
     * @brief Batches scene root and mesh primitive bounding box line geometry into CPU buffers.
     */
    void BatchSceneAABBs(const scene::Scene& scene, bool showSceneAABB = true, bool showPrimitiveAABBs = true);

    /**
     * @brief Batches spatial Quadtree cell boundary lines into CPU buffers.
     */
    void BatchQuadtreeBounds(const scene::Quadtree& quadtree);

    /**
     * @brief Batches and renders scene bounding boxes in a single draw call.
     */
    void RenderSceneAABBs(WGPURenderPassEncoder pass, const scene::Scene& scene, const glm::mat4& viewProj,
                          bool showSceneAABB = true, bool showPrimitiveAABBs = true);

    /**
     * @brief Batches and renders Quadtree cell boundaries in a single draw call.
     */
    void RenderQuadtreeBounds(WGPURenderPassEncoder pass, const scene::Quadtree& quadtree, const glm::mat4& viewProj);

    /**
     * @brief Appends 12 line segments of an AABB to the wireframe batch.
     */
    void AppendAABBLines(const core::AABB& aabb, const glm::vec4& color);

    /**
     * @brief Flushes all accumulated wireframe line vertices to the GPU in a single batched draw call.
     */
    void FlushAABBLines(WGPURenderPassEncoder pass, const glm::mat4& viewProj);

    // Environment & IBL Management
    void SetEnvironmentPreset(EnvironmentPreset preset, scene::Scene* activeScene = nullptr);
    void CycleEnvironmentPreset(scene::Scene* activeScene = nullptr);
    [[nodiscard]] EnvironmentPreset GetEnvironmentPreset() const noexcept { return currentEnvPreset_; }
    [[nodiscard]] static constexpr std::string_view GetEnvironmentPresetName(EnvironmentPreset preset) noexcept {
        switch (preset) {
        case EnvironmentPreset::MediterraneanCourtyard:
            return "Mediterranean Courtyard";
        case EnvironmentPreset::SunsetStudio:
            return "Sunset Studio";
        case EnvironmentPreset::OvercastAtelier:
            return "Overcast Atelier";
        case EnvironmentPreset::NightNeon:
            return "Night Neon";
        case EnvironmentPreset::CustomHdr:
            return "Custom HDR/EXR";
        }
        return "Mediterranean Courtyard";
    }
    bool LoadCustomHdrEnvironment(const scene::DecodedHdrImage& hdrImage, scene::Scene* activeScene = nullptr);

    void SetEnvironmentRotation(float radians) noexcept {
        if (std::abs(envRotation_ - radians) > 1e-4f) {
            envRotation_ = radians;
            shadowMapDirty_ = true;
        }
    }
    [[nodiscard]] float GetEnvironmentRotation() const noexcept { return envRotation_; }

    [[nodiscard]] WGPUTextureView GetEnvironmentCubemapView() const noexcept { return envCubemapView_.Get(); }
    [[nodiscard]] WGPUSampler GetDefaultSampler() const noexcept { return defaultSampler_.Get(); }
    [[nodiscard]] const std::array<glm::vec4, 9>& GetSphericalHarmonics() const noexcept { return shIrradiance_; }

    // Tonemapping & Exposure Controls
    void SetToneMapOperator(ToneMapOperator op) noexcept { toneMapOperator_ = op; }
    void CycleToneMapOperator() noexcept {
        switch (toneMapOperator_) {
        case ToneMapOperator::KhronosPbrNeutral:
            toneMapOperator_ = ToneMapOperator::AgX;
            break;
        case ToneMapOperator::AgX:
            toneMapOperator_ = ToneMapOperator::ACES;
            break;
        case ToneMapOperator::ACES:
            toneMapOperator_ = ToneMapOperator::KhronosPbrNeutral;
            break;
        }
    }
    [[nodiscard]] ToneMapOperator GetToneMapOperator() const noexcept { return toneMapOperator_; }
    [[nodiscard]] static constexpr std::string_view GetToneMapOperatorName(ToneMapOperator op) noexcept {
        switch (op) {
        case ToneMapOperator::KhronosPbrNeutral:
            return "Khronos PBR Neutral";
        case ToneMapOperator::AgX:
            return "AgX";
        case ToneMapOperator::ACES:
            return "ACES Filmic";
        }
        return "Khronos PBR Neutral";
    }

    void SetExposure(float exposure) noexcept { exposure_ = std::clamp(exposure, 0.05f, 16.0f); }
    [[nodiscard]] float GetExposure() const noexcept { return exposure_; }

    // Feature Toggles (Bump Mapping, Mipmapping, Shadow Mapping, Environment Reflections)
    void SetBumpMappingEnabled(bool enabled) noexcept { bumpMappingEnabled_ = enabled; }
    void ToggleBumpMapping() noexcept { bumpMappingEnabled_ = !bumpMappingEnabled_; }
    [[nodiscard]] bool IsBumpMappingEnabled() const noexcept { return bumpMappingEnabled_; }

    void SetMipmappingEnabled(bool enabled, scene::Scene* activeScene = nullptr);
    void ToggleMipmapping(scene::Scene* activeScene = nullptr) {
        SetMipmappingEnabled(!mipmappingEnabled_, activeScene);
    }
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

    void SetAmbientOcclusionEnabled(bool enabled) noexcept { ambientOcclusionEnabled_ = enabled; }
    void ToggleAmbientOcclusion() noexcept { ambientOcclusionEnabled_ = !ambientOcclusionEnabled_; }
    [[nodiscard]] bool IsAmbientOcclusionEnabled() const noexcept { return ambientOcclusionEnabled_; }

    void SetAoIntensity(float intensity) noexcept { aoIntensity_ = std::clamp(intensity, 0.0f, 2.0f); }
    [[nodiscard]] float GetAoIntensity() const noexcept { return aoIntensity_; }

    void SetParallaxEnabled(bool enabled) noexcept { parallaxEnabled_ = enabled; }
    void ToggleParallaxOcclusion() noexcept { parallaxEnabled_ = !parallaxEnabled_; }
    [[nodiscard]] bool IsParallaxEnabled() const noexcept { return parallaxEnabled_; }

    void SetParallaxScale(float scale) noexcept { parallaxScale_ = std::clamp(scale, 0.0f, 0.2f); }
    [[nodiscard]] float GetParallaxScale() const noexcept { return parallaxScale_; }

    void SetLensFlareEnabled(bool enabled) noexcept { lensFlareEnabled_ = enabled; }
    void ToggleLensFlare() noexcept { lensFlareEnabled_ = !lensFlareEnabled_; }
    [[nodiscard]] bool IsLensFlareEnabled() const noexcept { return lensFlareEnabled_; }

    void SetLensFlareIntensity(float intensity) noexcept { lensFlareIntensity_ = std::clamp(intensity, 0.0f, 5.0f); }
    [[nodiscard]] float GetLensFlareIntensity() const noexcept { return lensFlareIntensity_; }

    void SetBloomEnabled(bool enabled) noexcept { bloomEnabled_ = enabled; }
    void ToggleBloom() noexcept { bloomEnabled_ = !bloomEnabled_; }
    [[nodiscard]] bool IsBloomEnabled() const noexcept { return bloomEnabled_; }

    void SetBloomIntensity(float intensity) noexcept { bloomIntensity_ = std::max(0.0f, intensity); }
    [[nodiscard]] float GetBloomIntensity() const noexcept { return bloomIntensity_; }

    void SetBloomThreshold(float threshold) noexcept { bloomThreshold_ = std::max(0.0f, threshold); }
    [[nodiscard]] float GetBloomThreshold() const noexcept { return bloomThreshold_; }

    void SetSsaoEnabled(bool enabled) noexcept { ssaoEnabled_ = enabled; }
    void ToggleSSAO() noexcept { ssaoEnabled_ = !ssaoEnabled_; }
    [[nodiscard]] bool IsSsaoEnabled() const noexcept { return ssaoEnabled_; }

    void SetSsaoIntensity(float intensity) noexcept { ssaoIntensity_ = std::clamp(intensity, 0.0f, 2.0f); }
    [[nodiscard]] float GetSsaoIntensity() const noexcept { return ssaoIntensity_; }

    void SetSsaoRadius(float radius) noexcept { ssaoRadius_ = std::clamp(radius, 0.001f, 0.1f); }
    [[nodiscard]] float GetSsaoRadius() const noexcept { return ssaoRadius_; }

    void SetOcclusionCullingEnabled(bool enabled) noexcept { occlusionCullingEnabled_ = enabled; }
    void ToggleOcclusionCulling() noexcept { occlusionCullingEnabled_ = !occlusionCullingEnabled_; }
    [[nodiscard]] bool IsOcclusionCullingEnabled() const noexcept { return occlusionCullingEnabled_; }

    void EnsureOpaqueCopyTexture(uint32_t width, uint32_t height);

    [[nodiscard]] size_t GetVisiblePrimitiveCount() const noexcept { return lastVisiblePrimitiveCount_; }
    [[nodiscard]] size_t GetOccludedPrimitiveCount() const noexcept { return lastOccludedPrimitiveCount_; }
    [[nodiscard]] uint32_t GetActivePunctualLightCount() const noexcept { return activePunctualLightCount_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

    // Modular Render Passes & GPU Compute Helpers
    [[nodiscard]] AabbRenderer& GetAabbRenderer() noexcept { return aabbRenderer_; }
    [[nodiscard]] const AabbRenderer& GetAabbRenderer() const noexcept { return aabbRenderer_; }
    [[nodiscard]] ShadowPass& GetShadowPass() noexcept { return shadowPass_; }
    [[nodiscard]] const ShadowPass& GetShadowPass() const noexcept { return shadowPass_; }
    [[nodiscard]] PostProcessPass& GetPostProcessPass() noexcept { return postProcessPass_; }
    [[nodiscard]] const PostProcessPass& GetPostProcessPass() const noexcept { return postProcessPass_; }
    [[nodiscard]] GpuCuller& GetGpuCuller() noexcept { return gpuCuller_; }
    [[nodiscard]] const GpuCuller& GetGpuCuller() const noexcept { return gpuCuller_; }
    [[nodiscard]] GpuRayPicker& GetGpuRayPicker() noexcept { return gpuRayPicker_; }
    [[nodiscard]] const GpuRayPicker& GetGpuRayPicker() const noexcept { return gpuRayPicker_; }

    // Pure CPU helpers for Mipmap Generation, Shadow Matrices, SH Irradiance, Tonemapping, and Clustered Forward+ (also
    // used by unit tests)
    [[nodiscard]] static uint32_t CalculateMipLevels(uint32_t width, uint32_t height) noexcept;
    [[nodiscard]] static std::vector<uint8_t> GenerateNextMipLevel(std::span<const uint8_t> src, uint32_t srcW,
                                                                   uint32_t srcH, uint32_t dstW, uint32_t dstH,
                                                                   bool isNormalMap = false);
    [[nodiscard]] static std::vector<uint8_t> GenerateNextMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                                                                   uint32_t dstW, uint32_t dstH,
                                                                   bool isNormalMap = false);
    [[nodiscard]] static glm::mat4 ComputeDirectionalLightMatrix(const core::AABB& sceneBounds,
                                                                 const glm::vec3& lightDir) noexcept;
    [[nodiscard]] static std::vector<uint8_t> GenerateEnvCubemapFaceMip(uint32_t faceIndex, uint32_t faceSize,
                                                                        float roughness, const glm::vec3& sunDir);
    [[nodiscard]] static std::vector<float> GenerateProceduralHdrCubemapFace(uint32_t faceIndex, uint32_t faceSize,
                                                                             EnvironmentPreset preset,
                                                                             const glm::vec3& sunDir);
    [[nodiscard]] static std::vector<float> ConvertEquirectHdrToCubemapFace(const scene::DecodedHdrImage& hdrImg,
                                                                            uint32_t faceIndex, uint32_t faceSize);
    [[nodiscard]] static std::array<glm::vec4, 9>
    ComputeSphericalHarmonicsL2(const std::array<std::vector<float>, 6>& cubemapFaces, uint32_t faceSize) noexcept;
    [[nodiscard]] static glm::vec3 EvaluateSphericalHarmonicsL2(const std::array<glm::vec4, 9>& shCoeffs,
                                                                const glm::vec3& normal) noexcept;
    [[nodiscard]] static glm::vec3 ApplyToneMappingCPU(const glm::vec3& linearHdr, float exposure,
                                                       ToneMapOperator op) noexcept;
    [[nodiscard]] static glm::vec3 EvaluateBeerLambertVolumeAttenuation(const glm::vec3& attenuationColor,
                                                                        float attenuationDistance, float thickness,
                                                                        float NdotV) noexcept;
    [[nodiscard]] static glm::vec3 EvaluateBloomThreshold(const glm::vec3& color, float exposure,
                                                          float threshold) noexcept;
    [[nodiscard]] static float EvaluateScreenSpaceOcclusionFactor(float depthCenter, float depthNeighbor, float radius,
                                                                  float bias) noexcept;
    static void BuildClusteredLightGrid(std::span<const GpuPunctualLight> lights, uint32_t lightCount,
                                        const glm::mat4& viewMatrix, const glm::mat4& projMatrix, float zNear,
                                        float zFar, std::array<glm::uvec4, 256>& outClusterMasks) noexcept;

private:
    void InitializeComputeIBLPipelines();
    void InitializeTonemapPipeline();
    void DispatchBrdfLutCompute();
    void BuildEnvironmentFromHdrFaces(const std::array<std::vector<float>, 6>& hdrFaces, uint32_t faceSize);
    void UpdateSceneLightingUniforms(scene::Scene& scene, const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                                     uint32_t viewportW, uint32_t viewportH, float zNear, float zFar);

    void RenderPrimitive(WGPURenderPassEncoder pass, scene::Scene& scene, scene::MeshPrimitive& prim,
                         const glm::mat4& worldMat, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                         bool wireframe, WGPURenderPipeline& currentBoundPipeline,
                         const scene::SceneNode* node = nullptr);
    void RenderNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& viewProj,
                    const glm::mat4& parentTransform, const glm::vec3& cameraPos, bool wireframe,
                    const core::Frustum& frustum, WGPURenderPipeline& currentBoundPipeline);
    void RenderShadowNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node,
                          const glm::mat4& parentTransform);

    bool initialized_{false};
    bool bumpMappingEnabled_{true};
    bool mipmappingEnabled_{true};
    bool shadowsEnabled_{true};
    bool reflectionsEnabled_{true};
    bool ambientOcclusionEnabled_{true};
    float aoIntensity_{1.0f};
    bool parallaxEnabled_{true};
    float parallaxScale_{0.035f};
    bool lensFlareEnabled_{true};
    float lensFlareIntensity_{0.75f};
    bool bloomEnabled_{true};
    float bloomIntensity_{0.65f};
    float bloomThreshold_{1.05f};
    bool ssaoEnabled_{true};
    float ssaoIntensity_{0.75f};
    float ssaoRadius_{0.02f};
    bool occlusionCullingEnabled_{true};
    bool shadowMapDirty_{true};

    TextureHandle opaqueColorCopyTexture_{};
    TextureViewHandle opaqueColorCopyView_{};
    uint32_t opaqueCopyWidth_{0};
    uint32_t opaqueCopyHeight_{0};

    ToneMapOperator toneMapOperator_{ToneMapOperator::KhronosPbrNeutral};
    EnvironmentPreset currentEnvPreset_{EnvironmentPreset::MediterraneanCourtyard};
    float exposure_{1.0f};
    float envRotation_{0.0f};
    uint32_t activePunctualLightCount_{0};

    uint32_t sampleCount_{4};
    uint32_t envMipLevels_{8};
    size_t lastVisiblePrimitiveCount_{0};
    size_t lastOccludedPrimitiveCount_{0};
    const scene::Scene* lastShadowScene_{nullptr};
    glm::vec3 lastShadowLightDir_{0.0f};
    glm::mat4 currentLightViewProj_{1.0f};
    glm::vec3 currentLightDir_{0.35f, 1.0f, 0.25f};
    std::array<glm::vec4, 9> shIrradiance_{};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};
    WGPUTextureFormat hdrFormat_{WGPUTextureFormat_RGBA16Float};

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

    // True HDR IBL Resources (RGBA16Float Cubemap + Compute GGX Prefilter + Compute BRDF LUT + SceneLightingUniforms)
    TextureHandle envCubemapTexture_{};
    TextureViewHandle envCubemapView_{};
    TextureHandle brdfLutTexture_{};
    TextureViewHandle brdfLutView_{};
    ComputePipelineHandle brdfLutComputePipeline_{};
    BindGroupLayoutHandle brdfLutComputeBgl_{};
    ComputePipelineHandle ggxPrefilterComputePipeline_{};
    BindGroupLayoutHandle ggxPrefilterComputeBgl_{};
    BufferHandle ggxPrefilterUniformBuffer_{};
    BufferHandle sceneLightingBuffer_{};

    // Fullscreen HDR Tonemapping Post-Process Resources
    RenderPipelineHandle tonemapPipeline_{};
    BindGroupLayoutHandle tonemapBindGroupLayout_{};
    BufferHandle tonemapUniformBuffer_{};
    SamplerHandle tonemapSampler_{};
    WGPUTextureView cachedHdrResolveView_{nullptr};
    BindGroupHandle tonemapBindGroup_{};

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

    // Modular Extracted Passes & Compute Engines
    AabbRenderer aabbRenderer_{};
    ShadowPass shadowPass_{};
    PostProcessPass postProcessPass_{};
    GpuCuller gpuCuller_{};
    GpuRayPicker gpuRayPicker_{};
};

} // namespace bgl::gpu
