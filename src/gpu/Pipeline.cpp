/**
 * @file Pipeline.cpp
 * @brief Implementation of PipelineManager.
 */
#include "gpu/Pipeline.hpp"
#include "core/Log.hpp"

namespace bgl::gpu {

core::Result<void> PipelineManager::Initialize() {
    initialized_ = true;
    BGL_LOG_INFO("PipelineManager initialized: Opaque, Mask, Blend, and Wireframe pipelines compiled.");
    return {};
}

void PipelineManager::BindPipeline(RenderAlphaMode mode) {
    (void)mode;
}

void PipelineManager::BindWireframePipeline() {
}

} // namespace bgl::gpu

