/**
 * @file FastGltfSceneBuilder.cpp
 * @brief glTF material, punctual light, instancing, hierarchy, camera, skin, and animation builders.
 */
#include "core/Log.hpp"
#include "core/ThreadPool.hpp"
#include "scene/FastGltfInternal.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <numeric>
#include <utility>

namespace bgl::scene::detail {

namespace {

constexpr size_t kMaxInstancesPerNode{1'000'000u};
constexpr size_t kMaxSkinJoints{4096u};
constexpr size_t kMaxAnimKeyframes{2'000'000u};

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

void ParseInstancingTransforms(const fastgltf::Asset& asset, const fastgltf::Node& gltfNode, SceneNode& node) {
    if (gltfNode.instancingAttributes.empty()) {
        return;
    }

    size_t instCount{0};
    auto transIt = gltfNode.findInstancingAttribute("TRANSLATION");
    auto rotIt = gltfNode.findInstancingAttribute("ROTATION");
    auto scaleIt = gltfNode.findInstancingAttribute("SCALE");

    const bool validTrans = (transIt != gltfNode.instancingAttributes.cend() &&
                             ValidateAccessor(asset, transIt->second, sizeof(float) * 3u, kMaxInstancesPerNode,
                                              fastgltf::AccessorType::Vec3));
    const bool validRot = (rotIt != gltfNode.instancingAttributes.cend() &&
                           ValidateAccessor(asset, rotIt->second, sizeof(uint8_t) * 4u, kMaxInstancesPerNode,
                                            fastgltf::AccessorType::Vec4));
    const bool validScale = (scaleIt != gltfNode.instancingAttributes.cend() &&
                             ValidateAccessor(asset, scaleIt->second, sizeof(float) * 3u, kMaxInstancesPerNode,
                                              fastgltf::AccessorType::Vec3));

    if (validTrans) {
        instCount = std::max(instCount, asset.accessors[transIt->second].count);
    }
    if (validRot) {
        instCount = std::max(instCount, asset.accessors[rotIt->second].count);
    }
    if (validScale) {
        instCount = std::max(instCount, asset.accessors[scaleIt->second].count);
    }
    if (instCount == 0 || instCount > kMaxInstancesPerNode) {
        return;
    }

    std::vector<glm::vec3> instT(instCount, glm::vec3{0.0f});
    std::vector<glm::quat> instR(instCount, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    std::vector<glm::vec3> instS(instCount, glm::vec3{1.0f});

    if (validTrans) {
        fastgltf::iterateAccessorWithIndex<glm::vec3>(
            asset, asset.accessors[transIt->second], [&](glm::vec3 t, size_t idx) {
                if (idx < instT.size() && std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z)) {
                    instT[idx] = t;
                }
            });
    }
    if (validRot) {
        fastgltf::iterateAccessorWithIndex<glm::vec4>(
            asset, asset.accessors[rotIt->second], [&](glm::vec4 q, size_t idx) {
                if (idx < instR.size() && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
                    std::isfinite(q.w)) {
                    instR[idx] = glm::quat{q.w, q.x, q.y, q.z};
                }
            });
    }
    if (validScale) {
        fastgltf::iterateAccessorWithIndex<glm::vec3>(
            asset, asset.accessors[scaleIt->second], [&](glm::vec3 s, size_t idx) {
                if (idx < instS.size() && std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z)) {
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
        skin.skeletonRootNodeIndex = (gltfSkin.skeleton.has_value() && *gltfSkin.skeleton < asset.nodes.size())
                                         ? static_cast<int>(*gltfSkin.skeleton)
                                         : -1;
        const size_t jointCount{std::min(gltfSkin.joints.size(), kMaxSkinJoints)};
        skin.joints.reserve(jointCount);
        for (size_t j = 0; j < jointCount; ++j) {
            const size_t jIdx = gltfSkin.joints[j];
            skin.joints.push_back((jIdx < asset.nodes.size()) ? static_cast<int>(jIdx) : -1);
        }

        if (gltfSkin.inverseBindMatrices.has_value() &&
            ValidateAccessor(asset, *gltfSkin.inverseBindMatrices, sizeof(float) * 16u, kMaxSkinJoints,
                             fastgltf::AccessorType::Mat4)) {
            const auto& ibmAccessor = asset.accessors[*gltfSkin.inverseBindMatrices];
            skin.inverseBindMatrices.reserve(std::min(ibmAccessor.count, jointCount));
            fastgltf::iterateAccessor<glm::mat4>(asset, ibmAccessor, [&](glm::mat4 ibm) {
                if (skin.inverseBindMatrices.size() < jointCount) {
                    const float* ptr = glm::value_ptr(ibm);
                    const bool finite = std::all_of(ptr, ptr + 16, [](float v) { return std::isfinite(v); });
                    skin.inverseBindMatrices.push_back(finite ? ibm : glm::mat4{1.0f});
                }
            });
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

            if (ValidateAccessor(asset, gltfSampler.inputAccessor, sizeof(float), kMaxAnimKeyframes,
                                 fastgltf::AccessorType::Scalar)) {
                const auto& inputAcc = asset.accessors[gltfSampler.inputAccessor];
                sampler.timestamps.reserve(inputAcc.count);
                float lastT{0.0f};
                fastgltf::iterateAccessor<float>(asset, inputAcc, [&](float t) {
                    const float finiteT = std::isfinite(t) ? std::max(0.0f, t) : lastT;
                    lastT = sampler.timestamps.empty() ? finiteT : std::max(lastT, finiteT);
                    sampler.timestamps.push_back(lastT);
                });
            }
            if (ValidateAccessor(asset, gltfSampler.outputAccessor, sizeof(uint8_t), kMaxAnimKeyframes * 12u)) {
                const auto& outputAcc = asset.accessors[gltfSampler.outputAccessor];
                auto pushFinite = [&](float v) { sampler.keyframeValues.push_back(std::isfinite(v) ? v : 0.0f); };
                if (outputAcc.type == fastgltf::AccessorType::Scalar) {
                    sampler.keyframeValues.reserve(std::min(outputAcc.count, kMaxAnimKeyframes * 12u));
                    fastgltf::iterateAccessor<float>(asset, outputAcc, pushFinite);
                } else if (outputAcc.type == fastgltf::AccessorType::Vec2) {
                    sampler.keyframeValues.reserve(std::min(outputAcc.count * 2u, kMaxAnimKeyframes * 12u));
                    fastgltf::iterateAccessor<glm::vec2>(asset, outputAcc, [&](glm::vec2 v) {
                        pushFinite(v.x);
                        pushFinite(v.y);
                    });
                } else if (outputAcc.type == fastgltf::AccessorType::Vec3) {
                    sampler.keyframeValues.reserve(std::min(outputAcc.count * 3u, kMaxAnimKeyframes * 12u));
                    fastgltf::iterateAccessor<glm::vec3>(asset, outputAcc, [&](glm::vec3 v) {
                        pushFinite(v.x);
                        pushFinite(v.y);
                        pushFinite(v.z);
                    });
                } else if (outputAcc.type == fastgltf::AccessorType::Vec4) {
                    sampler.keyframeValues.reserve(std::min(outputAcc.count * 4u, kMaxAnimKeyframes * 12u));
                    fastgltf::iterateAccessor<glm::vec4>(asset, outputAcc, [&](glm::vec4 v) {
                        pushFinite(v.x);
                        pushFinite(v.y);
                        pushFinite(v.z);
                        pushFinite(v.w);
                    });
                }
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
                     scene.animations.size(), core::SanitizeForLog(scene.animations[0].name),
                     scene.animations[0].duration);
    }
}

} // namespace

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
            mIdx, core::SanitizeForLog(sceneMat.name), sceneMat.baseColorFactor.r, sceneMat.baseColorFactor.g,
            sceneMat.baseColorFactor.b, sceneMat.baseColorFactor.a, sceneMat.metallicFactor, sceneMat.roughnessFactor,
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
        BGL_LOG_DEBUG("  * Mesh #{}: '{}' ({} primitives)", mIdx, core::SanitizeForLog(meshName),
                      mesh.primitives.size());

        for (size_t pIdx = 0; pIdx < mesh.primitives.size(); ++pIdx) {
            const auto& prim = mesh.primitives[pIdx];
            const int matIdx = prim.materialIndex.has_value() ? static_cast<int>(*prim.materialIndex) : -1;
            const std::string matName = (matIdx >= 0 && matIdx < static_cast<int>(scene.materials.size()))
                                            ? scene.materials[matIdx].name
                                            : "Default";

            size_t vertCount = 0;
            if (auto posIt = prim.findAttribute("POSITION");
                posIt != prim.attributes.end() && posIt->second < asset.accessors.size()) {
                vertCount = asset.accessors[posIt->second].count;
            }
            const size_t idxCount = (prim.indicesAccessor.has_value() && *prim.indicesAccessor < asset.accessors.size())
                                        ? asset.accessors[*prim.indicesAccessor].count
                                        : 0;
            const size_t triCount = idxCount > 0 ? (idxCount / 3) : (vertCount / 3);

            BGL_LOG_DEBUG("    - Prim #{}: Material #{} ('{}') | {} vertices, {} indices ({} tris)", pIdx, matIdx,
                          core::SanitizeForLog(matName), vertCount, idxCount, triCount);

            for (const auto& [attrName, accIdx] : prim.attributes) {
                if (accIdx >= asset.accessors.size()) {
                    continue;
                }
                const auto& acc = asset.accessors[accIdx];
                BGL_LOG_DEBUG("      + Attr {}: Acc #{}, {} {}, count={}", core::SanitizeForLog(attrName), accIdx,
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
                     core::SanitizeForLog(pLight.name),
                     pLight.type == LightType::Directional ? "Directional"
                                                           : (pLight.type == LightType::Point ? "Point" : "Spot"),
                     pLight.color.r, pLight.color.g, pLight.color.b, pLight.intensity, pLight.range);
        scene.lights.push_back(std::move(pLight));
    }
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
            node->translation = glm::vec3(std::isfinite(trs.translation[0]) ? trs.translation[0] : 0.0f,
                                          std::isfinite(trs.translation[1]) ? trs.translation[1] : 0.0f,
                                          std::isfinite(trs.translation[2]) ? trs.translation[2] : 0.0f);
            const glm::quat rawQ(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
            const float qLen = glm::length(rawQ);
            node->rotation = (std::isfinite(qLen) && qLen > 1e-6f) ? (rawQ / qLen) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            node->scale = glm::vec3(std::isfinite(trs.scale[0]) ? trs.scale[0] : 1.0f,
                                    std::isfinite(trs.scale[1]) ? trs.scale[1] : 1.0f,
                                    std::isfinite(trs.scale[2]) ? trs.scale[2] : 1.0f);
        } else if (std::holds_alternative<fastgltf::Node::TransformMatrix>(gltfNode.transform)) {
            const auto& mat = std::get<fastgltf::Node::TransformMatrix>(gltfNode.transform);
            glm::mat4 m = glm::make_mat4(mat.data());
            const float* mPtr = glm::value_ptr(m);
            if (!std::all_of(mPtr, mPtr + 16, [](float v) { return std::isfinite(v); })) {
                m = glm::mat4{1.0f};
            }
            node->translation = glm::vec3(m[3]);
            node->scale =
                glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            const glm::mat3 rotM(node->scale.x > 1e-6f ? glm::vec3(m[0]) / node->scale.x : glm::vec3(1, 0, 0),
                                 node->scale.y > 1e-6f ? glm::vec3(m[1]) / node->scale.y : glm::vec3(0, 1, 0),
                                 node->scale.z > 1e-6f ? glm::vec3(m[2]) / node->scale.z : glm::vec3(0, 0, 1));
            const glm::quat rawQ = glm::quat_cast(rotM);
            const float qLen = glm::length(rawQ);
            node->rotation = (std::isfinite(qLen) && qLen > 1e-6f) ? (rawQ / qLen) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
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
    std::vector<size_t> ufParent(asset.nodes.size());
    std::iota(ufParent.begin(), ufParent.end(), size_t{0});
    auto findRoot = [&ufParent](size_t x) noexcept {
        while (ufParent[x] != x) {
            ufParent[x] = ufParent[ufParent[x]];
            x = ufParent[x];
        }
        return x;
    };
    for (size_t i{0}; i < asset.nodes.size(); ++i) {
        if (!loadedNodes[i]) {
            continue;
        }
        for (size_t childIdx : asset.nodes[i].children) {
            if (childIdx >= loadedNodes.size() || !loadedNodes[childIdx] || childIdx == i || isChild[childIdx]) {
                continue;
            }
            const size_t rootParent{findRoot(i)};
            const size_t rootChild{findRoot(childIdx)};
            if (rootParent == rootChild) {
                BGL_LOG_WARN("[glTF Loader] Rejected cyclic node edge {} -> {}", i, childIdx);
                continue;
            }
            ufParent[rootChild] = rootParent;
            loadedNodes[i]->children.push_back(loadedNodes[childIdx]);
            isChild[childIdx] = true;
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
