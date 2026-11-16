/**
 * @file FastGltfParser.cpp
 * @brief glTF asset stage parsers: textures, materials, lights, meshes/LODs, hierarchy, skins, and animations.
 */
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/ThreadPool.hpp"
#include "scene/FastGltfInternal.hpp"
#include "scene/TextureDecoder.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <utility>

namespace bgl::scene::detail {

namespace {

[[nodiscard]] constexpr std::string_view AccessorTypeToString(fastgltf::AccessorType type) noexcept {
    switch (type) {
    case fastgltf::AccessorType::Scalar:
        return "Scalar";
    case fastgltf::AccessorType::Vec2:
        return "Vec2";
    case fastgltf::AccessorType::Vec3:
        return "Vec3";
    case fastgltf::AccessorType::Vec4:
        return "Vec4";
    case fastgltf::AccessorType::Mat2:
        return "Mat2";
    case fastgltf::AccessorType::Mat3:
        return "Mat3";
    case fastgltf::AccessorType::Mat4:
        return "Mat4";
    default:
        return "Unknown";
    }
}

[[nodiscard]] constexpr std::string_view ComponentTypeToString(fastgltf::ComponentType comp) noexcept {
    switch (comp) {
    case fastgltf::ComponentType::Byte:
        return "Byte (i8)";
    case fastgltf::ComponentType::UnsignedByte:
        return "UByte (u8)";
    case fastgltf::ComponentType::Short:
        return "Short (i16)";
    case fastgltf::ComponentType::UnsignedShort:
        return "UShort (u16)";
    case fastgltf::ComponentType::Int:
        return "Int (i32)";
    case fastgltf::ComponentType::UnsignedInt:
        return "UInt (u32)";
    case fastgltf::ComponentType::Float:
        return "Float (f32)";
    case fastgltf::ComponentType::Double:
        return "Double (f64)";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::span<const uint8_t> ExtractSafeSlice(const void* basePtr, size_t totalSize, size_t offset,
                                                        size_t length) noexcept {
    if (!basePtr || offset > totalSize || length > (totalSize - offset)) {
        return {};
    }
    return std::span<const uint8_t>{static_cast<const uint8_t*>(basePtr) + offset, length};
}

[[nodiscard]] std::string MimeTypeToString(fastgltf::MimeType mime) {
    switch (mime) {
    case fastgltf::MimeType::JPEG:
        return "image/jpeg";
    case fastgltf::MimeType::PNG:
        return "image/png";
    case fastgltf::MimeType::KTX2:
        return "image/ktx2";
    case fastgltf::MimeType::GltfBuffer:
    case fastgltf::MimeType::OctetStream:
    default:
        return {};
    }
}

[[nodiscard]] std::pair<std::span<const uint8_t>, std::string>
ExtractContiguousSourceBytes(const fastgltf::DataSource& data) {
    if (std::holds_alternative<fastgltf::sources::Array>(data)) {
        const auto& arr = std::get<fastgltf::sources::Array>(data);
        return {{reinterpret_cast<const uint8_t*>(arr.bytes.data()), arr.bytes.size()}, MimeTypeToString(arr.mimeType)};
    }
    if (std::holds_alternative<fastgltf::sources::ByteView>(data)) {
        const auto& bv = std::get<fastgltf::sources::ByteView>(data);
        return {{reinterpret_cast<const uint8_t*>(bv.bytes.data()), bv.bytes.size()}, MimeTypeToString(bv.mimeType)};
    }
    if (std::holds_alternative<fastgltf::sources::Vector>(data)) {
        const auto& vec = std::get<fastgltf::sources::Vector>(data);
        return {{reinterpret_cast<const uint8_t*>(vec.bytes.data()), vec.bytes.size()}, MimeTypeToString(vec.mimeType)};
    }
    return {};
}

void ComputeFallbackNormals(MeshPrimitive& meshPrim) {
    auto safeFaceNormal = [](const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2) noexcept {
        const glm::vec3 cp{glm::cross(p1 - p0, p2 - p0)};
        const float lenSq{glm::dot(cp, cp)};
        return (lenSq > 1e-12f) ? (cp * (1.0f / std::sqrt(lenSq))) : glm::vec3{0.0f, 1.0f, 0.0f};
    };

    if (!meshPrim.indices.empty()) {
        for (size_t i{0}; i + 2 < meshPrim.indices.size(); i += 3) {
            const uint32_t i0{meshPrim.indices[i]};
            const uint32_t i1{meshPrim.indices[i + 1]};
            const uint32_t i2{meshPrim.indices[i + 2]};
            if (i0 < meshPrim.vertices.size() && i1 < meshPrim.vertices.size() && i2 < meshPrim.vertices.size()) {
                const glm::vec3 fn{safeFaceNormal(meshPrim.vertices[i0].position, meshPrim.vertices[i1].position,
                                                  meshPrim.vertices[i2].position)};
                meshPrim.vertices[i0].normal += fn;
                meshPrim.vertices[i1].normal += fn;
                meshPrim.vertices[i2].normal += fn;
            }
        }
        for (auto& v : meshPrim.vertices) {
            const float len{glm::length(v.normal)};
            v.normal = (len > 0.0001f) ? (v.normal / len) : glm::vec3{0.0f, 1.0f, 0.0f};
        }
        return;
    }

    for (size_t i{0}; i + 2 < meshPrim.vertices.size(); i += 3) {
        const glm::vec3 fn{safeFaceNormal(meshPrim.vertices[i].position, meshPrim.vertices[i + 1].position,
                                          meshPrim.vertices[i + 2].position)};
        meshPrim.vertices[i].normal = fn;
        meshPrim.vertices[i + 1].normal = fn;
        meshPrim.vertices[i + 2].normal = fn;
    }
}

[[nodiscard]] MeshPrimitive ParseMeshPrimitive(const fastgltf::Asset& asset, const fastgltf::Primitive& prim,
                                               bool& outNeedsTangents) {
    MeshPrimitive meshPrim;
    if (prim.materialIndex.has_value()) {
        meshPrim.materialIndex = static_cast<int>(*prim.materialIndex);
    }

    if (auto posIt = prim.findAttribute("POSITION"); posIt != prim.attributes.end()) {
        const auto& posAccessor = asset.accessors[posIt->second];
        meshPrim.vertices.resize(posAccessor.count);
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAccessor, [&](glm::vec3 pos, size_t idx) {
            meshPrim.vertices[idx].position = pos;
            meshPrim.boundingBox.Expand(pos);
        });
    }

    auto normIt = prim.findAttribute("NORMAL");
    if (normIt != prim.attributes.end()) {
        const auto& normAccessor = asset.accessors[normIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, normAccessor, [&](glm::vec3 norm, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].normal = norm;
            }
        });
    }

    if (auto uvIt = prim.findAttribute("TEXCOORD_0"); uvIt != prim.attributes.end()) {
        const auto& uvAccessor = asset.accessors[uvIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, uvAccessor, [&](glm::vec2 uv, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].uv = uv;
            }
        });
    }

    auto tanIt = prim.findAttribute("TANGENT");
    const bool hasExplicitTangents = (tanIt != prim.attributes.end());
    if (hasExplicitTangents) {
        const auto& tanAccessor = asset.accessors[tanIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, tanAccessor, [&](glm::vec4 tan, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].tangent = tan;
            }
        });
    }

    if (auto jointsIt = prim.findAttribute("JOINTS_0"); jointsIt != prim.attributes.end()) {
        const auto& jAccessor = asset.accessors[jointsIt->second];
        fastgltf::iterateAccessorWithIndex<glm::uvec4>(asset, jAccessor, [&](glm::uvec4 j, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                meshPrim.vertices[idx].joints = j;
            }
        });
    }

    if (auto weightsIt = prim.findAttribute("WEIGHTS_0"); weightsIt != prim.attributes.end()) {
        const auto& wAccessor = asset.accessors[weightsIt->second];
        fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, wAccessor, [&](glm::vec4 w, size_t idx) {
            if (idx < meshPrim.vertices.size()) {
                const float sum = w.x + w.y + w.z + w.w;
                if (sum > 1e-6f) {
                    w /= sum;
                }
                meshPrim.vertices[idx].weights = w;
            }
        });
    }

    if (!prim.targets.empty()) {
        meshPrim.morphTargets.resize(prim.targets.size());
        for (size_t tIdx = 0; tIdx < prim.targets.size(); ++tIdx) {
            auto& mt = meshPrim.morphTargets[tIdx];
            mt.positionDeltas.resize(meshPrim.vertices.size(), glm::vec3{0.0f});
            mt.normalDeltas.resize(meshPrim.vertices.size(), glm::vec3{0.0f});
            mt.tangentDeltas.resize(meshPrim.vertices.size(), glm::vec3{0.0f});

            auto loadDelta = [&](const char* attr, std::vector<glm::vec3>& dst) {
                if (auto it = prim.findTargetAttribute(tIdx, attr); it != prim.targets[tIdx].cend()) {
                    const auto& acc = asset.accessors[it->second];
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, acc, [&](glm::vec3 d, size_t idx) {
                        if (idx < dst.size()) {
                            dst[idx] = d;
                        }
                    });
                }
            };
            loadDelta("POSITION", mt.positionDeltas);
            loadDelta("NORMAL", mt.normalDeltas);
            loadDelta("TANGENT", mt.tangentDeltas);
        }
        meshPrim.baseVertices = meshPrim.vertices;
    }

    if (prim.indicesAccessor.has_value()) {
        const auto& idxAccessor = asset.accessors[*prim.indicesAccessor];
        meshPrim.indices.resize(idxAccessor.count);
        const size_t vertexCount{meshPrim.vertices.size()};
        bool hasInvalidIndices{false};
        fastgltf::iterateAccessorWithIndex<uint32_t>(asset, idxAccessor, [&](uint32_t val, size_t idx) {
            if (vertexCount > 0 && val >= vertexCount) {
                hasInvalidIndices = true;
                meshPrim.indices[idx] = 0;
            } else {
                meshPrim.indices[idx] = val;
            }
        });
        if (hasInvalidIndices) {
            BGL_LOG_WARN("[glTF Loader] Mesh primitive contains out-of-bounds index references; clamped to 0 "
                         "for GPU buffer safety");
        }
    }

    if (normIt == prim.attributes.end() && !meshPrim.vertices.empty()) {
        ComputeFallbackNormals(meshPrim);
    }

    outNeedsTangents = !hasExplicitTangents;
    if (hasExplicitTangents && !meshPrim.vertices.empty()) {
        const auto& firstTan{meshPrim.vertices.front().tangent};
        const glm::vec3 tVec{firstTan.x, firstTan.y, firstTan.z};
        if (glm::dot(tVec, tVec) < 0.25f || std::abs(firstTan.w) < 0.1f) {
            outNeedsTangents = true;
        }
    }

    return meshPrim;
}

void ParseInstancingTransforms(const fastgltf::Asset& asset, const fastgltf::Node& gltfNode, SceneNode& node) {
    if (gltfNode.instancingAttributes.empty()) {
        return;
    }

    size_t instCount{0};
    auto transIt = gltfNode.findInstancingAttribute("TRANSLATION");
    auto rotIt = gltfNode.findInstancingAttribute("ROTATION");
    auto scaleIt = gltfNode.findInstancingAttribute("SCALE");

    for (const auto& it : {transIt, rotIt, scaleIt}) {
        if (it != gltfNode.instancingAttributes.cend() && it->second < asset.accessors.size()) {
            instCount = std::max(instCount, asset.accessors[it->second].count);
        }
    }
    if (instCount == 0) {
        return;
    }

    std::vector<glm::vec3> instT(instCount, glm::vec3{0.0f});
    std::vector<glm::quat> instR(instCount, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    std::vector<glm::vec3> instS(instCount, glm::vec3{1.0f});

    if (transIt != gltfNode.instancingAttributes.cend() && transIt->second < asset.accessors.size()) {
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, asset.accessors[transIt->second],
                                                      [&](glm::vec3 t, size_t idx) {
                                                          if (idx < instT.size()) {
                                                              instT[idx] = t;
                                                          }
                                                      });
    }
    if (rotIt != gltfNode.instancingAttributes.cend() && rotIt->second < asset.accessors.size()) {
        fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, asset.accessors[rotIt->second],
                                                      [&](glm::vec4 q, size_t idx) {
                                                          if (idx < instR.size()) {
                                                              instR[idx] = glm::quat{q.w, q.x, q.y, q.z};
                                                          }
                                                      });
    }
    if (scaleIt != gltfNode.instancingAttributes.cend() && scaleIt->second < asset.accessors.size()) {
        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, asset.accessors[scaleIt->second],
                                                      [&](glm::vec3 s, size_t idx) {
                                                          if (idx < instS.size()) {
                                                              instS[idx] = s;
                                                          }
                                                      });
    }

    node.instanceTransforms.resize(instCount);
    for (size_t i = 0; i < instCount; ++i) {
        const glm::mat4 tMat = glm::translate(glm::mat4{1.0f}, instT[i]);
        const float qLen = glm::length(instR[i]);
        const glm::quat safeQ =
            (qLen > 1e-6f && std::isfinite(qLen)) ? (instR[i] / qLen) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        const glm::mat4 rMat = glm::mat4_cast(safeQ);
        const glm::mat4 sMat = glm::scale(glm::mat4{1.0f}, instS[i]);
        node.instanceTransforms[i] = tMat * rMat * sMat;
    }
}

void ParseCameras(const fastgltf::Asset& asset, Scene& scene) {
    scene.cameras.reserve(asset.cameras.size());
    for (size_t cIdx = 0; cIdx < asset.cameras.size(); ++cIdx) {
        const auto& cam = asset.cameras[cIdx];
        SceneCamera sc{};
        sc.name = cam.name.empty() ? ("Camera_" + std::to_string(cIdx)) : std::string(cam.name);
        if (std::holds_alternative<fastgltf::Camera::Perspective>(cam.camera)) {
            const auto& persp = std::get<fastgltf::Camera::Perspective>(cam.camera);
            sc.isPerspective = true;
            sc.yfov = static_cast<float>(persp.yfov);
            sc.znear = static_cast<float>(persp.znear);
            sc.zfar = persp.zfar.has_value() ? static_cast<float>(*persp.zfar) : 1000.0f;
        } else if (std::holds_alternative<fastgltf::Camera::Orthographic>(cam.camera)) {
            const auto& ortho = std::get<fastgltf::Camera::Orthographic>(cam.camera);
            sc.isPerspective = false;
            sc.xmag = static_cast<float>(ortho.xmag);
            sc.ymag = static_cast<float>(ortho.ymag);
            sc.znear = static_cast<float>(ortho.znear);
            sc.zfar = static_cast<float>(ortho.zfar);
        }

        for (size_t nIdx = 0; nIdx < scene.allNodes.size(); ++nIdx) {
            if (scene.allNodes[nIdx] && scene.allNodes[nIdx]->cameraIndex == static_cast<int>(cIdx)) {
                sc.nodeIndex = static_cast<int>(nIdx);
                sc.worldTransform = scene.allNodes[nIdx]->worldTransform;
                break;
            }
        }
        scene.cameras.push_back(std::move(sc));
    }
    if (!scene.cameras.empty()) {
        BGL_LOG_INFO("[glTF Loader] Successfully parsed {} glTF camera(s)", scene.cameras.size());
    }
}

void ParseSkins(const fastgltf::Asset& asset, Scene& scene) {
    scene.skins.reserve(asset.skins.size());
    for (size_t sIdx = 0; sIdx < asset.skins.size(); ++sIdx) {
        const auto& gltfSkin = asset.skins[sIdx];
        Skin skin{};
        skin.name = gltfSkin.name.empty() ? ("Skin_" + std::to_string(sIdx)) : std::string(gltfSkin.name);
        skin.skeletonRootNodeIndex = gltfSkin.skeleton.has_value() ? static_cast<int>(*gltfSkin.skeleton) : -1;
        skin.joints.reserve(gltfSkin.joints.size());
        for (size_t jIdx : gltfSkin.joints) {
            skin.joints.push_back(static_cast<int>(jIdx));
        }

        if (gltfSkin.inverseBindMatrices.has_value() && *gltfSkin.inverseBindMatrices < asset.accessors.size()) {
            const auto& ibmAccessor = asset.accessors[*gltfSkin.inverseBindMatrices];
            skin.inverseBindMatrices.reserve(ibmAccessor.count);
            fastgltf::iterateAccessor<glm::mat4>(asset, ibmAccessor,
                                                 [&](glm::mat4 ibm) { skin.inverseBindMatrices.push_back(ibm); });
        }
        if (skin.inverseBindMatrices.size() < skin.joints.size()) {
            skin.inverseBindMatrices.resize(skin.joints.size(), glm::mat4(1.0f));
        }
        scene.skins.push_back(std::move(skin));
    }
    if (!scene.skins.empty()) {
        BGL_LOG_INFO("[glTF Loader] Successfully parsed {} glTF skin(s)", scene.skins.size());
    }
}

void ParseAnimations(const fastgltf::Asset& asset, Scene& scene) {
    scene.animations.reserve(asset.animations.size());
    for (size_t aIdx = 0; aIdx < asset.animations.size(); ++aIdx) {
        const auto& gltfAnim = asset.animations[aIdx];
        Animation anim{};
        anim.name = gltfAnim.name.empty() ? ("Animation_" + std::to_string(aIdx)) : std::string(gltfAnim.name);

        anim.samplers.reserve(gltfAnim.samplers.size());
        for (const auto& gltfSampler : gltfAnim.samplers) {
            AnimationSampler sampler{};
            switch (gltfSampler.interpolation) {
            case fastgltf::AnimationInterpolation::Step:
                sampler.interpolation = AnimationInterpolation::Step;
                break;
            case fastgltf::AnimationInterpolation::CubicSpline:
                sampler.interpolation = AnimationInterpolation::CubicSpline;
                break;
            case fastgltf::AnimationInterpolation::Linear:
            default:
                sampler.interpolation = AnimationInterpolation::Linear;
                break;
            }

            if (gltfSampler.inputAccessor < asset.accessors.size()) {
                const auto& inputAcc = asset.accessors[gltfSampler.inputAccessor];
                sampler.timestamps.reserve(inputAcc.count);
                fastgltf::iterateAccessor<float>(asset, inputAcc, [&](float t) { sampler.timestamps.push_back(t); });
            }
            if (gltfSampler.outputAccessor < asset.accessors.size()) {
                const auto& outputAcc = asset.accessors[gltfSampler.outputAccessor];
                fastgltf::iterateAccessor<float>(asset, outputAcc,
                                                 [&](float v) { sampler.keyframeValues.push_back(v); });
            }
            anim.samplers.push_back(std::move(sampler));
        }

        anim.channels.reserve(gltfAnim.channels.size());
        for (const auto& gltfChannel : gltfAnim.channels) {
            AnimationChannel channel{};
            channel.samplerIndex = gltfChannel.samplerIndex;
            channel.targetNodeIndex = gltfChannel.nodeIndex.has_value() ? static_cast<int>(*gltfChannel.nodeIndex) : -1;

            switch (gltfChannel.path) {
            case fastgltf::AnimationPath::Translation:
                channel.path = AnimationPath::Translation;
                break;
            case fastgltf::AnimationPath::Rotation:
                channel.path = AnimationPath::Rotation;
                break;
            case fastgltf::AnimationPath::Scale:
                channel.path = AnimationPath::Scale;
                break;
            case fastgltf::AnimationPath::Weights:
                channel.path = AnimationPath::Weights;
                break;
            }
            anim.channels.push_back(channel);
        }

        anim.CalculateDuration();
        scene.animations.push_back(std::move(anim));
    }
    if (!scene.animations.empty()) {
        scene.activeAnimationIndex = 0;
        scene.animationPlaying = true;
        scene.animationTime = 0.0f;
        BGL_LOG_INFO("[glTF Loader] Successfully parsed {} glTF animation(s) (Active: '{}', Duration: {:.2f}s)",
                     scene.animations.size(), scene.animations[0].name, scene.animations[0].duration);
    }
}

} // namespace

void DecodeImagesAndTextures(const fastgltf::Asset& asset, std::string_view baseUri, AssetManager& assetManager,
                             Scene& scene, ProgressReporter* reporter) {
    const size_t numImages = asset.images.size();
    const size_t numTextures = asset.textures.size();
    BGL_LOG_INFO("[glTF Loader] --- Images & Textures ({} images, {} textures) ---", numImages, numTextures);
    scene.gpuTextures.resize(numTextures);
    scene.cpuImages.resize(numTextures);

    BGL_LOG_INFO("[glTF Loader] Launching parallel image decoding via centralized ThreadPool...");

    std::vector<std::optional<CpuImage>> decodedImages(numImages);
    std::atomic<size_t> completedImages{0};
    core::ThreadPool::Instance().ParallelFor(0, numImages, [&](size_t imgIdx) {
        if (reporter && reporter->IsCancelled()) {
            return;
        }
        const auto& image = asset.images[imgIdx];
        std::span<const uint8_t> rawBytes{};
        std::vector<uint8_t> fetchedUriBytes{};
        std::string decodeHint{};
        size_t sourceBvIdx{0};

        if (std::holds_alternative<fastgltf::sources::BufferView>(image.data)) {
            const auto& bvSource = std::get<fastgltf::sources::BufferView>(image.data);
            decodeHint = MimeTypeToString(bvSource.mimeType);
            sourceBvIdx = bvSource.bufferViewIndex;
            if (bvSource.bufferViewIndex < asset.bufferViews.size()) {
                const auto& bufferView = asset.bufferViews[bvSource.bufferViewIndex];
                if (bufferView.bufferIndex < asset.buffers.size()) {
                    const auto [bufBytes, unusedHint] =
                        ExtractContiguousSourceBytes(asset.buffers[bufferView.bufferIndex].data);
                    (void)unusedHint;
                    rawBytes = ExtractSafeSlice(bufBytes.data(), bufBytes.size(), bufferView.byteOffset,
                                                bufferView.byteLength);
                }
            }
        } else if (auto [srcBytes, srcHint] = ExtractContiguousSourceBytes(image.data); !srcBytes.empty()) {
            rawBytes = srcBytes;
            decodeHint = std::move(srcHint);
        } else if (std::holds_alternative<fastgltf::sources::URI>(image.data)) {
            const auto& uriSource = std::get<fastgltf::sources::URI>(image.data);
            const std::string rawUriStr{uriSource.uri.string()};
            decodeHint = MimeTypeToString(uriSource.mimeType);
            if (decodeHint.empty()) {
                decodeHint = rawUriStr;
            }
            const std::string resolvedUri{core::NetworkFetcher::ResolveRelativeUri(baseUri, rawUriStr)};
            BGL_LOG_INFO("  * Image #{}: Fetching external URI '{}'...", imgIdx, resolvedUri);
            auto fetchRes = core::NetworkFetcher::FetchSync(resolvedUri);
            if (fetchRes) {
                fetchedUriBytes = std::move(*fetchRes);
                const size_t offset{std::min(uriSource.fileByteOffset, fetchedUriBytes.size())};
                rawBytes = std::span<const uint8_t>{fetchedUriBytes.data() + offset, fetchedUriBytes.size() - offset};
            } else {
                BGL_LOG_ERROR("  * Image #{}: Failed to fetch external image URI '{}': {}", imgIdx, resolvedUri,
                              fetchRes.error());
            }
        }

        if (decodeHint.empty() && !image.name.empty()) {
            decodeHint = std::string{image.name};
        }

        const std::string imgName = image.name.empty()
                                        ? (decodeHint.empty() ? ("Image_" + std::to_string(imgIdx))
                                                              : core::NetworkFetcher::ExtractDisplayName(decodeHint))
                                        : std::string(image.name);

        if (rawBytes.empty()) {
            BGL_LOG_WARN("  * Image #{}: Raw byte stream is empty", imgIdx);
            return;
        }

        const uint64_t contentHash = Fnv1a64(rawBytes);
        if (auto cached = assetManager.GetCachedTexture(contentHash)) {
            decodedImages[imgIdx] = CpuImage{.width = cached->width,
                                             .height = cached->height,
                                             .rgba = cached->rgbaData,
                                             .isCompressed = cached->isCompressed,
                                             .compressionFormat = cached->compressionFormat,
                                             .mipLevels = cached->mipLevels};
            BGL_LOG_DEBUG("  * Image #{} ('{}') hit asset cache (content hash {:x})", imgIdx, imgName, contentHash);
            return;
        }

        auto decoded = TextureDecoder::DecodeAuto(rawBytes, decodeHint);
        if (!decoded) {
            BGL_LOG_ERROR("  * Image #{}: Failed to decode: {}", imgIdx, decoded.error());
            return;
        }

        auto sharedImg = std::make_shared<DecodedImage>(*decoded);
        assetManager.InsertTexture(contentHash, sharedImg);
        decodedImages[imgIdx] = CpuImage{.width = decoded->width,
                                         .height = decoded->height,
                                         .rgba = std::move(decoded->rgbaData),
                                         .isCompressed = decoded->isCompressed,
                                         .compressionFormat = decoded->compressionFormat,
                                         .mipLevels = std::move(decoded->mipLevels)};
        if (TextureDecoder::IsKtx2Data(rawBytes)) {
            decodedImages[imgIdx]->ktx2SourceBytes.assign(rawBytes.begin(), rawBytes.end());
        }
        BGL_LOG_DEBUG("  * Image #{} (BufferView #{}): '{}' -> Decoded {} {}x{} (Mips: {}, Payload: {:.2f} MB)", imgIdx,
                      sourceBvIdx, imgName, TextureDecoder::GetKtxTargetName(decodedImages[imgIdx]->compressionFormat),
                      decoded->width, decoded->height, std::max<size_t>(1, decodedImages[imgIdx]->mipLevels.size()),
                      static_cast<double>(decodedImages[imgIdx]->rgba.size()) / (1024.0 * 1024.0));
        if (reporter && numImages > 0) {
            const size_t done = completedImages.fetch_add(1, std::memory_order_relaxed) + 1;
            const float localFrac = 0.12f + (static_cast<float>(done) / static_cast<float>(numImages)) * 0.43f;
            (void)reporter->ReportLocal(localFrac, std::format("Decoding textures ({} / {})...", done, numImages));
        }
    });

    for (size_t texIdx{0}; texIdx < numTextures; ++texIdx) {
        const auto& texture = asset.textures[texIdx];
        fastgltf::Optional<size_t> resolvedImgIdx{texture.imageIndex};
        if (texture.basisuImageIndex.has_value()) {
            resolvedImgIdx = texture.basisuImageIndex;
        } else if (texture.webpImageIndex.has_value()) {
            resolvedImgIdx = texture.webpImageIndex;
        }
        if (resolvedImgIdx.has_value() && *resolvedImgIdx < decodedImages.size()) {
            const size_t imgIdx{*resolvedImgIdx};
            if (const auto& decodedOpt = decodedImages[imgIdx]; decodedOpt.has_value()) {
                scene.cpuImages[texIdx] = *decodedOpt;
            }
        }
    }
}

void ParseMaterialsAndLogStructure(const fastgltf::Asset& asset, Scene& scene) {
    BGL_LOG_INFO("[glTF Loader] --- Materials ({} total) ---", asset.materials.size());
    scene.materials.reserve(asset.materials.size());

    auto extractUvTransform = [](const auto& texInfo, Material::UvTransform& dst) noexcept {
        if (texInfo.transform) {
            const auto& tf = *texInfo.transform;
            dst.offset = glm::vec2{static_cast<float>(tf.uvOffset[0]), static_cast<float>(tf.uvOffset[1])};
            dst.scale = glm::vec2{static_cast<float>(tf.uvScale[0]), static_cast<float>(tf.uvScale[1])};
            dst.rotation = static_cast<float>(tf.rotation);
        }
    };

    for (size_t mIdx = 0; mIdx < asset.materials.size(); ++mIdx) {
        const auto& mat = asset.materials[mIdx];
        Material sceneMat;
        sceneMat.name = mat.name.empty() ? ("Material_" + std::to_string(mIdx)) : std::string(mat.name);
        sceneMat.baseColorFactor = glm::vec4(mat.pbrData.baseColorFactor[0], mat.pbrData.baseColorFactor[1],
                                             mat.pbrData.baseColorFactor[2], mat.pbrData.baseColorFactor[3]);
        sceneMat.metallicFactor = mat.pbrData.metallicFactor;
        sceneMat.roughnessFactor = mat.pbrData.roughnessFactor;

        if (mat.pbrData.baseColorTexture.has_value()) {
            sceneMat.baseColorTextureIndex = static_cast<int>(mat.pbrData.baseColorTexture->textureIndex);
            extractUvTransform(*mat.pbrData.baseColorTexture, sceneMat.baseColorUvTransform);
        }
        if (mat.pbrData.metallicRoughnessTexture.has_value()) {
            sceneMat.metallicRoughnessTextureIndex =
                static_cast<int>(mat.pbrData.metallicRoughnessTexture->textureIndex);
            extractUvTransform(*mat.pbrData.metallicRoughnessTexture, sceneMat.metallicRoughnessUvTransform);
        }
        if (mat.normalTexture.has_value()) {
            sceneMat.normalTextureIndex = static_cast<int>(mat.normalTexture->textureIndex);
            sceneMat.normalScale = mat.normalTexture->scale;
            extractUvTransform(*mat.normalTexture, sceneMat.normalUvTransform);
        }
        if (mat.occlusionTexture.has_value()) {
            sceneMat.occlusionTextureIndex = static_cast<int>(mat.occlusionTexture->textureIndex);
            sceneMat.occlusionStrength = mat.occlusionTexture->strength;
            extractUvTransform(*mat.occlusionTexture, sceneMat.occlusionUvTransform);
        }

        sceneMat.emissiveFactor = glm::vec3(mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]);
        if (mat.emissiveTexture.has_value()) {
            sceneMat.emissiveTextureIndex = static_cast<int>(mat.emissiveTexture->textureIndex);
            extractUvTransform(*mat.emissiveTexture, sceneMat.emissiveUvTransform);
        }
        sceneMat.emissiveStrength = mat.emissiveStrength;

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

        if (mat.clearcoat) {
            sceneMat.clearcoatFactor = mat.clearcoat->clearcoatFactor;
            sceneMat.clearcoatRoughnessFactor = mat.clearcoat->clearcoatRoughnessFactor;
            if (mat.clearcoat->clearcoatTexture.has_value()) {
                sceneMat.clearcoatTextureIndex = static_cast<int>(mat.clearcoat->clearcoatTexture->textureIndex);
            }
        }
        if (mat.transmission) {
            sceneMat.transmissionFactor = mat.transmission->transmissionFactor;
            if (mat.transmission->transmissionTexture.has_value()) {
                sceneMat.transmissionTextureIndex =
                    static_cast<int>(mat.transmission->transmissionTexture->textureIndex);
            }
        }
        sceneMat.ior = mat.ior;

        if (mat.sheen) {
            sceneMat.sheenColorFactor = glm::vec3(mat.sheen->sheenColorFactor[0], mat.sheen->sheenColorFactor[1],
                                                  mat.sheen->sheenColorFactor[2]);
            sceneMat.sheenRoughnessFactor = mat.sheen->sheenRoughnessFactor;
            if (mat.sheen->sheenColorTexture.has_value()) {
                sceneMat.sheenColorTextureIndex = static_cast<int>(mat.sheen->sheenColorTexture->textureIndex);
            }
        }
        if (mat.specular) {
            sceneMat.specularFactor = mat.specular->specularFactor;
            sceneMat.specularColorFactor =
                glm::vec3(mat.specular->specularColorFactor[0], mat.specular->specularColorFactor[1],
                          mat.specular->specularColorFactor[2]);
            if (mat.specular->specularTexture.has_value()) {
                sceneMat.specularTextureIndex = static_cast<int>(mat.specular->specularTexture->textureIndex);
            }
        }
        if (mat.volume) {
            sceneMat.thicknessFactor = mat.volume->thicknessFactor;
            sceneMat.attenuationDistance =
                std::isfinite(mat.volume->attenuationDistance) ? mat.volume->attenuationDistance : 1e9f;
            sceneMat.attenuationColor = glm::vec3(mat.volume->attenuationColor[0], mat.volume->attenuationColor[1],
                                                  mat.volume->attenuationColor[2]);
            if (mat.volume->thicknessTexture.has_value()) {
                sceneMat.thicknessTextureIndex = static_cast<int>(mat.volume->thicknessTexture->textureIndex);
            }
        }
        if (mat.iridescence) {
            sceneMat.iridescenceFactor = mat.iridescence->iridescenceFactor;
            sceneMat.iridescenceIor = mat.iridescence->iridescenceIor;
            sceneMat.iridescenceThicknessMinimum = mat.iridescence->iridescenceThicknessMinimum;
            sceneMat.iridescenceThicknessMaximum = mat.iridescence->iridescenceThicknessMaximum;
        }
        if (mat.anisotropy) {
            sceneMat.anisotropyStrength = mat.anisotropy->anisotropyStrength;
            sceneMat.anisotropyRotation = mat.anisotropy->anisotropyRotation;
        }

        std::string extDesc{};
        if (sceneMat.clearcoatFactor > 0.0f) {
            extDesc += " Clearcoat(" + std::to_string(sceneMat.clearcoatFactor) + ")";
        }
        if (sceneMat.transmissionFactor > 0.0f) {
            extDesc += " Transmission(" + std::to_string(sceneMat.transmissionFactor) + ")";
        }
        if (sceneMat.thicknessFactor > 0.0f) {
            extDesc += " Volume(thick=" + std::to_string(sceneMat.thicknessFactor) + ")";
        }
        if (sceneMat.iridescenceFactor > 0.0f) {
            extDesc += " Iridescence(" + std::to_string(sceneMat.iridescenceFactor) + ")";
        }
        if (sceneMat.anisotropyStrength > 0.0f) {
            extDesc += " Anisotropy(" + std::to_string(sceneMat.anisotropyStrength) + ")";
        }
        if (glm::length(sceneMat.sheenColorFactor) > 0.0f) {
            extDesc += " Sheen";
        }
        if (sceneMat.unlit) {
            extDesc += " Unlit";
        }
        if (sceneMat.doubleSided) {
            extDesc += " DoubleSided";
        }

        BGL_LOG_DEBUG(
            "  * Material #{}: '{}' | baseColorFactor=[{:.2f}, {:.2f}, {:.2f}, {:.2f}], metal={:.2f}, rough={:.2f}, "
            "baseColorTex={}, normalTex={}, emissiveTex={}{}",
            mIdx, sceneMat.name, sceneMat.baseColorFactor.r, sceneMat.baseColorFactor.g, sceneMat.baseColorFactor.b,
            sceneMat.baseColorFactor.a, sceneMat.metallicFactor, sceneMat.roughnessFactor,
            sceneMat.baseColorTextureIndex >= 0 ? std::to_string(sceneMat.baseColorTextureIndex) : "None",
            sceneMat.normalTextureIndex >= 0 ? std::to_string(sceneMat.normalTextureIndex) : "None",
            sceneMat.emissiveTextureIndex >= 0 ? std::to_string(sceneMat.emissiveTextureIndex) : "None", extDesc);

        scene.materials.push_back(sceneMat);
    }

    BGL_LOG_INFO("[glTF Loader] --- Accessors ({} total) ---", asset.accessors.size());
    for (size_t aIdx = 0; aIdx < asset.accessors.size(); ++aIdx) {
        const auto& acc = asset.accessors[aIdx];
        BGL_LOG_DEBUG("  * Accessor #{}: BufferView #{}, Count={}, Type={}, CompType={}", aIdx,
                      acc.bufferViewIndex.value_or(0), acc.count, AccessorTypeToString(acc.type),
                      ComponentTypeToString(acc.componentType));
    }

    BGL_LOG_INFO("[glTF Loader] --- Meshes & Primitives ({} meshes) ---", asset.meshes.size());
    for (size_t mIdx = 0; mIdx < asset.meshes.size(); ++mIdx) {
        const auto& mesh = asset.meshes[mIdx];
        const std::string meshName = mesh.name.empty() ? ("Mesh_" + std::to_string(mIdx)) : std::string(mesh.name);
        BGL_LOG_DEBUG("  * Mesh #{}: '{}' ({} primitives)", mIdx, meshName, mesh.primitives.size());

        for (size_t pIdx = 0; pIdx < mesh.primitives.size(); ++pIdx) {
            const auto& prim = mesh.primitives[pIdx];
            const int matIdx = prim.materialIndex.has_value() ? static_cast<int>(*prim.materialIndex) : -1;
            const std::string matName = (matIdx >= 0 && matIdx < static_cast<int>(scene.materials.size()))
                                            ? scene.materials[matIdx].name
                                            : "Default";

            size_t vertCount = 0;
            if (auto posIt = prim.findAttribute("POSITION"); posIt != prim.attributes.end()) {
                vertCount = asset.accessors[posIt->second].count;
            }
            const size_t idxCount = prim.indicesAccessor.has_value() ? asset.accessors[*prim.indicesAccessor].count : 0;
            const size_t triCount = idxCount > 0 ? (idxCount / 3) : (vertCount / 3);

            BGL_LOG_DEBUG("    - Prim #{}: Material #{} ('{}') | {} vertices, {} indices ({} tris)", pIdx, matIdx,
                          matName, vertCount, idxCount, triCount);

            for (const auto& [attrName, accIdx] : prim.attributes) {
                const auto& acc = asset.accessors[accIdx];
                BGL_LOG_DEBUG("      + Attr {}: Acc #{}, {} {}, count={}", attrName, accIdx,
                              AccessorTypeToString(acc.type), ComponentTypeToString(acc.componentType), acc.count);
            }
        }
    }
}

void ParsePunctualLights(const fastgltf::Asset& asset, Scene& scene) {
    if (asset.lights.empty()) {
        return;
    }

    BGL_LOG_INFO("[glTF Loader] --- KHR_lights_punctual ({} lights) ---", asset.lights.size());
    scene.lights.reserve(asset.lights.size());
    for (size_t lIdx{0}; lIdx < asset.lights.size(); ++lIdx) {
        const auto& gltfLight = asset.lights[lIdx];
        PunctualLight pLight{};
        pLight.name = gltfLight.name.empty() ? ("Light_" + std::to_string(lIdx)) : std::string{gltfLight.name};
        switch (gltfLight.type) {
        case fastgltf::LightType::Directional:
            pLight.type = LightType::Directional;
            break;
        case fastgltf::LightType::Point:
            pLight.type = LightType::Point;
            break;
        case fastgltf::LightType::Spot:
            pLight.type = LightType::Spot;
            break;
        }
        pLight.color = glm::vec3{static_cast<float>(gltfLight.color[0]), static_cast<float>(gltfLight.color[1]),
                                 static_cast<float>(gltfLight.color[2])};
        pLight.intensity = static_cast<float>(gltfLight.intensity);
        pLight.range = gltfLight.range.has_value() ? static_cast<float>(*gltfLight.range) : 0.0f;
        pLight.innerConeAngle =
            gltfLight.innerConeAngle.has_value() ? static_cast<float>(*gltfLight.innerConeAngle) : 0.0f;
        pLight.outerConeAngle =
            gltfLight.outerConeAngle.has_value() ? static_cast<float>(*gltfLight.outerConeAngle) : 0.785398163f;

        BGL_LOG_INFO("  * Light #{}: '{}' Type={} Color=[{:.2f}, {:.2f}, {:.2f}] Intensity={:.2f} Range={:.2f}", lIdx,
                     pLight.name,
                     pLight.type == LightType::Directional ? "Directional"
                                                           : (pLight.type == LightType::Point ? "Point" : "Spot"),
                     pLight.color.r, pLight.color.g, pLight.color.b, pLight.intensity, pLight.range);
        scene.lights.push_back(std::move(pLight));
    }
}

void ParseMeshesAndGenerateLods(const fastgltf::Asset& asset, Scene& scene, ProgressReporter* reporter) {
    std::vector<std::shared_ptr<Mesh>> parsedMeshes(asset.meshes.size());
    std::vector<std::vector<uint8_t>> meshPrimNeedsTangents(asset.meshes.size());

    core::ThreadPool::Instance().ParallelFor(0, asset.meshes.size(), [&](size_t mIdx) {
        if (reporter && reporter->IsCancelled()) {
            return;
        }
        const auto& mesh = asset.meshes[mIdx];
        auto sharedMesh = std::make_shared<Mesh>();
        sharedMesh->name = mesh.name.empty() ? ("Mesh_" + std::to_string(mIdx)) : std::string(mesh.name);
        sharedMesh->primitives.reserve(mesh.primitives.size());
        meshPrimNeedsTangents[mIdx].reserve(mesh.primitives.size());

        for (const auto& prim : mesh.primitives) {
            if (prim.type != fastgltf::PrimitiveType::Triangles) {
                continue;
            }
            bool needsTangents{false};
            MeshPrimitive meshPrim = ParseMeshPrimitive(asset, prim, needsTangents);
            meshPrimNeedsTangents[mIdx].push_back(needsTangents ? 1u : 0u);
            sharedMesh->primitives.push_back(std::move(meshPrim));
        }
        parsedMeshes[mIdx] = std::move(sharedMesh);
    });

    struct PrimitivePostTask {
        MeshPrimitive* prim{nullptr};
        bool computeTangents{false};
    };
    std::vector<PrimitivePostTask> postTasks{};
    for (size_t mIdx{0}; mIdx < parsedMeshes.size(); ++mIdx) {
        const auto& sharedMesh = parsedMeshes[mIdx];
        if (!sharedMesh) {
            continue;
        }
        for (size_t pIdx{0}; pIdx < sharedMesh->primitives.size(); ++pIdx) {
            const bool needTan = pIdx < meshPrimNeedsTangents[mIdx].size() && meshPrimNeedsTangents[mIdx][pIdx] != 0;
            postTasks.push_back(PrimitivePostTask{&sharedMesh->primitives[pIdx], needTan});
        }
    }
    if (!postTasks.empty()) {
        std::atomic<size_t> completedPrims{0};
        const size_t totalPrims = postTasks.size();
        core::ThreadPool::Instance().ParallelFor(
            0, totalPrims,
            [&](size_t tIdx) {
                if (reporter && reporter->IsCancelled()) {
                    return;
                }
                const PrimitivePostTask task = postTasks[tIdx];
                if (task.computeTangents) {
                    task.prim->ComputeTangents();
                }
                task.prim->GenerateLODs();
                if (reporter) {
                    const size_t done = completedPrims.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done == totalPrims || (done % 4u) == 0u || totalPrims <= 16u) {
                        const float localFrac =
                            0.58f + (static_cast<float>(done) / static_cast<float>(totalPrims)) * 0.34f;
                        (void)reporter->ReportLocal(
                            localFrac, std::format("Building meshes & LODs ({} / {})...", done, totalPrims));
                    }
                }
            },
            1);
    }
    scene.meshes = std::move(parsedMeshes);
}

void BuildSceneHierarchy(const fastgltf::Asset& asset, Scene& scene) {
    std::vector<std::shared_ptr<SceneNode>> loadedNodes(asset.nodes.size());
    BGL_LOG_INFO("[glTF Loader] --- Scene Nodes Hierarchy ({} nodes total, parallel parsing) ---", asset.nodes.size());

    core::ThreadPool::Instance().ParallelFor(0, asset.nodes.size(), [&](size_t nodeIdx) {
        const auto& gltfNode = asset.nodes[nodeIdx];
        auto node = std::make_shared<SceneNode>();
        node->nodeIndex = static_cast<int>(nodeIdx);
        node->name = gltfNode.name.empty() ? ("Node_" + std::to_string(nodeIdx)) : std::string(gltfNode.name);
        if (gltfNode.lightIndex.has_value() && *gltfNode.lightIndex < asset.lights.size()) {
            node->lightIndex = static_cast<int>(*gltfNode.lightIndex);
        }
        if (gltfNode.cameraIndex.has_value() && *gltfNode.cameraIndex < asset.cameras.size()) {
            node->cameraIndex = static_cast<int>(*gltfNode.cameraIndex);
        }
        if (gltfNode.skinIndex.has_value() && *gltfNode.skinIndex < asset.skins.size()) {
            node->skinIndex = static_cast<int>(*gltfNode.skinIndex);
        }

        if (std::holds_alternative<fastgltf::TRS>(gltfNode.transform)) {
            const auto& trs = std::get<fastgltf::TRS>(gltfNode.transform);
            node->translation = glm::vec3(trs.translation[0], trs.translation[1], trs.translation[2]);
            node->rotation = glm::quat(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
            node->scale = glm::vec3(trs.scale[0], trs.scale[1], trs.scale[2]);
        } else if (std::holds_alternative<fastgltf::Node::TransformMatrix>(gltfNode.transform)) {
            const auto& mat = std::get<fastgltf::Node::TransformMatrix>(gltfNode.transform);
            const glm::mat4 m = glm::make_mat4(mat.data());
            node->translation = glm::vec3(m[3]);
            node->scale =
                glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            const glm::mat3 rotM(node->scale.x > 1e-6f ? glm::vec3(m[0]) / node->scale.x : glm::vec3(1, 0, 0),
                                 node->scale.y > 1e-6f ? glm::vec3(m[1]) / node->scale.y : glm::vec3(0, 1, 0),
                                 node->scale.z > 1e-6f ? glm::vec3(m[2]) / node->scale.z : glm::vec3(0, 0, 1));
            node->rotation = glm::quat_cast(rotM);
        }
        node->UpdateLocalTransform();

        if (!gltfNode.weights.empty()) {
            node->morphWeights.assign(gltfNode.weights.begin(), gltfNode.weights.end());
        } else if (gltfNode.meshIndex.has_value() && *gltfNode.meshIndex < asset.meshes.size() &&
                   !asset.meshes[*gltfNode.meshIndex].weights.empty()) {
            const auto& mw = asset.meshes[*gltfNode.meshIndex].weights;
            node->morphWeights.assign(mw.begin(), mw.end());
        }

        if (gltfNode.meshIndex.has_value() && *gltfNode.meshIndex < scene.meshes.size()) {
            node->mesh = scene.meshes[*gltfNode.meshIndex];
        }

        ParseInstancingTransforms(asset, gltfNode, *node);
        loadedNodes[nodeIdx] = std::move(node);
    });

    std::vector<bool> isChild(asset.nodes.size(), false);
    for (size_t i{0}; i < asset.nodes.size(); ++i) {
        for (size_t childIdx : asset.nodes[i].children) {
            if (childIdx < loadedNodes.size() && childIdx != i && !isChild[childIdx]) {
                loadedNodes[i]->children.push_back(loadedNodes[childIdx]);
                isChild[childIdx] = true;
            }
        }
    }

    if (!asset.scenes.empty()) {
        const size_t activeSceneIdx{
            (asset.defaultScene.has_value() && *asset.defaultScene < asset.scenes.size()) ? *asset.defaultScene : 0u};
        for (size_t rootNodeIdx : asset.scenes[activeSceneIdx].nodeIndices) {
            if (rootNodeIdx < loadedNodes.size() && loadedNodes[rootNodeIdx]) {
                scene.rootNodes.push_back(loadedNodes[rootNodeIdx]);
            }
        }
    }
    if (scene.rootNodes.empty()) {
        for (size_t i{0}; i < loadedNodes.size(); ++i) {
            if (!isChild[i] && loadedNodes[i]) {
                scene.rootNodes.push_back(loadedNodes[i]);
            }
        }
    }

    scene.allNodes = std::move(loadedNodes);
    scene.Update();
}

void ParseCamerasSkinsAndAnimations(const fastgltf::Asset& asset, Scene& scene) {
    ParseCameras(asset, scene);
    ParseSkins(asset, scene);
    ParseAnimations(asset, scene);
}

} // namespace bgl::scene::detail
