/**
 * @file Skybox.hpp
 * @brief HDR Environment Cubemap & Procedural Atmospheric Skybox with WebGPU 4x MSAA RGBA16Float pipeline.
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include <glm/glm.hpp>
#include <string_view>
#include <webgpu/webgpu.h>

namespace bgl::gpu {

enum class SkyboxMode : uint32_t { Sharp = 0, Blurred = 1, Hidden = 2 };

struct alignas(16) SkyboxUniforms {
    glm::mat4 invViewProj{1.0f};
    glm::vec4 sunDir{0.35f, 1.0f, 0.25f, 0.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 params{0.0f, 0.0f, 1.0f,
                     0.0f}; // x = envRotation (rad), y = blurLod, z = useEnvCubemap (1 or 0), w = mode
};

/**
 * @class Skybox
 * @brief HDR environment cubemap & atmospheric skybox rendering into RGBA16Float 4x MSAA target.
 */
class Skybox {
public:
    Skybox() = default;
    ~Skybox();

    /**
     * @brief Initializes the WebGPU skybox render pipeline and fallback cubemap.
     * @param device WebGPU device handle.
     * @param queue WebGPU default queue handle.
     * @param hdrFormat HDR render target texture format (default RGBA16Float).
     * @param sampleCount Multisample count (default 4 for 4x MSAA).
     * @return Result indicating success or error string.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue,
                                  WGPUTextureFormat hdrFormat = WGPUTextureFormat_RGBA16Float,
                                  uint32_t sampleCount = 4);

    /**
     * @brief Releases WebGPU pipelines, bind groups, and fallback cubemap textures.
     */
    void Shutdown() noexcept;

    /**
     * @brief Assigns an active HDR environment cubemap texture view and sampler for background rendering.
     * @param cubemapView Sampled cubemap texture view.
     * @param sampler Filtering sampler handle.
     */
    void SetEnvironmentCubemap(WGPUTextureView cubemapView, WGPUSampler sampler);

    /**
     * @brief Executes the skybox render pass, writing procedural sky or cubemap pixels at the far plane.
     * @param pass Active WebGPU render pass encoder.
     * @param viewProj Camera view-projection matrix.
     * @param cameraPos Camera world-space eye position.
     * @param sunDir Direction vector pointing toward the sun.
     */
    void Render(WGPURenderPassEncoder pass, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                const glm::vec3& sunDir);

    void SetEnabled(bool enabled) noexcept {
        enabled_ = enabled;
        if (!enabled_) {
            mode_ = SkyboxMode::Hidden;
        } else if (mode_ == SkyboxMode::Hidden) {
            mode_ = SkyboxMode::Sharp;
        }
    }
    void Toggle() noexcept { CycleMode(); }
    void SetMode(SkyboxMode mode) noexcept {
        mode_ = mode;
        enabled_ = (mode_ != SkyboxMode::Hidden);
    }
    void CycleMode() noexcept {
        switch (mode_) {
        case SkyboxMode::Sharp:
            SetMode(SkyboxMode::Blurred);
            break;
        case SkyboxMode::Blurred:
            SetMode(SkyboxMode::Hidden);
            break;
        case SkyboxMode::Hidden:
            SetMode(SkyboxMode::Sharp);
            break;
        }
    }
    [[nodiscard]] SkyboxMode GetMode() const noexcept { return mode_; }
    [[nodiscard]] static constexpr std::string_view GetModeName(SkyboxMode mode) noexcept {
        switch (mode) {
        case SkyboxMode::Sharp:
            return "Sharp";
        case SkyboxMode::Blurred:
            return "Blurred";
        case SkyboxMode::Hidden:
            return "Hidden";
        }
        return "Sharp";
    }

    void SetRotation(float radians) noexcept { rotation_ = radians; }
    [[nodiscard]] float GetRotation() const noexcept { return rotation_; }

    void SetBlurRoughness(float roughness) noexcept { blurRoughness_ = roughness; }
    [[nodiscard]] float GetBlurRoughness() const noexcept { return blurRoughness_; }

    [[nodiscard]] bool IsEnabled() const noexcept { return enabled_ && mode_ != SkyboxMode::Hidden; }
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    void RebuildBindGroup(WGPUTextureView cubemapView, WGPUSampler sampler);

    bool enabled_{true};
    bool initialized_{false};
    SkyboxMode mode_{SkyboxMode::Sharp};
    float rotation_{0.0f};
    float blurRoughness_{0.55f};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    RenderPipelineHandle pipeline_{};
    BindGroupLayoutHandle bindGroupLayout_{};
    BindGroupHandle bindGroup_{};
    BufferHandle uniformBuffer_{};
    TextureHandle fallbackCubeTexture_{};
    TextureViewHandle fallbackCubeView_{};
    SamplerHandle fallbackSampler_{};
    bool hasCustomCubemap_{false};
};

} // namespace bgl::gpu
