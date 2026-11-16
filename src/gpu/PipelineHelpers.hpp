/**
 * @file PipelineHelpers.hpp
 * @brief Internal helper functions shared across PipelineManager translation units.
 */
#pragma once

#include "gpu/Uniforms.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <utility>
#include <vector>

namespace bgl::gpu::detail {

[[nodiscard]] inline uint16_t FloatToHalfBits(float val) noexcept {
    if (std::isnan(val))
        return 0x7E00u;
    if (std::isinf(val))
        return (val < 0.0f) ? 0xFC00u : 0x7C00u;
    val = std::clamp(val, -65504.0f, 65504.0f);
    const uint32_t f32{std::bit_cast<uint32_t>(val)};
    const uint32_t sign{(f32 >> 16u) & 0x8000u};
    const int32_t exp{static_cast<int32_t>((f32 >> 23u) & 0xFFu) - 127 + 15};
    uint32_t mant{f32 & 0x007FFFFFu};

    if (exp <= 0) {
        if (exp < -10)
            return static_cast<uint16_t>(sign);
        mant = (mant | 0x00800000u) >> static_cast<uint32_t>(1 - exp);
        return static_cast<uint16_t>(sign | (mant >> 13u));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10u) | (mant >> 13u));
}

[[nodiscard]] inline std::vector<uint16_t> PackRgba32FloatToRgba16Float(std::span<const float> rgba32f) {
    std::vector<uint16_t> out(rgba32f.size());
    for (size_t i{0}; i < rgba32f.size(); ++i) {
        out[i] = FloatToHalfBits(rgba32f[i]);
    }
    return out;
}

[[nodiscard]] inline glm::vec3 CubeFaceDirection(uint32_t faceIndex, float u, float v) noexcept {
    glm::vec3 dir{0.0f};
    switch (faceIndex) {
    case 0:
        dir = glm::vec3{1.0f, -v, -u};
        break; // +X
    case 1:
        dir = glm::vec3{-1.0f, -v, u};
        break; // -X
    case 2:
        dir = glm::vec3{u, 1.0f, v};
        break; // +Y
    case 3:
        dir = glm::vec3{u, -1.0f, -v};
        break; // -Y
    case 4:
        dir = glm::vec3{u, -v, 1.0f};
        break; // +Z
    default:
        dir = glm::vec3{-u, -v, -1.0f};
        break; // -Z
    }
    return glm::normalize(dir);
}

inline void CollectUniquePrimitives(scene::Scene& scene, std::vector<scene::MeshPrimitive*>& outPrims) {
    outPrims.clear();
    for (auto& mesh : scene.meshes) {
        if (mesh) {
            for (auto& prim : mesh->primitives) {
                outPrims.push_back(&prim);
            }
        }
    }
    auto collectNode = [&](auto& self, scene::SceneNode& node) -> void {
        for (auto& prim : node.GetPrimitives()) {
            outPrims.push_back(&prim);
        }
        for (auto& child : node.children) {
            if (child) {
                self(self, *child);
            }
        }
    };
    for (auto& root : scene.rootNodes) {
        if (root) {
            collectNode(collectNode, *root);
        }
    }
    std::sort(outPrims.begin(), outPrims.end());
    outPrims.erase(std::unique(outPrims.begin(), outPrims.end()), outPrims.end());
}

inline void FillMaterialUniforms(const scene::Material* mat, MaterialUniforms& out) noexcept {
    out = MaterialUniforms{};
    if (!mat) {
        return;
    }
    out.baseColorFactor = mat->baseColorFactor;
    out.emissiveFactor = glm::vec4{mat->emissiveFactor, mat->emissiveStrength};
    out.sheenColorFactor = glm::vec4{mat->sheenColorFactor, mat->sheenRoughnessFactor};
    out.specularColorFactor = glm::vec4{mat->specularColorFactor, 0.0f};
    out.attenuationColor = glm::vec4{mat->attenuationColor, 1.0f};
    out.metallicFactor = mat->metallicFactor;
    out.roughnessFactor = mat->roughnessFactor;
    out.normalScale = mat->normalScale;
    out.occlusionStrength = mat->occlusionStrength;
    out.bumpScale = mat->bumpScale;
    out.alphaCutoff = mat->alphaCutoff;
    out.alphaMode = std::to_underlying(mat->alphaMode);
    out.ior = mat->ior;
    out.clearcoatFactor = mat->clearcoatFactor;
    out.clearcoatRoughness = mat->clearcoatRoughnessFactor;
    out.transmissionFactor = mat->transmissionFactor;
    out.specularFactor = mat->specularFactor;
    out.thicknessFactor = mat->thicknessFactor;
    out.attenuationDistance = mat->attenuationDistance;

    out.baseColorUvTransform = glm::vec4{mat->baseColorUvTransform.scale, mat->baseColorUvTransform.offset};
    out.metallicRoughnessUvTransform =
        glm::vec4{mat->metallicRoughnessUvTransform.scale, mat->metallicRoughnessUvTransform.offset};
    out.normalUvTransform = glm::vec4{mat->normalUvTransform.scale, mat->normalUvTransform.offset};
    out.occlusionUvTransform = glm::vec4{mat->occlusionUvTransform.scale, mat->occlusionUvTransform.offset};
    out.emissiveUvTransform = glm::vec4{mat->emissiveUvTransform.scale, mat->emissiveUvTransform.offset};
    out.uvRotations0 = glm::vec4{mat->baseColorUvTransform.rotation, mat->metallicRoughnessUvTransform.rotation,
                                 mat->normalUvTransform.rotation, mat->occlusionUvTransform.rotation};
    out.uvRotations1 = glm::vec4{mat->emissiveUvTransform.rotation, 0.0f, 0.0f, 0.0f};

    uint32_t flags{0};
    if (mat->baseColorTextureIndex >= 0)
        flags |= MaterialFlags::HasBaseColorTex;
    if (mat->metallicRoughnessTextureIndex >= 0)
        flags |= MaterialFlags::HasMetallicRoughnessTex;
    if (mat->normalTextureIndex >= 0)
        flags |= MaterialFlags::HasNormalTex;
    if (mat->emissiveTextureIndex >= 0)
        flags |= MaterialFlags::HasEmissiveTex;
    if (mat->occlusionTextureIndex >= 0)
        flags |= MaterialFlags::HasOcclusionTex;
    if (mat->clearcoatTextureIndex >= 0)
        flags |= MaterialFlags::HasClearcoatTex;
    if (mat->transmissionTextureIndex >= 0)
        flags |= MaterialFlags::HasTransmissionTex;
    if (mat->thicknessTextureIndex >= 0)
        flags |= MaterialFlags::HasThicknessTex;
    if (mat->sheenColorTextureIndex >= 0)
        flags |= MaterialFlags::HasSheenTex;
    if (mat->unlit)
        flags |= MaterialFlags::Unlit;
    if (mat->doubleSided)
        flags |= MaterialFlags::DoubleSided;
    out.materialFlags = flags;
}

} // namespace bgl::gpu::detail
