/**
 * @file PostProcessPass.hpp
 * @brief Fullscreen HDR Tonemapping, Depth Linearization, Half-Res GTAO, 5-Mip Bloom, GPU Auto-Exposure,
 *        Bokeh DoF, Screen-Space Reflections, Volumetric God Rays, Color Grading Presets, and FXAA.
 */
#ifndef BGL_GPU_POSTPROCESSPASS_HPP
#define BGL_GPU_POSTPROCESSPASS_HPP

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

enum class ToneMapOperator : uint32_t { KhronosPbrNeutral = 0, AgX = 1, ACES = 2 };

enum class ColorGradingPreset : uint32_t { Neutral = 0, WarmCinema = 1, CoolSciFi = 2, BleachBypass = 3, Noir = 4 };

struct alignas(16) TonemapUniforms {
    float exposure{1.0f};
    uint32_t operatorId{0};
    uint32_t isSrgbSurface{0};
    uint32_t lensFlareEnabled{1};
    glm::vec4 sunScreenPos{0.5f, 0.5f, 0.0f, 1.0f}; // xy = sun UV, z = visibility, w = aspect ratio
    glm::vec4 flareParams{0.75f, 0.38f, 1.0f,
                          0.0f}; // x = intensity, y = haloRadius, z = chromaticDispersal, w = unused
    glm::vec4 bloomParams{0.65f, 1.05f, 0.85f, 1.0f}; // x = intensity, y = threshold, z = scatter, w = enabled
    glm::vec4 ssaoParams{0.75f, 0.02f, 0.005f, 1.0f}; // x = intensity, y = radius, z = bias, w = enabled
};

static_assert(sizeof(TonemapUniforms) == 80, "TonemapUniforms must be 80 bytes");

struct alignas(16) PostProcessExtraUniforms {
    glm::mat4 proj{1.0f};
    glm::mat4 invProj{1.0f};
    glm::vec4 dofParams{0.0f, 6.0f, 4.5f, 6.0f};         // x: enabled, y: focusDist, z: focusRange, w: maxBlurPx
    glm::vec4 ssrParams{1.0f, 0.45f, 14.0f, 0.35f};      // x: enabled, y: intensity, z: maxDist, w: thickness
    glm::vec4 godRayParams{1.0f, 0.55f, 0.94f, 0.85f};   // x: enabled, y: intensity, z: decay, w: density
    glm::vec4 gradingParams{0.0f, 0.25f, 0.22f, 0.20f};  // x: presetId, y: vignette, z: chromaAberr, w: filmGrain
    glm::vec4 screenParams{1280.0f, 720.0f, 0.0f, 0.0f}; // x: width, y: height, z: timeSec, w: autoExposureEnabled
};

static_assert(sizeof(PostProcessExtraUniforms) == 208, "PostProcessExtraUniforms must be 208 bytes");

class PostProcessPass {
public:
    PostProcessPass() = default;
    ~PostProcessPass();

    PostProcessPass(const PostProcessPass&) = delete;
    PostProcessPass& operator=(const PostProcessPass&) = delete;
    PostProcessPass(PostProcessPass&& other) noexcept;
    PostProcessPass& operator=(PostProcessPass&& other) noexcept;

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat);
    core::Result<void> ReloadShader();
    void Shutdown() noexcept;

    /**
     * @brief Executes pre-swapchain GPU passes (Depth Linearize, GTAO + blur, 5-Mip Bloom,
     *        GPU Auto-Exposure, and optional Tonemap->LDR prepass when FXAA is active).
     */
    void Prepare(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView, WGPUTextureView depthReadView,
                 uint32_t sampleCount, const glm::mat4& viewProj, const glm::mat4& projMatrix,
                 const glm::vec3& cameraPos, const glm::vec3& lightDir, float envRotation, uint32_t viewportW,
                 uint32_t viewportH, float deltaTime, float elapsedTime);

    /**
     * @brief Records final swapchain pass (FXAA edge antialiasing when active, else fullscreen Tonemap).
     */
    void Render(WGPURenderPassEncoder swapchainPass, WGPUTextureView hdrResolveView, const glm::mat4& viewProj,
                const glm::vec3& cameraPos, const glm::vec3& lightDir, float envRotation, uint32_t viewportW,
                uint32_t viewportH);

    /**
     * @brief Returns blurred Bloom Mip-2 view suitable for frosted UI glassmorphism panels (nullable).
     */
    [[nodiscard]] WGPUTextureView GetUiBackdropView() const noexcept;

    void SetExposure(float exposure) noexcept { exposure_ = std::clamp(exposure, 0.05f, 20.0f); }
    [[nodiscard]] float GetExposure() const noexcept { return exposure_; }

    void SetAutoExposureEnabled(bool enable) noexcept { enableAutoExposure_ = enable; }
    [[nodiscard]] bool IsAutoExposureEnabled() const noexcept { return enableAutoExposure_; }

    void SetToneMapOperator(ToneMapOperator op) noexcept { toneMapOp_ = op; }
    [[nodiscard]] ToneMapOperator GetToneMapOperator() const noexcept { return toneMapOp_; }
    void CycleToneMapOperator() noexcept;

    void SetColorGradingPreset(ColorGradingPreset preset) noexcept { colorGradingPreset_ = preset; }
    [[nodiscard]] ColorGradingPreset GetColorGradingPreset() const noexcept { return colorGradingPreset_; }
    void CycleColorGradingPreset() noexcept;
    [[nodiscard]] static const char* GetColorGradingPresetName(ColorGradingPreset preset) noexcept;

    void SetLensFlareEnabled(bool enable) noexcept { enableLensFlare_ = enable; }
    [[nodiscard]] bool IsLensFlareEnabled() const noexcept { return enableLensFlare_; }
    void ToggleLensFlare() noexcept { enableLensFlare_ = !enableLensFlare_; }
    void SetLensFlareIntensity(float intensity) noexcept { lensFlareIntensity_ = std::clamp(intensity, 0.0f, 5.0f); }
    [[nodiscard]] float GetLensFlareIntensity() const noexcept { return lensFlareIntensity_; }

    void SetGodRaysEnabled(bool enable) noexcept { enableGodRays_ = enable; }
    [[nodiscard]] bool IsGodRaysEnabled() const noexcept { return enableGodRays_; }
    void SetGodRaysIntensity(float intensity) noexcept { godRaysIntensity_ = std::clamp(intensity, 0.0f, 3.0f); }
    [[nodiscard]] float GetGodRaysIntensity() const noexcept { return godRaysIntensity_; }

    void SetSsaoEnabled(bool enable) noexcept { enableSsao_ = enable; }
    [[nodiscard]] bool IsSsaoEnabled() const noexcept { return enableSsao_; }
    void ToggleSSAO() noexcept { enableSsao_ = !enableSsao_; }
    void SetSsaoIntensity(float intensity) noexcept { ssaoIntensity_ = std::clamp(intensity, 0.0f, 2.0f); }
    [[nodiscard]] float GetSsaoIntensity() const noexcept { return ssaoIntensity_; }
    void SetSsaoRadius(float radius) noexcept { ssaoRadius_ = std::clamp(radius, 0.001f, 0.1f); }
    [[nodiscard]] float GetSsaoRadius() const noexcept { return ssaoRadius_; }

    void SetBloomEnabled(bool enable) noexcept { enableBloom_ = enable; }
    [[nodiscard]] bool IsBloomEnabled() const noexcept { return enableBloom_; }
    void ToggleBloom() noexcept { enableBloom_ = !enableBloom_; }
    void SetBloomIntensity(float intensity) noexcept { bloomIntensity_ = std::clamp(intensity, 0.0f, 4.0f); }
    [[nodiscard]] float GetBloomIntensity() const noexcept { return bloomIntensity_; }
    void SetBloomThreshold(float threshold) noexcept { bloomThreshold_ = std::clamp(threshold, 0.1f, 8.0f); }
    [[nodiscard]] float GetBloomThreshold() const noexcept { return bloomThreshold_; }

    void SetDoFEnabled(bool enable) noexcept { enableDof_ = enable; }
    [[nodiscard]] bool IsDoFEnabled() const noexcept { return enableDof_; }
    void SetDoFFocusDistance(float dist) noexcept { dofFocusDistance_ = std::clamp(dist, 0.25f, 200.0f); }
    [[nodiscard]] float GetDoFFocusDistance() const noexcept { return dofFocusDistance_; }
    void SetDoFFocusRange(float range) noexcept { dofFocusRange_ = std::clamp(range, 0.5f, 50.0f); }
    [[nodiscard]] float GetDoFFocusRange() const noexcept { return dofFocusRange_; }
    void SetDoFMaxBlur(float maxPx) noexcept { dofMaxBlurPx_ = std::clamp(maxPx, 1.0f, 12.0f); }
    [[nodiscard]] float GetDoFMaxBlur() const noexcept { return dofMaxBlurPx_; }

    void SetSsrEnabled(bool enable) noexcept { enableSsr_ = enable; }
    [[nodiscard]] bool IsSsrEnabled() const noexcept { return enableSsr_; }
    void SetSsrIntensity(float intensity) noexcept { ssrIntensity_ = std::clamp(intensity, 0.0f, 2.0f); }
    [[nodiscard]] float GetSsrIntensity() const noexcept { return ssrIntensity_; }

    void SetFxaaEnabled(bool enable) noexcept { enableFxaa_ = enable; }
    [[nodiscard]] bool IsFxaaEnabled() const noexcept { return enableFxaa_; }

    void SetVignetteIntensity(float v) noexcept { vignetteIntensity_ = std::clamp(v, 0.0f, 1.0f); }
    [[nodiscard]] float GetVignetteIntensity() const noexcept { return vignetteIntensity_; }

    void SetChromaticAberration(float c) noexcept { chromaAberration_ = std::clamp(c, 0.0f, 2.0f); }
    [[nodiscard]] float GetChromaticAberration() const noexcept { return chromaAberration_; }

    void SetFilmGrainIntensity(float g) noexcept { filmGrainIntensity_ = std::clamp(g, 0.0f, 2.0f); }
    [[nodiscard]] float GetFilmGrainIntensity() const noexcept { return filmGrainIntensity_; }

    /**
     * @brief Deterministic CPU helper computing Circle of Confusion [0..1] matching tonemap.wgsl DoF curve.
     */
    [[nodiscard]] static float ComputeDoFCoc(float linearDepth, float focusDist, float focusRange) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return tonemapPipeline_ != nullptr; }

    /**
     * @brief Invalidates cached HDR/depth bind groups (e.g., after swapchain/HDR resize).
     */
    void InvalidateBindGroup() noexcept;

private:
    static constexpr uint32_t kMaxBloomMips = 5u;

    core::Result<void> CreatePipelines(bool validateScope);
    core::Result<void> CreateFallbackResources();
    void EnsureResolutionTargets(uint32_t width, uint32_t height);
    void ReleaseResolutionTargets() noexcept;
    void EnsureTonemapBindGroup(WGPUTextureView hdrResolveView, WGPUTextureView depthView, WGPUTextureView bloomView,
                                WGPUTextureView gtaoView);
    void UploadTonemapUniforms(const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec3& lightDir,
                               float envRotation, uint32_t viewportW, uint32_t viewportH);
    void PrepareDepthAndGtaoPasses(WGPUCommandEncoder encoder, WGPUTextureView depthReadView, uint32_t sampleCount);
    void PrepareBloomAndBackdropPasses(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView);
    void PrepareAutoExposurePass(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView, float deltaTime);
    void PrepareFxaaTonemapPass(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView,
                                WGPUTextureView depthReadView, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                const glm::vec3& lightDir, float envRotation, uint32_t viewportW, uint32_t viewportH);
    void MoveFrom(PostProcessPass& other) noexcept;

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};

    // Main Tonemap (swapchain target & optional LDR RGBA8Unorm intermediate for FXAA)
    BindGroupLayoutHandle tonemapBindGroupLayout_{};
    PipelineLayoutHandle tonemapPipelineLayout_{};
    RenderPipelineHandle tonemapPipeline_{};
    RenderPipelineHandle tonemapLdrPipeline_{};
    BufferHandle tonemapUniformBuffer_{};
    BufferHandle extraUniformBuffer_{};
    SamplerHandle linearSampler_{};
    BindGroupHandle tonemapBindGroup_{};
    WGPUTextureView cachedHdrResolveView_{nullptr};
    WGPUTextureView cachedLinearDepthView_{nullptr};
    WGPUTextureView cachedBloomView_{nullptr};
    WGPUTextureView cachedGtaoView_{nullptr};
    WGPUTextureView cachedBloomHdrView_{nullptr};

    // Fallbacks (1x1 black/white/far-depth + initial auto-exposure storage)
    TextureHandle fallbackBlackTex_{};
    TextureViewHandle fallbackBlackView_{};
    TextureHandle fallbackWhiteTex_{};
    TextureViewHandle fallbackWhiteView_{};
    TextureHandle fallbackDepthTex_{};
    TextureViewHandle fallbackDepthView_{};
    BufferHandle exposureStateBuffer_{};

    // Depth Linearize (MSAA & 1x)
    BindGroupLayoutHandle depthLinMsaaBgl_{};
    PipelineLayoutHandle depthLinMsaaPl_{};
    RenderPipelineHandle depthLinMsaaPipeline_{};
    BindGroupLayoutHandle depthLin1xBgl_{};
    PipelineLayoutHandle depthLin1xPl_{};
    RenderPipelineHandle depthLin1xPipeline_{};
    BufferHandle depthLinUniformBuffer_{};
    BindGroupHandle depthLinBindGroup_{};
    WGPUTextureView cachedDepthReadView_{nullptr};
    uint32_t cachedDepthSampleCount_{0};

    // GTAO + Edge-Preserving Bilateral Blur
    BindGroupLayoutHandle gtaoBgl_{};
    PipelineLayoutHandle gtaoPl_{};
    RenderPipelineHandle gtaoPipeline_{};
    BindGroupLayoutHandle gtaoBlurBgl_{};
    PipelineLayoutHandle gtaoBlurPl_{};
    RenderPipelineHandle gtaoBlurPipeline_{};
    BufferHandle gtaoUniformBuffer_{};
    BindGroupHandle gtaoBindGroup_{};
    BindGroupHandle gtaoBlurBindGroup_{};

    // 5-Mip Jimenez Bloom Pyramid
    BindGroupLayoutHandle bloomBgl_{};
    PipelineLayoutHandle bloomPl_{};
    RenderPipelineHandle bloomDownPipeline_{};
    RenderPipelineHandle bloomUpPipeline_{};
    BufferHandle bloomUniformBuffer_{}; // 9 * 256B dynamic-offset slots
    std::array<BindGroupHandle, kMaxBloomMips * 2u> bloomBindGroups_{};

    // GPU Auto-Exposure Compute
    BindGroupLayoutHandle autoExpBgl_{};
    PipelineLayoutHandle autoExpPl_{};
    ComputePipelineHandle autoExpPipeline_{};
    BufferHandle autoExpUniformBuffer_{};
    BindGroupHandle autoExpBindGroup_{};
    WGPUTextureView cachedAutoExpHdrView_{nullptr};

    // Post-Tonemap FXAA 3.11
    BindGroupLayoutHandle fxaaBgl_{};
    PipelineLayoutHandle fxaaPl_{};
    RenderPipelineHandle fxaaPipeline_{};
    BufferHandle fxaaUniformBuffer_{};
    BindGroupHandle fxaaBindGroup_{};

    // Resolution-dependent offscreen textures
    uint32_t targetWidth_{0};
    uint32_t targetHeight_{0};
    uint32_t bloomMipCount_{0};
    TextureHandle linearDepthTex_{};
    TextureViewHandle linearDepthView_{};
    TextureHandle gtaoRawTex_{};
    TextureViewHandle gtaoRawView_{};
    TextureHandle gtaoBlurTex_{};
    TextureViewHandle gtaoBlurView_{};
    TextureHandle bloomTex_{};
    std::array<TextureViewHandle, kMaxBloomMips> bloomMipViews_{};
    TextureHandle ldrSceneTex_{};
    TextureViewHandle ldrSceneView_{};

    // Interactive rendering parameters & cached frame state
    glm::mat4 projMatrix_{1.0f};
    glm::mat4 invProjMatrix_{1.0f};
    float elapsedTime_{0.0f};
    bool preparedLdrThisFrame_{false};

    float exposure_{1.0f};
    bool enableAutoExposure_{false};
    ToneMapOperator toneMapOp_{ToneMapOperator::KhronosPbrNeutral};
    ColorGradingPreset colorGradingPreset_{ColorGradingPreset::Neutral};
    bool enableLensFlare_{true};
    float lensFlareIntensity_{0.75f};
    bool enableGodRays_{true};
    float godRaysIntensity_{0.55f};
    bool enableSsao_{true};
    float ssaoIntensity_{0.75f};
    float ssaoRadius_{0.02f};
    bool enableBloom_{true};
    float bloomIntensity_{0.65f};
    float bloomThreshold_{1.05f};
    bool enableDof_{false};
    float dofFocusDistance_{6.0f};
    float dofFocusRange_{4.5f};
    float dofMaxBlurPx_{6.0f};
    bool enableSsr_{true};
    float ssrIntensity_{0.45f};
    bool enableFxaa_{true};
    float vignetteIntensity_{0.25f};
    float chromaAberration_{0.22f};
    float filmGrainIntensity_{0.20f};
};

} // namespace bgl::gpu

#endif // BGL_GPU_POSTPROCESSPASS_HPP
