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

struct PrimitiveUniforms {
    glm::mat4 viewProj{1.0f};
    glm::mat4 model{1.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 lightDir{0.0f, 1.0f, 0.5f, 0.0f};
    glm::vec4 baseColorFactor{1.0f};
};

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

    WGPUTexture defaultTexture_ = nullptr;
    WGPUTextureView defaultTextureView_ = nullptr;
    WGPUSampler defaultSampler_ = nullptr;
};

} // namespace bgl::gpu
