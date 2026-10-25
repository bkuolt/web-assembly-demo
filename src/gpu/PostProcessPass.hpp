/**
 * @file PostProcessPass.hpp
 * @brief Fullscreen HDR Tonemapping, Screen-Space Ambient Occlusion (SSAO), Emissive Bloom, and Sun Lens Flares.
 */
#pragma once

#include "core/Types.hpp"
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

enum class ToneMapOperator : uint32_t { KhronosPbrNeutral = 0, AgX = 1, ACES = 2 };

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

class PostProcessPass {
public:
    PostProcessPass() = default;
    ~PostProcessPass();

    PostProcessPass(const PostProcessPass&) = delete;
    PostProcessPass& operator=(const PostProcessPass&) = delete;
    PostProcessPass(PostProcessPass&& other) noexcept;
    PostProcessPass& operator=(PostProcessPass&& other) noexcept;

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat);
    void Shutdown() noexcept;

    /**
     * @brief Records fullscreen HDR tonemapping, SSAO, Bloom, and Lens Flare post-processing.
     */
    void Render(WGPURenderPassEncoder swapchainPass, WGPUTextureView hdrResolveView, const glm::mat4& viewProj,
                const glm::vec3& cameraPos, const glm::vec3& lightDir, float envRotation, uint32_t viewportW,
                uint32_t viewportH);

    void SetExposure(float exposure) noexcept { exposure_ = std::clamp(exposure, 0.05f, 20.0f); }
    [[nodiscard]] float GetExposure() const noexcept { return exposure_; }

    void SetToneMapOperator(ToneMapOperator op) noexcept { toneMapOp_ = op; }
    [[nodiscard]] ToneMapOperator GetToneMapOperator() const noexcept { return toneMapOp_; }
    void CycleToneMapOperator() noexcept;

    void SetLensFlareEnabled(bool enable) noexcept { enableLensFlare_ = enable; }
    [[nodiscard]] bool IsLensFlareEnabled() const noexcept { return enableLensFlare_; }
    void ToggleLensFlare() noexcept { enableLensFlare_ = !enableLensFlare_; }
    void SetLensFlareIntensity(float intensity) noexcept { lensFlareIntensity_ = std::clamp(intensity, 0.0f, 5.0f); }
    [[nodiscard]] float GetLensFlareIntensity() const noexcept { return lensFlareIntensity_; }

    void SetSsaoEnabled(bool enable) noexcept { enableSsao_ = enable; }
    [[nodiscard]] bool IsSsaoEnabled() const noexcept { return enableSsao_; }
    void ToggleSSAO() noexcept { enableSsao_ = !enableSsao_; }
    void SetSsaoIntensity(float intensity) noexcept { ssaoIntensity_ = std::clamp(intensity, 0.0f, 3.0f); }
    [[nodiscard]] float GetSsaoIntensity() const noexcept { return ssaoIntensity_; }

    void SetSsaoRadius(float radius) noexcept { ssaoRadius_ = std::clamp(radius, 0.001f, 0.1f); }
    [[nodiscard]] float GetSsaoRadius() const noexcept { return ssaoRadius_; }

    void SetBloomEnabled(bool enable) noexcept { enableBloom_ = enable; }
    [[nodiscard]] bool IsBloomEnabled() const noexcept { return enableBloom_; }
    void ToggleBloom() noexcept { enableBloom_ = !enableBloom_; }
    void SetBloomIntensity(float intensity) noexcept { bloomIntensity_ = std::clamp(intensity, 0.0f, 5.0f); }
    [[nodiscard]] float GetBloomIntensity() const noexcept { return bloomIntensity_; }
    void SetBloomThreshold(float threshold) noexcept { bloomThreshold_ = std::max(0.0f, threshold); }
    [[nodiscard]] float GetBloomThreshold() const noexcept { return bloomThreshold_; }

    [[nodiscard]] bool IsInitialized() const noexcept { return tonemapPipeline_ != nullptr; }

private:
    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};

    WGPURenderPipeline tonemapPipeline_{nullptr};
    WGPUBindGroupLayout tonemapBindGroupLayout_{nullptr};
    WGPUBuffer tonemapUniformBuffer_{nullptr};
    WGPUBuffer tonemapVertexBuffer_{nullptr};
    WGPUSampler tonemapSampler_{nullptr};
    WGPUBindGroup tonemapBindGroup_{nullptr};
    WGPUTextureView cachedHdrResolveView_{nullptr};

    float exposure_{1.0f};
    ToneMapOperator toneMapOp_{ToneMapOperator::KhronosPbrNeutral};
    bool enableLensFlare_{true};
    float lensFlareIntensity_{0.75f};
    bool enableSsao_{true};
    float ssaoIntensity_{0.75f};
    float ssaoRadius_{0.02f};
    bool enableBloom_{true};
    float bloomIntensity_{0.65f};
    float bloomThreshold_{1.05f};
};

} // namespace bgl::gpu
