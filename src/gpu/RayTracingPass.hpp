/**
 * @file RayTracingPass.hpp
 * @brief Desktop WebGPU Hybrid Ray Tracing (G-Buffer, RT Shadows, RTAO, RT Reflections, Temporal/À-Trous Denoiser,
 *        and Progressive Compute Path Tracer).
 */
#ifndef BGL_GPU_RAYTRACINGPASS_HPP
#define BGL_GPU_RAYTRACINGPASS_HPP

#include "core/Types.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/Handle.hpp"
#include "gpu/RayTracingScene.hpp"
#include "gpu/Uniforms.hpp"
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::scene {
struct Scene;
struct MeshPrimitive;
class SceneNode;
} // namespace bgl::scene

namespace bgl::gpu {

struct alignas(16) RtPassUniforms {
    glm::mat4 invViewProj{1.0f};
    glm::mat4 prevViewProj{1.0f};
    glm::mat4 viewProj{1.0f};
    glm::vec4 cameraPos{0.0f}; // xyz = pos, w = frameIndex
    glm::vec4 sunDir{0.35f, 1.0f, 0.25f, 0.025f}; // xyz = sunDir, w = shadowSoftness
    glm::vec4 params0{1280.0f, 720.0f, 640.0f, 360.0f}; // fullW, fullH, traceW, traceH
    glm::vec4 params1{0.65f, 1.0f, 1.0f, 0.0f}; // aoRadius, aoIntensity, reflIntensity, envRotation
    glm::vec4 params2{3.0f, 0.0f, 1.0f, 1.0f};  // maxBounces, pathSampleIndex, halfResFlag, hasPrimarySun
    glm::vec4 params3{0.0f, 0.866f, 8.0f, 1.0f}; // punctualCount, solarElev, envMipCount, stepStride
    std::array<glm::vec4, 4> lightsPos{};
    std::array<glm::vec4, 4> lightsCol{};
};
static_assert(sizeof(RtPassUniforms) == 416, "RtPassUniforms must be 416 bytes");

struct alignas(16) RtCompositeUniforms {
    glm::vec4 params{1280.0f, 720.0f, 640.0f, 360.0f};
};
static_assert(sizeof(RtCompositeUniforms) == 16, "RtCompositeUniforms must be 16 bytes");

/**
 * @struct RtConfig
 * @brief Runtime tuning knobs for Desktop Ray Tracing passes.
 */
struct RtConfig {
    bool enabled{true};
    bool shadows{true};
    bool ambientOcclusion{true};
    bool reflections{true};
    bool pathTracing{false};
    bool halfRes{true};
    float aoRadius{0.65f};
    float shadowSoftness{0.025f};
    float reflectionIntensity{1.0f};
    uint32_t pathBounces{3};
};

/**
 * @struct RtPassStats
 * @brief Telemetry metrics exposed to DebugOverlay and Studio UI.
 */
struct RtPassStats {
    bool active{false};
    bool bindlessActive{false};
    bool pathTracingActive{false};
    uint32_t pathSamples{0};
    float lastGpuMs{0.0f};
    RtSceneStats sceneStats{};
};

/**
 * @class RayTracingPass
 * @brief Orchestrates G-Buffer generation, compute ray tracing, temporal/À-Trous denoising, and HDR compositing.
 */
class RayTracingPass {
public:
    static constexpr uint32_t kUniformSlotStride = 512u; // Aligned >= sizeof(RtPassUniforms) and >= 256
    static constexpr uint32_t kMaxUniformSlots = 16u;

    RayTracingPass() = default;
    ~RayTracingPass();

    RayTracingPass(const RayTracingPass&) = delete;
    RayTracingPass& operator=(const RayTracingPass&) = delete;
    RayTracingPass(RayTracingPass&&) = delete;
    RayTracingPass& operator=(RayTracingPass&&) = delete;

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUPipelineLayout mainPipelineLayout,
                                  WGPUBindGroupLayout perDrawBgl, WGPUBindGroup dummySkinBindGroup,
                                  const RayTracingScene& rtScene, WGPUTextureView envCubemapView,
                                  WGPUSampler defaultSampler, WGPUTextureView fallbackWhiteView,
                                  bool supportsTimestampQueries);

    core::Result<void> ReloadShaders(std::span<const std::string> modifiedVirtualPaths);
    void Shutdown() noexcept;

    void SetConfig(const RtConfig& cfg) noexcept;
    [[nodiscard]] const RtConfig& GetConfig() const noexcept { return config_; }
    void ResetPathAccumulation() noexcept { pathSampleIndex_ = 0; }

    void SetEnvironmentCubemap(WGPUTextureView envCubemapView, uint32_t envMipCount) noexcept;

    /**
     * @brief Records the 1x opaque G-Buffer pass, RT Shadows, and RTAO compute + denoise passes before `pbr.wgsl`.
     */
    void ExecutePrePass(WGPUCommandEncoder encoder, const RayTracingScene& rtScene, scene::Scene& scene,
                        WGPUBindGroup sceneGlobalsBindGroup,
                        std::span<const std::pair<const scene::SceneNode*, size_t>> opaqueItems,
                        const glm::mat4& viewProj, const glm::mat4& lightViewProj, const glm::vec3& cameraPos,
                        const glm::vec3& sunDir, float solarElev, float envRotation, uint32_t baseDrawFlags,
                        uint64_t skinFrameId, float aoIntensity, uint32_t viewportW, uint32_t viewportH);

    /**
     * @brief Records RT Reflections + Temporal/À-Trous Denoiser + HDR additive composite after opaque + skybox.
     */
    void ExecutePostOpaque(WGPUCommandEncoder encoder, const RayTracingScene& rtScene,
                           WGPUTextureView hdrResolveView, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                           const glm::vec3& sunDir, float solarElev, float envRotation, uint32_t viewportW,
                           uint32_t viewportH);

    /**
     * @brief Records progressive compute path tracing and presents the accumulated HDR image onto `hdrResolveView`.
     */
    void ExecutePathTrace(WGPUCommandEncoder encoder, const RayTracingScene& rtScene, const scene::Scene& scene,
                          WGPUTextureView hdrResolveView, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                          const glm::vec3& sunDir, float solarElev, float envRotation, uint32_t viewportW,
                          uint32_t viewportH);

    /**
     * @brief Polls non-blocking GPU timestamp readback if available.
     */
    void PollGpuTimestamps() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] WGPUTextureView GetShadowView() const noexcept { return shadowFinalView_.Get(); }
    [[nodiscard]] WGPUTextureView GetAoView() const noexcept { return aoFinalView_.Get(); }
    [[nodiscard]] bool IsShadowActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && !config_.pathTracing && config_.shadows && rtScene.IsReady() &&
               shadowFinalView_ != nullptr;
    }
    [[nodiscard]] bool IsAoActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && !config_.pathTracing && config_.ambientOcclusion &&
               rtScene.IsReady() && aoFinalView_ != nullptr;
    }
    [[nodiscard]] bool IsReflectionsActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && !config_.pathTracing && config_.reflections && rtScene.IsReady() &&
               reflPipeline_ != nullptr;
    }
    [[nodiscard]] bool IsPathTracingActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && config_.pathTracing && rtScene.IsReady() &&
               pathTracePipeline_ != nullptr;
    }
    [[nodiscard]] RtPassStats GetStats(const RayTracingScene& rtScene) const noexcept;

private:
    core::Result<void> CreateLayoutsAndBuffers(WGPUPipelineLayout mainPipelineLayout, const RayTracingScene& rtScene);
    core::Result<void> CreatePipelines(bool validateScope, WGPUPipelineLayout mainPipelineLayout,
                                       const RayTracingScene& rtScene);
    void EnsureResolutionTargets(uint32_t fullW, uint32_t fullH);
    void RebuildPassBindGroups();
    void ReleaseResolutionTargets() noexcept;

    void PopulateUniformTemplate(RtPassUniforms& out, const scene::Scene* scene, const glm::mat4& viewProj,
                                 const glm::vec3& cameraPos, const glm::vec3& sunDir, float solarElev,
                                 float envRotation, uint32_t fullW, uint32_t fullH) const noexcept;
    void WriteUniformSlot(uint32_t slotIdx, const RtPassUniforms& u) noexcept;

    bool initialized_{false};
    bool bindlessActive_{false};
    bool supportsTimestampQueries_{false};
    RtConfig config_{};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUPipelineLayout mainPipelineLayout_{nullptr};
    WGPUBindGroup dummySkinBindGroup_{nullptr};
    WGPUTextureView envCubemapView_{nullptr};
    WGPUSampler defaultSampler_{nullptr};
    WGPUTextureView fallbackWhiteView_{nullptr};
    uint32_t envMipCount_{8};

    uint32_t fullWidth_{0};
    uint32_t fullHeight_{0};
    uint32_t traceWidth_{0};
    uint32_t traceHeight_{0};
    bool lastHalfRes_{true};

    uint32_t frameIndex_{0};
    uint32_t currHistIdx_{0};
    uint32_t pathSampleIndex_{0};
    glm::mat4 prevViewProj_{1.0f};
    glm::mat4 lastPathViewProj_{1.0f};
    glm::vec3 lastPathSunDir_{0.0f};
    float lastPathEnvRot_{0.0f};
    uint64_t lastPathTransformVer_{~uint64_t{0}};
    uint32_t lastPathBounces_{3};
    const scene::Scene* lastPathScene_{nullptr};

    // G-Buffer pass resources
    RenderPipelineHandle gbufferPipeline_{};
    RenderPipelineHandle gbufferBackCulledPipeline_{};
    DynamicUniformRing<PerDrawUniforms> gbufferPerDrawRing_{};
    DynamicInstanceBuffer gbufferInstanceBuffer_{};
    std::vector<PerDrawUniforms> gbufferPerDrawScratch_{};
    std::vector<glm::mat4> gbufferInstanceTransformsScratch_{};

    TextureHandle gbufferNormalRoughnessTex_{};
    TextureViewHandle gbufferNormalRoughnessView_{};
    TextureHandle gbufferAlbedoMetallicTex_{};
    TextureViewHandle gbufferAlbedoMetallicView_{};
    TextureHandle gbufferDepthTex_{};
    TextureViewHandle gbufferDepthView_{};

    // Shared uniform ring & sampler
    BufferHandle passUniformBuffer_{};
    BufferHandle compositeUniformBuffer_{};
    SamplerHandle linearSampler_{};

    // Compute & composite pipelines
    BindGroupLayoutHandle trace8Bgl_{};
    PipelineLayoutHandle trace8Pl_{};
    ComputePipelineHandle shadowPipeline_{};
    ComputePipelineHandle aoPipeline_{};

    BindGroupLayoutHandle reflBgl_{};
    PipelineLayoutHandle reflPl_{};
    ComputePipelineHandle reflPipeline_{};

    BindGroupLayoutHandle denoise8Bgl_{};
    PipelineLayoutHandle denoise8Pl_{};
    ComputePipelineHandle temporal8Pipeline_{};
    ComputePipelineHandle atrous8Pipeline_{};
    ComputePipelineHandle upsample8Pipeline_{};

    BindGroupLayoutHandle denoise16Bgl_{};
    PipelineLayoutHandle denoise16Pl_{};
    ComputePipelineHandle temporal16Pipeline_{};
    ComputePipelineHandle atrous16Pipeline_{};

    BindGroupLayoutHandle compositeBgl_{};
    PipelineLayoutHandle compositePl_{};
    RenderPipelineHandle compositeReflPipeline_{};
    RenderPipelineHandle pathTracePresentPipeline_{};

    BindGroupLayoutHandle pathTraceBgl_{};
    PipelineLayoutHandle pathTracePl_{};
    ComputePipelineHandle pathTracePipeline_{};

    // Intermediate & denoised RT textures
    TextureHandle shadowRawTex_{};
    TextureViewHandle shadowRawView_{};
    TextureHandle shadowScratchTex_{};
    TextureViewHandle shadowScratchView_{};
    std::array<TextureHandle, 2> shadowHistoryTex_{};
    std::array<TextureViewHandle, 2> shadowHistoryView_{};
    TextureHandle shadowFinalTex_{};
    TextureViewHandle shadowFinalView_{};

    TextureHandle aoRawTex_{};
    TextureViewHandle aoRawView_{};
    TextureHandle aoScratchTex_{};
    TextureViewHandle aoScratchView_{};
    std::array<TextureHandle, 2> aoHistoryTex_{};
    std::array<TextureViewHandle, 2> aoHistoryView_{};
    TextureHandle aoFinalTex_{};
    TextureViewHandle aoFinalView_{};

    TextureHandle reflRawTex_{};
    TextureViewHandle reflRawView_{};
    TextureHandle reflScratchTex_{};
    TextureViewHandle reflScratchView_{};
    std::array<TextureHandle, 2> reflHistoryTex_{};
    std::array<TextureViewHandle, 2> reflHistoryView_{};

    BufferHandle pathAccumBuffer_{};
    uint64_t pathAccumBytes_{0};

    // Cached BindGroups
    BindGroupHandle shadowTraceBg_{};
    BindGroupHandle aoTraceBg_{};
    BindGroupHandle reflTraceBg_{};
    BindGroupHandle pathTraceBg_{};
    BindGroupHandle compositeReflBg_{};
    BindGroupHandle pathPresentBg_{};

    std::array<BindGroupHandle, 2> shadowTemporalBg_{};
    std::array<BindGroupHandle, 2> shadowAtrousStep1Bg_{};
    BindGroupHandle shadowAtrousStep2Bg_{};
    BindGroupHandle shadowAtrousStep3Bg_{};
    BindGroupHandle shadowUpsampleBg_{};

    std::array<BindGroupHandle, 2> aoTemporalBg_{};
    std::array<BindGroupHandle, 2> aoAtrousStep1Bg_{};
    BindGroupHandle aoAtrousStep2Bg_{};
    BindGroupHandle aoAtrousStep3Bg_{};
    BindGroupHandle aoUpsampleBg_{};

    std::array<BindGroupHandle, 2> reflTemporalBg_{};
    std::array<BindGroupHandle, 2> reflAtrousStep1Bg_{};
    BindGroupHandle reflAtrousStep2Bg_{};
    BindGroupHandle reflAtrousStep3Bg_{};

    // Non-blocking GPU timestamp query telemetry
    QuerySetHandle timestampQuerySet_{};
    BufferHandle timestampResolveBuf_{};
    BufferHandle timestampReadbackBuf_{};
    bool timestampReadInFlight_{false};
    bool timestampDataWritten_{false};
    bool timestampCopyQueued_{false};
    bool timestampHasPrePass_{false};
    bool timestampHasPostPass_{false};
    float timestampPeriodNs_{1.0f};
    float lastGpuMs_{0.0f};

    std::array<uint8_t, kMaxUniformSlots * kUniformSlotStride> uniformStaging_{};
};

} // namespace bgl::gpu

#endif // BGL_GPU_RAYTRACINGPASS_HPP
