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

/**
 * @struct RtPassUniforms
 * @brief Dynamic-offset uniform block (`416` bytes) bound at `@group(0) @binding(0)` for RT compute & denoiser passes.
 */
struct alignas(16) RtPassUniforms {
    glm::mat4 invViewProj{1.0f};                          ///< Inverse view-projection matrix for primary ray reconstruction.
    glm::mat4 prevViewProj{1.0f};                         ///< Previous frame view-projection matrix for temporal reprojection.
    glm::mat4 viewProj{1.0f};                             ///< Current frame view-projection matrix.
    glm::vec4 cameraPos{0.0f};                            ///< `xyz` = camera world position, `w` = frame counter.
    glm::vec4 sunDir{0.35f, 1.0f, 0.25f, 0.025f};         ///< `xyz` = unit sun direction, `w` = shadow cone jitter angle.
    glm::vec4 params0{1280.0f, 720.0f, 640.0f, 360.0f};   ///< `{fullWidth, fullHeight, traceWidth, traceHeight}`.
    glm::vec4 params1{0.65f, 1.0f, 1.0f, 0.0f};           ///< `{aoRadius, aoIntensity, reflectionIntensity, envRotationRad}`.
    glm::vec4 params2{3.0f, 0.0f, 1.0f, 1.0f};            ///< `{maxBounces, pathSampleIndex, halfResFlag, hasPrimarySun}`.
    glm::vec4 params3{0.0f, 0.866f, 8.0f, 1.0f};          ///< `{punctualCount, solarElevation, envMipCount, atrousStride}`.
    std::array<glm::vec4, 4> lightsPos{};                 ///< Packed light world positions / types (`w`: 0=dir, 1=point, 2=spot).
    std::array<glm::vec4, 4> lightsCol{};                 ///< Packed light linear RGB * intensity (`xyz`) and range (`w`).
};
static_assert(sizeof(RtPassUniforms) == 416, "RtPassUniforms must be 416 bytes");

/**
 * @struct RtCompositeUniforms
 * @brief Dimensions uniform block (`16` bytes) passed to the RT reflection composite and path-trace present shaders.
 */
struct alignas(16) RtCompositeUniforms {
    glm::vec4 params{1280.0f, 720.0f, 640.0f, 360.0f}; ///< `{fullWidth, fullHeight, sourceWidth, sourceHeight}`.
};
static_assert(sizeof(RtCompositeUniforms) == 16, "RtCompositeUniforms must be 16 bytes");

/**
 * @struct RtConfig
 * @brief Runtime tuning knobs for Desktop Ray Tracing passes.
 */
struct RtConfig {
    bool enabled{true};             ///< Master desktop hybrid ray tracing toggle.
    bool shadows{true};             ///< Enables ray-traced soft directional and punctual light shadows.
    bool ambientOcclusion{true};    ///< Enables cosine-weighted ray-traced ambient occlusion (RTAO).
    bool reflections{true};         ///< Enables 1-bounce GGX importance-sampled specular reflections.
    bool pathTracing{false};        ///< Switches to the progressive multi-bounce compute path tracer.
    bool halfRes{true};             ///< Traces hybrid effects at half resolution with bilateral upsampling.
    float aoRadius{0.65f};          ///< World-space RTAO ray distance `[0.05, 5.0]`.
    float shadowSoftness{0.025f};   ///< Directional sun jitter cone half-angle `[0.0, 0.15]`.
    float reflectionIntensity{1.0f};///< Additive reflection brightness scale `[0.0, 2.0]`.
    uint32_t pathBounces{3};        ///< Maximum path tracer bounce depth `[1, 6]`.
};

/**
 * @struct RtPassStats
 * @brief Telemetry metrics exposed to `DebugOverlay` and Studio UI.
 */
struct RtPassStats {
    bool active{false};            ///< True when RT pass is initialized, enabled, and has valid BVH scene data.
    bool bindlessActive{false};    ///< True when bindless texture arrays (`@group(2)`) are bound in ray shaders.
    bool pathTracingActive{false}; ///< True when the progressive compute path tracer is driving output.
    uint32_t pathSamples{0};       ///< Accumulated path-tracer samples since last reset.
    float lastGpuMs{0.0f};         ///< Exponentially smoothed GPU ray tracing duration in milliseconds.
    RtSceneStats sceneStats{};     ///< Active 2-level BVH scene geometry and instance statistics.
};

/**
 * @class RayTracingPass
 * @brief Orchestrates G-Buffer generation, compute ray tracing, temporal/À-Trous denoising, and HDR compositing.
 */
class RayTracingPass {
public:
    static constexpr uint32_t kUniformSlotStride = 512u;   ///< Dynamic UBO stride aligned >= 256 bytes and `sizeof(RtPassUniforms)`.
    static constexpr uint32_t kMaxUniformSlots = 16u;      ///< Number of per-dispatch uniform slots in the ring buffer.
    static constexpr uint32_t kNoTimestampIndex = ~0u;     ///< Sentinel disabling begin/end timestamp writes on a compute pass.

    RayTracingPass() = default;
    ~RayTracingPass();

    RayTracingPass(const RayTracingPass&) = delete;
    RayTracingPass& operator=(const RayTracingPass&) = delete;
    RayTracingPass(RayTracingPass&&) = delete;
    RayTracingPass& operator=(RayTracingPass&&) = delete;

    /**
     * @brief Allocates shared RT uniform/timestamp buffers, layouts, and compute/render pipelines.
     * @param device Active WebGPU device.
     * @param queue Active WebGPU command queue.
     * @param mainPipelineLayout Shared 4-group pipeline layout reused by the 1x G-Buffer pass.
     * @param perDrawBgl Per-draw dynamic uniform bind group layout for G-Buffer draws.
     * @param dummySkinBindGroup Default empty skin SSBO bind group.
     * @param rtScene Initialized `RayTracingScene` providing BVH and bindless layouts.
     * @param envCubemapView Environment cubemap view sampled on reflection/path-trace ray misses.
     * @param defaultSampler Shared filtering sampler.
     * @param fallbackWhiteView Fallback white texture view.
     * @param supportsTimestampQueries Enables `WGPUQuerySet` GPU timing when supported by device.
     * @return Success or descriptive initialization failure message.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUPipelineLayout mainPipelineLayout,
                                  WGPUBindGroupLayout perDrawBgl, WGPUBindGroup dummySkinBindGroup,
                                  const RayTracingScene& rtScene, WGPUTextureView envCubemapView,
                                  WGPUSampler defaultSampler, WGPUTextureView fallbackWhiteView,
                                  bool supportsTimestampQueries);

    /**
     * @brief Recompiles affected RT compute/render pipelines when watched `.wgsl` files change on disk.
     * @param modifiedVirtualPaths Virtual shader paths reported by `ShaderManager`.
     * @return Success or shader compilation error while keeping prior valid pipelines intact.
     */
    core::Result<void> ReloadShaders(std::span<const std::string> modifiedVirtualPaths);

    /**
     * @brief Releases all pipelines, offscreen RT/denoiser textures, timestamp queries, and bind groups.
     */
    void Shutdown() noexcept;

    /**
     * @brief Applies new RT configuration and resets temporal/path accumulation counters when structural toggles change.
     * @param cfg Updated runtime RT tuning configuration.
     */
    void SetConfig(const RtConfig& cfg) noexcept;

    /** @brief Returns the active RT configuration. */
    [[nodiscard]] const RtConfig& GetConfig() const noexcept { return config_; }

    /** @brief Resets progressive path tracer sample counter (`pathSampleIndex_ = 0`). */
    void ResetPathAccumulation() noexcept { pathSampleIndex_ = 0; }

    /**
     * @brief Updates the environment cubemap texture view used on secondary ray misses and rebuilds bind groups.
     * @param envCubemapView New pre-filtered cubemap view.
     * @param envMipCount Mip level count of the environment cubemap.
     */
    void SetEnvironmentCubemap(WGPUTextureView envCubemapView, uint32_t envMipCount) noexcept;

    /**
     * @brief Records the 1x opaque G-Buffer pass, RT Shadows, and RTAO compute + denoise passes before `pbr.wgsl`.
     * @param encoder Active frame command encoder.
     * @param rtScene Built 2-level BVH scene.
     * @param scene CPU/GPU scene state containing materials, skins, lights, and meshes.
     * @param sceneGlobalsBindGroup Main `@group(0)` bind group shared with the 1x G-Buffer pass.
     * @param opaqueItems Visible opaque node/primitive pairs for G-Buffer rasterization.
     * @param viewProj Current camera view-projection matrix.
     * @param lightViewProj Directional shadow light view-projection matrix.
     * @param cameraPos Camera world coordinates.
     * @param sunDir Normalized direction toward the primary directional light.
     * @param solarElev Diurnal sun elevation scalar.
     * @param envRotation Environment azimuth rotation in radians.
     * @param baseDrawFlags Active frame draw flags.
     * @param skinFrameId Monotonic skinning upload generation counter.
     * @param aoIntensity RTAO intensity scalar forwarded to denoised/PBR shading.
     * @param viewportW Current viewport width in pixels.
     * @param viewportH Current viewport height in pixels.
     */
    void ExecutePrePass(WGPUCommandEncoder encoder, const RayTracingScene& rtScene, scene::Scene& scene,
                        WGPUBindGroup sceneGlobalsBindGroup,
                        std::span<const std::pair<const scene::SceneNode*, size_t>> opaqueItems,
                        const glm::mat4& viewProj, const glm::mat4& lightViewProj, const glm::vec3& cameraPos,
                        const glm::vec3& sunDir, float solarElev, float envRotation, uint32_t baseDrawFlags,
                        uint64_t skinFrameId, float aoIntensity, uint32_t viewportW, uint32_t viewportH);

    /**
     * @brief Records RT Reflections + Temporal/À-Trous Denoiser + HDR additive composite after opaque + skybox.
     * @param encoder Active frame command encoder.
     * @param rtScene Built 2-level BVH scene.
     * @param hdrResolveView Single-sampled `RGBA16Float` resolve target receiving additive reflections.
     * @param viewProj Current camera view-projection matrix.
     * @param cameraPos Camera world coordinates.
     * @param sunDir Normalized direction toward the primary directional light.
     * @param solarElev Diurnal sun elevation scalar.
     * @param envRotation Environment azimuth rotation in radians.
     * @param viewportW Current viewport width in pixels.
     * @param viewportH Current viewport height in pixels.
     */
    void ExecutePostOpaque(WGPUCommandEncoder encoder, const RayTracingScene& rtScene, WGPUTextureView hdrResolveView,
                           const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec3& sunDir,
                           float solarElev, float envRotation, uint32_t viewportW, uint32_t viewportH);

    /**
     * @brief Records progressive compute path tracing and presents the accumulated HDR image onto `hdrResolveView`.
     * @param encoder Active frame command encoder.
     * @param rtScene Built 2-level BVH scene.
     * @param scene Scene light/transform metadata evaluated for path accumulation resets.
     * @param hdrResolveView Single-sampled `RGBA16Float` resolve target receiving the presented HDR image.
     * @param viewProj Current camera view-projection matrix.
     * @param cameraPos Camera world coordinates.
     * @param sunDir Normalized direction toward the primary directional light.
     * @param solarElev Diurnal sun elevation scalar.
     * @param envRotation Environment azimuth rotation in radians.
     * @param viewportW Current viewport width in pixels.
     * @param viewportH Current viewport height in pixels.
     */
    void ExecutePathTrace(WGPUCommandEncoder encoder, const RayTracingScene& rtScene, const scene::Scene& scene,
                          WGPUTextureView hdrResolveView, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                          const glm::vec3& sunDir, float solarElev, float envRotation, uint32_t viewportW,
                          uint32_t viewportH);

    /**
     * @brief Polls non-blocking GPU timestamp readback if available.
     */
    void PollGpuTimestamps() noexcept;

    /** @brief Returns true if GPU pipelines and uniform buffers are initialized. */
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    /** @brief Returns the denoised full-resolution RT shadow factor texture view. */
    [[nodiscard]] WGPUTextureView GetShadowView() const noexcept { return shadowFinalView_.Get(); }
    /** @brief Returns the denoised full-resolution RTAO factor texture view. */
    [[nodiscard]] WGPUTextureView GetAoView() const noexcept { return aoFinalView_.Get(); }
    /** @brief Returns true if RT shadows are active for `rtScene` in the current mode. */
    [[nodiscard]] bool IsShadowActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && !config_.pathTracing && config_.shadows && rtScene.IsReady() &&
               shadowFinalView_ != nullptr;
    }
    /** @brief Returns true if RTAO is active for `rtScene` in the current mode. */
    [[nodiscard]] bool IsAoActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && !config_.pathTracing && config_.ambientOcclusion &&
               rtScene.IsReady() && aoFinalView_ != nullptr;
    }
    /** @brief Returns true if 1-bounce RT reflections are active for `rtScene` in the current mode. */
    [[nodiscard]] bool IsReflectionsActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && !config_.pathTracing && config_.reflections && rtScene.IsReady() &&
               reflPipeline_ != nullptr;
    }
    /** @brief Returns true if the progressive compute path tracer is active for `rtScene`. */
    [[nodiscard]] bool IsPathTracingActive(const RayTracingScene& rtScene) const noexcept {
        return initialized_ && config_.enabled && config_.pathTracing && rtScene.IsReady() &&
               pathTracePipeline_ != nullptr;
    }
    /** @brief Returns combined RT configuration status, path-sample count, GPU timing, and BVH scene metrics. */
    [[nodiscard]] RtPassStats GetStats(const RayTracingScene& rtScene) const noexcept;

private:
    struct DenoisePassLabels {
        const char* trace{nullptr};
        const char* temporal{nullptr};
        const char* atrous1{nullptr};
        const char* atrous2{nullptr};
        const char* atrous3{nullptr};
        const char* upsample{nullptr};
    };

    struct GbufDraw {
        scene::MeshPrimitive* prim{nullptr};
        WGPUBindGroup skinBg{nullptr};
        WGPURenderPipeline pipe{nullptr};
        uint32_t dynOffset{0};
        uint32_t instanceStart{0};
        uint32_t instanceCount{1};
    };
    struct DenoiseBindGroupSet {
        std::array<BindGroupHandle, 2> temporal{};
        std::array<BindGroupHandle, 2> atrousStep1{};
        BindGroupHandle atrousStep2{};
        BindGroupHandle atrousStep3{};
        BindGroupHandle upsample{};

        void Reset() noexcept {
            for (auto& bg : temporal) {
                bg.Reset();
            }
            for (auto& bg : atrousStep1) {
                bg.Reset();
            }
            atrousStep2.Reset();
            atrousStep3.Reset();
            upsample.Reset();
        }
    };

    core::Result<void> CreateLayoutsAndBuffers(WGPUPipelineLayout mainPipelineLayout, const RayTracingScene& rtScene);
    core::Result<void> CreatePipelines(bool validateScope, bool allowBindlessFallback);
    void EnsureResolutionTargets(uint32_t fullW, uint32_t fullH);
    void RebuildPassBindGroups();
    void ReleaseResolutionTargets() noexcept;

    void PopulateUniformTemplate(RtPassUniforms& out, const scene::Scene* scene, const glm::mat4& viewProj,
                                 const glm::vec3& cameraPos, const glm::vec3& sunDir, float solarElev,
                                 float envRotation, uint32_t fullW, uint32_t fullH) const noexcept;
    void WriteUniformSlot(uint32_t slotIdx, const RtPassUniforms& u) noexcept;
    void DispatchComputePass(WGPUCommandEncoder encoder, WGPUComputePipeline pipe, WGPUBindGroup bg0, WGPUBindGroup bg1,
                             WGPUBindGroup bg2, uint32_t slotIdx, uint32_t wgX, uint32_t wgY, const char* label,
                             uint32_t startTsIdx = 0xFFFFFFFFu, uint32_t endTsIdx = 0xFFFFFFFFu) noexcept;
    void ExecuteDenoise8Chain(WGPUCommandEncoder encoder, WGPUComputePipeline tracePipe, WGPUBindGroup traceBg,
                              WGPUBindGroup rtSceneBg, const DenoiseBindGroupSet& denoiseSet, uint32_t baseSlot,
                              uint32_t traceWgX, uint32_t traceWgY, uint32_t fullWgX, uint32_t fullWgY,
                              const char* labelPrefix, bool writeStartTs, bool writeEndTs) noexcept;
    void ResolveTimestampQueries(WGPUCommandEncoder encoder) noexcept;

    bool initialized_{false};
    bool bindlessActive_{false};
    bool supportsTimestampQueries_{false};
    RtConfig config_{};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUPipelineLayout mainPipelineLayout_{nullptr};
    WGPUBindGroupLayout sceneBgl_{nullptr};
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

    DenoiseBindGroupSet shadowBgs_{};
    DenoiseBindGroupSet aoBgs_{};
    DenoiseBindGroupSet reflBgs_{};

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
