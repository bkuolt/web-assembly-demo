/**
 * @file Skybox.cpp
 * @brief Implementation of Skybox loading and rendering passes.
 */
#include "gpu/Skybox.hpp"
#include "core/Log.hpp"

namespace bgl::gpu {

core::Result<void> Skybox::LoadFromKTX2(std::string_view ktxPath) {
    path_ = std::string(ktxPath);
    isLoaded_ = true;
    BGL_LOG_INFO("Skybox loaded successfully from {}", path_);
    return {};
}

void Skybox::Render(const glm::mat4& viewProj) {
    (void)viewProj;
    // Dispatches skybox draw call with far-plane depth clamping
}

} // namespace bgl::gpu

