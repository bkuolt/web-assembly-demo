/**
 * @file Skybox.hpp
 * @brief Skybox cube geometry and Image-Based Lighting (IBL) environment pass.
 */
#pragma once

#include <vector>
#include <cstdint>
#include <string_view>
#include "core/Types.hpp"

namespace bgl::gpu {

/**
 * @class Skybox
 * @brief Cubemap skybox renderer and environment map manager.
 */
class Skybox {
public:
    Skybox() = default;
    core::Result<void> LoadFromKTX2(std::string_view ktxPath);
    void Render(const glm::mat4& viewProj);

    [[nodiscard]] bool IsLoaded() const noexcept { return isLoaded_; }

private:
    bool isLoaded_ = false;
    std::string path_;
};

} // namespace bgl::gpu

