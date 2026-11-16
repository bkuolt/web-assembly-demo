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
#include "gpu/DrawHelpers.hpp"
#include "gpu/GpuCuller.hpp"
#include "gpu/GpuRayPicker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/PostProcessPass.hpp"
#include "gpu/ShadowPass.hpp"
#include "gpu/Uniforms.hpp"
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

enum class WeatherMode : uint32_t { None = 0, Rain = 1, Snow = 2, Fog = 3, Smoke = 4 };

enum class DebugViewMode : uint32_t { LitPbr = 0, LodHeatmap = 1, Normals = 2, MetallicRoughnessAo = 3, UvChecker = 4 };

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
     * @brief Recompiles and hot-swaps GPU pipelines affected by the given modified entry WGSL virtual paths.
     */
    core::Result<void> ReloadShaders(std::span<const std::string> modifiedVirtualPaths);

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
     * @brief Re-transcodes loaded KTX2 textures to a new target GPU compression format using a PMR staging arena.
     */
    void RetranscodeKtxTextures(scene::Scene& scene, scene::KtxTranscodeTarget target);

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
     * @brief Prepares frame visibility/lighting/sorting and renders only opaque/alpha-masked primitives.
     */
    void RenderOpaqueScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj,
                           const glm::vec3& cameraPos, bool wireframe = false,
                           const scene::Quadtree* spatialIndex = nullptr, const glm::mat4& viewMatrix = glm::mat4{1.0f},
                           const glm::mat4& projMatrix = glm::mat4{1.0f}, uint32_t viewportW = 1280,
                           uint32_t viewportH = 720, float zNear = 0.1f, float zFar = 500.0f);

    /**
     * @brief Renders back-to-front alpha-blended and transmissive primitives prepared by RenderOpaqueScene.
     */
    void RenderTransparentScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj,
                                bool wireframe = false);

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
            shadowPass_.MarkDirty();
        }
    }
    [[nodiscard]] float GetEnvironmentRotation() const noexcept { return envRotation_; }

    [[nodiscard]] WGPUTextureView GetEnvironmentCubemapView() const noexcept { return envCubemapView_.Get(); }
    [[nodiscard]] WGPUSampler GetDefaultSampler() const noexcept { return defaultSampler_.Get(); }
    [[nodiscard]] const std::array<glm::vec4, 9>& GetSphericalHarmonics() const noexcept { return shIrradiance_; }

    // Tonemapping & Exposure Controls (forwarded to single-source PostProcessPass)
    void SetToneMapOperator(ToneMapOperator op) noexcept { postProcessPass_.SetToneMapOperator(op); }
    void CycleToneMapOperator() noexcept { postProcessPass_.CycleToneMapOperator(); }
    [[nodiscard]] ToneMapOperator GetToneMapOperator() const noexcept { return postProcessPass_.GetToneMapOperator(); }
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

    void SetExposure(float exposure) noexcept { postProcessPass_.SetExposure(exposure); }
    [[nodiscard]] float GetExposure() const noexcept { return postProcessPass_.GetExposure(); }

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
            shadowPass_.MarkDirty();
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

    void SetLensFlareEnabled(bool enabled) noexcept { postProcessPass_.SetLensFlareEnabled(enabled); }
    void ToggleLensFlare() noexcept { postProcessPass_.ToggleLensFlare(); }
    [[nodiscard]] bool IsLensFlareEnabled() const noexcept { return postProcessPass_.IsLensFlareEnabled(); }

    void SetLensFlareIntensity(float intensity) noexcept { postProcessPass_.SetLensFlareIntensity(intensity); }
    [[nodiscard]] float GetLensFlareIntensity() const noexcept { return postProcessPass_.GetLensFlareIntensity(); }

    void SetBloomEnabled(bool enabled) noexcept { postProcessPass_.SetBloomEnabled(enabled); }
    void ToggleBloom() noexcept { postProcessPass_.ToggleBloom(); }
    [[nodiscard]] bool IsBloomEnabled() const noexcept { return postProcessPass_.IsBloomEnabled(); }

    void SetBloomIntensity(float intensity) noexcept { postProcessPass_.SetBloomIntensity(intensity); }
    [[nodiscard]] float GetBloomIntensity() const noexcept { return postProcessPass_.GetBloomIntensity(); }

    void SetBloomThreshold(float threshold) noexcept { postProcessPass_.SetBloomThreshold(threshold); }
    [[nodiscard]] float GetBloomThreshold() const noexcept { return postProcessPass_.GetBloomThreshold(); }

    void SetSsaoEnabled(bool enabled) noexcept { postProcessPass_.SetSsaoEnabled(enabled); }
    void ToggleSSAO() noexcept { postProcessPass_.ToggleSSAO(); }
    [[nodiscard]] bool IsSsaoEnabled() const noexcept { return postProcessPass_.IsSsaoEnabled(); }

    void SetSsaoIntensity(float intensity) noexcept { postProcessPass_.SetSsaoIntensity(intensity); }
    [[nodiscard]] float GetSsaoIntensity() const noexcept { return postProcessPass_.GetSsaoIntensity(); }

    void SetSsaoRadius(float radius) noexcept { postProcessPass_.SetSsaoRadius(radius); }
    [[nodiscard]] float GetSsaoRadius() const noexcept { return postProcessPass_.GetSsaoRadius(); }

    void SetOcclusionCullingEnabled(bool enabled) noexcept { occlusionCullingEnabled_ = enabled; }
    void ToggleOcclusionCulling() noexcept { occlusionCullingEnabled_ = !occlusionCullingEnabled_; }
    [[nodiscard]] bool IsOcclusionCullingEnabled() const noexcept { return occlusionCullingEnabled_; }

    // Level-of-Detail (LOD) Management & Multi-Mode Debug Render Views
    void SetLodEnabled(bool enabled) noexcept { lodEnabled_ = enabled; }
    void ToggleLod() noexcept { lodEnabled_ = !lodEnabled_; }
    [[nodiscard]] bool IsLodEnabled() const noexcept { return lodEnabled_; }

    void SetLodBias(float bias) noexcept { lodBias_ = std::clamp(bias, 0.25f, 4.0f); }
    [[nodiscard]] float GetLodBias() const noexcept { return lodBias_; }

    void SetAutoLodEnabled(bool enabled) noexcept { autoLodEnabled_ = enabled; }
    void ToggleAutoLod() noexcept { autoLodEnabled_ = !autoLodEnabled_; }
    [[nodiscard]] bool IsAutoLodEnabled() const noexcept { return autoLodEnabled_; }
    void UpdateAutoLod(float deltaTime) noexcept;

    void SetDebugViewMode(DebugViewMode mode) noexcept { debugViewMode_ = mode; }
    void CycleDebugViewMode() noexcept {
        switch (debugViewMode_) {
        case DebugViewMode::LitPbr:
            debugViewMode_ = DebugViewMode::LodHeatmap;
            break;
        case DebugViewMode::LodHeatmap:
            debugViewMode_ = DebugViewMode::Normals;
            break;
        case DebugViewMode::Normals:
            debugViewMode_ = DebugViewMode::MetallicRoughnessAo;
            break;
        case DebugViewMode::MetallicRoughnessAo:
            debugViewMode_ = DebugViewMode::UvChecker;
            break;
        case DebugViewMode::UvChecker:
            debugViewMode_ = DebugViewMode::LitPbr;
            break;
        }
    }
    void ToggleLodHeatmap() noexcept {
        debugViewMode_ =
            (debugViewMode_ == DebugViewMode::LodHeatmap) ? DebugViewMode::LitPbr : DebugViewMode::LodHeatmap;
    }
    [[nodiscard]] DebugViewMode GetDebugViewMode() const noexcept { return debugViewMode_; }
    [[nodiscard]] static constexpr std::string_view GetDebugViewModeName(DebugViewMode mode) noexcept {
        switch (mode) {
        case DebugViewMode::LitPbr:
            return "Lit PBR";
        case DebugViewMode::LodHeatmap:
            return "LOD Heatmap";
        case DebugViewMode::Normals:
            return "Normals";
        case DebugViewMode::MetallicRoughnessAo:
            return "Metal/Rough/AO";
        case DebugViewMode::UvChecker:
            return "UV Checker";
        }
        return "Lit PBR";
    }

    [[nodiscard]] size_t GetRenderedTriangleCount() const noexcept { return lastRenderedTriangleCount_; }
    [[nodiscard]] size_t GetTotalVisibleTriangleCount() const noexcept { return lastTotalVisibleTriangleCount_; }
    [[nodiscard]] const std::array<uint32_t, 4>& GetLodLevelCounts() const noexcept { return lastLodCounts_; }

    // Atmospheric Weather Controls (Rain, Snow, Fog, Smoke) & Diurnal Time-of-Day
    void SetWeatherMode(WeatherMode mode) noexcept { weatherMode_ = mode; }
    void CycleWeatherMode() noexcept {
        switch (weatherMode_) {
        case WeatherMode::None:
            weatherMode_ = WeatherMode::Rain;
            break;
        case WeatherMode::Rain:
            weatherMode_ = WeatherMode::Snow;
            break;
        case WeatherMode::Snow:
            weatherMode_ = WeatherMode::Fog;
            break;
        case WeatherMode::Fog:
            weatherMode_ = WeatherMode::Smoke;
            break;
        case WeatherMode::Smoke:
            weatherMode_ = WeatherMode::None;
            break;
        }
    }
    void ToggleRain() noexcept {
        weatherMode_ = (weatherMode_ == WeatherMode::Rain) ? WeatherMode::None : WeatherMode::Rain;
    }
    void ToggleSnow() noexcept {
        weatherMode_ = (weatherMode_ == WeatherMode::Snow) ? WeatherMode::None : WeatherMode::Snow;
    }
    void ToggleFog() noexcept {
        weatherMode_ = (weatherMode_ == WeatherMode::Fog) ? WeatherMode::None : WeatherMode::Fog;
    }
    void ToggleSmoke() noexcept {
        weatherMode_ = (weatherMode_ == WeatherMode::Smoke) ? WeatherMode::None : WeatherMode::Smoke;
    }
    [[nodiscard]] WeatherMode GetWeatherMode() const noexcept { return weatherMode_; }
    [[nodiscard]] static constexpr std::string_view GetWeatherModeName(WeatherMode mode) noexcept {
        switch (mode) {
        case WeatherMode::None:
            return "Clear";
        case WeatherMode::Rain:
            return "Rain";
        case WeatherMode::Snow:
            return "Snow";
        case WeatherMode::Fog:
            return "Fog";
        case WeatherMode::Smoke:
            return "Smoke";
        }
        return "Clear";
    }

    void SetWeatherIntensity(float intensity) noexcept { weatherIntensity_ = std::clamp(intensity, 0.0f, 2.0f); }
    [[nodiscard]] float GetWeatherIntensity() const noexcept { return weatherIntensity_; }

    void SetWindStrength(float wind) noexcept { windStrength_ = std::clamp(wind, -1.0f, 1.0f); }
    [[nodiscard]] float GetWindStrength() const noexcept { return windStrength_; }

    void AdvanceWeatherTime(float dt) noexcept { weatherTimeSec_ += std::max(0.0f, dt); }
    void SetWeatherTime(float timeSec) noexcept { weatherTimeSec_ = std::max(0.0f, timeSec); }
    [[nodiscard]] float GetWeatherTime() const noexcept { return weatherTimeSec_; }

    void SetTimeOfDay(float hour) noexcept;
    [[nodiscard]] float GetTimeOfDay() const noexcept { return timeOfDay_; }
    [[nodiscard]] float GetSolarElevationFactor() const noexcept { return solarElevation_; }

    void EnsureOpaqueCopyTexture(uint32_t width, uint32_t height);
    [[nodiscard]] WGPUTexture GetOpaqueColorCopyTexture() const noexcept { return opaqueColorCopyTexture_.Get(); }
    [[nodiscard]] uint32_t GetOpaqueCopyWidth() const noexcept { return opaqueCopyWidth_; }
    [[nodiscard]] uint32_t GetOpaqueCopyHeight() const noexcept { return opaqueCopyHeight_; }

    [[nodiscard]] static bool IsMaterialTransparentOrTransmissive(const scene::Material& mat) noexcept;
    [[nodiscard]] static WGPUBlendState GetTransparentBlendState() noexcept;

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
    [[nodiscard]] GpuCuller& GetGpuCuller() noexcept;
    [[nodiscard]] const GpuCuller& GetGpuCuller() const noexcept { return gpuCuller_; }
    [[nodiscard]] GpuRayPicker& GetGpuRayPicker() noexcept;
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
    [[nodiscard]] static glm::vec3 EvaluateRainWetAlbedo(const glm::vec3& dryAlbedo, float metallic,
                                                         float intensity) noexcept;
    [[nodiscard]] static float EvaluateRainWetRoughness(float dryRoughness, float intensity) noexcept;
    [[nodiscard]] static float EvaluateSnowAccumulationFactor(float normalY, float intensity) noexcept;
    [[nodiscard]] static float EvaluateFogFactor(float distance, float worldY, float intensity) noexcept;
    [[nodiscard]] static float EvaluateSmokeDensity(float worldY, float intensity) noexcept;
    [[nodiscard]] static glm::vec3 EvaluateDiurnalSunRadiance(float timeOfDayHours) noexcept;
    [[nodiscard]] static glm::vec3 EvaluateDiurnalAmbientScale(float timeOfDayHours) noexcept;
    [[nodiscard]] static glm::vec3 EvaluateLodHeatmapColor(uint32_t lodLevel, float transition = 0.0f) noexcept;
    [[nodiscard]] static float EvaluateBayerDither4x4(uint32_t x, uint32_t y) noexcept;
    static void BuildClusteredLightGrid(std::span<const GpuPunctualLight> lights, uint32_t lightCount,
                                        const glm::mat4& viewMatrix, const glm::mat4& projMatrix, float zNear,
                                        float zFar, std::array<glm::uvec4, 256>& outClusterMasks) noexcept;

private:
    struct RenderItem {
        scene::SceneNode* node{nullptr};
        size_t primitiveIndex{0};
        glm::mat4 worldTransform{1.0f};
        float distance{0.0f};
        int materialIndex{-1};
        bool isTransparent{false};
        bool isDoubleSided{false};
        uint32_t lodLevel{0};
        float lodTransition{0.0f};
        uint32_t instanceStart{0};
        uint32_t instanceCount{1};
    };

    struct DrawDispatchItem {
        scene::MeshPrimitive* prim{nullptr};
        WGPUBindGroup skinBindGroup{nullptr};
        WGPURenderPipeline pipeline{nullptr};
        uint32_t dynamicOffset{0};
        uint32_t clampedLod{0};
        uint32_t instanceStart{0};
        uint32_t instanceCount{1};
    };

    core::Result<void> CreateSharedBindGroupLayouts();
    core::Result<void> CreateDummyStorageBuffers();
    void CreateSamplersAndLightingBuffer();
    void CreateFallbackTextures();
    void InitializeComputeIBLPipelines();
    void InitMipGenPipelines();
    core::Result<void> CreateMainRenderPipelines(bool validateScope);
    core::Result<void> CreateMipGenPipelines(bool validateScope);
    void GenerateTextureMipsGpu(WGPUCommandEncoder encoder, WGPUTexture texture, uint32_t width, uint32_t height,
                                uint32_t mipLevels, bool isSrgb, bool isNormalMap);
    void DispatchBrdfLutCompute();
    void BuildEnvironmentFromHdrFaces(const std::array<std::vector<float>, 6>& hdrFaces, uint32_t faceSize);
    void RebuildSceneGlobalsBindGroup();
    void UpdateSceneLightingUniforms(scene::Scene& scene, const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                                     const glm::vec3& cameraPos, uint32_t viewportW, uint32_t viewportH, float zNear,
                                     float zFar);
    void PrepareFrameScene(scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                           const scene::Quadtree* spatialIndex, const glm::mat4& viewMatrix,
                           const glm::mat4& projMatrix, uint32_t viewportW, uint32_t viewportH, float zNear,
                           float zFar);
    void EncodeDrawRange(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj, bool wireframe,
                         bool transparentPass, size_t startIdx, size_t endIdx,
                         DynamicUniformRing<PerDrawUniforms>& ring);

    bool initialized_{false};
    bool bumpMappingEnabled_{true};
    bool mipmappingEnabled_{true};
    bool shadowsEnabled_{true};
    bool reflectionsEnabled_{true};
    bool ambientOcclusionEnabled_{true};
    float aoIntensity_{1.0f};
    bool parallaxEnabled_{true};
    float parallaxScale_{0.035f};
    bool occlusionCullingEnabled_{true};
    bool lodEnabled_{true};
    bool autoLodEnabled_{false};
    float lodBias_{1.0f};
    DebugViewMode debugViewMode_{DebugViewMode::LitPbr};
    size_t lastRenderedTriangleCount_{0};
    size_t lastTotalVisibleTriangleCount_{0};
    std::array<uint32_t, 4> lastLodCounts_{0, 0, 0, 0};
    WeatherMode weatherMode_{WeatherMode::None};
    float weatherIntensity_{1.0f};
    float windStrength_{0.3f};
    float weatherTimeSec_{0.0f};
    float timeOfDay_{14.0f};
    float solarElevation_{0.8660254f}; // -cos(14/24 * 2 * pi)

    TextureHandle opaqueColorCopyTexture_{};
    TextureViewHandle opaqueColorCopyView_{};
    uint32_t opaqueCopyWidth_{0};
    uint32_t opaqueCopyHeight_{0};

    EnvironmentPreset currentEnvPreset_{EnvironmentPreset::MediterraneanCourtyard};
    float envRotation_{0.0f};
    uint32_t activePunctualLightCount_{0};

    uint32_t sampleCount_{4};
    uint32_t envMipLevels_{8};
    size_t lastVisiblePrimitiveCount_{0};
    size_t lastOccludedPrimitiveCount_{0};
    glm::mat4 currentLightViewProj_{1.0f};
    glm::vec3 currentLightDir_{0.35f, 1.0f, 0.25f};
    std::array<glm::vec4, 9> shIrradiance_{};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};
    WGPUTextureFormat hdrFormat_{WGPUTextureFormat_RGBA16Float};

    PipelineLayoutHandle mainPipelineLayout_{};
    RenderPipelineHandle opaquePipeline_{};
    RenderPipelineHandle opaqueBackCulledPipeline_{};
    RenderPipelineHandle blendPipeline_{};
    RenderPipelineHandle blendBackCulledPipeline_{};
    RenderPipelineHandle wireframePipeline_{};
    BindGroupLayoutHandle sceneGlobalsBgl_{};
    BindGroupLayoutHandle perDrawBgl_{};
    BindGroupLayoutHandle materialBgl_{};
    BindGroupLayoutHandle skinBgl_{};
    BindGroupHandle sceneGlobalsBindGroup_{};
    DynamicUniformRing<PerDrawUniforms> perDrawRing_{};
    DynamicUniformRing<PerDrawUniforms> transparentPerDrawRing_{};
    DynamicInstanceBuffer instanceBuffer_{};
    size_t transparentStartIndex_{0};
    BufferHandle dummyMorphDeltaBuffer_{};
    BufferHandle dummySkinBuffer_{};
    BindGroupHandle dummySkinBindGroup_{};

    BindGroupLayoutHandle mipGenBgl_{};
    PipelineLayoutHandle mipGenPipelineLayout_{};
    RenderPipelineHandle mipGenUnormPipeline_{};
    RenderPipelineHandle mipGenSrgbPipeline_{};
    RenderPipelineHandle mipGenNormalPipeline_{};

    uint64_t skinFrameId_{1};
    bool skinFrameAdvanced_{false};
    std::vector<scene::QuadtreeItem> visibleItemsScratch_{};
    std::vector<RenderItem> renderItemsScratch_{};
    std::vector<DrawDispatchItem> drawCallsScratch_{};
    std::vector<PerDrawUniforms> mainPerDrawScratch_{};
    std::vector<glm::mat4> frameInstanceTransformsScratch_{};
    std::unordered_map<const scene::SceneNode*, uint32_t> nodeInstanceStartScratch_{};

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
