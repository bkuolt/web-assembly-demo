/**
 * @file Skybox.hpp
 * @brief Procedural Atmospheric Skybox with WebGPU 4x MSAA render pipeline.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <glm/glm.hpp>
#include <string_view>
#include "core/Types.hpp"
#include "gpu/Handle.hpp"

namespace bgl::gpu {

struct alignas(16) SkyboxUniforms {
    glm::mat4 invViewProj{1.0f};
    glm::vec4 sunDir{0.35f, 1.0f, 0.25f, 0.0f};
    glm::vec4 cameraPos{0.0f};
};

/**
 * @class Skybox
 * @brief Atmospheric skybox rendering with far-plane depth clamping and 4x MSAA support.
 */
class Skybox {
public:
    Skybox() = default;
    ~Skybox();

    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat, uint32_t sampleCount = 4);
    void Shutdown() noexcept;
    void Render(WGPURenderPassEncoder pass, const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec3& sunDir);

    void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }
    void Toggle() noexcept { enabled_ = !enabled_; }
    [[nodiscard]] bool IsEnabled() const noexcept { return enabled_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    bool enabled_{true};
    bool initialized_{false};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    RenderPipelineHandle pipeline_{};
    BindGroupLayoutHandle bindGroupLayout_{};
    BindGroupHandle bindGroup_{};
    BufferHandle uniformBuffer_{};
};

} // namespace bgl::gpu
