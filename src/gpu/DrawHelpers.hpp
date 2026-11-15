/**
 * @file DrawHelpers.hpp
 * @brief Shared helpers for populating per-draw uniforms, GPU skinning SSBOs, and morph weights across passes.
 */
#pragma once

#include "gpu/Uniforms.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <span>

namespace bgl::gpu {

struct PreparedDrawBindings {
    PerDrawUniforms uniforms{};
    WGPUBindGroup skinBindGroup{nullptr};
};

/**
 * @brief Populates per-draw transform/skinning/morph uniforms and uploads cached skin matrices when needed.
 */
inline void PopulatePerDrawMorphAndSkin(WGPUQueue queue, scene::MeshPrimitive& prim, const scene::SceneNode* node,
                                        const glm::mat4& worldMat, const glm::mat4& viewProj,
                                        const glm::mat4& lightViewProj, uint32_t baseDrawFlags, float lodTransition,
                                        std::span<const scene::Skin> skins,
                                        std::span<const std::shared_ptr<scene::SceneNode>> allNodes,
                                        uint64_t skinFrameId, WGPUBindGroup dummySkinBg,
                                        PreparedDrawBindings& out) noexcept {
    out.uniforms = PerDrawUniforms{};
    out.uniforms.viewProj = viewProj;
    out.uniforms.model = worldMat;
    out.uniforms.lightViewProj = lightViewProj;
    out.uniforms.drawParams = glm::vec4{lodTransition, 0.0f, 0.0f, 0.0f};
    uint32_t flags{baseDrawFlags};

    if (prim.morphDirty && !prim.gpuMorphDeltaBuffer && prim.gpuVertexBuffer && !prim.vertices.empty()) {
        wgpuQueueWriteBuffer(queue, prim.gpuVertexBuffer, 0, prim.vertices.data(),
                             prim.vertices.size() * sizeof(core::Vertex));
        prim.morphDirty = false;
    }

    if (node && prim.gpuMorphDeltaBuffer && prim.gpuMorphTargetCount > 0 && !node->morphWeights.empty()) {
        const uint32_t count{
            std::min<uint32_t>({static_cast<uint32_t>(node->morphWeights.size()), prim.gpuMorphTargetCount, 8u})};
        bool hasActiveMorph{false};
        for (uint32_t m{0}; m < count; ++m) {
            const float w{node->morphWeights[m]};
            if (m < 4u) {
                out.uniforms.morphWeights0[static_cast<glm::length_t>(m)] = w;
            } else {
                out.uniforms.morphWeights1[static_cast<glm::length_t>(m - 4u)] = w;
            }
            if (std::abs(w) > 1e-6f) {
                hasActiveMorph = true;
            }
        }
        if (hasActiveMorph) {
            out.uniforms.morphTargetCount = count;
            out.uniforms.vertexCount = prim.gpuMorphVertexCount;
            flags |= MaterialFlags::HasMorphTargets;
        }
    }

    out.skinBindGroup = dummySkinBg;
    if (node && node->skinIndex >= 0 && static_cast<size_t>(node->skinIndex) < skins.size()) {
        const auto& skin = skins[static_cast<size_t>(node->skinIndex)];
        if (!skin.joints.empty() && skin.gpuJointBuffer && skin.gpuBindGroup) {
            const auto jointsSpan = skin.GetOrComputeJointMatrices(skinFrameId, node, worldMat, allNodes);
            if (!jointsSpan.empty()) {
                if (skin.NeedsGpuUpload(skinFrameId, node)) {
                    wgpuQueueWriteBuffer(queue, skin.gpuJointBuffer, 0, jointsSpan.data(),
                                         jointsSpan.size() * sizeof(glm::mat4));
                    skin.MarkGpuUploaded(skinFrameId, node);
                }
                out.uniforms.jointCount = static_cast<uint32_t>(jointsSpan.size());
                out.skinBindGroup = skin.gpuBindGroup;
                flags |= MaterialFlags::HasSkinning;
            }
        }
    }

    out.uniforms.drawFlags = flags;
}

} // namespace bgl::gpu
