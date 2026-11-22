/**
 * @file RayTracingScene.cpp
 * @brief Implementation of 2-level SAH BVH (BLAS + TLAS), geometry/material pools, and bindless texture arrays.
 */
#include "gpu/RayTracingScene.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "core/ThreadPool.hpp"
#include "gpu/Handle.hpp"
#include "gpu/Uniforms.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

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

[[nodiscard]] inline float SurfaceArea(const core::AABB& b) noexcept {
    if (b.minBound.x > b.maxBound.x) {
        return 0.0f;
    }
    const glm::vec3 e = glm::max(b.maxBound - b.minBound, glm::vec3{0.0f});
    return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
}

struct SahSplit {
    int axis{-1};
    uint32_t splitBin{0};
    float splitPos{0.0f};
    float cost{std::numeric_limits<float>::infinity()};
};

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
        const float maxC = centroidBounds.maxBound[axis];
        const float extent = maxC - minC;
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
            const float areaR = SurfaceArea(accumR);
            const float cost =
                1.0f +
                (leftArea[ub] * static_cast<float>(leftCount[ub]) + areaR * static_cast<float>(countR)) / parentArea;
            if (cost < best.cost) {
                best.cost = cost;
                best.axis = axis;
                best.splitBin = ub;
                best.splitPos = minC + (static_cast<float>(ub + 1u) / static_cast<float>(kSahBins)) * extent;
            }
        }
    }
    return best;
}

template <typename RefT>
uint32_t PartitionSahOrMedian(std::span<RefT> refs, uint32_t start, uint32_t count, const core::AABB& nodeBounds) {
    const SahSplit split = FindBestSahSplit<RefT>(refs, start, count, nodeBounds);
    uint32_t mid = start + count / 2u;

    if (split.axis >= 0) {
        auto beginIt = refs.begin() + start;
        auto endIt = beginIt + count;
        auto splitIt =
            std::partition(beginIt, endIt, [&](const RefT& r) { return r.centroid[split.axis] < split.splitPos; });
        const uint32_t splitIdx = static_cast<uint32_t>(splitIt - refs.begin());
        if (splitIdx > start && splitIdx < start + count) {
            return splitIdx;
        }
    }

    // Fallback: median split along longest extent axis
    const glm::vec3 size = nodeBounds.Size();
    int axis = 0;
    if (size.y > size.x && size.y >= size.z) {
        axis = 1;
    } else if (size.z > size.x && size.z >= size.y) {
        axis = 2;
    }
    auto beginIt = refs.begin() + start;
    auto midIt = refs.begin() + mid;
    auto endIt = beginIt + count;
    std::nth_element(beginIt, midIt, endIt,
                     [axis](const RefT& a, const RefT& b) { return a.centroid[axis] < b.centroid[axis]; });
    return mid;
}

void UploadOrRecreateStorageBuffer(WGPUDevice device, WGPUQueue queue, BufferHandle& buffer, uint64_t& capacityBytes,
                                   const void* data, uint64_t dataBytes, const char* label) {
#if defined(BGL_PLATFORM_NATIVE)
    if (!device || !queue) {
        return;
    }
    const uint64_t safeBytes = std::max<uint64_t>(16u, (dataBytes + 15u) & ~uint64_t{15});
    if (!buffer || safeBytes > capacityBytes) {
        if (buffer) {
            core::MemoryTracker::Instance().TrackBufferFree(capacityBytes);
            buffer.Reset();
            capacityBytes = 0;
        }
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = safeBytes;
        desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        desc.label = ToWgpuStringView(label);
        buffer = wgpuDeviceCreateBuffer(device, &desc);
        if (buffer) {
            capacityBytes = safeBytes;
            core::MemoryTracker::Instance().TrackBufferAlloc(capacityBytes);
        }
    }
    if (buffer && data && dataBytes > 0) {
        wgpuQueueWriteBuffer(queue, buffer.Get(), 0, data, dataBytes);
    }
#else
    (void)device;
    (void)queue;
    (void)buffer;
    (void)capacityBytes;
    (void)data;
    (void)dataBytes;
    (void)label;
#endif
}

void FreeTrackedBuffer(BufferHandle& buffer, uint64_t& bytes) noexcept {
    if (buffer) {
        core::MemoryTracker::Instance().TrackBufferFree(bytes);
        buffer.Reset();
    }
    bytes = 0;
}

[[nodiscard]] uint32_t PackMaterialFlags(const scene::Material& mat) noexcept {
    uint32_t flags = 0u;
    if (mat.baseColorTextureIndex >= 0) {
        flags |= MaterialFlags::HasBaseColorTex;
    }
    if (mat.metallicRoughnessTextureIndex >= 0) {
        flags |= MaterialFlags::HasMetallicRoughnessTex;
    }
    if (mat.normalTextureIndex >= 0) {
        flags |= MaterialFlags::HasNormalTex;
    }
    if (mat.emissiveTextureIndex >= 0) {
        flags |= MaterialFlags::HasEmissiveTex;
    }
    if (mat.occlusionTextureIndex >= 0) {
        flags |= MaterialFlags::HasOcclusionTex;
    }
    if (mat.unlit) {
        flags |= MaterialFlags::Unlit;
    }
    if (mat.doubleSided) {
        flags |= MaterialFlags::DoubleSided;
    }
    return flags;
}

} // namespace

RayTracingScene::~RayTracingScene() {
    Shutdown();
}

void RayTracingScene::ReleaseGpuBuffers() noexcept {
    bindlessBindGroup_.Reset();
    sceneBindGroup_.Reset();
    FreeTrackedBuffer(tlasNodesBuffer_, tlasBufferBytes_);
    FreeTrackedBuffer(instancesBuffer_, instancesBufferBytes_);
    FreeTrackedBuffer(blasNodesBuffer_, blasBufferBytes_);
    FreeTrackedBuffer(indicesBuffer_, indicesBufferBytes_);
    FreeTrackedBuffer(verticesBuffer_, verticesBufferBytes_);
    FreeTrackedBuffer(materialsBuffer_, materialsBufferBytes_);
}

void RayTracingScene::Shutdown() noexcept {
    ReleaseGpuBuffers();
    bindlessBgl_.Reset();
    sceneBgl_.Reset();

    vertices_.clear();
    indices_.clear();
    blasNodes_.clear();
    materials_.clear();
    instances_.clear();
    tlasNodes_.clear();
    primSliceMap_.clear();

    lastScene_ = nullptr;
    lastTransformVersion_ = ~uint64_t{0};
    hasSceneData_ = false;
    stats_ = {};
    fallbackWhiteView_ = nullptr;
    defaultSampler_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
    supportsBindlessTextures_ = false;
    initialized_ = false;
}

core::Result<void> RayTracingScene::Initialize(WGPUDevice device, WGPUQueue queue, bool supportsBindlessTextures,
                                               WGPUTextureView fallbackWhiteView, WGPUSampler defaultSampler) {
    Shutdown();
    device_ = device;
    queue_ = queue;
    supportsBindlessTextures_ = supportsBindlessTextures;
    fallbackWhiteView_ = fallbackWhiteView;
    defaultSampler_ = defaultSampler;

#if defined(BGL_PLATFORM_NATIVE)
    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize RayTracingScene without valid device and queue");
    }

    // Group 1: 6 read-only storage buffers (TLAS, Instances, BLAS, Indices, Vertices, Materials)
    constexpr std::array<uint64_t, 6> kMinBindingSizes{
        sizeof(RtBvhNode), sizeof(RtInstance), sizeof(RtBvhNode),
        sizeof(uint32_t),  sizeof(RtVertex),   sizeof(RtMaterial),
    };
    std::array<WGPUBindGroupLayoutEntry, 6> entries{};
    for (uint32_t i = 0; i < 6u; ++i) {
        entries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        entries[i].buffer.minBindingSize = kMinBindingSizes[i];
    }

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = entries.size();
    bglDesc.entries = entries.data();
    bglDesc.label = ToWgpuStringView("RayTracingScene_BGL");
    sceneBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!sceneBgl_) {
        Shutdown();
        return std::unexpected("Failed to create RayTracingScene bind group layout");
    }

    if (supportsBindlessTextures_) {
        WGPUBindGroupLayoutEntryExtras texArrayExtras{};
        texArrayExtras.chain.sType = static_cast<WGPUSType>(WGPUSType_BindGroupLayoutEntryExtras);
        texArrayExtras.count = kMaxBindlessTextures;

        std::array<WGPUBindGroupLayoutEntry, 2> bindlessEntries{};
        bindlessEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        bindlessEntries[0].binding = 0;
        bindlessEntries[0].visibility = WGPUShaderStage_Compute;
        bindlessEntries[0].sampler.type = WGPUSamplerBindingType_Filtering;

        bindlessEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        bindlessEntries[1].nextInChain = &texArrayExtras.chain;
        bindlessEntries[1].binding = 1;
        bindlessEntries[1].visibility = WGPUShaderStage_Compute;
        bindlessEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
        bindlessEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        bindlessEntries[1].texture.multisampled = WGPU_FALSE;
        bindlessEntries[1].bindingArraySize = kMaxBindlessTextures;

        WGPUBindGroupLayoutDescriptor blDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        blDesc.entryCount = bindlessEntries.size();
        blDesc.entries = bindlessEntries.data();
        blDesc.label = ToWgpuStringView("RayTracingBindless_BGL");
        bindlessBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &blDesc);
        if (!bindlessBgl_) {
            BGL_LOG_WARN("Bindless texture array BGL creation failed; falling back to factor-only hit shading");
            supportsBindlessTextures_ = false;
        }
    }
#endif

    initialized_ = true;
    return {};
}

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
        const uint32_t i0 = std::min(sourceIndices[t * 3u + 0u], vertCount - 1u);
        const uint32_t i1 = std::min(sourceIndices[t * 3u + 1u], vertCount - 1u);
        const uint32_t i2 = std::min(sourceIndices[t * 3u + 2u], vertCount - 1u);
        const glm::vec3 p0 = glm::vec3{vertices[i0].posU};
        const glm::vec3 p1 = glm::vec3{vertices[i1].posU};
        const glm::vec3 p2 = glm::vec3{vertices[i2].posU};

        core::AABB b{};
        b.Expand(p0);
        b.Expand(p1);
        b.Expand(p2);
        refs[t].origTriIdx = t;
        refs[t].bounds = b;
        refs[t].centroid = (p0 + p1 + p2) * (1.0f / 3.0f);
    }

    outNodes.reserve(triCount * 2u);
    auto subdivide = [&](auto& self, uint32_t nodeIdx, uint32_t start, uint32_t count, uint32_t depth) -> void {
        core::AABB bounds{};
        for (uint32_t i = 0; i < count; ++i) {
            bounds.Expand(refs[start + i].bounds);
        }
        outNodes[nodeIdx].aabbMin = bounds.minBound;
        outNodes[nodeIdx].aabbMax = bounds.maxBound;

        if (count <= kMaxLeafTris || depth >= kMaxBvhDepth) {
            outNodes[nodeIdx].leftFirst = start;
            outNodes[nodeIdx].triCount = count;
            return;
        }

        const uint32_t splitIdx = PartitionSahOrMedian<TriBuildRef>(refs, start, count, bounds);
        const uint32_t leftCount = splitIdx - start;
        const uint32_t rightCount = count - leftCount;
        if (leftCount == 0u || rightCount == 0u) {
            outNodes[nodeIdx].leftFirst = start;
            outNodes[nodeIdx].triCount = count;
            return;
        }

        outNodes[nodeIdx].triCount = 0u;

        // Push left child immediately so leftChild == nodeIdx + 1 holds across the entire tree
        const uint32_t leftIdx = static_cast<uint32_t>(outNodes.size());
        outNodes.emplace_back();
        self(self, leftIdx, start, leftCount, depth + 1u);

        const uint32_t rightIdx = static_cast<uint32_t>(outNodes.size());
        outNodes[nodeIdx].leftFirst = rightIdx;
        outNodes.emplace_back();
        self(self, rightIdx, splitIdx, rightCount, depth + 1u);
    };

    outNodes.emplace_back();
    subdivide(subdivide, 0u, 0u, triCount, 0u);

    outReorderedIndices.resize(static_cast<size_t>(triCount) * 3u);
    for (uint32_t t = 0; t < triCount; ++t) {
        const uint32_t origT = refs[t].origTriIdx;
        outReorderedIndices[t * 3u + 0u] = sourceIndices[origT * 3u + 0u];
        outReorderedIndices[t * 3u + 1u] = sourceIndices[origT * 3u + 1u];
        outReorderedIndices[t * 3u + 2u] = sourceIndices[origT * 3u + 2u];
    }

    return static_cast<uint32_t>(outNodes.size());
}

void RayTracingScene::BuildFromScene(const scene::Scene& scene) {
    vertices_.clear();
    indices_.clear();
    blasNodes_.clear();
    materials_.clear();
    instances_.clear();
    tlasNodes_.clear();
    primSliceMap_.clear();
    hasSceneData_ = false;
    stats_ = {};

    // 1. Pack materials (index 0 = default PBR material fallback if prim.materialIndex < 0)
    const uint32_t maxTexSlot = supportsBindlessTextures_ ? (kMaxBindlessTextures - 1u) : 0u;
    auto mapTexSlot = [maxTexSlot](int texIdx) -> int32_t {
        if (texIdx < 0 || maxTexSlot == 0u) {
            return 0;
        }
        const uint32_t slot = static_cast<uint32_t>(texIdx) + 1u;
        return (slot <= maxTexSlot) ? static_cast<int32_t>(slot) : 0;
    };

    materials_.reserve(scene.materials.size() + 1u);
    materials_.push_back(RtMaterial{}); // slot 0 = default material
    for (const auto& mat : scene.materials) {
        RtMaterial rtMat{};
        rtMat.baseColorFactor = mat.baseColorFactor;
        rtMat.emissiveFactor = glm::vec4{mat.emissiveFactor * mat.emissiveStrength, mat.emissiveStrength};
        rtMat.pbrParams = glm::vec4{mat.metallicFactor, mat.roughnessFactor, mat.alphaCutoff, mat.ior};
        rtMat.texIndices = glm::ivec4{
            mapTexSlot(mat.baseColorTextureIndex),
            mapTexSlot(mat.metallicRoughnessTextureIndex),
            mapTexSlot(mat.normalTextureIndex),
            mapTexSlot(mat.emissiveTextureIndex),
        };
        rtMat.flagsAlpha = glm::uvec4{
            PackMaterialFlags(mat),
            static_cast<uint32_t>(mat.alphaMode),
            mat.doubleSided ? 1u : 0u,
            (mat.transmissionFactor > 0.05f) ? 1u : 0u,
        };
        materials_.push_back(rtMat);
    }

    // 2. Collect unique MeshPrimitives across shared meshes and standalone nodes
    std::vector<const scene::MeshPrimitive*> uniquePrims;
    auto collectPrim = [&](const scene::MeshPrimitive* p) {
        if (!p || p->vertices.empty()) {
            return;
        }
        if (primSliceMap_.find(p) == primSliceMap_.end()) {
            primSliceMap_.emplace(p, PrimitiveBlasSlice{});
            uniquePrims.push_back(p);
        }
    };

    for (const auto& mesh : scene.meshes) {
        if (!mesh) {
            continue;
        }
        for (const auto& prim : mesh->primitives) {
            collectPrim(&prim);
        }
    }
    for (const auto& node : scene.allNodes) {
        if (!node) {
            continue;
        }
        for (const auto& prim : node->GetPrimitives()) {
            collectPrim(&prim);
        }
    }

    if (uniquePrims.empty()) {
        return;
    }

    // 3. Build BLAS per unique primitive in parallel
    struct PrimBuildResult {
        std::vector<RtVertex> verts{};
        std::vector<RtBvhNode> nodes{};
        std::vector<uint32_t> triIndices{};
        core::AABB bounds{};
    };
    std::vector<PrimBuildResult> buildResults(uniquePrims.size());

    auto buildOne = [&](size_t idx) {
        const scene::MeshPrimitive& prim = *uniquePrims[idx];
        auto& out = buildResults[idx];

        const auto& srcVerts = (!prim.baseVertices.empty() && prim.baseVertices.size() == prim.vertices.size())
                                   ? prim.baseVertices
                                   : prim.vertices;
        out.verts.resize(srcVerts.size());
        for (size_t v = 0; v < srcVerts.size(); ++v) {
            out.verts[v].posU = glm::vec4{srcVerts[v].position, srcVerts[v].uv.x};
            out.verts[v].normV = glm::vec4{srcVerts[v].normal, srcVerts[v].uv.y};
            out.bounds.Expand(srcVerts[v].position);
        }

        std::vector<uint32_t> tempSeqIndices;
        std::span<const uint32_t> srcIndices;
        if (!prim.indices.empty()) {
            srcIndices = std::span<const uint32_t>{prim.indices.data(), prim.indices.size()};
        } else {
            tempSeqIndices.resize(srcVerts.size());
            std::iota(tempSeqIndices.begin(), tempSeqIndices.end(), 0u);
            srcIndices = tempSeqIndices;
        }

        BuildBinnedSahBlas(out.verts, srcIndices, out.nodes, out.triIndices);
    };

#if !defined(__EMSCRIPTEN__)
    if (uniquePrims.size() > 1) {
        core::ThreadPool::Instance().ParallelFor(0, uniquePrims.size(), buildOne);
    } else {
        buildOne(0);
    }
#else
    for (size_t i = 0; i < uniquePrims.size(); ++i) {
        buildOne(i);
    }
#endif

    // 4. Concatenate primitive BLASes into flat global buffers
    size_t totalVerts = 0;
    size_t totalIndices = 0;
    size_t totalNodes = 0;
    for (const auto& br : buildResults) {
        totalVerts += br.verts.size();
        totalIndices += br.triIndices.size();
        totalNodes += br.nodes.size();
    }
    vertices_.reserve(totalVerts);
    indices_.reserve(totalIndices);
    blasNodes_.reserve(totalNodes);

    uint32_t builtBlasCount = 0;
    for (size_t i = 0; i < uniquePrims.size(); ++i) {
        const auto& br = buildResults[i];
        if (br.nodes.empty() || br.triIndices.empty()) {
            continue;
        }
        PrimitiveBlasSlice slice{};
        slice.blasRootNode = static_cast<uint32_t>(blasNodes_.size());
        slice.vertexOffset = static_cast<uint32_t>(vertices_.size());
        slice.triOffset = static_cast<uint32_t>(indices_.size() / 3u);
        slice.triCount = static_cast<uint32_t>(br.triIndices.size() / 3u);
        slice.localBounds = br.bounds;
        primSliceMap_[uniquePrims[i]] = slice;

        vertices_.insert(vertices_.end(), br.verts.begin(), br.verts.end());
        indices_.insert(indices_.end(), br.triIndices.begin(), br.triIndices.end());
        blasNodes_.insert(blasNodes_.end(), br.nodes.begin(), br.nodes.end());
        ++builtBlasCount;
    }

    stats_.blasCount = builtBlasCount;
    stats_.blasNodeCount = static_cast<uint32_t>(blasNodes_.size());
    stats_.triangleCount = static_cast<uint32_t>(indices_.size() / 3u);
    stats_.materialCount = static_cast<uint32_t>(materials_.size());

    UploadStaticGeometryBuffers();
    RebuildTlasInternal(scene, /*forceUpload=*/true);
    RebuildBindGroups(&scene);
    hasSceneData_ = !instances_.empty() && !tlasNodes_.empty();
}

void RayTracingScene::UpdateTlas(const scene::Scene& scene) {
    if (!hasSceneData_) {
        return;
    }
    if (&scene == lastScene_ && scene.transformVersion_ == lastTransformVersion_) {
        return;
    }
    RebuildTlasInternal(scene, /*forceUpload=*/false);
    RebuildBindGroups(&scene);
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
        auto prims = node->GetPrimitives();
        for (const auto& prim : prims) {
            auto it = primSliceMap_.find(&prim);
            if (it == primSliceMap_.end() || it->second.triCount == 0u) {
                continue;
            }
            const PrimitiveBlasSlice& slice = it->second;
            const uint32_t matIdx =
                (prim.materialIndex >= 0 && static_cast<size_t>(prim.materialIndex + 1) < materials_.size())
                    ? static_cast<uint32_t>(prim.materialIndex + 1)
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

        tlasNodes_.reserve(instCount * 2u);
        auto subdivideTlas = [&](auto& self, uint32_t nodeIdx, uint32_t start, uint32_t count, uint32_t depth) -> void {
            core::AABB bounds{};
            for (uint32_t i = 0; i < count; ++i) {
                bounds.Expand(refs[start + i].bounds);
            }
            tlasNodes_[nodeIdx].aabbMin = bounds.minBound;
            tlasNodes_[nodeIdx].aabbMax = bounds.maxBound;

            if (count <= kMaxLeafInstances || depth >= kMaxBvhDepth) {
                tlasNodes_[nodeIdx].leftFirst = start;
                tlasNodes_[nodeIdx].triCount = count;
                return;
            }

            const uint32_t splitIdx = PartitionSahOrMedian<InstBuildRef>(refs, start, count, bounds);
            const uint32_t leftCount = splitIdx - start;
            const uint32_t rightCount = count - leftCount;
            if (leftCount == 0u || rightCount == 0u) {
                tlasNodes_[nodeIdx].leftFirst = start;
                tlasNodes_[nodeIdx].triCount = count;
                return;
            }

            tlasNodes_[nodeIdx].triCount = 0u;
            const uint32_t leftIdx = static_cast<uint32_t>(tlasNodes_.size());
            tlasNodes_.emplace_back();
            self(self, leftIdx, start, leftCount, depth + 1u);

            const uint32_t rightIdx = static_cast<uint32_t>(tlasNodes_.size());
            tlasNodes_[nodeIdx].leftFirst = rightIdx;
            tlasNodes_.emplace_back();
            self(self, rightIdx, splitIdx, rightCount, depth + 1u);
        };

        tlasNodes_.emplace_back();
        subdivideTlas(subdivideTlas, 0u, 0u, instCount, 0u);

        // Reorder instances_ in leaf order so TLAS leaves index contiguously
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

void RayTracingScene::UploadStaticGeometryBuffers() {
    if (!initialized_ || !device_ || !queue_) {
        return;
    }
    UploadOrRecreateStorageBuffer(device_, queue_, blasNodesBuffer_, blasBufferBytes_, blasNodes_.data(),
                                  blasNodes_.size() * sizeof(RtBvhNode), "RT_BLASNodes_SSBO");
    UploadOrRecreateStorageBuffer(device_, queue_, indicesBuffer_, indicesBufferBytes_, indices_.data(),
                                  indices_.size() * sizeof(uint32_t), "RT_Indices_SSBO");
    UploadOrRecreateStorageBuffer(device_, queue_, verticesBuffer_, verticesBufferBytes_, vertices_.data(),
                                  vertices_.size() * sizeof(RtVertex), "RT_Vertices_SSBO");
    UploadOrRecreateStorageBuffer(device_, queue_, materialsBuffer_, materialsBufferBytes_, materials_.data(),
                                  materials_.size() * sizeof(RtMaterial), "RT_Materials_SSBO");
}

void RayTracingScene::UploadTlasBuffers() {
    if (!initialized_ || !device_ || !queue_) {
        return;
    }
    UploadOrRecreateStorageBuffer(device_, queue_, tlasNodesBuffer_, tlasBufferBytes_, tlasNodes_.data(),
                                  tlasNodes_.size() * sizeof(RtBvhNode), "RT_TLASNodes_SSBO");
    UploadOrRecreateStorageBuffer(device_, queue_, instancesBuffer_, instancesBufferBytes_, instances_.data(),
                                  instances_.size() * sizeof(RtInstance), "RT_Instances_SSBO");
}

void RayTracingScene::RebuildBindGroups(const scene::Scene* scene) {
#if defined(BGL_PLATFORM_NATIVE)
    if (!initialized_ || !device_ || !sceneBgl_) {
        return;
    }
    sceneBindGroup_.Reset();
    bindlessBindGroup_.Reset();

    if (!tlasNodesBuffer_ || !instancesBuffer_ || !blasNodesBuffer_ || !indicesBuffer_ || !verticesBuffer_ ||
        !materialsBuffer_ || tlasNodes_.empty() || instances_.empty()) {
        stats_.hasGpuBuffers = false;
        return;
    }

    std::array<WGPUBindGroupEntry, 6> entries{};
    auto bindBuf = [&](uint32_t idx, WGPUBuffer buf, uint64_t size) {
        entries[idx] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[idx].binding = idx;
        entries[idx].buffer = buf;
        entries[idx].offset = 0;
        entries[idx].size = size;
    };
    bindBuf(0, tlasNodesBuffer_.Get(), std::max<uint64_t>(sizeof(RtBvhNode), tlasNodes_.size() * sizeof(RtBvhNode)));
    bindBuf(1, instancesBuffer_.Get(), std::max<uint64_t>(sizeof(RtInstance), instances_.size() * sizeof(RtInstance)));
    bindBuf(2, blasNodesBuffer_.Get(), std::max<uint64_t>(sizeof(RtBvhNode), blasNodes_.size() * sizeof(RtBvhNode)));
    bindBuf(3, indicesBuffer_.Get(), std::max<uint64_t>(sizeof(uint32_t), indices_.size() * sizeof(uint32_t)));
    bindBuf(4, verticesBuffer_.Get(), std::max<uint64_t>(sizeof(RtVertex), vertices_.size() * sizeof(RtVertex)));
    bindBuf(5, materialsBuffer_.Get(), std::max<uint64_t>(sizeof(RtMaterial), materials_.size() * sizeof(RtMaterial)));

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = sceneBgl_.Get();
    bgDesc.entryCount = entries.size();
    bgDesc.entries = entries.data();
    bgDesc.label = ToWgpuStringView("RayTracingScene_BindGroup");
    sceneBindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    stats_.hasGpuBuffers = (sceneBindGroup_ != nullptr);

    if (supportsBindlessTextures_ && bindlessBgl_ && fallbackWhiteView_ && defaultSampler_) {
        std::array<WGPUTextureView, kMaxBindlessTextures> texViews{};
        texViews.fill(fallbackWhiteView_);
        uint32_t boundCount = 1u; // slot 0 = white fallback
        if (scene) {
            const size_t copyCount = std::min<size_t>(scene->gpuTextures.size(), kMaxBindlessTextures - 1u);
            for (size_t i = 0; i < copyCount; ++i) {
                if (scene->gpuTextures[i].view) {
                    texViews[i + 1u] = scene->gpuTextures[i].view;
                    ++boundCount;
                }
            }
        }
        stats_.bindlessTextureCount = boundCount;

        WGPUBindGroupEntry samplerEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        samplerEntry.binding = 0;
        samplerEntry.sampler = defaultSampler_;

        WGPUBindGroupEntryExtras texArrayExtras{};
        texArrayExtras.chain.sType = static_cast<WGPUSType>(WGPUSType_BindGroupEntryExtras);
        texArrayExtras.textureViews = texViews.data();
        texArrayExtras.textureViewCount = texViews.size();

        WGPUBindGroupEntry texArrayEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        texArrayEntry.nextInChain = &texArrayExtras.chain;
        texArrayEntry.binding = 1;

        std::array<WGPUBindGroupEntry, 2> blEntries{samplerEntry, texArrayEntry};
        WGPUBindGroupDescriptor blBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        blBgDesc.layout = bindlessBgl_.Get();
        blBgDesc.entryCount = blEntries.size();
        blBgDesc.entries = blEntries.data();
        blBgDesc.label = ToWgpuStringView("RayTracingBindless_BindGroup");
        bindlessBindGroup_ = wgpuDeviceCreateBindGroup(device_, &blBgDesc);
    }
#else
    (void)scene;
#endif
}

RtCpuHit RayTracingScene::TraceRayCpu(const core::Ray& ray, float maxDist) const noexcept {
    RtCpuHit best{};
    best.distance = maxDist;
    if (tlasNodes_.empty() || instances_.empty() || blasNodes_.empty()) {
        return best;
    }

    auto intersectAabbT = [](const glm::vec3& ro, const glm::vec3& invD, const glm::vec3& bmin, const glm::vec3& bmax,
                             float tMax) noexcept -> float {
        const glm::vec3 t0 = (bmin - ro) * invD;
        const glm::vec3 t1 = (bmax - ro) * invD;
        const glm::vec3 tSmall = glm::min(t0, t1);
        const glm::vec3 tBig = glm::max(t0, t1);
        const float tmin = std::max(0.0f, std::max(tSmall.x, std::max(tSmall.y, tSmall.z)));
        const float tmax = std::min(tMax, std::min(tBig.x, std::min(tBig.y, tBig.z)));
        return (tmax >= tmin) ? tmin : 1e30f;
    };

    auto safeInvDir = [](const glm::vec3& d) noexcept -> glm::vec3 {
        return glm::vec3{
            std::abs(d.x) > 1e-8f ? (1.0f / d.x) : std::copysign(1e8f, d.x),
            std::abs(d.y) > 1e-8f ? (1.0f / d.y) : std::copysign(1e8f, d.y),
            std::abs(d.z) > 1e-8f ? (1.0f / d.z) : std::copysign(1e8f, d.z),
        };
    };

    const glm::vec3 worldInvD = safeInvDir(ray.direction);
    std::array<uint32_t, 64> tlasStack{};
    uint32_t tlasSp = 0u;
    tlasStack[tlasSp++] = 0u;

    while (tlasSp > 0u) {
        const uint32_t nodeIdx = tlasStack[--tlasSp];
        const RtBvhNode& node = tlasNodes_[nodeIdx];
        if (intersectAabbT(ray.origin, worldInvD, node.aabbMin, node.aabbMax, best.distance) >= best.distance) {
            continue;
        }
        if (node.triCount > 0u) {
            for (uint32_t i = 0u; i < node.triCount; ++i) {
                const uint32_t instIdx = node.leftFirst + i;
                const RtInstance& inst = instances_[instIdx];
                if (intersectAabbT(ray.origin, worldInvD, inst.aabbMin, inst.aabbMax, best.distance) >= best.distance) {
                    continue;
                }

                const glm::vec3 localRo = glm::vec3{inst.invModel * glm::vec4{ray.origin, 1.0f}};
                const glm::vec3 localRd = glm::vec3{inst.invModel * glm::vec4{ray.direction, 0.0f}};
                const glm::vec3 localInvD = safeInvDir(localRd);

                std::array<uint32_t, 64> blasStack{};
                uint32_t blasSp = 0u;
                blasStack[blasSp++] = 0u;

                while (blasSp > 0u) {
                    const uint32_t localNodeIdx = blasStack[--blasSp];
                    const RtBvhNode& bNode = blasNodes_[inst.blasRootNode + localNodeIdx];
                    if (intersectAabbT(localRo, localInvD, bNode.aabbMin, bNode.aabbMax, best.distance) >=
                        best.distance) {
                        continue;
                    }
                    if (bNode.triCount > 0u) {
                        for (uint32_t t = 0u; t < bNode.triCount; ++t) {
                            const uint32_t localTri = bNode.leftFirst + t;
                            const uint32_t gTri = inst.triOffset + localTri;
                            const glm::vec3 p0 =
                                glm::vec3{vertices_[inst.vertexOffset + indices_[gTri * 3u + 0u]].posU};
                            const glm::vec3 p1 =
                                glm::vec3{vertices_[inst.vertexOffset + indices_[gTri * 3u + 1u]].posU};
                            const glm::vec3 p2 =
                                glm::vec3{vertices_[inst.vertexOffset + indices_[gTri * 3u + 2u]].posU};

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
                                best.worldPos = ray.At(hitT);
                                const glm::vec3 n0 =
                                    glm::vec3{vertices_[inst.vertexOffset + indices_[gTri * 3u + 0u]].normV};
                                const glm::vec3 n1 =
                                    glm::vec3{vertices_[inst.vertexOffset + indices_[gTri * 3u + 1u]].normV};
                                const glm::vec3 n2 =
                                    glm::vec3{vertices_[inst.vertexOffset + indices_[gTri * 3u + 2u]].normV};
                                const float w = 1.0f - u - v;
                                const glm::vec3 localN = n0 * w + n1 * u + n2 * v;
                                const glm::vec3 worldN = glm::transpose(glm::mat3{inst.invModel}) * localN;
                                const float nLen = glm::length(worldN);
                                best.worldNormal = (nLen > 1e-6f) ? (worldN / nLen) : glm::vec3{0.0f, 1.0f, 0.0f};
                            }
                        }
                    } else {
                        const uint32_t leftLocal = localNodeIdx + 1u;
                        const uint32_t rightLocal = bNode.leftFirst;
                        const float dL =
                            intersectAabbT(localRo, localInvD, blasNodes_[inst.blasRootNode + leftLocal].aabbMin,
                                           blasNodes_[inst.blasRootNode + leftLocal].aabbMax, best.distance);
                        const float dR =
                            intersectAabbT(localRo, localInvD, blasNodes_[inst.blasRootNode + rightLocal].aabbMin,
                                           blasNodes_[inst.blasRootNode + rightLocal].aabbMax, best.distance);
                        if (dL < dR) {
                            if (dR < best.distance && blasSp < 64u) {
                                blasStack[blasSp++] = rightLocal;
                            }
                            if (dL < best.distance && blasSp < 64u) {
                                blasStack[blasSp++] = leftLocal;
                            }
                        } else {
                            if (dL < best.distance && blasSp < 64u) {
                                blasStack[blasSp++] = leftLocal;
                            }
                            if (dR < best.distance && blasSp < 64u) {
                                blasStack[blasSp++] = rightLocal;
                            }
                        }
                    }
                }
            }
        } else {
            const uint32_t leftIdx = nodeIdx + 1u;
            const uint32_t rightIdx = node.leftFirst;
            const float dL = intersectAabbT(ray.origin, worldInvD, tlasNodes_[leftIdx].aabbMin,
                                            tlasNodes_[leftIdx].aabbMax, best.distance);
            const float dR = intersectAabbT(ray.origin, worldInvD, tlasNodes_[rightIdx].aabbMin,
                                            tlasNodes_[rightIdx].aabbMax, best.distance);
            if (dL < dR) {
                if (dR < best.distance && tlasSp < 64u) {
                    tlasStack[tlasSp++] = rightIdx;
                }
                if (dL < best.distance && tlasSp < 64u) {
                    tlasStack[tlasSp++] = leftIdx;
                }
            } else {
                if (dL < best.distance && tlasSp < 64u) {
                    tlasStack[tlasSp++] = leftIdx;
                }
                if (dR < best.distance && tlasSp < 64u) {
                    tlasStack[tlasSp++] = rightIdx;
                }
            }
        }
    }

    return best;
}

} // namespace bgl::gpu
