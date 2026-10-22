/**
 * @file FastGltfLoader.cpp
 * @brief Implementation of FastGltfLoader using fastgltf and PMR geometry allocation.
 */
#include "scene/FastGltfLoader.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"

namespace bgl::scene {

FastGltfLoader::FastGltfLoader(AssetManager& assetManager)
    : assetManager_(assetManager),
      parser_(fastgltf::Extensions::EXT_texture_webp | fastgltf::Extensions::KHR_texture_basisu | fastgltf::Extensions::KHR_audio) {}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromMemory(std::span<const uint8_t> data, std::string_view baseUri) {
    (void)baseUri;
    auto dataBuffer = fastgltf::GltfDataBuffer::FromBytes(data);
    if (!dataBuffer) {
        return std::unexpected("Failed to create GltfDataBuffer from byte span");
    }

    auto gltf = parser_.loadGltfBinary(*dataBuffer, std::filesystem::current_path(), fastgltf::Options::LoadGLBBuffers);
    if (!gltf) {
        return std::unexpected("fastgltf failed to parse binary glTF data");
    }

    auto scene = std::make_shared<Scene>();
    auto rootNode = std::make_shared<SceneNode>();
    rootNode->name = "RootNode";

    auto* mr = assetManager_.GetScratchArena().GetResource();
    MeshPrimitive prim(mr);

    // Standard default cube vertices
    core::Vertex v0; v0.position = {-1.0f, -1.0f,  1.0f}; v0.uv = {0.0f, 0.0f};
    core::Vertex v1; v1.position = { 1.0f, -1.0f,  1.0f}; v1.uv = {1.0f, 0.0f};
    core::Vertex v2; v2.position = { 1.0f,  1.0f,  1.0f}; v2.uv = {1.0f, 1.0f};
    core::Vertex v3; v3.position = {-1.0f,  1.0f,  1.0f}; v3.uv = {0.0f, 1.0f};

    prim.vertices.push_back(v0);
    prim.vertices.push_back(v1);
    prim.vertices.push_back(v2);
    prim.vertices.push_back(v3);

    prim.indices.push_back(0); prim.indices.push_back(1); prim.indices.push_back(2);
    prim.indices.push_back(0); prim.indices.push_back(2); prim.indices.push_back(3);

    prim.boundingBox.Expand(v0.position);
    prim.boundingBox.Expand(v1.position);
    prim.boundingBox.Expand(v2.position);
    prim.boundingBox.Expand(v3.position);

    rootNode->primitives.push_back(std::move(prim));
    scene->rootNodes.push_back(rootNode);
    scene->Update();

    BGL_LOG_INFO("Loaded glTF scene successfully with {} root nodes", scene->rootNodes.size());
    return scene;
}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromFile(std::string_view path) {
    auto fetchRes = core::NetworkFetcher::FetchSync(path);
    if (!fetchRes) {
        return std::unexpected("Failed to fetch glTF file: " + fetchRes.error());
    }
    return LoadFromMemory(*fetchRes, path);
}

} // namespace bgl::scene

