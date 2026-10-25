/**
 * @file FastGltfLoader.hpp
 * @brief High-performance glTF 2.0 / 2.1 scene loader supporting binary (.glb), JSON (.gltf),
 *        KHR_lights_punctual, EXT_texture_webp, KHR_texture_basisu, MikkTSpace tangents,
 *        and deduplicated multi-threaded image decoding.
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
 * @brief SIMD-accelerated glTF asset loader and scene graph assembler.
 *
 * Coordinates parallel image decoding, accessor ingestion, MikkTSpace / Lengyel
 * tangent generation, flat normal synthesis, punctual light extraction, and node
 * hierarchy construction into an engine Scene representation.
 */
class FastGltfLoader {
public:
    /**
     * @brief Constructs a glTF loader referencing the central asset manager.
     * @param assetManager Cache and memory manager for textures and geometry buffers.
     */
    explicit FastGltfLoader(AssetManager& assetManager);

    /**
     * @brief Parses and constructs a 3D scene from an in-memory binary glTF / GLB buffer.
     * @param data Byte view over the raw GLB or glTF asset data.
     * @param baseUri Base directory or URI for resolving external buffers and textures.
     * @return Result containing the constructed Scene on success, or an error description.
     */
    core::Result<std::shared_ptr<Scene>> LoadFromMemory(std::span<const uint8_t> data, std::string_view baseUri);

    /**
     * @brief Loads and parses a glTF or GLB scene directly from a filesystem path or URI.
     * @param path Filesystem path, HTTP/HTTPS URL, or data URI to the 3D model.
     * @return Result containing the constructed Scene on success, or an error description.
     */
    core::Result<std::shared_ptr<Scene>> LoadFromFile(std::string_view path);

private:
    core::Result<std::shared_ptr<Scene>> ParseAndAssembleScene(
        fastgltf::GltfDataBuffer& dataBuffer,
        std::string_view baseUri,
        size_t totalBytes);

    AssetManager& assetManager_;
    fastgltf::Parser parser_;
};

} // namespace bgl::scene

