/**
 * @file SceneMeshOps.cpp
 * @brief MeshPrimitive geometry processing: tangent-space basis computation, multi-LOD generation, and morph blending.
 */
#include "core/ThreadPool.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace bgl::scene {

namespace {

/// Clusters vertices onto a quantized spatial grid (`gridRes` along max axis) and emits non-degenerate triangles.
[[nodiscard]] std::vector<uint32_t> ClusterMeshWithGrid(std::span<const core::Vertex> vertices,
                                                        std::span<const uint32_t> indices, const core::AABB& box,
                                                        const glm::vec3& boxSize, float maxSize, uint32_t gridRes,
                                                        std::vector<uint32_t>& remapScratch) {
    struct CellSample {
        uint64_t key{0};
        uint32_t vertexIndex{0};
        float distSq{0.0f};
    };

    const uint32_t gx{
        std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(gridRes) * (boxSize.x / maxSize))))};
    const uint32_t gy{
        std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(gridRes) * (boxSize.y / maxSize))))};
    const uint32_t gz{
        std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(gridRes) * (boxSize.z / maxSize))))};

    std::vector<CellSample> samples(vertices.size());
    for (size_t i{0}; i < vertices.size(); ++i) {
        const glm::vec3 rawNormPos{(vertices[i].position - box.minBound) / boxSize};
        const glm::vec3 safeRaw{
            std::isfinite(rawNormPos.x) ? rawNormPos.x : 0.0f,
            std::isfinite(rawNormPos.y) ? rawNormPos.y : 0.0f,
            std::isfinite(rawNormPos.z) ? rawNormPos.z : 0.0f,
        };
        const glm::vec3 normPos{glm::clamp(safeRaw, glm::vec3{0.0f}, glm::vec3{0.99999f})};
        const uint32_t ix{static_cast<uint32_t>(normPos.x * static_cast<float>(gx))};
        const uint32_t iy{static_cast<uint32_t>(normPos.y * static_cast<float>(gy))};
        const uint32_t iz{static_cast<uint32_t>(normPos.z * static_cast<float>(gz))};

        // Pack normal octant sign (3 bits) + 20-bit cell coords to preserve hard edges
        const uint64_t nBits{(static_cast<uint64_t>(vertices[i].normal.x >= 0.0f ? 1 : 0) << 60u) |
                             (static_cast<uint64_t>(vertices[i].normal.y >= 0.0f ? 1 : 0) << 61u) |
                             (static_cast<uint64_t>(vertices[i].normal.z >= 0.0f ? 1 : 0) << 62u)};
        const uint64_t key{nBits | (static_cast<uint64_t>(ix & 0xFFFFFu) << 40u) |
                           (static_cast<uint64_t>(iy & 0xFFFFFu) << 20u) | static_cast<uint64_t>(iz & 0xFFFFFu)};

        const glm::vec3 cellCenter{(static_cast<float>(ix) + 0.5f) / static_cast<float>(gx),
                                   (static_cast<float>(iy) + 0.5f) / static_cast<float>(gy),
                                   (static_cast<float>(iz) + 0.5f) / static_cast<float>(gz)};
        const glm::vec3 diff{normPos - cellCenter};
        samples[i] = CellSample{.key = key, .vertexIndex = static_cast<uint32_t>(i), .distSq = glm::dot(diff, diff)};
    }

    std::sort(samples.begin(), samples.end(), [](const CellSample& a, const CellSample& b) noexcept {
        return (a.key != b.key) ? (a.key < b.key) : (a.distSq < b.distSq);
    });

    size_t sIdx{0};
    while (sIdx < samples.size()) {
        const uint64_t cellKey{samples[sIdx].key};
        const uint32_t repVertex{samples[sIdx].vertexIndex};
        while (sIdx < samples.size() && samples[sIdx].key == cellKey) {
            remapScratch[samples[sIdx].vertexIndex] = repVertex;
            ++sIdx;
        }
    }

    std::vector<uint32_t> simplified;
    simplified.reserve(indices.size());
    for (size_t i{0}; i + 2 < indices.size(); i += 3) {
        const uint32_t i0{indices[i]};
        const uint32_t i1{indices[i + 1]};
        const uint32_t i2{indices[i + 2]};
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }
        const uint32_t r0{remapScratch[i0]};
        const uint32_t r1{remapScratch[i1]};
        const uint32_t r2{remapScratch[i2]};
        if (r0 == r1 || r1 == r2 || r2 == r0) {
            continue;
        }
        const glm::vec3 e1{vertices[r1].position - vertices[r0].position};
        const glm::vec3 e2{vertices[r2].position - vertices[r0].position};
        const glm::vec3 crossVec{glm::cross(e1, e2)};
        if (glm::dot(crossVec, crossVec) <= 1e-12f) {
            continue;
        }
        simplified.push_back(r0);
        simplified.push_back(r1);
        simplified.push_back(r2);
    }
    return simplified;
}

/// Area-ordered decimation fallback when spatial clustering cannot reduce triangle counts further.
[[nodiscard]] std::vector<uint32_t> DecimateTrianglesByArea(std::span<const core::Vertex> vertices,
                                                            std::span<const uint32_t> src, size_t keepTris) {
    struct TriArea {
        size_t triIdx{0};
        float areaSq{0.0f};
    };
    const size_t srcTris{src.size() / 3};
    keepTris = std::min(keepTris, srcTris);
    if (keepTris == 0) {
        return {};
    }
    std::vector<TriArea> areas(srcTris);
    for (size_t t{0}; t < srcTris; ++t) {
        const uint32_t a{src[t * 3]};
        const uint32_t b{src[t * 3 + 1]};
        const uint32_t c{src[t * 3 + 2]};
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
            areas[t] = TriArea{t, 0.0f};
            continue;
        }
        const glm::vec3 cp{
            glm::cross(vertices[b].position - vertices[a].position, vertices[c].position - vertices[a].position)};
        areas[t] = TriArea{t, glm::dot(cp, cp)};
    }
    std::nth_element(areas.begin(), areas.begin() + static_cast<std::ptrdiff_t>(keepTris), areas.end(),
                     [](const TriArea& x, const TriArea& y) { return x.areaSq > y.areaSq; });
    areas.resize(keepTris);
    std::sort(areas.begin(), areas.end(), [](const TriArea& x, const TriArea& y) { return x.triIdx < y.triIdx; });

    std::vector<uint32_t> reduced;
    reduced.reserve(keepTris * 3);
    for (const auto& entry : areas) {
        reduced.push_back(src[entry.triIdx * 3]);
        reduced.push_back(src[entry.triIdx * 3 + 1]);
        reduced.push_back(src[entry.triIdx * 3 + 2]);
    }
    return reduced;
}

} // namespace

void MeshPrimitive::ComputeTangents() {
    if (vertices.empty()) {
        return;
    }
    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3{0.0f});
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3{0.0f});

    std::span<core::Vertex> vertSpan{vertices.data(), vertices.size()};
    std::span<const uint32_t> idxSpan{indices.data(), indices.size()};

    auto processTri = [&](size_t i0, size_t i1, size_t i2, std::vector<glm::vec3>& t1, std::vector<glm::vec3>& t2) {
        if (i0 >= vertSpan.size() || i1 >= vertSpan.size() || i2 >= vertSpan.size()) {
            return;
        }
        const glm::vec3 e1{vertSpan[i1].position - vertSpan[i0].position};
        const glm::vec3 e2{vertSpan[i2].position - vertSpan[i0].position};
        const glm::vec2 duv1{vertSpan[i1].uv - vertSpan[i0].uv};
        const glm::vec2 duv2{vertSpan[i2].uv - vertSpan[i0].uv};

        const float det{duv1.x * duv2.y - duv2.x * duv1.y};
        if (std::abs(det) > 1e-8f) {
            const float r{1.0f / det};
            const glm::vec3 sdir{(e1 * duv2.y - e2 * duv1.y) * r};
            const glm::vec3 tdir{(e2 * duv1.x - e1 * duv2.x) * r};
            for (size_t idx : {i0, i1, i2}) {
                t1[idx] += sdir;
                t2[idx] += tdir;
            }
        }
    };

    const bool hasIndices = !idxSpan.empty();
    const size_t triCount = hasIndices ? (idxSpan.size() / 3) : (vertSpan.size() / 3);
    auto accumRange = [&](size_t triStart, size_t triEnd, std::vector<glm::vec3>& t1, std::vector<glm::vec3>& t2) {
        for (size_t t = triStart; t < triEnd; ++t) {
            const size_t b = t * 3;
            if (hasIndices) {
                processTri(idxSpan[b], idxSpan[b + 1], idxSpan[b + 2], t1, t2);
            } else {
                processTri(b, b + 1, b + 2, t1, t2);
            }
        }
    };

#if !defined(__EMSCRIPTEN__)
    const size_t maxWorkers = core::ThreadPool::Instance().GetThreadCount();
    const size_t bytesPerChunk = vertSpan.size() * sizeof(glm::vec3) * 2;
    const size_t maxChunksByMem = (bytesPerChunk > 0) ? ((32u * 1024u * 1024u) / bytesPerChunk) + 1u : 1u;
    const size_t chunkCount =
        (triCount >= 32768 && maxWorkers > 1) ? std::min({maxWorkers, triCount / 8192u, maxChunksByMem}) : 1u;
    if (chunkCount > 1u) {
        const size_t extraChunks = chunkCount - 1u;
        std::vector<std::vector<glm::vec3>> extraTan1(extraChunks,
                                                      std::vector<glm::vec3>(vertSpan.size(), glm::vec3{0.0f}));
        std::vector<std::vector<glm::vec3>> extraTan2(extraChunks,
                                                      std::vector<glm::vec3>(vertSpan.size(), glm::vec3{0.0f}));
        core::ThreadPool::Instance().ParallelFor(
            0, chunkCount,
            [&](size_t chunkIdx) {
                const size_t tStart = (chunkIdx * triCount) / chunkCount;
                const size_t tEnd = ((chunkIdx + 1) * triCount) / chunkCount;
                accumRange(tStart, tEnd, (chunkIdx == 0) ? tan1 : extraTan1[chunkIdx - 1],
                           (chunkIdx == 0) ? tan2 : extraTan2[chunkIdx - 1]);
            },
            1);
        for (size_t c = 0; c < extraChunks; ++c) {
            for (size_t v = 0; v < vertSpan.size(); ++v) {
                tan1[v] += extraTan1[c][v];
                tan2[v] += extraTan2[c][v];
            }
        }
    } else {
        accumRange(0, triCount, tan1, tan2);
    }
#else
    accumRange(0, triCount, tan1, tan2);
#endif

    auto orthogonalizeVertex = [&](size_t i) noexcept {
        const glm::vec3& n{vertSpan[i].normal};
        const glm::vec3& t{tan1[i]};
        glm::vec3 orthoT{t - n * glm::dot(n, t)};
        const float lenSq{glm::dot(orthoT, orthoT)};
        if (lenSq > 1e-10f) {
            orthoT *= (1.0f / std::sqrt(lenSq));
            const float w{(glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f};
            vertSpan[i].tangent = glm::vec4{orthoT, w};
        } else {
            const glm::vec3 up{(std::abs(n.y) < 0.999f) ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f}};
            const glm::vec3 fallback{glm::cross(up, n)};
            const float fbLenSq{glm::dot(fallback, fallback)};
            vertSpan[i].tangent = glm::vec4{
                (fbLenSq > 1e-10f) ? (fallback * (1.0f / std::sqrt(fbLenSq))) : glm::vec3{1.0f, 0.0f, 0.0f}, 1.0f};
        }
    };

#if !defined(__EMSCRIPTEN__)
    if (vertSpan.size() >= 8192) {
        core::ThreadPool::Instance().ParallelFor(0, vertSpan.size(), orthogonalizeVertex);
    } else {
        for (size_t i{0}; i < vertSpan.size(); ++i) {
            orthogonalizeVertex(i);
        }
    }
#else
    for (size_t i{0}; i < vertSpan.size(); ++i) {
        orthogonalizeVertex(i);
    }
#endif
}

void MeshPrimitive::GenerateLODs() {
    lodIndices[0].assign(indices.begin(), indices.end());
    lodLevels[0] =
        LodSubMesh{.firstIndex = 0, .indexCount = static_cast<uint32_t>(lodIndices[0].size()), .errorMetric = 0.0f};

    if (vertices.empty() || indices.size() < 36) {
        for (size_t lvl{1}; lvl < 4; ++lvl) {
            lodIndices[lvl] = lodIndices[0];
            lodLevels[lvl] = lodLevels[0];
        }
        return;
    }

    core::AABB box{boundingBox};
    if (box.minBound.x > box.maxBound.x || box.minBound.y > box.maxBound.y || box.minBound.z > box.maxBound.z ||
        !std::isfinite(box.minBound.x) || !std::isfinite(box.minBound.y) || !std::isfinite(box.minBound.z) ||
        !std::isfinite(box.maxBound.x) || !std::isfinite(box.maxBound.y) || !std::isfinite(box.maxBound.z)) {
        box = core::AABB{};
        for (const auto& v : vertices) {
            box.Expand(v.position);
        }
    }
    const glm::vec3 boxSize{glm::max(box.Size(), glm::vec3{1e-5f})};
    const float maxSize{std::max({boxSize.x, boxSize.y, boxSize.z, 1e-4f})};
    std::vector<uint32_t> remap(vertices.size(), 0);

    const size_t baseTriCount{indices.size() / 3};
    const float baseDim{std::sqrt(static_cast<float>(baseTriCount))};
    constexpr std::array<float, 3> kTargetRatios{{0.55f, 0.30f, 0.14f}};
    constexpr std::array<float, 3> kErrorMetrics{{0.015f, 0.045f, 0.120f}};

    for (size_t tier{0}; tier < 3; ++tier) {
        const size_t lvl{tier + 1};
        const size_t prevIndexCount{lodIndices[lvl - 1].size()};
        const size_t targetIndices{std::max<size_t>(
            12, static_cast<size_t>(static_cast<double>(indices.size()) * static_cast<double>(kTargetRatios[tier])) /
                    3 * 3)};

        uint32_t gridRes{
            std::max(2u, static_cast<uint32_t>(std::round(baseDim * std::sqrt(kTargetRatios[tier]) * 1.35f)))};
        std::vector<uint32_t> bestCandidate{};

        for (int attempt{0}; attempt < 4; ++attempt) {
            auto candidate = ClusterMeshWithGrid(vertices, indices, box, boxSize, maxSize, gridRes, remap);
            if (candidate.size() >= 12) {
                bestCandidate = std::move(candidate);
                if (static_cast<double>(bestCandidate.size()) <= static_cast<double>(targetIndices) * 1.25 &&
                    bestCandidate.size() < prevIndexCount) {
                    break;
                }
            }
            if (gridRes <= 2u) {
                break;
            }
            gridRes = std::max(2u, (gridRes * 2u) / 3u);
        }

        if (bestCandidate.size() < 12 || bestCandidate.size() >= prevIndexCount) {
            const auto& src = (bestCandidate.size() >= 12) ? bestCandidate : lodIndices[lvl - 1];
            const size_t srcTris{src.size() / 3};
            const size_t maxKeepTris{(srcTris > 4) ? (srcTris - 1) : 4};
            const size_t keepTris{
                std::clamp<size_t>(static_cast<size_t>(std::round(static_cast<double>(baseTriCount) *
                                                                  static_cast<double>(kTargetRatios[tier]))),
                                   4, maxKeepTris)};
            bestCandidate =
                (keepTris < srcTris) ? DecimateTrianglesByArea(vertices, src, keepTris) : lodIndices[lvl - 1];
        }

        lodIndices[lvl] = std::move(bestCandidate);
        lodLevels[lvl] = LodSubMesh{.firstIndex = 0,
                                    .indexCount = static_cast<uint32_t>(lodIndices[lvl].size()),
                                    .errorMetric = kErrorMetrics[tier]};
    }
}

void MeshPrimitive::ApplyMorphTargets(std::span<const float> weights) {
    if (morphTargets.empty() || baseVertices.empty() || vertices.size() != baseVertices.size()) {
        return;
    }
    vertices = baseVertices;
    boundingBox = core::AABB{};

    const size_t numTargets = std::min(morphTargets.size(), weights.size());
    for (size_t t = 0; t < numTargets; ++t) {
        const float w = weights[t];
        if (!std::isfinite(w) || std::abs(w) < 1e-6f) {
            continue;
        }
        const auto& target = morphTargets[t];
        for (size_t i = 0; i < vertices.size(); ++i) {
            if (i < target.positionDeltas.size()) {
                vertices[i].position += w * target.positionDeltas[i];
            }
            if (i < target.normalDeltas.size()) {
                vertices[i].normal += w * target.normalDeltas[i];
            }
            if (i < target.tangentDeltas.size()) {
                vertices[i].tangent.x += w * target.tangentDeltas[i].x;
                vertices[i].tangent.y += w * target.tangentDeltas[i].y;
                vertices[i].tangent.z += w * target.tangentDeltas[i].z;
            }
        }
    }

    for (auto& v : vertices) {
        const float len = glm::length(v.normal);
        if (std::isfinite(len) && len > 1e-6f) {
            v.normal /= len;
        } else {
            v.normal = glm::vec3{0.0f, 1.0f, 0.0f};
        }
        boundingBox.Expand(v.position);
    }
    morphDirty = true;
}

} // namespace bgl::scene
