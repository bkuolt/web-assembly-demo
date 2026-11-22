/**
 * @file RayTracingScene.cpp
 * @brief GPU buffer uploads, material/geometry packing, and bindless bind groups for RayTracingScene.
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
#include <numeric>

#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

namespace {

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
    materials_.push_back(RtMaterial{});
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

    // 2. Collect unique MeshPrimitives across shared meshes and scene nodes
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

} // namespace bgl::gpu
