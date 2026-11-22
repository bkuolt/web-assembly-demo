/**
 * @file PipelineRender.cpp
 * @brief Clustered Forward+ light culling, shadow/opaque/transparent frame preparation, and draw dispatch encoding.
 */
#include "core/Log.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/PipelineHelpers.hpp"
#include "scene/Quadtree.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>

namespace bgl::gpu {
using namespace detail;

namespace {

void PackSpotLightCone(const scene::PunctualLight& srcLight, const glm::vec3& targetDir, GpuPunctualLight& dstLight,
                       glm::vec4& outSpotAngles) noexcept {
    dstLight.colorInt = glm::vec4{srcLight.color * srcLight.intensity, srcLight.range};
    dstLight.spotParams = glm::vec4{targetDir, 0.0f};
    const float cosInner{std::cos(srcLight.innerConeAngle)};
    const float cosOuter{std::cos(srcLight.outerConeAngle)};
    const float angleScale{1.0f / std::max(0.001f, cosInner - cosOuter)};
    const float angleOffset{-cosOuter * angleScale};
    outSpotAngles =
        glm::vec4{angleScale, angleOffset, (srcLight.type == scene::LightType::Directional) ? 1.0f : 0.0f, 0.0f};
}

} // namespace

glm::mat4 PipelineManager::ComputeDirectionalLightMatrix(const core::AABB& sceneBounds,
                                                         const glm::vec3& lightDir) noexcept {
    return ShadowPass::ComputeDirectionalLightMatrix(sceneBounds, lightDir);
}

void PipelineManager::BuildClusteredLightGrid(std::span<const GpuPunctualLight> lights, uint32_t lightCount,
                                              const glm::mat4& viewMatrix, const glm::mat4& projMatrix, float zNear,
                                              float zFar, std::array<glm::uvec4, 256>& outClusterMasks) noexcept {
    outClusterMasks.fill(glm::uvec4{0u});
    const uint32_t count{std::min<uint32_t>({lightCount, static_cast<uint32_t>(lights.size()), 32u})};
    if (count == 0) {
        return;
    }

    const float safeNear{std::max(zNear, 0.05f)};
    const float safeFar{std::max(zFar, safeNear + 1.0f)};
    const float logRatio{std::log(safeFar / safeNear)};

    const float p00{(std::abs(projMatrix[0][0]) > 1e-5f) ? projMatrix[0][0] : 1.0f};
    const float p11{(std::abs(projMatrix[1][1]) > 1e-5f) ? projMatrix[1][1] : 1.0f};

    auto setClusterBit = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t bitMask) {
        const uint32_t clusterIdx{(z * kClusterCountY + y) * kClusterCountX + x};
        const uint32_t vecIdx{clusterIdx >> 2u};
        const uint32_t compIdx{clusterIdx & 3u};
        outClusterMasks[vecIdx][static_cast<glm::length_t>(compIdx)] |= bitMask;
    };

    for (uint32_t li{0}; li < count; ++li) {
        const auto& light = lights[li];
        const uint32_t lightType{static_cast<uint32_t>(std::lround(light.posOrDir.w))};
        const uint32_t bitMask{1u << li};

        if (lightType == 0u) {
            for (auto& vecMask : outClusterMasks) {
                vecMask = vecMask | glm::uvec4{bitMask};
            }
            continue;
        }

        const glm::vec3 viewPos{glm::vec3{viewMatrix * glm::vec4{glm::vec3{light.posOrDir}, 1.0f}}};
        const float intensityMag{glm::length(glm::vec3{light.colorInt})};
        const float radius{(light.colorInt.w > 0.0f) ? light.colorInt.w
                                                     : std::clamp(std::sqrt(intensityMag * 25.0f), 2.0f, 100.0f)};

        const float minZ{-viewPos.z - radius};
        const float maxZ{-viewPos.z + radius};
        if (maxZ < safeNear || minZ > safeFar) {
            continue;
        }

        const float normMinZ{std::clamp(std::log(std::max(minZ, safeNear) / safeNear) / logRatio, 0.0f, 0.9999f)};
        const float normMaxZ{
            std::clamp(std::log(std::clamp(maxZ, safeNear, safeFar) / safeNear) / logRatio, 0.0f, 0.9999f)};
        const uint32_t sliceMin{static_cast<uint32_t>(normMinZ * static_cast<float>(kClusterCountZ))};
        const uint32_t sliceMax{
            std::min(kClusterCountZ - 1u, static_cast<uint32_t>(normMaxZ * static_cast<float>(kClusterCountZ)))};

        uint32_t tileMinX{0};
        uint32_t tileMaxX{kClusterCountX - 1u};
        uint32_t tileMinY{0};
        uint32_t tileMaxY{kClusterCountY - 1u};

        const float closestDepth{std::max(-viewPos.z - radius, safeNear)};
        if (-viewPos.z > radius) {
            const float halfAngleX{radius * p00 / closestDepth};
            const float halfAngleY{radius * p11 / closestDepth};
            const float centerNdcX{(viewPos.x * p00) / (-viewPos.z)};
            const float centerNdcY{(viewPos.y * p11) / (-viewPos.z)};

            const float minScreenX{std::clamp((centerNdcX - halfAngleX) * 0.5f + 0.5f, 0.0f, 0.9999f)};
            const float maxScreenX{std::clamp((centerNdcX + halfAngleX) * 0.5f + 0.5f, 0.0f, 0.9999f)};
            const float minScreenY{std::clamp((0.5f - (centerNdcY + halfAngleY) * 0.5f), 0.0f, 0.9999f)};
            const float maxScreenY{std::clamp((0.5f - (centerNdcY - halfAngleY) * 0.5f), 0.0f, 0.9999f)};

            if (centerNdcX + halfAngleX < -1.0f || centerNdcX - halfAngleX > 1.0f || centerNdcY + halfAngleY < -1.0f ||
                centerNdcY - halfAngleY > 1.0f) {
                continue;
            }

            tileMinX = static_cast<uint32_t>(minScreenX * static_cast<float>(kClusterCountX));
            tileMaxX =
                std::min(kClusterCountX - 1u, static_cast<uint32_t>(maxScreenX * static_cast<float>(kClusterCountX)));
            tileMinY = static_cast<uint32_t>(minScreenY * static_cast<float>(kClusterCountY));
            tileMaxY =
                std::min(kClusterCountY - 1u, static_cast<uint32_t>(maxScreenY * static_cast<float>(kClusterCountY)));
        }

        for (uint32_t z{sliceMin}; z <= sliceMax; ++z) {
            for (uint32_t y{tileMinY}; y <= tileMaxY; ++y) {
                for (uint32_t x{tileMinX}; x <= tileMaxX; ++x) {
                    setClusterBit(x, y, z, bitMask);
                }
            }
        }
    }
}

void PipelineManager::PreparePostProcessPass(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView,
                                             WGPUTextureView depthReadView, uint32_t sampleCount,
                                             const glm::mat4& viewProj, const glm::mat4& projMatrix,
                                             const glm::vec3& cameraPos, const glm::vec3& lightDir, float envRotation,
                                             uint32_t viewportW, uint32_t viewportH, float deltaTime,
                                             float elapsedTime) {
    postProcessPass_.Prepare(encoder, hdrResolveView, depthReadView, sampleCount, viewProj, projMatrix, cameraPos,
                             lightDir, envRotation, viewportW, viewportH, deltaTime, elapsedTime);
}

void PipelineManager::RenderTonemapPass(WGPURenderPassEncoder swapchainPass, WGPUTextureView hdrResolveView,
                                        const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                        const glm::vec3& lightDir, float envRotation, uint32_t viewportW,
                                        uint32_t viewportH) {
    postProcessPass_.Render(swapchainPass, hdrResolveView, viewProj, cameraPos, lightDir, envRotation, viewportW,
                            viewportH);
}

void PipelineManager::UpdateSceneLightingUniforms(scene::Scene& scene, const glm::mat4& viewMatrix,
                                                  const glm::mat4& projMatrix, const glm::vec3& cameraPos,
                                                  uint32_t viewportW, uint32_t viewportH, float zNear, float zFar) {
    if (!sceneLightingBuffer_) {
        return;
    }

    SceneLightingUniforms sl{};
    sl.shIrradiance = shIrradiance_;
    sl.viewMatrix = viewMatrix;
    sl.gridParams = glm::vec4{static_cast<float>(std::max(1u, viewportW)), static_cast<float>(std::max(1u, viewportH)),
                              zNear, zFar};
    sl.cameraPos = glm::vec4{cameraPos, 1.0f};
    sl.lightDir = glm::vec4{currentLightDir_, solarElevation_};
    sl.renderParams = glm::vec4{static_cast<float>(envMipLevels_), 0.0015f, aoIntensity_,
                                static_cast<float>(std::max(1u, opaqueCopyMipCount_) - 1u)};

    uint32_t lightCount{0};
    bool hasExplicitDirectional{false};

    auto collectNodeLights = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform) -> void {
        const glm::mat4 worldMat{parentTransform * node.localTransform};
        if (node.lightIndex >= 0 && static_cast<size_t>(node.lightIndex) < scene.lights.size() && lightCount < 16u) {
            const auto& srcLight = scene.lights[static_cast<size_t>(node.lightIndex)];
            auto& dstLight = sl.lights[lightCount];

            const glm::vec3 worldPos{worldMat[3]};
            // In glTF KHR_lights_punctual, directional and spot lights shine along the node's local -Z axis
            const glm::vec3 rawDir{glm::vec3{worldMat * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}}};
            const glm::vec3 lightToTargetDir{glm::length(rawDir) > 1e-5f ? glm::normalize(rawDir)
                                                                         : glm::vec3{0.0f, -1.0f, 0.0f}};

            if (srcLight.type == scene::LightType::Directional) {
                dstLight.posOrDir = glm::vec4{-lightToTargetDir, 0.0f};
                hasExplicitDirectional = true;
            } else {
                dstLight.posOrDir = glm::vec4{worldPos, (srcLight.type == scene::LightType::Point) ? 1.0f : 2.0f};
            }
            PackSpotLightCone(srcLight, lightToTargetDir, dstLight, sl.spotAngles[lightCount]);
            ++lightCount;
        }

        for (const auto& child : node.children) {
            if (child) {
                self(self, *child, worldMat);
            }
        }
    };

    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            collectNodeLights(collectNodeLights, *rootNode, glm::mat4{1.0f});
        }
    }

    // Fallback for unattached scene.lights if none are referenced by nodes
    if (lightCount == 0 && !scene.lights.empty()) {
        for (const auto& srcLight : scene.lights) {
            if (lightCount >= 16u) {
                break;
            }
            auto& dstLight = sl.lights[lightCount];
            if (srcLight.type == scene::LightType::Directional) {
                dstLight.posOrDir = glm::vec4{currentLightDir_, 0.0f};
                hasExplicitDirectional = true;
            } else {
                dstLight.posOrDir =
                    glm::vec4{0.0f, 2.0f, 0.0f, (srcLight.type == scene::LightType::Point) ? 1.0f : 2.0f};
            }
            PackSpotLightCone(srcLight, glm::vec3{0.0f, -1.0f, 0.0f}, dstLight, sl.spotAngles[lightCount]);
            sl.spotAngles[lightCount].z = 1.0f;
            ++lightCount;
        }
    }

    activePunctualLightCount_ = lightCount;
    BuildClusteredLightGrid(std::span<const GpuPunctualLight>{sl.lights.data(), lightCount}, lightCount, viewMatrix,
                            projMatrix, zNear, zFar, sl.clusterMasks);

    sl.envParams = glm::vec4{envRotation_, static_cast<float>(lightCount), 1.0f, hasExplicitDirectional ? 0.0f : 1.0f};
    sl.weatherParams = glm::vec4{static_cast<float>(weatherMode_), weatherIntensity_, windStrength_, weatherTimeSec_};

    wgpuQueueWriteBuffer(queue_, sceneLightingBuffer_, 0, &sl, sizeof(sl));
}

void PipelineManager::RenderShadowPass(WGPUCommandEncoder encoder, scene::Scene& scene, const glm::vec3& lightDir,
                                       const scene::Quadtree* spatialIndex) {
    ++skinFrameId_;
    skinFrameAdvanced_ = true;

    const glm::vec3 baseDir{glm::length(lightDir) > 1e-5f ? glm::normalize(lightDir)
                                                          : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
    // Rotate primary sun direction by -envRotation_ so shadows match rotated HDR environment
    const float c{std::cos(-envRotation_)};
    const float s{std::sin(-envRotation_)};
    currentLightDir_ =
        glm::normalize(glm::vec3{c * baseDir.x + s * baseDir.z, baseDir.y, -s * baseDir.x + c * baseDir.z});
    currentLightViewProj_ = ShadowPass::ComputeDirectionalLightMatrix(scene.sceneAABB, currentLightDir_);

    if (!shadowPass_.IsInitialized()) {
        return;
    }
    if (!shadowsEnabled_) {
        if (encoder) {
            shadowPass_.Clear(encoder);
        }
        return;
    }
    shadowPass_.Render(encoder, scene, currentLightDir_, spatialIndex, skinFrameId_);
    currentLightViewProj_ = shadowPass_.GetLightViewProjMatrix();
}

void PipelineManager::RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj,
                                  const glm::vec3& cameraPos, bool wireframe, const scene::Quadtree* spatialIndex,
                                  const glm::mat4& viewMatrix, const glm::mat4& projMatrix, uint32_t viewportW,
                                  uint32_t viewportH, float zNear, float zFar) {
    if (!pass || !initialized_ || !sceneGlobalsBindGroup_) {
        return;
    }
    PrepareFrameScene(scene, viewProj, cameraPos, spatialIndex, viewMatrix, projMatrix, viewportW, viewportH, zNear,
                      zFar);
    EncodeDrawRange(pass, scene, viewProj, wireframe, false, 0, transparentStartIndex_, perDrawRing_);
    EncodeDrawRange(pass, scene, viewProj, wireframe, true, transparentStartIndex_, renderItemsScratch_.size(),
                    transparentPerDrawRing_);
}

void PipelineManager::RenderOpaqueScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj,
                                        const glm::vec3& cameraPos, bool wireframe, const scene::Quadtree* spatialIndex,
                                        const glm::mat4& viewMatrix, const glm::mat4& projMatrix, uint32_t viewportW,
                                        uint32_t viewportH, float zNear, float zFar) {
    if (!pass || !initialized_ || !sceneGlobalsBindGroup_) {
        return;
    }
    PrepareFrameScene(scene, viewProj, cameraPos, spatialIndex, viewMatrix, projMatrix, viewportW, viewportH, zNear,
                      zFar);
    EncodeDrawRange(pass, scene, viewProj, wireframe, false, 0, transparentStartIndex_, perDrawRing_);
}

void PipelineManager::RenderTransparentScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj,
                                             bool wireframe) {
    if (!pass || !initialized_ || !sceneGlobalsBindGroup_) {
        return;
    }
    EncodeDrawRange(pass, scene, viewProj, wireframe, true, transparentStartIndex_, renderItemsScratch_.size(),
                    transparentPerDrawRing_);
}

void PipelineManager::PrepareFrameScene(scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                        const scene::Quadtree* spatialIndex, const glm::mat4& viewMatrix,
                                        const glm::mat4& projMatrix, uint32_t viewportW, uint32_t viewportH,
                                        float zNear, float zFar) {
    if (!skinFrameAdvanced_) {
        ++skinFrameId_;
    }
    skinFrameAdvanced_ = false;

    aabbRenderer_.ResetFrameOffset();
    UpdateSceneLightingUniforms(scene, viewMatrix, projMatrix, cameraPos, viewportW, viewportH, zNear, zFar);

    const core::Frustum frustum{core::Frustum::FromViewProjection(viewProj)};
    lastVisiblePrimitiveCount_ = 0;
    lastOccludedPrimitiveCount_ = 0;
    lastRenderedTriangleCount_ = 0;
    lastTotalVisibleTriangleCount_ = 0;
    lastLodCounts_ = {0, 0, 0, 0};

    renderItemsScratch_.clear();

    if (spatialIndex) {
        spatialIndex->QueryVisible(frustum, cameraPos, viewProj, visibleItemsScratch_, occlusionCullingEnabled_,
                                   lodBias_);
        lastVisiblePrimitiveCount_ = visibleItemsScratch_.size();
        lastOccludedPrimitiveCount_ = spatialIndex->GetLastOccludedCount();

        renderItemsScratch_.reserve(visibleItemsScratch_.size());
        for (const auto& vis : visibleItemsScratch_) {
            if (!vis.node) {
                continue;
            }
            auto prims = vis.node->GetPrimitives();
            if (vis.primitiveIndex >= prims.size()) {
                continue;
            }
            const auto& prim = prims[vis.primitiveIndex];
            const bool hasMat =
                (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size()));
            const bool isTransparent =
                hasMat && IsMaterialTransparentOrTransmissive(scene.materials[prim.materialIndex]);
            const bool isDoubleSided = hasMat && scene.materials[prim.materialIndex].doubleSided;
            const uint32_t activeLod = lodEnabled_ ? static_cast<uint32_t>(vis.currentLOD) : 0u;
            renderItemsScratch_.push_back(RenderItem{
                .node = vis.node.get(),
                .primitiveIndex = vis.primitiveIndex,
                .worldTransform = vis.node->worldTransform,
                .distance = vis.distanceToCamera,
                .materialIndex = prim.materialIndex,
                .isTransparent = isTransparent,
                .isDoubleSided = isDoubleSided,
                .lodLevel = std::min(activeLod, 3u),
                .lodTransition = lodEnabled_ ? vis.lodTransition : 0.0f,
            });
        }
    } else {
        auto collectNode = [&](auto& self, const std::shared_ptr<scene::SceneNode>& node,
                               const glm::mat4& parentMat) -> void {
            if (!node) {
                return;
            }
            const glm::mat4 worldMat{parentMat * node->localTransform};
            if (node->worldAABB.minBound.x <= node->worldAABB.maxBound.x && !frustum.IntersectsAABB(node->worldAABB)) {
                return;
            }
            auto prims = node->GetPrimitives();
            for (size_t pIdx = 0; pIdx < prims.size(); ++pIdx) {
                const auto& prim = prims[pIdx];
                const core::AABB worldBox =
                    !node->instanceTransforms.empty() ? node->worldAABB : prim.boundingBox.Transformed(worldMat);
                const float dist = glm::length(worldBox.Center() - cameraPos);
                const float radius = std::max(0.001f, glm::length(worldBox.Extents()));
                const float metric = (dist / radius) * lodBias_;
                uint32_t lodLvl = 0u;
                float lodTrans = 0.0f;
                if (lodEnabled_) {
                    const auto eval = scene::Quadtree::EvaluateLodForMetric(metric);
                    lodLvl = std::min(static_cast<uint32_t>(eval.level), 3u);
                    lodTrans = eval.transition;
                }
                const bool hasMat =
                    (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size()));
                const bool isTransparent =
                    hasMat && IsMaterialTransparentOrTransmissive(scene.materials[prim.materialIndex]);
                const bool isDoubleSided = hasMat && scene.materials[prim.materialIndex].doubleSided;
                renderItemsScratch_.push_back(RenderItem{
                    .node = node.get(),
                    .primitiveIndex = pIdx,
                    .worldTransform = worldMat,
                    .distance = dist,
                    .materialIndex = prim.materialIndex,
                    .isTransparent = isTransparent,
                    .isDoubleSided = isDoubleSided,
                    .lodLevel = lodLvl,
                    .lodTransition = lodTrans,
                });
            }
            for (const auto& child : node->children) {
                self(self, child, worldMat);
            }
        };
        for (const auto& root : scene.rootNodes) {
            collectNode(collectNode, root, glm::mat4{1.0f});
        }
        lastVisiblePrimitiveCount_ = renderItemsScratch_.size();
    }

    // Opaques front-to-back (early-Z & material coherence), transparents back-to-front (painter's order)
    std::sort(renderItemsScratch_.begin(), renderItemsScratch_.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.isTransparent != b.isTransparent) {
            return !a.isTransparent;
        }
        if (!a.isTransparent) {
            if (a.materialIndex != b.materialIndex && std::abs(a.distance - b.distance) < 0.2f) {
                return a.materialIndex < b.materialIndex;
            }
            return a.distance < b.distance;
        }
        return a.distance > b.distance;
    });

    auto firstTransIt =
        std::ranges::find_if(renderItemsScratch_, [](const RenderItem& it) { return it.isTransparent; });
    transparentStartIndex_ = static_cast<size_t>(std::distance(renderItemsScratch_.begin(), firstTransIt));

    frameInstanceTransformsScratch_.clear();
    const bool hasInstanced = std::ranges::any_of(renderItemsScratch_, [](const RenderItem& item) {
        return item.node && !item.node->instanceTransforms.empty();
    });
    if (hasInstanced) {
        nodeInstanceStartScratch_.clear();
        for (auto& item : renderItemsScratch_) {
            if (!item.node || item.node->instanceTransforms.empty()) {
                item.instanceStart = 0;
                item.instanceCount = 1;
                continue;
            }
            const auto [it, inserted] = nodeInstanceStartScratch_.try_emplace(
                item.node, static_cast<uint32_t>(1u + frameInstanceTransformsScratch_.size()));
            if (inserted) {
                frameInstanceTransformsScratch_.insert(frameInstanceTransformsScratch_.end(),
                                                       item.node->instanceTransforms.begin(),
                                                       item.node->instanceTransforms.end());
            }
            item.instanceStart = it->second;
            item.instanceCount = static_cast<uint32_t>(item.node->instanceTransforms.size());
        }
    }
    instanceBuffer_.Upload(queue_, frameInstanceTransformsScratch_);
}

void PipelineManager::EncodeDrawRange(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj,
                                      bool wireframe, bool transparentPass, size_t startIdx, size_t endIdx,
                                      DynamicUniformRing<PerDrawUniforms>& ring) {
    if (!pass || startIdx >= endIdx || endIdx > renderItemsScratch_.size()) {
        return;
    }

    uint32_t baseGlobalFlags{0};
    if (bumpMappingEnabled_) {
        baseGlobalFlags |= MaterialFlags::EnableBumpMapping;
    }
    if (mipmappingEnabled_) {
        baseGlobalFlags |= MaterialFlags::EnableMipmapping;
    }
    if (shadowsEnabled_) {
        baseGlobalFlags |= MaterialFlags::EnableShadows;
    }
    if (reflectionsEnabled_) {
        baseGlobalFlags |= MaterialFlags::EnableReflections;
    }
    if (ambientOcclusionEnabled_) {
        baseGlobalFlags |= MaterialFlags::EnableAmbientOcclusion;
    }
    if (parallaxEnabled_) {
        baseGlobalFlags |= MaterialFlags::EnableParallaxOcclusion;
    }
    baseGlobalFlags |= ((static_cast<uint32_t>(debugViewMode_) & 7u) << MaterialFlags::DebugViewShift);

    drawCallsScratch_.clear();
    mainPerDrawScratch_.clear();
    drawCallsScratch_.reserve(endIdx - startIdx);
    mainPerDrawScratch_.reserve(endIdx - startIdx);

    WGPURenderPipeline singleSidedPipe =
        transparentPass ? (blendBackCulledPipeline_ ? blendBackCulledPipeline_.Get() : blendPipeline_.Get())
                        : (opaqueBackCulledPipeline_ ? opaqueBackCulledPipeline_.Get() : opaquePipeline_.Get());
    WGPURenderPipeline doubleSidedPipe = transparentPass ? blendPipeline_.Get() : opaquePipeline_.Get();

    for (size_t idx = startIdx; idx < endIdx; ++idx) {
        const auto& item = renderItemsScratch_[idx];
        if (!item.node) {
            continue;
        }
        auto prims = item.node->GetPrimitives();
        if (item.primitiveIndex >= prims.size()) {
            continue;
        }
        scene::MeshPrimitive& prim = prims[item.primitiveIndex];
        if (!prim.gpuVertexBuffer || !prim.gpuBindGroup) {
            continue;
        }

        const uint32_t clampedLod = lodEnabled_ ? std::min(item.lodLevel, 3u) : 0u;
        const float clampedTrans = lodEnabled_ ? std::clamp(item.lodTransition, 0.0f, 1.0f) : 0.0f;
        const uint32_t drawFlags = baseGlobalFlags | ((clampedLod & 3u) << MaterialFlags::LodLevelShift);

        PreparedDrawBindings prepared{};
        PopulatePerDrawMorphAndSkin(queue_, prim, item.node, item.worldTransform, viewProj, currentLightViewProj_,
                                    drawFlags, clampedTrans, scene.skins, scene.allNodes, skinFrameId_,
                                    dummySkinBindGroup_.Get(), prepared);

        WGPURenderPipeline desiredPipeline{singleSidedPipe};
        if (wireframe && wireframePipeline_) {
            desiredPipeline = wireframePipeline_.Get();
        } else if (doubleSidedPipe && item.isDoubleSided) {
            desiredPipeline = doubleSidedPipe;
        }

        const uint32_t dynOffset = static_cast<uint32_t>(mainPerDrawScratch_.size() * sizeof(PerDrawUniforms));
        mainPerDrawScratch_.push_back(prepared.uniforms);
        drawCallsScratch_.push_back(DrawDispatchItem{
            .prim = &prim,
            .skinBindGroup = prepared.skinBindGroup,
            .pipeline = desiredPipeline,
            .dynamicOffset = dynOffset,
            .clampedLod = clampedLod,
            .instanceStart = item.instanceStart,
            .instanceCount = item.instanceCount,
        });
    }

    WGPUBuffer instBuf{instanceBuffer_.GetBuffer()};
    if (drawCallsScratch_.empty() || !instBuf) {
        return;
    }

    WGPUBindGroup perDrawBg = ring.Upload(mainPerDrawScratch_);
    if (!perDrawBg) {
        return;
    }

    WGPURenderPipeline currentBoundPipeline{drawCallsScratch_[0].pipeline};
    if (currentBoundPipeline) {
        wgpuRenderPassEncoderSetPipeline(pass, currentBoundPipeline);
    }
    wgpuRenderPassEncoderSetBindGroup(pass, 0, sceneGlobalsBindGroup_.Get(), 0, nullptr);

    WGPUBindGroup boundMatBg{nullptr};
    WGPUBindGroup boundSkinBg{nullptr};
    WGPUBuffer boundVb{nullptr};
    WGPUBuffer boundIb{nullptr};
    uint32_t boundInstStart{std::numeric_limits<uint32_t>::max()};
    uint32_t boundInstCount{0};

    for (const auto& dc : drawCallsScratch_) {
        scene::MeshPrimitive& prim = *dc.prim;
        if (dc.pipeline && dc.pipeline != currentBoundPipeline) {
            wgpuRenderPassEncoderSetPipeline(pass, dc.pipeline);
            currentBoundPipeline = dc.pipeline;
        }

        wgpuRenderPassEncoderSetBindGroup(pass, 1, perDrawBg, 1, &dc.dynamicOffset);
        if (prim.gpuBindGroup != boundMatBg) {
            wgpuRenderPassEncoderSetBindGroup(pass, 2, prim.gpuBindGroup, 0, nullptr);
            boundMatBg = prim.gpuBindGroup;
        }
        if (dc.skinBindGroup != boundSkinBg) {
            wgpuRenderPassEncoderSetBindGroup(pass, 3, dc.skinBindGroup, 0, nullptr);
            boundSkinBg = dc.skinBindGroup;
        }
        if (prim.gpuVertexBuffer != boundVb) {
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, prim.gpuVertexBuffer, 0, prim.gpuVertexBufferSize);
            boundVb = prim.gpuVertexBuffer;
        }
        if (dc.instanceStart != boundInstStart || dc.instanceCount != boundInstCount) {
            wgpuRenderPassEncoderSetVertexBuffer(pass, 1, instBuf,
                                                 DynamicInstanceBuffer::GetByteOffset(dc.instanceStart),
                                                 DynamicInstanceBuffer::GetSliceSize(dc.instanceCount));
            boundInstStart = dc.instanceStart;
            boundInstCount = dc.instanceCount;
        }

        const uint32_t instCount{std::max(1u, dc.instanceCount)};
        const uint32_t fullTriCount = !prim.indices.empty() ? static_cast<uint32_t>(prim.indices.size() / 3)
                                                            : static_cast<uint32_t>(prim.vertices.size() / 3);
        lastTotalVisibleTriangleCount_ += static_cast<size_t>(fullTriCount) * instCount;
        ++lastLodCounts_[dc.clampedLod];

        if (wireframe && prim.gpuWireframeIndexBuffer && prim.wireframeIndexCount > 0) {
            if (prim.gpuWireframeIndexBuffer != boundIb) {
                wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuWireframeIndexBuffer, WGPUIndexFormat_Uint32, 0,
                                                    prim.wireframeIndexCount * sizeof(uint32_t));
                boundIb = prim.gpuWireframeIndexBuffer;
            }
            wgpuRenderPassEncoderDrawIndexed(pass, prim.wireframeIndexCount, instCount, 0, 0, 0);
            lastRenderedTriangleCount_ += static_cast<size_t>(fullTriCount) * instCount;
        } else if (prim.gpuIndexBuffer && !prim.indices.empty()) {
            const auto& slice =
                (prim.lodLevels[dc.clampedLod].indexCount > 0) ? prim.lodLevels[dc.clampedLod] : prim.lodLevels[0];
            const uint32_t firstIdx = (slice.indexCount > 0) ? slice.firstIndex : 0u;
            const uint32_t idxCount =
                (slice.indexCount > 0) ? slice.indexCount : static_cast<uint32_t>(prim.indices.size());
            if (prim.gpuIndexBuffer != boundIb) {
                wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuIndexBuffer, prim.indexFormat, 0,
                                                    prim.gpuIndexBufferSize);
                boundIb = prim.gpuIndexBuffer;
            }
            wgpuRenderPassEncoderDrawIndexed(pass, idxCount, instCount, firstIdx, 0, 0);
            lastRenderedTriangleCount_ += static_cast<size_t>(idxCount / 3) * instCount;
        } else {
            wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(prim.vertices.size()), instCount, 0, 0);
            lastRenderedTriangleCount_ += static_cast<size_t>(fullTriCount) * instCount;
        }
    }
}

void PipelineManager::AppendAABBLines(const core::AABB& aabb, const glm::vec4& color) {
    aabbRenderer_.AppendAABBLines(aabb, color);
}

void PipelineManager::FlushAABBLines(WGPURenderPassEncoder pass, const glm::mat4& viewProj) {
    aabbRenderer_.FlushAABBLines(pass, viewProj);
}

void PipelineManager::RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj,
                                 const glm::vec4& color) {
    aabbRenderer_.RenderAABB(pass, aabb, viewProj, color);
}

void PipelineManager::BatchSceneAABBs(const scene::Scene& scene, bool showSceneAABB, bool showPrimitiveAABBs) {
    aabbRenderer_.BatchSceneAABBs(scene, showSceneAABB, showPrimitiveAABBs);
}

void PipelineManager::BatchQuadtreeBounds(const scene::Quadtree& quadtree) {
    aabbRenderer_.BatchQuadtreeBounds(quadtree);
}

void PipelineManager::RenderSceneAABBs(WGPURenderPassEncoder pass, const scene::Scene& scene, const glm::mat4& viewProj,
                                       bool showSceneAABB, bool showPrimitiveAABBs) {
    aabbRenderer_.BatchSceneAABBs(scene, showSceneAABB, showPrimitiveAABBs);
    aabbRenderer_.FlushAABBLines(pass, viewProj);
}

void PipelineManager::RenderQuadtreeBounds(WGPURenderPassEncoder pass, const scene::Quadtree& quadtree,
                                           const glm::mat4& viewProj) {
    aabbRenderer_.BatchQuadtreeBounds(quadtree);
    aabbRenderer_.FlushAABBLines(pass, viewProj);
}

} // namespace bgl::gpu
