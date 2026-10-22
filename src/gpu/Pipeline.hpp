/**
 * @file Pipeline.hpp
 * @brief WebGPU RenderPipelines for Opaque, Mask, Blend, and Bounding-Box rendering.
 */
#pragma once

#include <webgpu/webgpu.h>
#include <string_view>
#include "core/Types.hpp"

namespace bgl::gpu {

enum class RenderAlphaMode {
    Opaque,
    Mask,
    Blend
};

/**
 * @class PipelineManager
 * @brief Creates and caches WebGPU render pipelines for PBR and wireframes.
 */
class PipelineManager {
public:
    PipelineManager() = default;
    core::Result<void> Initialize();

    void BindPipeline(RenderAlphaMode mode);
    void BindWireframePipeline();

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    bool initialized_ = false;
};

} // namespace bgl::gpu

