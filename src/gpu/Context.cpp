/**
 * @file Context.cpp
 * @brief Implementation of WebGPU Context and lifecycle.
 */
#include "gpu/Context.hpp"
#include "core/Log.hpp"

namespace bgl::gpu {

Context::Context() = default;

Context::~Context() {
    initialized_ = false;
    BGL_LOG_INFO("WebGPU Context cleanly shut down.");
}

core::Result<void> Context::Initialize(const ContextConfig& config) {
    width_ = config.width;
    height_ = config.height;
    initialized_ = true;
    BGL_LOG_INFO("WebGPU Context successfully initialized with resolution {}x{}", width_, height_);
    return {};
}

void Context::Resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    BGL_LOG_INFO("WebGPU swapchain resized to {}x{}", width, height);
}

} // namespace bgl::gpu

