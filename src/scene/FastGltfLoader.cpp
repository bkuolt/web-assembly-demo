/**
 * @file FastGltfLoader.cpp
 * @brief Implementation of glTF 2.1 loader with fastgltf accessor parsing and embedded image decoding.
 */
#include "scene/FastGltfLoader.hpp"
#include "scene/TextureDecoder.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include <glm/gtc/type_ptr.hpp>

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

    // 1. Decode Embedded Images into CPU buffers
    scene->gpuTextures.resize(asset.textures.size());
    scene->cpuImages.resize(asset.textures.size());

    for (size_t texIdx = 0; texIdx < asset.textures.size(); ++texIdx) {
        const auto& texture = asset.textures[texIdx];
        if (!texture.imageIndex.has_value()) continue;

        size_t imgIdx = *texture.imageIndex;
        if (imgIdx >= asset.images.size()) continue;

        const auto& image = asset.images[imgIdx];
        std::span<const uint8_t> rawBytes;

        if (std::holds_alternative<fastgltf::sources::BufferView>(image.data)) {
            const auto& bvSource = std::get<fastgltf::sources::BufferView>(image.data);
            if (bvSource.bufferViewIndex < asset.bufferViews.size()) {
                const auto& bufferView = asset.bufferViews[bvSource.bufferViewIndex];
                if (bufferView.bufferIndex < asset.buffers.size()) {
                    const auto& buffer = asset.buffers[bufferView.bufferIndex];
                    if (std::holds_alternative<fastgltf::sources::Array>(buffer.data)) {
                        const auto& array = std::get<fastgltf::sources::Array>(buffer.data);
                        rawBytes = std::span<const uint8_t>(array.bytes.data() + bufferView.byteOffset, bufferView.byteLength);
                    } else if (std::holds_alternative<fastgltf::sources::ByteView>(buffer.data)) {
                        const auto& bv = std::get<fastgltf::sources::ByteView>(buffer.data);
                        rawBytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bv.bytes.data()) + bufferView.byteOffset, bufferView.byteLength);
                    } else if (std::holds_alternative<fastgltf::sources::Vector>(buffer.data)) {
                        const auto& vec = std::get<fastgltf::sources::Vector>(buffer.data);
                        rawBytes = std::span<const uint8_t>(vec.bytes.data() + bufferView.byteOffset, bufferView.byteLength);
                    }
                }
            }
        } else if (std::holds_alternative<fastgltf::sources::ByteView>(image.data)) {
            const auto& bv = std::get<fastgltf::sources::ByteView>(image.data);
            rawBytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bv.bytes.data()), bv.bytes.size());
        } else if (std::holds_alternative<fastgltf::sources::Vector>(image.data)) {
            const auto& vec = std::get<fastgltf::sources::Vector>(image.data);
            rawBytes = std::span<const uint8_t>(vec.bytes.data(), vec.bytes.size());
        } else if (std::holds_alternative<fastgltf::sources::Array>(image.data)) {
            const auto& arr = std::get<fastgltf::sources::Array>(image.data);
            rawBytes = std::span<const uint8_t>(arr.bytes.data(), arr.bytes.size());
        }

        if (!rawBytes.empty()) {
            auto decoded = TextureDecoder::DecodeAuto(rawBytes, image.name);
            if (decoded) {
                scene->cpuImages[texIdx] = CpuImage{
                    .width = decoded->width,
                    .height = decoded->height,
                    .rgba = std::move(decoded->rgbaData)
                };
                BGL_LOG_INFO("Decoded glTF texture #{} ({}x{}, {} bytes)", texIdx, decoded->width, decoded->height, scene->cpuImages[texIdx].rgba.size());
            } else {
                BGL_LOG_WARN("Failed to decode glTF texture #{}: {}", texIdx, decoded.error());
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
    BGL_LOG_INFO("glTF 2.1 loader parsed {} nodes, {} meshes, {} materials, {} textures. Scene bounds: [{:.4f}, {:.4f}, {:.4f}] to [{:.4f}, {:.4f}, {:.4f}]",
                 asset.nodes.size(), asset.meshes.size(), scene->materials.size(), scene->cpuImages.size(),
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