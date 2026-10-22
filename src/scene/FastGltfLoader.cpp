/**
 * @file FastGltfLoader.cpp
 * @brief Implementation of glTF 2.1 loader with fastgltf accessor parsing, embedded image decoding, and detailed logging.
 */
#include "scene/FastGltfLoader.hpp"
#include "scene/TextureDecoder.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <sstream>
#include <iomanip>

namespace bgl::scene {

static const char* AccessorTypeToString(fastgltf::AccessorType type) {
    switch (type) {
        case fastgltf::AccessorType::Scalar: return "Scalar";
        case fastgltf::AccessorType::Vec2: return "Vec2";
        case fastgltf::AccessorType::Vec3: return "Vec3";
        case fastgltf::AccessorType::Vec4: return "Vec4";
        case fastgltf::AccessorType::Mat2: return "Mat2";
        case fastgltf::AccessorType::Mat3: return "Mat3";
        case fastgltf::AccessorType::Mat4: return "Mat4";
        default: return "Unknown";
    }
}

static const char* ComponentTypeToString(fastgltf::ComponentType comp) {
    switch (comp) {
        case fastgltf::ComponentType::Byte: return "Byte (i8)";
        case fastgltf::ComponentType::UnsignedByte: return "UByte (u8)";
        case fastgltf::ComponentType::Short: return "Short (i16)";
        case fastgltf::ComponentType::UnsignedShort: return "UShort (u16)";
        case fastgltf::ComponentType::Int: return "Int (i32)";
        case fastgltf::ComponentType::UnsignedInt: return "UInt (u32)";
        case fastgltf::ComponentType::Float: return "Float (f32)";
        case fastgltf::ComponentType::Double: return "Double (f64)";
        default: return "Unknown";
    }
}

FastGltfLoader::FastGltfLoader(AssetManager& assetManager)
    : assetManager_(assetManager),
      parser_(fastgltf::Extensions::EXT_texture_webp |
              fastgltf::Extensions::KHR_texture_basisu |
              fastgltf::Extensions::KHR_texture_transform |
              fastgltf::Extensions::KHR_materials_clearcoat |
              fastgltf::Extensions::KHR_materials_transmission |
              fastgltf::Extensions::KHR_materials_sheen |
              fastgltf::Extensions::KHR_materials_specular |
              fastgltf::Extensions::KHR_materials_ior |
              fastgltf::Extensions::KHR_materials_iridescence |
              fastgltf::Extensions::KHR_materials_volume |
              fastgltf::Extensions::KHR_materials_emissive_strength |
              fastgltf::Extensions::KHR_materials_unlit) {}

core::Result<std::shared_ptr<Scene>> FastGltfLoader::LoadFromMemory(std::span<const uint8_t> data, std::string_view baseUri) {
    (void)baseUri;
    BGL_LOG_INFO("================================================================================");
    BGL_LOG_INFO("[glTF Loader] Starting Ingestion of Asset: '{}' (Total size: {} bytes / {:.2f} MB)",
                 baseUri.empty() ? "In-Memory Stream" : baseUri, data.size(), data.size() / (1024.0 * 1024.0));
    BGL_LOG_INFO("================================================================================");

    fastgltf::GltfDataBuffer dataBuffer;
    if (!dataBuffer.copyBytes(data.data(), data.size())) {
        return std::unexpected("Failed to copy bytes into fastgltf::GltfDataBuffer");
    }

    auto gltf = parser_.loadGltfBinary(&dataBuffer, std::filesystem::current_path(), fastgltf::Options::LoadGLBBuffers | fastgltf::Options::DecomposeNodeMatrices);
    if (gltf.error() != fastgltf::Error::None) {
        return std::unexpected(std::string("fastgltf failed to parse binary glTF data: ") + std::string(fastgltf::getErrorMessage(gltf.error())));
    }

    const auto& asset = gltf.get();
    auto scene = std::make_shared<Scene>();
    auto* mr = assetManager_.GetScratchArena().GetResource();

    // 1. Detailed Logging: Buffers & BufferViews
    BGL_LOG_INFO("[glTF Loader] --- Buffers ({} total) ---", asset.buffers.size());
    size_t totalBufferBytes = 0;
    for (size_t bIdx = 0; bIdx < asset.buffers.size(); ++bIdx) {
        const auto& buf = asset.buffers[bIdx];
        totalBufferBytes += buf.byteLength;
        const char* typeStr = "Unknown";
        if (std::holds_alternative<fastgltf::sources::Array>(buf.data)) typeStr = "Array";
        else if (std::holds_alternative<fastgltf::sources::ByteView>(buf.data)) typeStr = "ByteView";
        else if (std::holds_alternative<fastgltf::sources::Vector>(buf.data)) typeStr = "Vector";
        else if (std::holds_alternative<fastgltf::sources::URI>(buf.data)) typeStr = "URI";

        BGL_LOG_INFO("  * Buffer #{}: {} bytes ({:.2f} MB), Source: {}", bIdx, buf.byteLength, buf.byteLength / (1024.0 * 1024.0), typeStr);
    }

    BGL_LOG_INFO("[glTF Loader] --- BufferViews ({} total) ---", asset.bufferViews.size());
    for (size_t bvIdx = 0; bvIdx < asset.bufferViews.size(); ++bvIdx) {
        const auto& bv = asset.bufferViews[bvIdx];
        const char* targetStr = "None";
        if (bv.target == fastgltf::BufferTarget::ArrayBuffer) targetStr = "ArrayBuffer (Vertices)";
        else if (bv.target == fastgltf::BufferTarget::ElementArrayBuffer) targetStr = "ElementArrayBuffer (Indices)";

        BGL_LOG_DEBUG("  * BufferView #{}: Buffer #{}, Offset={}, Length={}, Stride={}, Target={}",
                      bvIdx, bv.bufferIndex, bv.byteOffset, bv.byteLength, bv.byteStride.value_or(0), targetStr);
    }

    // 2. Decode Embedded Images into CPU buffers
    BGL_LOG_INFO("[glTF Loader] --- Images & Textures ({} images, {} textures) ---", asset.images.size(), asset.textures.size());
    scene->gpuTextures.resize(asset.textures.size());
    scene->cpuImages.resize(asset.textures.size());

    for (size_t texIdx = 0; texIdx < asset.textures.size(); ++texIdx) {
        const auto& texture = asset.textures[texIdx];
        if (!texture.imageIndex.has_value()) {
            BGL_LOG_WARN("  * Texture #{}: No imageIndex associated", texIdx);
            continue;
        }

        size_t imgIdx = *texture.imageIndex;
        if (imgIdx >= asset.images.size()) {
            BGL_LOG_WARN("  * Texture #{}: Invalid imageIndex {} >= {}", texIdx, imgIdx, asset.images.size());
            continue;
        }

        const auto& image = asset.images[imgIdx];
        std::span<const uint8_t> rawBytes;
        size_t sourceBvIdx = 0;

        if (std::holds_alternative<fastgltf::sources::BufferView>(image.data)) {
            const auto& bvSource = std::get<fastgltf::sources::BufferView>(image.data);
            sourceBvIdx = bvSource.bufferViewIndex;
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

        std::string imgName = image.name.empty() ? ("Image_" + std::to_string(imgIdx)) : std::string(image.name);

        if (!rawBytes.empty()) {
            auto decoded = TextureDecoder::DecodeAuto(rawBytes, image.name);
            if (decoded) {
                scene->cpuImages[texIdx] = CpuImage{
                    .width = decoded->width,
                    .height = decoded->height,
                    .rgba = std::move(decoded->rgbaData)
                };
                BGL_LOG_INFO("  * Texture #{} (Image #{}, BufferView #{}): '{}' (MIME: {}) -> Decoded RGBA8 {}x{} (Raw: {} bytes, Decoded: {} bytes / {:.2f} MB)",
                             texIdx, imgIdx, sourceBvIdx, imgName,
                             image.name.empty() ? "embedded" : std::string(image.name),
                             decoded->width, decoded->height, rawBytes.size(),
                             scene->cpuImages[texIdx].rgba.size(),
                             scene->cpuImages[texIdx].rgba.size() / (1024.0 * 1024.0));
            } else {
                BGL_LOG_ERROR("  * Texture #{} (Image #{}): Failed to decode: {}", texIdx, imgIdx, decoded.error());
            }
        } else {
            BGL_LOG_WARN("  * Texture #{} (Image #{}): Raw byte stream is empty", texIdx, imgIdx);
        }
    }

    // 3. Decode Materials
    BGL_LOG_INFO("[glTF Loader] --- Materials ({} total) ---", asset.materials.size());
    scene->materials.reserve(asset.materials.size());
    for (size_t mIdx = 0; mIdx < asset.materials.size(); ++mIdx) {
        const auto& mat = asset.materials[mIdx];
        Material sceneMat;
        sceneMat.name = mat.name.empty() ? ("Material_" + std::to_string(mIdx)) : std::string(mat.name);
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

        // Metallic-Roughness Texture
        if (mat.pbrData.metallicRoughnessTexture.has_value()) {
            sceneMat.metallicRoughnessTextureIndex = static_cast<int>(mat.pbrData.metallicRoughnessTexture->textureIndex);
        }

        // Normal Texture
        if (mat.normalTexture.has_value()) {
            sceneMat.normalTextureIndex = static_cast<int>(mat.normalTexture->textureIndex);
            sceneMat.normalScale = mat.normalTexture->scale;
        }

        // Occlusion Texture
        if (mat.occlusionTexture.has_value()) {
            sceneMat.occlusionTextureIndex = static_cast<int>(mat.occlusionTexture->textureIndex);
            sceneMat.occlusionStrength = mat.occlusionTexture->strength;
        }

        // Emissive
        sceneMat.emissiveFactor = glm::vec3(mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]);
        if (mat.emissiveTexture.has_value()) {
            sceneMat.emissiveTextureIndex = static_cast<int>(mat.emissiveTexture->textureIndex);
        }
        sceneMat.emissiveStrength = mat.emissiveStrength;

        // Alpha Mode & Culling
        if (mat.alphaMode == fastgltf::AlphaMode::Mask) {
            sceneMat.alphaMode = AlphaMode::Mask;
        } else if (mat.alphaMode == fastgltf::AlphaMode::Blend) {
            sceneMat.alphaMode = AlphaMode::Blend;
        } else {
            sceneMat.alphaMode = AlphaMode::Opaque;
        }
        sceneMat.alphaCutoff = mat.alphaCutoff;
        sceneMat.doubleSided = mat.doubleSided;
        sceneMat.unlit = mat.unlit;

        // KHR_materials_clearcoat
        if (mat.clearcoat) {
            sceneMat.clearcoatFactor = mat.clearcoat->clearcoatFactor;
            sceneMat.clearcoatRoughnessFactor = mat.clearcoat->clearcoatRoughnessFactor;
            if (mat.clearcoat->clearcoatTexture.has_value()) {
                sceneMat.clearcoatTextureIndex = static_cast<int>(mat.clearcoat->clearcoatTexture->textureIndex);
            }
        }

        // KHR_materials_transmission & IOR
        if (mat.transmission) {
            sceneMat.transmissionFactor = mat.transmission->transmissionFactor;
            if (mat.transmission->transmissionTexture.has_value()) {
                sceneMat.transmissionTextureIndex = static_cast<int>(mat.transmission->transmissionTexture->textureIndex);
            }
        }
        sceneMat.ior = mat.ior;

        // KHR_materials_sheen
        if (mat.sheen) {
            sceneMat.sheenColorFactor = glm::vec3(mat.sheen->sheenColorFactor[0], mat.sheen->sheenColorFactor[1], mat.sheen->sheenColorFactor[2]);
            sceneMat.sheenRoughnessFactor = mat.sheen->sheenRoughnessFactor;
            if (mat.sheen->sheenColorTexture.has_value()) {
                sceneMat.sheenColorTextureIndex = static_cast<int>(mat.sheen->sheenColorTexture->textureIndex);
            }
        }

        // KHR_materials_specular
        if (mat.specular) {
            sceneMat.specularFactor = mat.specular->specularFactor;
            sceneMat.specularColorFactor = glm::vec3(mat.specular->specularColorFactor[0], mat.specular->specularColorFactor[1], mat.specular->specularColorFactor[2]);
            if (mat.specular->specularTexture.has_value()) {
                sceneMat.specularTextureIndex = static_cast<int>(mat.specular->specularTexture->textureIndex);
            }
        }

        std::string extDesc = "";
        if (sceneMat.clearcoatFactor > 0.0f) extDesc += " Clearcoat(" + std::to_string(sceneMat.clearcoatFactor) + ")";
        if (sceneMat.transmissionFactor > 0.0f) extDesc += " Transmission(" + std::to_string(sceneMat.transmissionFactor) + ")";
        if (glm::length(sceneMat.sheenColorFactor) > 0.0f) extDesc += " Sheen";
        if (sceneMat.unlit) extDesc += " Unlit";
        if (sceneMat.doubleSided) extDesc += " DoubleSided";

        BGL_LOG_INFO("  * Material #{}: '{}' | baseColorFactor=[{:.2f}, {:.2f}, {:.2f}, {:.2f}], metal={:.2f}, rough={:.2f}, baseColorTex={}, normalTex={}, emissiveTex={}{}",
                     mIdx, sceneMat.name,
                     sceneMat.baseColorFactor.r, sceneMat.baseColorFactor.g, sceneMat.baseColorFactor.b, sceneMat.baseColorFactor.a,
                     sceneMat.metallicFactor, sceneMat.roughnessFactor,
                     sceneMat.baseColorTextureIndex >= 0 ? std::to_string(sceneMat.baseColorTextureIndex) : "None",
                     sceneMat.normalTextureIndex >= 0 ? std::to_string(sceneMat.normalTextureIndex) : "None",
                     sceneMat.emissiveTextureIndex >= 0 ? std::to_string(sceneMat.emissiveTextureIndex) : "None",
                     extDesc);

        scene->materials.push_back(sceneMat);
    }

    // 4. Decode Accessors & Meshes
    BGL_LOG_INFO("[glTF Loader] --- Accessors ({} total) ---", asset.accessors.size());
    for (size_t aIdx = 0; aIdx < asset.accessors.size(); ++aIdx) {
        const auto& acc = asset.accessors[aIdx];
        BGL_LOG_DEBUG("  * Accessor #{}: BufferView #{}, Count={}, Type={}, CompType={}",
                      aIdx, acc.bufferViewIndex.value_or(0), acc.count,
                      AccessorTypeToString(acc.type), ComponentTypeToString(acc.componentType));
    }

    BGL_LOG_INFO("[glTF Loader] --- Meshes & Primitives ({} meshes) ---", asset.meshes.size());
    for (size_t mIdx = 0; mIdx < asset.meshes.size(); ++mIdx) {
        const auto& mesh = asset.meshes[mIdx];
        std::string meshName = mesh.name.empty() ? ("Mesh_" + std::to_string(mIdx)) : std::string(mesh.name);
        BGL_LOG_INFO("  * Mesh #{}: '{}' ({} primitives)", mIdx, meshName, mesh.primitives.size());

        for (size_t pIdx = 0; pIdx < mesh.primitives.size(); ++pIdx) {
            const auto& prim = mesh.primitives[pIdx];
            int matIdx = prim.materialIndex.has_value() ? static_cast<int>(*prim.materialIndex) : -1;
            std::string matName = (matIdx >= 0 && matIdx < static_cast<int>(scene->materials.size())) ? scene->materials[matIdx].name : "Default";

            size_t vertCount = 0;
            auto posIt = prim.findAttribute("POSITION");
            if (posIt != prim.attributes.end()) {
                vertCount = asset.accessors[posIt->second].count;
            }

            size_t idxCount = prim.indicesAccessor.has_value() ? asset.accessors[*prim.indicesAccessor].count : 0;
            size_t triCount = idxCount > 0 ? (idxCount / 3) : (vertCount / 3);

            BGL_LOG_INFO("    - Prim #{}: Material #{} ('{}') | {} vertices, {} indices ({} triangles)",
                         pIdx, matIdx, matName, vertCount, idxCount, triCount);

            for (const auto& [attrName, accIdx] : prim.attributes) {
                const auto& acc = asset.accessors[accIdx];
                BGL_LOG_INFO("      + Attr {}: Acc #{}, {} {}, count={}",
                             attrName, accIdx, AccessorTypeToString(acc.type), ComponentTypeToString(acc.componentType), acc.count);
            }
        }
    }

    std::vector<std::shared_ptr<SceneNode>> loadedNodes;
    loadedNodes.reserve(asset.nodes.size());

    // 5. Process all nodes and their meshes
    BGL_LOG_INFO("[glTF Loader] --- Scene Nodes Hierarchy ({} nodes total) ---", asset.nodes.size());
    for (size_t nodeIdx = 0; nodeIdx < asset.nodes.size(); ++nodeIdx) {
        const auto& gltfNode = asset.nodes[nodeIdx];
        auto node = std::make_shared<SceneNode>();
        node->name = gltfNode.name.empty() ? ("Node_" + std::to_string(nodeIdx)) : std::string(gltfNode.name);

        std::string transformDesc = "Identity";
        if (std::holds_alternative<fastgltf::TRS>(gltfNode.transform)) {
            const auto& trs = std::get<fastgltf::TRS>(gltfNode.transform);
            glm::vec3 translation(trs.translation[0], trs.translation[1], trs.translation[2]);
            glm::quat rotation(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
            glm::vec3 scale(trs.scale[0], trs.scale[1], trs.scale[2]);

            glm::mat4 tMat = glm::translate(glm::mat4(1.0f), translation);
            glm::mat4 rMat = glm::mat4_cast(rotation);
            glm::mat4 sMat = glm::scale(glm::mat4(1.0f), scale);
            node->localTransform = tMat * rMat * sMat;

            std::ostringstream ss;
            ss << std::fixed << std::setprecision(4)
               << "TRS(T=[" << translation.x << ", " << translation.y << ", " << translation.z << "], "
               << "S=[" << scale.x << ", " << scale.y << ", " << scale.z << "])";
            transformDesc = ss.str();
        } else if (std::holds_alternative<fastgltf::Node::TransformMatrix>(gltfNode.transform)) {
            const auto& mat = std::get<fastgltf::Node::TransformMatrix>(gltfNode.transform);
            node->localTransform = glm::make_mat4(mat.data());
            transformDesc = "Custom 4x4 Matrix";
        }

        if (gltfNode.meshIndex.has_value()) {
            size_t mIdx = *gltfNode.meshIndex;
            const auto& mesh = asset.meshes[mIdx];
            for (const auto& prim : mesh.primitives) {
                if (prim.type != fastgltf::PrimitiveType::Triangles) continue;

                MeshPrimitive meshPrim(mr);
                if (prim.materialIndex.has_value()) {
                    meshPrim.materialIndex = static_cast<int>(*prim.materialIndex);
                }

                // POSITION accessor
                auto posIt = prim.findAttribute("POSITION");
                if (posIt != prim.attributes.end()) {
                    const auto& posAccessor = asset.accessors[posIt->second];
                    meshPrim.vertices.resize(posAccessor.count);

                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAccessor, [&](glm::vec3 pos, size_t idx) {
                        meshPrim.vertices[idx].position = pos;
                        meshPrim.boundingBox.Expand(pos);
                    });
                }

                // NORMAL accessor
                auto normIt = prim.findAttribute("NORMAL");
                if (normIt != prim.attributes.end()) {
                    const auto& normAccessor = asset.accessors[normIt->second];
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, normAccessor, [&](glm::vec3 norm, size_t idx) {
                        if (idx < meshPrim.vertices.size()) {
                            meshPrim.vertices[idx].normal = norm;
                        }
                    });
                }

                // TEXCOORD_0 accessor
                auto uvIt = prim.findAttribute("TEXCOORD_0");
                if (uvIt != prim.attributes.end()) {
                    const auto& uvAccessor = asset.accessors[uvIt->second];
                    fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, uvAccessor, [&](glm::vec2 uv, size_t idx) {
                        if (idx < meshPrim.vertices.size()) {
                            meshPrim.vertices[idx].uv = uv;
                        }
                    });
                }

                // TANGENT accessor
                auto tanIt = prim.findAttribute("TANGENT");
                if (tanIt != prim.attributes.end()) {
                    const auto& tanAccessor = asset.accessors[tanIt->second];
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, tanAccessor, [&](glm::vec4 tan, size_t idx) {
                        if (idx < meshPrim.vertices.size()) {
                            meshPrim.vertices[idx].tangent = tan;
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

            BGL_LOG_INFO("  * Node #{}: '{}' -> Mesh #{} ('{}', {} prims) | {}",
                         nodeIdx, node->name, mIdx, mesh.name.empty() ? "" : std::string(mesh.name),
                         node->primitives.size(), transformDesc);
        } else if (gltfNode.cameraIndex.has_value()) {
            BGL_LOG_INFO("  * Node #{}: '{}' -> Camera #{} | {}", nodeIdx, node->name, *gltfNode.cameraIndex, transformDesc);
        } else {
            BGL_LOG_INFO("  * Node #{}: '{}' (Group / Empty) | {}", nodeIdx, node->name, transformDesc);
        }

        loadedNodes.push_back(node);
    }

    // 6. Build parent-child relationships
    std::vector<bool> isChild(asset.nodes.size(), false);
    for (size_t i = 0; i < asset.nodes.size(); ++i) {
        for (size_t childIdx : asset.nodes[i].children) {
            if (childIdx < loadedNodes.size()) {
                loadedNodes[i]->children.push_back(loadedNodes[childIdx]);
                isChild[childIdx] = true;
            }
        }
    }

    // 7. Register root nodes
    for (size_t i = 0; i < loadedNodes.size(); ++i) {
        if (!isChild[i]) {
            scene->rootNodes.push_back(loadedNodes[i]);
        }
    }

    scene->Update();

    // 8. Print ASCII Hierarchy Tree Diagram
    BGL_LOG_INFO("--------------------------------------------------------------------------------");
    BGL_LOG_INFO("[glTF Loader] Scene Graph Tree Diagram (Root Scene):");
    auto printTree = [&](auto& self, const std::shared_ptr<SceneNode>& n, const std::string& prefix, bool isLast) -> void {
        if (!n) return;
        std::string nodeInfo = n->name.empty() ? "(Unnamed Node)" : n->name;
        if (!n->primitives.empty()) {
            size_t totalVerts = 0;
            size_t totalIndices = 0;
            for (const auto& p : n->primitives) {
                totalVerts += p.vertices.size();
                totalIndices += p.indices.size();
            }
            nodeInfo += " [Mesh: " + std::to_string(n->primitives.size()) + " prims, " +
                        std::to_string(totalVerts) + " verts, " + std::to_string(totalIndices / 3) + " tris]";
        }
        BGL_LOG_INFO("{}{}-- {}", prefix, isLast ? "`" : "|", nodeInfo);
        for (size_t c = 0; c < n->children.size(); ++c) {
            bool childLast = (c + 1 == n->children.size());
            self(self, n->children[c], prefix + (isLast ? "   " : "|  "), childLast);
        }
    };
    for (size_t r = 0; r < scene->rootNodes.size(); ++r) {
        printTree(printTree, scene->rootNodes[r], "  ", (r + 1 == scene->rootNodes.size()));
    }
    BGL_LOG_INFO("--------------------------------------------------------------------------------");

    glm::vec3 minB = scene->sceneAABB.minBound;
    glm::vec3 maxB = scene->sceneAABB.maxBound;
    glm::vec3 center = scene->sceneAABB.Center();
    glm::vec3 extents = scene->sceneAABB.Extents();
    float radius = glm::length(extents);

    BGL_LOG_INFO("[glTF Loader] Ingestion Finished Successfully:");
    BGL_LOG_INFO("  * Total Nodes:        {}", asset.nodes.size());
    BGL_LOG_INFO("  * Total Meshes:       {}", asset.meshes.size());
    BGL_LOG_INFO("  * Total Materials:    {}", scene->materials.size());
    BGL_LOG_INFO("  * Total Textures:     {}", scene->cpuImages.size());
    BGL_LOG_INFO("  * Total Binary Size:  {} bytes ({:.2f} MB)", totalBufferBytes, totalBufferBytes / (1024.0 * 1024.0));
    BGL_LOG_INFO("  * Scene Bounds (min): [{:.4f}, {:.4f}, {:.4f}]", minB.x, minB.y, minB.z);
    BGL_LOG_INFO("  * Scene Bounds (max): [{:.4f}, {:.4f}, {:.4f}]", maxB.x, maxB.y, maxB.z);
    BGL_LOG_INFO("  * Scene Center:       [{:.4f}, {:.4f}, {:.4f}]", center.x, center.y, center.z);
    BGL_LOG_INFO("  * Scene Extents:      [{:.4f}, {:.4f}, {:.4f}] (Radius: {:.4f})", extents.x, extents.y, extents.z, radius);
    BGL_LOG_INFO("================================================================================");

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
