/**
 * @file Scene.cpp
 * @brief Implementation of Scene graph hierarchy transforms, WebGPU resource cleanup,
 *        and multithreaded MikkTSpace/Lengyel tangent-space basis computation.
 */
#include "scene/Scene.hpp"
#include "core/MemoryTracker.hpp"
#include "core/ThreadPool.hpp"
#include <algorithm>
#include <cmath>
#include <utility>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace bgl::scene {

MeshPrimitive::MeshPrimitive(std::pmr::memory_resource* mr) : vertices{mr}, baseVertices{mr}, indices{mr} {}

MeshPrimitive::MeshPrimitive(const MeshPrimitive& other)
    : vertices{other.vertices}, baseVertices{other.baseVertices}, indices{other.indices}, lodIndices{other.lodIndices},
      lodLevels{other.lodLevels}, morphTargets{other.morphTargets}, morphDirty{other.morphDirty},
      boundingBox{other.boundingBox}, materialIndex{other.materialIndex}, indexFormat{other.indexFormat},
      gpuVertexBuffer{nullptr}, gpuIndexBuffer{nullptr}, gpuWireframeIndexBuffer{nullptr}, gpuVertexBufferSize{0},
      gpuIndexBufferSize{0}, gpuWireframeIndexBufferSize{0}, wireframeIndexCount{0}, gpuUniformBuffer{nullptr},
      gpuUniformBufferSize{0}, gpuMorphDeltaBuffer{nullptr}, gpuMorphDeltaBufferSize{0}, gpuMorphTargetCount{0},
      gpuMorphVertexCount{0}, gpuBindGroup{nullptr} {}

MeshPrimitive& MeshPrimitive::operator=(const MeshPrimitive& other) {
    if (this != &other) {
        ReleaseGpu();
        vertices = other.vertices;
        baseVertices = other.baseVertices;
        indices = other.indices;
        lodIndices = other.lodIndices;
        lodLevels = other.lodLevels;
        morphTargets = other.morphTargets;
        morphDirty = other.morphDirty;
        boundingBox = other.boundingBox;
        materialIndex = other.materialIndex;
        indexFormat = other.indexFormat;
        gpuVertexBuffer = nullptr;
        gpuIndexBuffer = nullptr;
        gpuWireframeIndexBuffer = nullptr;
        gpuVertexBufferSize = 0;
        gpuIndexBufferSize = 0;
        gpuWireframeIndexBufferSize = 0;
        wireframeIndexCount = 0;
        gpuUniformBuffer = nullptr;
        gpuUniformBufferSize = 0;
        gpuMorphDeltaBuffer = nullptr;
        gpuMorphDeltaBufferSize = 0;
        gpuMorphTargetCount = 0;
        gpuMorphVertexCount = 0;
        gpuBindGroup = nullptr;
    }
    return *this;
}

MeshPrimitive::MeshPrimitive(MeshPrimitive&& other) noexcept
    : vertices{std::move(other.vertices)}, baseVertices{std::move(other.baseVertices)},
      indices{std::move(other.indices)}, lodIndices{std::move(other.lodIndices)}, lodLevels{other.lodLevels},
      morphTargets{std::move(other.morphTargets)}, morphDirty{std::exchange(other.morphDirty, false)},
      boundingBox{other.boundingBox}, materialIndex{other.materialIndex}, indexFormat{other.indexFormat},
      gpuVertexBuffer{std::exchange(other.gpuVertexBuffer, nullptr)},
      gpuIndexBuffer{std::exchange(other.gpuIndexBuffer, nullptr)},
      gpuWireframeIndexBuffer{std::exchange(other.gpuWireframeIndexBuffer, nullptr)},
      gpuVertexBufferSize{std::exchange(other.gpuVertexBufferSize, 0)},
      gpuIndexBufferSize{std::exchange(other.gpuIndexBufferSize, 0)},
      gpuWireframeIndexBufferSize{std::exchange(other.gpuWireframeIndexBufferSize, 0)},
      wireframeIndexCount{std::exchange(other.wireframeIndexCount, 0)},
      gpuUniformBuffer{std::exchange(other.gpuUniformBuffer, nullptr)},
      gpuUniformBufferSize{std::exchange(other.gpuUniformBufferSize, 0)},
      gpuMorphDeltaBuffer{std::exchange(other.gpuMorphDeltaBuffer, nullptr)},
      gpuMorphDeltaBufferSize{std::exchange(other.gpuMorphDeltaBufferSize, 0)},
      gpuMorphTargetCount{std::exchange(other.gpuMorphTargetCount, 0)},
      gpuMorphVertexCount{std::exchange(other.gpuMorphVertexCount, 0)},
      gpuBindGroup{std::exchange(other.gpuBindGroup, nullptr)} {}

MeshPrimitive& MeshPrimitive::operator=(MeshPrimitive&& other) noexcept {
    if (this != &other) {
        ReleaseGpu();
        vertices = std::move(other.vertices);
        baseVertices = std::move(other.baseVertices);
        indices = std::move(other.indices);
        lodIndices = std::move(other.lodIndices);
        lodLevels = other.lodLevels;
        morphTargets = std::move(other.morphTargets);
        morphDirty = std::exchange(other.morphDirty, false);
        boundingBox = other.boundingBox;
        materialIndex = other.materialIndex;
        indexFormat = other.indexFormat;
        gpuVertexBuffer = std::exchange(other.gpuVertexBuffer, nullptr);
        gpuIndexBuffer = std::exchange(other.gpuIndexBuffer, nullptr);
        gpuWireframeIndexBuffer = std::exchange(other.gpuWireframeIndexBuffer, nullptr);
        gpuVertexBufferSize = std::exchange(other.gpuVertexBufferSize, 0);
        gpuIndexBufferSize = std::exchange(other.gpuIndexBufferSize, 0);
        gpuWireframeIndexBufferSize = std::exchange(other.gpuWireframeIndexBufferSize, 0);
        wireframeIndexCount = std::exchange(other.wireframeIndexCount, 0);
        gpuUniformBuffer = std::exchange(other.gpuUniformBuffer, nullptr);
        gpuUniformBufferSize = std::exchange(other.gpuUniformBufferSize, 0);
        gpuMorphDeltaBuffer = std::exchange(other.gpuMorphDeltaBuffer, nullptr);
        gpuMorphDeltaBufferSize = std::exchange(other.gpuMorphDeltaBufferSize, 0);
        gpuMorphTargetCount = std::exchange(other.gpuMorphTargetCount, 0);
        gpuMorphVertexCount = std::exchange(other.gpuMorphVertexCount, 0);
        gpuBindGroup = std::exchange(other.gpuBindGroup, nullptr);
    }
    return *this;
}

MeshPrimitive::~MeshPrimitive() {
    ReleaseGpu();
}

void MeshPrimitive::ReleaseGpu() noexcept {
    if (auto bg = std::exchange(gpuBindGroup, nullptr))
        wgpuBindGroupRelease(bg);
    if (auto b = std::exchange(gpuMorphDeltaBuffer, nullptr)) {
        if (gpuMorphDeltaBufferSize > 0)
            core::MemoryTracker::Instance().TrackBufferFree(gpuMorphDeltaBufferSize);
        wgpuBufferRelease(b);
    }
    gpuMorphDeltaBufferSize = 0;
    gpuMorphTargetCount = 0;
    gpuMorphVertexCount = 0;
    if (auto b = std::exchange(gpuUniformBuffer, nullptr)) {
        if (gpuUniformBufferSize > 0)
            core::MemoryTracker::Instance().TrackBufferFree(gpuUniformBufferSize);
        wgpuBufferRelease(b);
    }
    gpuUniformBufferSize = 0;
    if (auto b = std::exchange(gpuIndexBuffer, nullptr)) {
        if (gpuIndexBufferSize > 0)
            core::MemoryTracker::Instance().TrackBufferFree(gpuIndexBufferSize);
        wgpuBufferRelease(b);
    }
    gpuIndexBufferSize = 0;
    if (auto b = std::exchange(gpuWireframeIndexBuffer, nullptr)) {
        if (gpuWireframeIndexBufferSize > 0)
            core::MemoryTracker::Instance().TrackBufferFree(gpuWireframeIndexBufferSize);
        wgpuBufferRelease(b);
    }
    gpuWireframeIndexBufferSize = 0;
    wireframeIndexCount = 0;
    if (auto b = std::exchange(gpuVertexBuffer, nullptr)) {
        if (gpuVertexBufferSize > 0)
            core::MemoryTracker::Instance().TrackBufferFree(gpuVertexBufferSize);
        wgpuBufferRelease(b);
    }
    gpuVertexBufferSize = 0;
}

void MeshPrimitive::ComputeTangents() {
    if (vertices.empty())
        return;
    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3{0.0f});
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3{0.0f});

    std::span<core::Vertex> vertSpan{vertices.data(), vertices.size()};
    std::span<const uint32_t> idxSpan{indices.data(), indices.size()};

    auto processTri = [&](size_t i0, size_t i1, size_t i2, std::vector<glm::vec3>& t1, std::vector<glm::vec3>& t2) {
        if (i0 >= vertSpan.size() || i1 >= vertSpan.size() || i2 >= vertSpan.size())
            return;
        const glm::vec3& p0{vertSpan[i0].position};
        const glm::vec3& p1{vertSpan[i1].position};
        const glm::vec3& p2{vertSpan[i2].position};

        const glm::vec2& uv0{vertSpan[i0].uv};
        const glm::vec2& uv1{vertSpan[i1].uv};
        const glm::vec2& uv2{vertSpan[i2].uv};

        const glm::vec3 e1{p1 - p0};
        const glm::vec3 e2{p2 - p0};
        const glm::vec2 duv1{uv1 - uv0};
        const glm::vec2 duv2{uv2 - uv0};

        const float det{duv1.x * duv2.y - duv2.x * duv1.y};
        if (std::abs(det) > 1e-8f) {
            const float r{1.0f / det};
            const glm::vec3 sdir{(e1 * duv2.y - e2 * duv1.y) * r};
            const glm::vec3 tdir{(e2 * duv1.x - e1 * duv2.x) * r};

            t1[i0] += sdir;
            t1[i1] += sdir;
            t1[i2] += sdir;
            t2[i0] += tdir;
            t2[i1] += tdir;
            t2[i2] += tdir;
        }
    };

    const bool hasIndices = !idxSpan.empty();
    const size_t triCount = hasIndices ? (idxSpan.size() / 3) : (vertSpan.size() / 3);
    auto accumRange = [&](size_t triStart, size_t triEnd, std::vector<glm::vec3>& t1, std::vector<glm::vec3>& t2) {
        if (hasIndices) {
            for (size_t t = triStart; t < triEnd; ++t) {
                const size_t b = t * 3;
                processTri(idxSpan[b], idxSpan[b + 1], idxSpan[b + 2], t1, t2);
            }
        } else {
            for (size_t t = triStart; t < triEnd; ++t) {
                const size_t b = t * 3;
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
                if (chunkIdx == 0) {
                    accumRange(tStart, tEnd, tan1, tan2);
                } else {
                    accumRange(tStart, tEnd, extraTan1[chunkIdx - 1], extraTan2[chunkIdx - 1]);
                }
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
    // LOD0 always contains 100% of original indices
    lodIndices[0].assign(indices.begin(), indices.end());
    lodLevels[0] =
        LodSubMesh{.firstIndex = 0, .indexCount = static_cast<uint32_t>(lodIndices[0].size()), .errorMetric = 0.0f};

    // Small meshes (< 12 triangles) or unindexed primitives retain full geometry at all LOD tiers
    if (vertices.empty() || indices.size() < 36) {
        for (size_t lvl{1}; lvl < 4; ++lvl) {
            lodIndices[lvl] = lodIndices[0];
            lodLevels[lvl] = lodLevels[0];
        }
        return;
    }

    // Ensure valid full bounding box size for [0, 1]^3 spatial quantization
    core::AABB box{boundingBox};
    if (box.minBound.x > box.maxBound.x) {
        for (const auto& v : vertices) {
            box.Expand(v.position);
        }
    }
    const glm::vec3 boxSize{glm::max(box.Size(), glm::vec3{1e-5f})};
    const float maxSize{std::max({boxSize.x, boxSize.y, boxSize.z, 1e-4f})};

    struct CellSample {
        uint64_t key{0};
        uint32_t vertexIndex{0};
        float distSq{0.0f};
    };
    std::vector<CellSample> cellSamples(vertices.size());
    std::vector<uint32_t> remap(vertices.size(), 0);

    // Helper: cluster vertices on an adaptive 3D spatial grid with resolution `gridRes`
    auto clusterWithGrid = [&](uint32_t gridRes) -> std::vector<uint32_t> {
        const uint32_t gx{
            std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(gridRes) * (boxSize.x / maxSize))))};
        const uint32_t gy{
            std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(gridRes) * (boxSize.y / maxSize))))};
        const uint32_t gz{
            std::max(1u, static_cast<uint32_t>(std::round(static_cast<float>(gridRes) * (boxSize.z / maxSize))))};

        for (size_t i{0}; i < vertices.size(); ++i) {
            const glm::vec3 normPos{
                glm::clamp((vertices[i].position - box.minBound) / boxSize, glm::vec3{0.0f}, glm::vec3{0.99999f})};
            const uint32_t ix{static_cast<uint32_t>(normPos.x * static_cast<float>(gx))};
            const uint32_t iy{static_cast<uint32_t>(normPos.y * static_cast<float>(gy))};
            const uint32_t iz{static_cast<uint32_t>(normPos.z * static_cast<float>(gz))};

            // Pack normal octant sign (3 bits) + 20-bit cell coords to preserve sharp creases
            const uint64_t nBits{(static_cast<uint64_t>(vertices[i].normal.x >= 0.0f ? 1 : 0) << 60u) |
                                 (static_cast<uint64_t>(vertices[i].normal.y >= 0.0f ? 1 : 0) << 61u) |
                                 (static_cast<uint64_t>(vertices[i].normal.z >= 0.0f ? 1 : 0) << 62u)};
            const uint64_t key{nBits | (static_cast<uint64_t>(ix & 0xFFFFFu) << 40u) |
                               (static_cast<uint64_t>(iy & 0xFFFFFu) << 20u) | static_cast<uint64_t>(iz & 0xFFFFFu)};

            const glm::vec3 cellCenter{(static_cast<float>(ix) + 0.5f) / static_cast<float>(gx),
                                       (static_cast<float>(iy) + 0.5f) / static_cast<float>(gy),
                                       (static_cast<float>(iz) + 0.5f) / static_cast<float>(gz)};
            const glm::vec3 diff{normPos - cellCenter};
            const float distSq{glm::dot(diff, diff)};

            cellSamples[i] = CellSample{.key = key, .vertexIndex = static_cast<uint32_t>(i), .distSq = distSq};
        }

        std::sort(cellSamples.begin(), cellSamples.end(), [](const CellSample& a, const CellSample& b) noexcept {
            if (a.key != b.key) {
                return a.key < b.key;
            }
            return a.distSq < b.distSq;
        });

        size_t sIdx{0};
        while (sIdx < cellSamples.size()) {
            const uint64_t cellKey{cellSamples[sIdx].key};
            const uint32_t repVertex{cellSamples[sIdx].vertexIndex};
            while (sIdx < cellSamples.size() && cellSamples[sIdx].key == cellKey) {
                remap[cellSamples[sIdx].vertexIndex] = repVertex;
                ++sIdx;
            }
        }

        std::vector<uint32_t> simplified{};
        simplified.reserve(indices.size());
        for (size_t i{0}; i + 2 < indices.size(); i += 3) {
            const uint32_t i0{indices[i]};
            const uint32_t i1{indices[i + 1]};
            const uint32_t i2{indices[i + 2]};
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                continue;
            const uint32_t r0{remap[i0]};
            const uint32_t r1{remap[i1]};
            const uint32_t r2{remap[i2]};
            if (r0 == r1 || r1 == r2 || r2 == r0)
                continue;

            // Reject zero-area collapsed slivers
            const glm::vec3 e1{vertices[r1].position - vertices[r0].position};
            const glm::vec3 e2{vertices[r2].position - vertices[r0].position};
            const glm::vec3 crossVec{glm::cross(e1, e2)};
            if (glm::dot(crossVec, crossVec) <= 1e-12f)
                continue;

            simplified.push_back(r0);
            simplified.push_back(r1);
            simplified.push_back(r2);
        }
        return simplified;
    };

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

        // Search a few grid resolutions to hit target triangle reduction cleanly
        for (int attempt{0}; attempt < 4; ++attempt) {
            auto candidate = clusterWithGrid(gridRes);
            if (candidate.size() >= 12) {
                bestCandidate = std::move(candidate);
                if (static_cast<double>(bestCandidate.size()) <= static_cast<double>(targetIndices) * 1.25 &&
                    bestCandidate.size() < prevIndexCount) {
                    break;
                }
            }
            if (gridRes <= 2u)
                break;
            gridRes = std::max(2u, (gridRes * 2u) / 3u);
        }

        // Fallback if spatial clustering left >= prevIndexCount or < 12 indices:
        // retain triangles sorted/strided by geometric area so LOD strictly reduces triangle count
        if (bestCandidate.size() < 12 || bestCandidate.size() >= prevIndexCount) {
            const auto& src = (bestCandidate.size() >= 12) ? bestCandidate : lodIndices[lvl - 1];
            const size_t srcTris{src.size() / 3};
            const size_t maxKeepTris{(srcTris > 4) ? (srcTris - 1) : 4};
            const size_t keepTris{
                std::clamp<size_t>(static_cast<size_t>(std::round(static_cast<double>(baseTriCount) *
                                                                  static_cast<double>(kTargetRatios[tier]))),
                                   4, maxKeepTris)};

            if (keepTris < srcTris) {
                struct TriArea {
                    size_t triIdx{0};
                    float areaSq{0.0f};
                };
                std::vector<TriArea> areas(srcTris);
                for (size_t t{0}; t < srcTris; ++t) {
                    const uint32_t a{src[t * 3]};
                    const uint32_t b{src[t * 3 + 1]};
                    const uint32_t c{src[t * 3 + 2]};
                    if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
                        areas[t] = TriArea{t, 0.0f};
                        continue;
                    }
                    const glm::vec3 cp{glm::cross(vertices[b].position - vertices[a].position,
                                                  vertices[c].position - vertices[a].position)};
                    areas[t] = TriArea{t, glm::dot(cp, cp)};
                }
                std::nth_element(areas.begin(), areas.begin() + static_cast<std::ptrdiff_t>(keepTris), areas.end(),
                                 [](const TriArea& x, const TriArea& y) { return x.areaSq > y.areaSq; });
                areas.resize(keepTris);
                std::sort(areas.begin(), areas.end(),
                          [](const TriArea& x, const TriArea& y) { return x.triIdx < y.triIdx; });

                // Write into a separate vector because src may alias bestCandidate
                std::vector<uint32_t> reduced;
                reduced.reserve(keepTris * 3);
                for (const auto& entry : areas) {
                    reduced.push_back(src[entry.triIdx * 3]);
                    reduced.push_back(src[entry.triIdx * 3 + 1]);
                    reduced.push_back(src[entry.triIdx * 3 + 2]);
                }
                bestCandidate = std::move(reduced);
            } else {
                bestCandidate = lodIndices[lvl - 1];
            }
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
        if (std::abs(w) < 1e-6f)
            continue;
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
        if (len > 1e-6f) {
            v.normal /= len;
        }
        boundingBox.Expand(v.position);
    }
    morphDirty = true;
}

void SceneNode::UpdateLocalTransform() noexcept {
    const glm::mat4 tMat = glm::translate(glm::mat4(1.0f), translation);
    const glm::mat4 rMat = glm::mat4_cast(rotation);
    const glm::mat4 sMat = glm::scale(glm::mat4(1.0f), scale);
    localTransform = tMat * rMat * sMat;
}

void SceneNode::ApplyMorphWeights() {
    if (morphWeights.empty())
        return;
    for (auto& prim : GetPrimitives()) {
        if (!prim.morphTargets.empty() && prim.gpuMorphDeltaBuffer == nullptr) {
            prim.ApplyMorphTargets(morphWeights);
        }
    }
}

void SceneNode::UpdateTransforms(const glm::mat4& parentTransform) {
    worldTransform = parentTransform * localTransform;
    worldAABB = core::AABB{};
    for (const auto& prim : GetPrimitives()) {
        if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
            if (instanceTransforms.empty()) {
                worldAABB.Expand(prim.boundingBox.Transformed(worldTransform));
            } else {
                for (const glm::mat4& instMat : instanceTransforms) {
                    worldAABB.Expand(prim.boundingBox.Transformed(worldTransform * instMat));
                }
            }
        }
    }
    for (auto& child : children) {
        if (child) {
            child->UpdateTransforms(worldTransform);
            worldAABB.Expand(child->worldAABB);
        }
    }
}

Scene::~Scene() {
    for (auto& t : gpuTextures) {
        if (auto view = std::exchange(t.view, nullptr)) {
            wgpuTextureViewRelease(view);
        }
        if (auto tex = std::exchange(t.texture, nullptr)) {
            wgpuTextureRelease(tex);
        }
        t.sampler = nullptr;
    }
    gpuTextures.clear();
}

void Scene::Update() {
    sceneAABB = core::AABB{};
    for (auto& node : rootNodes) {
        if (node) {
            node->UpdateTransforms();
            sceneAABB.Expand(node->worldAABB);
        }
    }
    ++transformVersion_;
}

void Scene::UpdateAnimation(float deltaTime) {
    if (activeAnimationIndex < 0 || static_cast<size_t>(activeAnimationIndex) >= animations.size()) {
        return;
    }
    const auto& anim = animations[activeAnimationIndex];
    if (anim.duration <= 0.0f) {
        return;
    }

    if (animationPlaying) {
        animationTime += deltaTime;
        if (animationTime > anim.duration) {
            animationTime = std::fmod(animationTime, anim.duration);
        }
    }
    EvaluateAnimation(static_cast<size_t>(activeAnimationIndex), animationTime);
}

void Scene::EvaluateAnimation(size_t animIndex, float time) {
    if (animIndex >= animations.size())
        return;
    const auto& anim = animations[animIndex];

    for (const auto& channel : anim.channels) {
        if (channel.samplerIndex >= anim.samplers.size())
            continue;
        const auto& sampler = anim.samplers[channel.samplerIndex];
        if (channel.targetNodeIndex < 0 || static_cast<size_t>(channel.targetNodeIndex) >= allNodes.size())
            continue;
        const auto& targetNode = allNodes[static_cast<size_t>(channel.targetNodeIndex)];
        if (!targetNode)
            continue;

        switch (channel.path) {
        case AnimationPath::Translation: {
            std::array<float, 3> val{};
            sampler.Sample(time, 3, channel.path, val);
            targetNode->translation = glm::vec3(val[0], val[1], val[2]);
            break;
        }
        case AnimationPath::Rotation: {
            std::array<float, 4> val{};
            sampler.Sample(time, 4, channel.path, val);
            targetNode->rotation = glm::quat(val[3], val[0], val[1], val[2]);
            break;
        }
        case AnimationPath::Scale: {
            std::array<float, 3> val{};
            sampler.Sample(time, 3, channel.path, val);
            targetNode->scale = glm::vec3(val[0], val[1], val[2]);
            break;
        }
        case AnimationPath::Weights: {
            size_t count = targetNode->morphWeights.size();
            if (count == 0 && targetNode->mesh && !targetNode->mesh->primitives.empty()) {
                count = targetNode->mesh->primitives[0].morphTargets.size();
                targetNode->morphWeights.resize(count, 0.0f);
            }
            if (count > 0) {
                if (targetNode->morphWeights.size() < count) {
                    targetNode->morphWeights.resize(count, 0.0f);
                }
                sampler.Sample(time, count, channel.path, targetNode->morphWeights);
                targetNode->ApplyMorphWeights();
            }
            break;
        }
        }
        targetNode->UpdateLocalTransform();
    }

    Update();
}

void Scene::SetAnimation(int index) noexcept {
    if (index >= 0 && static_cast<size_t>(index) < animations.size()) {
        activeAnimationIndex = index;
        animationTime = 0.0f;
        animationPlaying = true;
        EvaluateAnimation(static_cast<size_t>(activeAnimationIndex), 0.0f);
    } else {
        activeAnimationIndex = -1;
    }
}

Skin::Skin(const Skin& other)
    : name{other.name}, inverseBindMatrices{other.inverseBindMatrices},
      skeletonRootNodeIndex{other.skeletonRootNodeIndex}, joints{other.joints}, gpuJointBuffer{nullptr},
      gpuJointBufferSize{0}, gpuBindGroup{nullptr} {}

Skin& Skin::operator=(const Skin& other) {
    if (this != &other) {
        ReleaseGpu();
        name = other.name;
        inverseBindMatrices = other.inverseBindMatrices;
        skeletonRootNodeIndex = other.skeletonRootNodeIndex;
        joints = other.joints;
        gpuJointBufferSize = 0;
        cachedJointMatrices.clear();
        cachedFrameId = 0;
        cachedUploadedFrameId = 0;
        cachedMeshNode = nullptr;
        cachedJointCount = 0;
    }
    return *this;
}

Skin::Skin(Skin&& other) noexcept
    : name{std::move(other.name)}, inverseBindMatrices{std::move(other.inverseBindMatrices)},
      skeletonRootNodeIndex{std::exchange(other.skeletonRootNodeIndex, -1)}, joints{std::move(other.joints)},
      gpuJointBuffer{std::exchange(other.gpuJointBuffer, nullptr)},
      gpuJointBufferSize{std::exchange(other.gpuJointBufferSize, 0)},
      gpuBindGroup{std::exchange(other.gpuBindGroup, nullptr)},
      cachedJointMatrices{std::move(other.cachedJointMatrices)}, cachedFrameId{std::exchange(other.cachedFrameId, 0)},
      cachedUploadedFrameId{std::exchange(other.cachedUploadedFrameId, 0)},
      cachedMeshNode{std::exchange(other.cachedMeshNode, nullptr)},
      cachedJointCount{std::exchange(other.cachedJointCount, 0)} {}

Skin& Skin::operator=(Skin&& other) noexcept {
    if (this != &other) {
        ReleaseGpu();
        name = std::move(other.name);
        inverseBindMatrices = std::move(other.inverseBindMatrices);
        skeletonRootNodeIndex = std::exchange(other.skeletonRootNodeIndex, -1);
        joints = std::move(other.joints);
        gpuJointBuffer = std::exchange(other.gpuJointBuffer, nullptr);
        gpuJointBufferSize = std::exchange(other.gpuJointBufferSize, 0);
        gpuBindGroup = std::exchange(other.gpuBindGroup, nullptr);
        cachedJointMatrices = std::move(other.cachedJointMatrices);
        cachedFrameId = std::exchange(other.cachedFrameId, 0);
        cachedUploadedFrameId = std::exchange(other.cachedUploadedFrameId, 0);
        cachedMeshNode = std::exchange(other.cachedMeshNode, nullptr);
        cachedJointCount = std::exchange(other.cachedJointCount, 0);
    }
    return *this;
}

Skin::~Skin() {
    ReleaseGpu();
}

void Skin::ReleaseGpu() noexcept {
    if (auto bg = std::exchange(gpuBindGroup, nullptr)) {
        wgpuBindGroupRelease(bg);
    }
    if (auto b = std::exchange(gpuJointBuffer, nullptr)) {
        if (gpuJointBufferSize > 0) {
            core::MemoryTracker::Instance().TrackBufferFree(gpuJointBufferSize);
        }
        wgpuBufferRelease(b);
    }
    gpuJointBufferSize = 0;
}

uint32_t Skin::ComputeJointMatrices(const glm::mat4& meshNodeWorldTransform,
                                    std::span<const std::shared_ptr<SceneNode>> allNodes,
                                    std::span<glm::mat4> outJointMatrices) const noexcept {
    const uint32_t count =
        std::min(static_cast<uint32_t>(joints.size()), static_cast<uint32_t>(outJointMatrices.size()));
    const glm::mat4 invWorld = glm::inverse(meshNodeWorldTransform);
    for (uint32_t j = 0; j < count; ++j) {
        const int jointNodeIdx = joints[j];
        glm::mat4 jointWorld{1.0f};
        if (jointNodeIdx >= 0 && static_cast<size_t>(jointNodeIdx) < allNodes.size()) {
            if (const auto& jointNode = allNodes[static_cast<size_t>(jointNodeIdx)]) {
                jointWorld = jointNode->worldTransform;
            }
        }
        const glm::mat4 ibm = (j < inverseBindMatrices.size()) ? inverseBindMatrices[j] : glm::mat4{1.0f};
        outJointMatrices[j] = invWorld * jointWorld * ibm;
    }
    return count;
}

std::span<const glm::mat4>
Skin::GetOrComputeJointMatrices(uint64_t frameId, const SceneNode* meshNode, const glm::mat4& meshNodeWorldTransform,
                                std::span<const std::shared_ptr<SceneNode>> allNodes) const noexcept {
    if (frameId != 0 && cachedFrameId == frameId && cachedMeshNode == meshNode && !cachedJointMatrices.empty()) {
        return std::span<const glm::mat4>{cachedJointMatrices.data(), cachedJointCount};
    }
    const size_t count = std::max<size_t>(1, joints.size());
    if (cachedJointMatrices.size() < count) {
        cachedJointMatrices.resize(count, glm::mat4{1.0f});
    }
    cachedJointCount = ComputeJointMatrices(meshNodeWorldTransform, allNodes, cachedJointMatrices);
    cachedFrameId = frameId;
    cachedMeshNode = meshNode;
    return std::span<const glm::mat4>{cachedJointMatrices.data(), cachedJointCount};
}

} // namespace bgl::scene
