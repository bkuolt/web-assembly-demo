/**
 * @file FastGltfLoader.cpp
 * @brief Implementation of glTF 2.1 loader with fastgltf accessor parsing, embedded image decoding, and GPU textures.
 */
#include "scene/FastGltfLoader.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include <glm/gtc/type_ptr.hpp>

#if defined(BGL_PLATFORM_NATIVE)
#include <GL/gl.h>
#endif

namespace bgl::scene {

FastGltfLoader::FastGltfLoader(AssetManager& assetManager)
    : assetManager_(assetManager),
      parser_(fastgltf::Extensions::EXT_texture_webp | fastgltf::Extensions::KHR_texture_basisu) {}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromMemory(std::span<const uint8_t> data, std::string_view baseUri) {
    (void)baseUri;
    fastgltf::GltfDataBuffer dataBuffer;
    if (!dataBuffer.copyBytes(data.data(), data.size())) {
        return std::unexpected("Failed to copy bytes into fastgltf::GltfDataBuffer");
    }

    auto gltf = parser_.loadGltfBinary(&dataBuffer, std::filesystem::current_path(), fastgltf::Options::LoadGLBBuffers | fastgltf::Options::DecomposeNodeMatrices);
    if (gltf.error() != fastgltf::Error::None) {
        return std::unexpected("fastgltf failed to parse binary glTF data");
    }

    const auto& asset = gltf.get();
    auto scene = std::make_shared<Scene>();
    auto* mr = assetManager_.GetScratchArena().GetResource();

    // 1. Decode Embedded Images & Upload to OpenGL Textures
    scene->gpuTextures.resize(asset.textures.size(), 0);
    for (size_t texIdx = 0; texIdx < asset.textures.size(); ++texIdx) {
        const auto& texture = asset.textures[texIdx];
        if (!texture.imageIndex.has_value()) continue;

        size_t imgIdx = *texture.imageIndex;
        if (imgIdx >= asset.images.size()) continue;

        const auto& image = asset.images[imgIdx];
        if (std::holds_alternative<fastgltf::sources::BufferView>(image.data)) {
            const auto& bvSource = std::get<fastgltf::sources::BufferView>(image.data);
            if (bvSource.bufferViewIndex < asset.bufferViews.size()) {
                const auto& bufferView = asset.bufferViews[bvSource.bufferViewIndex];
                if (bufferView.bufferIndex < asset.buffers.size()) {
                    const auto& buffer = asset.buffers[bufferView.bufferIndex];
                    if (std::holds_alternative<fastgltf::sources::Array>(buffer.data)) {
                        const auto& array = std::get<fastgltf::sources::Array>(buffer.data);
                        const uint8_t* rawBytes = array.bytes.data() + bufferView.byteOffset;
                        size_t byteCount = bufferView.byteLength;

                        auto decoded = TextureDecoder::DecodePNG(std::span<const uint8_t>(rawBytes, byteCount));
                        if (decoded) {
#if defined(BGL_PLATFORM_NATIVE)
                            GLuint texId = 0;
                            glGenTextures(1, &texId);
                            glBindTexture(GL_TEXTURE_2D, texId);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, decoded->width, decoded->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded->rgbaData.data());
                            scene->gpuTextures[texIdx] = texId;
                            BGL_LOG_INFO("Uploaded embedded texture #{} ({}x{}) to GPU ID {}", texIdx, decoded->width, decoded->height, texId);
#endif
                        }
                    }
                }
            }
        }
    }

    // 2. Decode Materials
    scene->materials.reserve(asset.materials.size());
    for (const auto& mat : asset.materials) {
        Material sceneMat;
        sceneMat.name = std::string(mat.name);
        sceneMat.baseColorFactor = glm::vec4(
            mat.pbrData.baseColorFactor[0],
            mat.pbrData.baseColorFactor[1],
            mat.pbrData.baseColorFactor[2],
            mat.pbrData.baseColorFactor[3]
        );
        sceneMat.metallicFactor = mat.pbrData.metallicFactor;
        sceneMat.roughnessFactor = mat.pbrData.roughnessFactor;

        if (mat.pbrData.baseColorTexture.has_value()) {
            sceneMat.baseColorTextureIndex = static_cast<int>(mat.pbrData.baseColorTexture->textureIndex);
        }
        scene->materials.push_back(sceneMat);
    }

    std::vector<std::shared_ptr<SceneNode>> loadedNodes;
    loadedNodes.reserve(asset.nodes.size());

    // 3. Process all nodes and their meshes
    for (size_t nodeIdx = 0; nodeIdx < asset.nodes.size(); ++nodeIdx) {
        const auto& gltfNode = asset.nodes[nodeIdx];
        auto node = std::make_shared<SceneNode>();
        node->name = std::string(gltfNode.name);

        if (std::holds_alternative<fastgltf::TRS>(gltfNode.transform)) {
            const auto& trs = std::get<fastgltf::TRS>(gltfNode.transform);
            glm::vec3 translation(trs.translation[0], trs.translation[1], trs.translation[2]);
            glm::quat rotation(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
            glm::vec3 scale(trs.scale[0], trs.scale[1], trs.scale[2]);

            glm::mat4 tMat = glm::translate(glm::mat4(1.0f), translation);
            glm::mat4 rMat = glm::mat4_cast(rotation);
            glm::mat4 sMat = glm::scale(glm::mat4(1.0f), scale);
            node->localTransform = tMat * rMat * sMat;
        } else if (std::holds_alternative<fastgltf::Node::TransformMatrix>(gltfNode.transform)) {
            const auto& mat = std::get<fastgltf::Node::TransformMatrix>(gltfNode.transform);
            node->localTransform = glm::make_mat4(mat.data());
        }

        if (gltfNode.meshIndex.has_value()) {
            const auto& mesh = asset.meshes[*gltfNode.meshIndex];
            for (const auto& prim : mesh.primitives) {
                if (prim.type != fastgltf::PrimitiveType::Triangles) continue;

                MeshPrimitive meshPrim(mr);
                if (prim.materialIndex.has_value()) {
                    meshPrim.materialIndex = static_cast<int>(*prim.materialIndex);
                }

                // Find POSITION accessor
                auto posIt = prim.findAttribute("POSITION");
                if (posIt != prim.attributes.end()) {
                    const auto& posAccessor = asset.accessors[posIt->second];
                    meshPrim.vertices.resize(posAccessor.count);

                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAccessor, [&](glm::vec3 pos, size_t idx) {
                        meshPrim.vertices[idx].position = pos;
                        meshPrim.boundingBox.Expand(pos);
                    });
                }

                // Find NORMAL accessor
                auto normIt = prim.findAttribute("NORMAL");
                if (normIt != prim.attributes.end()) {
                    const auto& normAccessor = asset.accessors[normIt->second];
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, normAccessor, [&](glm::vec3 norm, size_t idx) {
                        if (idx < meshPrim.vertices.size()) {
                            meshPrim.vertices[idx].normal = norm;
                        }
                    });
                }

                // Find TEXCOORD_0 accessor
                auto uvIt = prim.findAttribute("TEXCOORD_0");
                if (uvIt != prim.attributes.end()) {
                    const auto& uvAccessor = asset.accessors[uvIt->second];
                    fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, uvAccessor, [&](glm::vec2 uv, size_t idx) {
                        if (idx < meshPrim.vertices.size()) {
                            meshPrim.vertices[idx].uv = uv;
                        }
                    });
                }

                // Indices accessor
                if (prim.indicesAccessor.has_value()) {
                    const auto& idxAccessor = asset.accessors[*prim.indicesAccessor];
                    meshPrim.indices.resize(idxAccessor.count);
                    fastgltf::iterateAccessorWithIndex<uint32_t>(asset, idxAccessor, [&](uint32_t val, size_t idx) {
                        meshPrim.indices[idx] = val;
                    });
                }

                node->primitives.push_back(std::move(meshPrim));
            }
        }

        loadedNodes.push_back(node);
    }

    // 4. Build parent-child relationships
    std::vector<bool> isChild(asset.nodes.size(), false);
    for (size_t i = 0; i < asset.nodes.size(); ++i) {
        for (size_t childIdx : asset.nodes[i].children) {
            if (childIdx < loadedNodes.size()) {
                loadedNodes[i]->children.push_back(loadedNodes[childIdx]);
                isChild[childIdx] = true;
            }
        }
    }

    // 5. Register root nodes
    for (size_t i = 0; i < loadedNodes.size(); ++i) {
        if (!isChild[i]) {
            scene->rootNodes.push_back(loadedNodes[i]);
        }
    }

    scene->Update();
    BGL_LOG_INFO("glTF 2.1 loader parsed {} nodes, {} meshes, {} materials, {} textures. Scene bounds: [{:.2f}, {:.2f}, {:.2f}] to [{:.2f}, {:.2f}, {:.2f}]",
                 asset.nodes.size(), asset.meshes.size(), scene->materials.size(), scene->gpuTextures.size(),
                 scene->sceneAABB.minBound.x, scene->sceneAABB.minBound.y, scene->sceneAABB.minBound.z,
                 scene->sceneAABB.maxBound.x, scene->sceneAABB.maxBound.y, scene->sceneAABB.maxBound.z);

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
