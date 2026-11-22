/**
 * @file RayTracingSceneBvh.cpp
 * @brief Binned-SAH BVH2 builders (BLAS + TLAS) and CPU ray traversal for RayTracingScene.
 */
#include "core/Types.hpp"
#include "gpu/RayTracingScene.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace bgl::gpu {

namespace {

constexpr uint32_t kSahBins = 12u;
constexpr uint32_t kMaxLeafTris = 4u;
constexpr uint32_t kMaxLeafInstances = 2u;
constexpr uint32_t kMaxBvhDepth = 30u;

struct TriBuildRef {
    uint32_t origTriIdx{0};
    core::AABB bounds{};
    glm::vec3 centroid{0.0f};
};

struct InstBuildRef {
    uint32_t instIdx{0};
    core::AABB bounds{};
    glm::vec3 centroid{0.0f};
};

struct SahSplit {
    int axis{-1};
    float splitPos{0.0f};
    float cost{std::numeric_limits<float>::infinity()};
};

[[nodiscard]] inline float SurfaceArea(const core::AABB& b) noexcept {
    if (b.minBound.x > b.maxBound.x) {
        return 0.0f;
    }
    const glm::vec3 e = glm::max(b.maxBound - b.minBound, glm::vec3{0.0f});
    return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
}

template <typename RefT>
[[nodiscard]] SahSplit FindBestSahSplit(std::span<const RefT> refs, uint32_t start, uint32_t count,
                                        const core::AABB& nodeBounds) noexcept {
    SahSplit best{};
    const float parentArea = SurfaceArea(nodeBounds);
    if (parentArea <= 1e-12f || count <= 1u) {
        return best;
    }

    core::AABB centroidBounds{};
    for (uint32_t i = 0; i < count; ++i) {
        centroidBounds.Expand(refs[start + i].centroid);
    }

    struct Bin {
        core::AABB bounds{};
        uint32_t count{0};
    };

    for (int axis = 0; axis < 3; ++axis) {
        const float minC = centroidBounds.minBound[axis];
        const float extent = centroidBounds.maxBound[axis] - minC;
        if (extent <= 1e-6f) {
            continue;
        }

        std::array<Bin, kSahBins> bins{};
        const float scale = static_cast<float>(kSahBins) / extent;
        for (uint32_t i = 0; i < count; ++i) {
            const float c = refs[start + i].centroid[axis];
            const uint32_t bIdx = std::min(kSahBins - 1u, static_cast<uint32_t>((c - minC) * scale));
            bins[bIdx].count += 1u;
            bins[bIdx].bounds.Expand(refs[start + i].bounds);
        }

        std::array<float, kSahBins - 1u> leftArea{};
        std::array<uint32_t, kSahBins - 1u> leftCount{};
        core::AABB accumL{};
        uint32_t countL = 0u;
        for (uint32_t b = 0; b + 1u < kSahBins; ++b) {
            accumL.Expand(bins[b].bounds);
            countL += bins[b].count;
            leftArea[b] = SurfaceArea(accumL);
            leftCount[b] = countL;
        }

        core::AABB accumR{};
        uint32_t countR = 0u;
        for (int b = static_cast<int>(kSahBins) - 2; b >= 0; --b) {
            const uint32_t ub = static_cast<uint32_t>(b);
            accumR.Expand(bins[ub + 1u].bounds);
            countR += bins[ub + 1u].count;
            if (leftCount[ub] == 0u || countR == 0u) {
                continue;
            }
            const float cost = 1.0f + (leftArea[ub] * static_cast<float>(leftCount[ub]) +
                                       SurfaceArea(accumR) * static_cast<float>(countR)) /
                                          parentArea;
            if (cost < best.cost) {
                best.cost = cost;
                best.axis = axis;
                best.splitPos = minC + (static_cast<float>(ub + 1u) / static_cast<float>(kSahBins)) * extent;
            }
        }
    }
    return best;
}

template <typename RefT>
[[nodiscard]] uint32_t PartitionSahOrMedian(std::span<RefT> refs, uint32_t start, uint32_t count,
                                            const core::AABB& nodeBounds) {
    const SahSplit split = FindBestSahSplit<RefT>(refs, start, count, nodeBounds);
    if (split.axis >= 0) {
        auto beginIt = refs.begin() + start;
        auto splitIt = std::partition(beginIt, beginIt + count,
                                      [&](const RefT& r) { return r.centroid[split.axis] < split.splitPos; });
        const uint32_t splitIdx = static_cast<uint32_t>(splitIt - refs.begin());
        if (splitIdx > start && splitIdx < start + count) {
            return splitIdx;
        }
    }

    const glm::vec3 size = nodeBounds.Size();
    int axis = 0;
    if (size.y > size.x && size.y >= size.z) {
        axis = 1;
    } else if (size.z > size.x && size.z >= size.y) {
        axis = 2;
    }
    const uint32_t mid = start + count / 2u;
    auto beginIt = refs.begin() + start;
    std::nth_element(beginIt, refs.begin() + mid, beginIt + count,
                     [axis](const RefT& a, const RefT& b) { return a.centroid[axis] < b.centroid[axis]; });
    return mid;
}

/// Shared recursive binned-SAH BVH2 builder used for both primitive BLAS and scene TLAS.
template <typename RefT>
void BuildSahBvh2(std::span<RefT> refs, uint32_t maxLeafCount, std::vector<RtBvhNode>& outNodes) {
    outNodes.clear();
    if (refs.empty()) {
        return;
    }

    outNodes.reserve(refs.size() * 2u);
    auto subdivide = [&](auto& self, uint32_t nodeIdx, uint32_t start, uint32_t count, uint32_t depth) -> void {
        core::AABB bounds{};
        for (uint32_t i = 0; i < count; ++i) {
            bounds.Expand(refs[start + i].bounds);
        }
        outNodes[nodeIdx].aabbMin = bounds.minBound;
        outNodes[nodeIdx].aabbMax = bounds.maxBound;

        if (count <= maxLeafCount || depth >= kMaxBvhDepth) {
            outNodes[nodeIdx].leftFirst = start;
            outNodes[nodeIdx].triCount = count;
            return;
        }

        const uint32_t splitIdx = PartitionSahOrMedian<RefT>(refs, start, count, bounds);
        const uint32_t leftCount = splitIdx - start;
        const uint32_t rightCount = count - leftCount;
        if (leftCount == 0u || rightCount == 0u) {
            outNodes[nodeIdx].leftFirst = start;
            outNodes[nodeIdx].triCount = count;
            return;
        }

        outNodes[nodeIdx].triCount = 0u;

        // Left child immediately follows parent (`leftIdx == nodeIdx + 1`) across the flat BVH array.
        const uint32_t leftIdx = static_cast<uint32_t>(outNodes.size());
        outNodes.emplace_back();
        self(self, leftIdx, start, leftCount, depth + 1u);

        const uint32_t rightIdx = static_cast<uint32_t>(outNodes.size());
        outNodes[nodeIdx].leftFirst = rightIdx;
        outNodes.emplace_back();
        self(self, rightIdx, splitIdx, rightCount, depth + 1u);
    };

    outNodes.emplace_back();
    subdivide(subdivide, 0u, 0u, static_cast<uint32_t>(refs.size()), 0u);
}

[[nodiscard]] inline glm::vec3 SafeInvDir(const glm::vec3& d) noexcept {
    return glm::vec3{
        std::abs(d.x) > 1e-8f ? (1.0f / d.x) : std::copysign(1e8f, d.x),
        std::abs(d.y) > 1e-8f ? (1.0f / d.y) : std::copysign(1e8f, d.y),
        std::abs(d.z) > 1e-8f ? (1.0f / d.z) : std::copysign(1e8f, d.z),
    };
}

[[nodiscard]] inline float IntersectAabbT(const glm::vec3& ro, const glm::vec3& invD, const glm::vec3& bmin,
                                          const glm::vec3& bmax, float tMax) noexcept {
    const glm::vec3 t0 = (bmin - ro) * invD;
    const glm::vec3 t1 = (bmax - ro) * invD;
    const glm::vec3 tSmall = glm::min(t0, t1);
    const glm::vec3 tBig = glm::max(t0, t1);
    const float tmin = std::max(0.0f, std::max(tSmall.x, std::max(tSmall.y, tSmall.z)));
    const float tmax = std::min(tMax, std::min(tBig.x, std::min(tBig.y, tBig.z)));
    return (tmax >= tmin) ? tmin : 1e30f;
}

inline void PushOrderedChildNodes(const glm::vec3& ro, const glm::vec3& invD, std::span<const RtBvhNode> nodes,
                                  uint32_t baseOffset, uint32_t leftLocal, uint32_t rightLocal, float maxDist,
                                  std::array<uint32_t, 64>& stack, uint32_t& sp) noexcept {
    const float dL =
        IntersectAabbT(ro, invD, nodes[baseOffset + leftLocal].aabbMin, nodes[baseOffset + leftLocal].aabbMax, maxDist);
    const float dR = IntersectAabbT(ro, invD, nodes[baseOffset + rightLocal].aabbMin,
                                    nodes[baseOffset + rightLocal].aabbMax, maxDist);
    const uint32_t nearNode = (dL < dR) ? leftLocal : rightLocal;
    const uint32_t farNode = (dL < dR) ? rightLocal : leftLocal;
    const float dNear = std::min(dL, dR);
    const float dFar = std::max(dL, dR);
    if (dFar < maxDist && sp < stack.size()) {
        stack[sp++] = farNode;
    }
    if (dNear < maxDist && sp < stack.size()) {
        stack[sp++] = nearNode;
    }
}

void TraverseInstanceBlas(const core::Ray& worldRay, const RtInstance& inst, uint32_t instIdx,
                          std::span<const RtBvhNode> blasNodes, std::span<const uint32_t> indices,
                          std::span<const RtVertex> vertices, RtCpuHit& best) noexcept {
    const glm::vec3 localRo = glm::vec3{inst.invModel * glm::vec4{worldRay.origin, 1.0f}};
    const glm::vec3 localRd = glm::vec3{inst.invModel * glm::vec4{worldRay.direction, 0.0f}};
    const glm::vec3 localInvD = SafeInvDir(localRd);

    std::array<uint32_t, 64> stack{};
    uint32_t sp = 0u;
    stack[sp++] = 0u;

    while (sp > 0u) {
        const uint32_t localNodeIdx = stack[--sp];
        const RtBvhNode& bNode = blasNodes[inst.blasRootNode + localNodeIdx];
        if (IntersectAabbT(localRo, localInvD, bNode.aabbMin, bNode.aabbMax, best.distance) >= best.distance) {
            continue;
        }
        if (bNode.triCount == 0u) {
            PushOrderedChildNodes(localRo, localInvD, blasNodes, inst.blasRootNode, localNodeIdx + 1u, bNode.leftFirst,
                                  best.distance, stack, sp);
            continue;
        }
        for (uint32_t t = 0u; t < bNode.triCount; ++t) {
            const uint32_t localTri = bNode.leftFirst + t;
            const uint32_t gTri = inst.triOffset + localTri;
            const uint32_t i0 = inst.vertexOffset + indices[gTri * 3u + 0u];
            const uint32_t i1 = inst.vertexOffset + indices[gTri * 3u + 1u];
            const uint32_t i2 = inst.vertexOffset + indices[gTri * 3u + 2u];
            const glm::vec3 p0 = glm::vec3{vertices[i0].posU};
            const glm::vec3 p1 = glm::vec3{vertices[i1].posU};
            const glm::vec3 p2 = glm::vec3{vertices[i2].posU};

            const glm::vec3 e1 = p1 - p0;
            const glm::vec3 e2 = p2 - p0;
            const glm::vec3 pvec = glm::cross(localRd, e2);
            const float det = glm::dot(e1, pvec);
            if (std::abs(det) < 1e-8f) {
                continue;
            }
            const float invDet = 1.0f / det;
            const glm::vec3 tvec = localRo - p0;
            const float u = glm::dot(tvec, pvec) * invDet;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            const glm::vec3 qvec = glm::cross(tvec, e1);
            const float v = glm::dot(localRd, qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            const float hitT = glm::dot(e2, qvec) * invDet;
            if (hitT > 1e-4f && hitT < best.distance) {
                best.hit = true;
                best.distance = hitT;
                best.instanceIndex = instIdx;
                best.triangleIndex = localTri;
                best.nodeIndex = inst.nodeIndex;
                best.worldPos = worldRay.At(hitT);
                const float w = 1.0f - u - v;
                const glm::vec3 localN = glm::vec3{vertices[i0].normV} * w + glm::vec3{vertices[i1].normV} * u +
                                         glm::vec3{vertices[i2].normV} * v;
                const glm::vec3 worldN = glm::transpose(glm::mat3{inst.invModel}) * localN;
                const float nLen = glm::length(worldN);
                best.worldNormal = (nLen > 1e-6f) ? (worldN / nLen) : glm::vec3{0.0f, 1.0f, 0.0f};
            }
        }
    }
}

} // namespace

uint32_t RayTracingScene::BuildBinnedSahBlas(std::span<const RtVertex> vertices,
                                             std::span<const uint32_t> sourceIndices, std::vector<RtBvhNode>& outNodes,
                                             std::vector<uint32_t>& outReorderedIndices) {
    outNodes.clear();
    outReorderedIndices.clear();

    const uint32_t triCount = static_cast<uint32_t>(sourceIndices.size() / 3u);
    if (triCount == 0u || vertices.empty()) {
        return 0u;
    }

    std::vector<TriBuildRef> refs(triCount);
    const uint32_t vertCount = static_cast<uint32_t>(vertices.size());
    for (uint32_t t = 0; t < triCount; ++t) {
        const glm::vec3 p0 = glm::vec3{vertices[std::min(sourceIndices[t * 3u + 0u], vertCount - 1u)].posU};
        const glm::vec3 p1 = glm::vec3{vertices[std::min(sourceIndices[t * 3u + 1u], vertCount - 1u)].posU};
        const glm::vec3 p2 = glm::vec3{vertices[std::min(sourceIndices[t * 3u + 2u], vertCount - 1u)].posU};

        core::AABB b{};
        b.Expand(p0);
        b.Expand(p1);
        b.Expand(p2);
        refs[t].origTriIdx = t;
        refs[t].bounds = b;
        refs[t].centroid = (p0 + p1 + p2) * (1.0f / 3.0f);
    }

    BuildSahBvh2<TriBuildRef>(refs, kMaxLeafTris, outNodes);

    outReorderedIndices.resize(static_cast<size_t>(triCount) * 3u);
    for (uint32_t t = 0; t < triCount; ++t) {
        const uint32_t origT = refs[t].origTriIdx;
        outReorderedIndices[t * 3u + 0u] = sourceIndices[origT * 3u + 0u];
        outReorderedIndices[t * 3u + 1u] = sourceIndices[origT * 3u + 1u];
        outReorderedIndices[t * 3u + 2u] = sourceIndices[origT * 3u + 2u];
    }

    return static_cast<uint32_t>(outNodes.size());
}

void RayTracingScene::RebuildTlasInternal(const scene::Scene& scene, bool forceUpload) {
    if (!forceUpload && &scene == lastScene_ && scene.transformVersion_ == lastTransformVersion_) {
        return;
    }

    instances_.clear();
    tlasNodes_.clear();

    auto appendInstances = [&](auto& self, const std::shared_ptr<scene::SceneNode>& node,
                               const glm::mat4& parentMat) -> void {
        if (!node) {
            return;
        }
        const glm::mat4 worldMat = parentMat * node->localTransform;
        for (const auto& prim : node->GetPrimitives()) {
            auto it = primSliceMap_.find(&prim);
            if (it == primSliceMap_.end() || it->second.triCount == 0u) {
                continue;
            }
            const PrimitiveBlasSlice& slice = it->second;
            const uint32_t matIdx =
                (prim.materialIndex >= 0 && (static_cast<size_t>(prim.materialIndex) + 1u) < materials_.size())
                    ? (static_cast<uint32_t>(prim.materialIndex) + 1u)
                    : 0u;
            const auto& rtMat = materials_[matIdx];
            const uint32_t instFlags = (rtMat.flagsAlpha.z != 0u ? 1u : 0u) | (rtMat.flagsAlpha.w != 0u ? 2u : 0u);

            auto pushInstance = [&](const glm::mat4& modelMat) {
                const float det = glm::determinant(glm::mat3{modelMat});
                if (!std::isfinite(det) || std::abs(det) < 1e-24f) {
                    return;
                }
                RtInstance inst{};
                inst.model = modelMat;
                inst.invModel = glm::inverse(modelMat);
                const core::AABB worldBox = slice.localBounds.Transformed(modelMat);
                inst.aabbMin = worldBox.minBound;
                inst.aabbMax = worldBox.maxBound;
                inst.blasRootNode = slice.blasRootNode;
                inst.materialIdx = matIdx;
                inst.vertexOffset = slice.vertexOffset;
                inst.triOffset = slice.triOffset;
                inst.flags = instFlags;
                inst.nodeIndex = static_cast<uint32_t>(std::max(0, node->nodeIndex));
                instances_.push_back(inst);
            };

            if (!node->instanceTransforms.empty()) {
                for (const glm::mat4& instLocal : node->instanceTransforms) {
                    pushInstance(worldMat * instLocal);
                }
            } else {
                pushInstance(worldMat);
            }
        }
        for (const auto& child : node->children) {
            self(self, child, worldMat);
        }
    };

    for (const auto& root : scene.rootNodes) {
        appendInstances(appendInstances, root, glm::mat4{1.0f});
    }

    if (!instances_.empty()) {
        const uint32_t instCount = static_cast<uint32_t>(instances_.size());
        std::vector<InstBuildRef> refs(instCount);
        for (uint32_t i = 0; i < instCount; ++i) {
            core::AABB b{};
            b.minBound = instances_[i].aabbMin;
            b.maxBound = instances_[i].aabbMax;
            refs[i].instIdx = i;
            refs[i].bounds = b;
            refs[i].centroid = b.Center();
        }

        BuildSahBvh2<InstBuildRef>(refs, kMaxLeafInstances, tlasNodes_);

        std::vector<RtInstance> reordered(instCount);
        for (uint32_t i = 0; i < instCount; ++i) {
            reordered[i] = instances_[refs[i].instIdx];
        }
        instances_ = std::move(reordered);
    }

    lastScene_ = &scene;
    lastTransformVersion_ = scene.transformVersion_;
    stats_.instanceCount = static_cast<uint32_t>(instances_.size());
    stats_.tlasNodeCount = static_cast<uint32_t>(tlasNodes_.size());
    UploadTlasBuffers();
}

RtCpuHit RayTracingScene::TraceRayCpu(const core::Ray& ray, float maxDist) const noexcept {
    RtCpuHit best{};
    best.distance = maxDist;
    if (tlasNodes_.empty() || instances_.empty() || blasNodes_.empty()) {
        return best;
    }

    const glm::vec3 worldInvD = SafeInvDir(ray.direction);
    std::array<uint32_t, 64> tlasStack{};
    uint32_t tlasSp = 0u;
    tlasStack[tlasSp++] = 0u;

    while (tlasSp > 0u) {
        const uint32_t nodeIdx = tlasStack[--tlasSp];
        const RtBvhNode& node = tlasNodes_[nodeIdx];
        if (IntersectAabbT(ray.origin, worldInvD, node.aabbMin, node.aabbMax, best.distance) >= best.distance) {
            continue;
        }
        if (node.triCount == 0u) {
            PushOrderedChildNodes(ray.origin, worldInvD, tlasNodes_, 0u, nodeIdx + 1u, node.leftFirst, best.distance,
                                  tlasStack, tlasSp);
            continue;
        }
        for (uint32_t i = 0u; i < node.triCount; ++i) {
            const uint32_t instIdx = node.leftFirst + i;
            const RtInstance& inst = instances_[instIdx];
            if (IntersectAabbT(ray.origin, worldInvD, inst.aabbMin, inst.aabbMax, best.distance) < best.distance) {
                TraverseInstanceBlas(ray, inst, instIdx, blasNodes_, indices_, vertices_, best);
            }
        }
    }

    return best;
}

} // namespace bgl::gpu
