/**
 * @file Pipeline.hpp
 * @brief WebGPU RenderPipelines for Opaque, Mask, Blend, and Bounding-Box rendering.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <string_view>
#include <glm/glm.hpp>
#include "core/Types.hpp"

namespace bgl::scene {
    class SceneNode;
    struct Scene;
}

namespace bgl::gpu {

enum class RenderAlphaMode {
    Opaque,
    Mask,
    Blend
};

/**
 * @class PipelineManager
 * @brief Creates and caches WebGPU/OpenGL render pipelines for PBR, mesh drawing, and wireframes.
 */
class PipelineManager {
public:
    PipelineManager() = default;
    core::Result<void> Initialize();

    void BindPipeline(RenderAlphaMode mode);
    void BindWireframePipeline();

    void RenderScene(const scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe = false);
    void RenderNode(const scene::Scene& scene, const scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, bool wireframe = false);

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    bool initialized_ = false;
};

} // namespace bgl::gpu

