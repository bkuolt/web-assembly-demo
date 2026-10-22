/**
 * @file FastGltfLoader.hpp
 * @brief glTF 2.1 loader supporting binary (.glb), EXT_texture_webp, KHR_audio, and tangent generation.
 */
#pragma once

#include <string_view>
#include <memory>
#include <span>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include "core/Types.hpp"
#include "scene/Scene.hpp"
#include "scene/AssetManager.hpp"

namespace bgl::scene {

/**
 * @class FastGltfLoader
 * @brief High-performance SIMD-ready glTF 2.1 loader.
 */
class FastGltfLoader {
public:
    explicit FastGltfLoader(AssetManager& assetManager);

    /**
     * @brief Load a glTF 2.1 scene from a binary buffer (GLB).
     */
    core::Result<std::shared_ptr<Scene>> LoadFromMemory(std::span<const uint8_t> data, std::string_view baseUri);

    /**
     * @brief Load a glTF 2.1 scene from a file path or URL.
     */
    core::Result<std::shared_ptr<Scene>> LoadFromFile(std::string_view path);

private:
    AssetManager& assetManager_;
    fastgltf::Parser parser_;
};

} // namespace bgl::scene

