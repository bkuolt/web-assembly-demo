/**
 * @file PipelineScene.cpp
 * @brief Scene GPU resource upload, material bind-group management, and KTX2 re-transcoding.
 */
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "core/ThreadPool.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/PipelineHelpers.hpp"
#include "scene/Scene.hpp"
#include "scene/TextureDecoder.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <span>
#include <unordered_set>
#include <utility>

namespace bgl::gpu {
using namespace detail;

namespace {

struct PendingGpuMipGen {
    WGPUTexture texture{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t mipLevels{1};
    bool isSrgb{false};
    bool isNormal{false};
};

struct PrimitiveStaging {
    std::vector<uint8_t> indexBytes{};
    bool useUint16{false};
    std::vector<uint32_t> wfIndices{};
    std::vector<glm::vec4> morphDeltas{};
    uint32_t morphTargetCount{0};
    uint32_t morphVertexCount{0};
};

/// Creates a 4-byte-aligned GPU buffer, tracks its VRAM size, and optionally uploads initial bytes.
[[nodiscard]] WGPUBuffer CreateAndUploadTrackedBuffer(WGPUDevice device, WGPUQueue queue, size_t byteSize,
                                                      WGPUBufferUsage usage, const char* label, const void* initData,
                                                      size_t writeBytes, uint64_t& outTrackedSize) {
    if (!device || byteSize == 0u || byteSize > (std::numeric_limits<size_t>::max() - 3u)) {
        return nullptr;
    }
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = (byteSize + 3u) & ~size_t{3};
    desc.usage = usage;
    desc.label = ToWgpuStringView(label);
    WGPUBuffer buf = wgpuDeviceCreateBuffer(device, &desc);
    if (buf) {
        outTrackedSize = desc.size;
        core::MemoryTracker::Instance().TrackBufferAlloc(outTrackedSize);
        if (initData && writeBytes > 0 && writeBytes <= desc.size) {
            wgpuQueueWriteBuffer(queue, buf, 0, initData, writeBytes);
        }
    }
    return buf;
}

/// Identifies sRGB color textures and normal maps across scene materials to pick hardware WebGPU texture formats.
void ClassifySceneTextureRoles(const scene::Scene& scene, std::vector<bool>& isNormalTexture,
                               std::vector<bool>& isSrgbTexture) {
    isNormalTexture.assign(scene.cpuImages.size(), false);
    isSrgbTexture.assign(scene.cpuImages.size(), false);
    auto mark = [&](int idx, bool isNorm, bool isSrgb) {
        if (idx >= 0 && static_cast<size_t>(idx) < isSrgbTexture.size()) {
            isNormalTexture[idx] = isNorm;
            isSrgbTexture[idx] = isSrgb;
        }
    };
    for (const auto& mat : scene.materials) {
        mark(mat.baseColorTextureIndex, false, true);
        mark(mat.emissiveTextureIndex, false, true);
        mark(mat.sheenColorTextureIndex, false, true);
        mark(mat.normalTextureIndex, true, false);
        mark(mat.metallicRoughnessTextureIndex, false, false);
        mark(mat.occlusionTextureIndex, false, false);
    }
}

void UploadCompressedSceneTexture(WGPUDevice device, WGPUQueue queue, WGPUSampler defaultSampler, size_t idx,
                                  const scene::CpuImage& img, bool isSrgb, scene::TextureGpuResource& dstTex) {
    WGPUTextureFormat wgpuFmt{WGPUTextureFormat_BC7RGBAUnorm};
    std::string_view fmtName{"BC7"};
    switch (static_cast<scene::KtxTranscodeTarget>(img.compressionFormat)) {
    case scene::KtxTranscodeTarget::Bc7:
        wgpuFmt = isSrgb ? WGPUTextureFormat_BC7RGBAUnormSrgb : WGPUTextureFormat_BC7RGBAUnorm;
        fmtName = isSrgb ? "BC7RGBAUnormSrgb" : "BC7RGBAUnorm";
        break;
    case scene::KtxTranscodeTarget::Astc4x4:
        wgpuFmt = isSrgb ? WGPUTextureFormat_ASTC4x4UnormSrgb : WGPUTextureFormat_ASTC4x4Unorm;
        fmtName = isSrgb ? "ASTC4x4UnormSrgb" : "ASTC4x4Unorm";
        break;
    case scene::KtxTranscodeTarget::Etc2Rgba:
        wgpuFmt = isSrgb ? WGPUTextureFormat_ETC2RGBA8UnormSrgb : WGPUTextureFormat_ETC2RGBA8Unorm;
        fmtName = isSrgb ? "ETC2RGBA8UnormSrgb" : "ETC2RGBA8Unorm";
        break;
    default:
        break;
    }

    if (img.width == 0 || img.height == 0 || img.width > 16384u || img.height > 16384u) {
        return;
    }

    uint32_t validMips{0};
    for (const auto& mip : img.mipLevels) {
        if (mip.width == 0 || mip.height == 0 || (mip.width % 4u) != 0u || (mip.height % 4u) != 0u) {
            break;
        }
        const uint64_t bx{std::max<uint64_t>(1u, (static_cast<uint64_t>(mip.width) + 3u) / 4u)};
        const uint64_t by{std::max<uint64_t>(1u, (static_cast<uint64_t>(mip.height) + 3u) / 4u)};
        const uint64_t requiredBytes{bx * by * 16ULL};
        if (mip.data.size() < requiredBytes) {
            break;
        }
        ++validMips;
    }
    if (validMips == 0u) {
        const uint64_t bx{std::max<uint64_t>(1u, (static_cast<uint64_t>(img.width) + 3u) / 4u)};
        const uint64_t by{std::max<uint64_t>(1u, (static_cast<uint64_t>(img.height) + 3u) / 4u)};
        const uint64_t requiredBytes{bx * by * 16ULL};
        if ((img.width % 4u) != 0u || (img.height % 4u) != 0u || img.rgba.size() < requiredBytes) {
            return;
        }
        validMips = 1u;
    }

    WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texDesc.dimension = WGPUTextureDimension_2D;
    texDesc.size = WGPUExtent3D{img.width, img.height, 1};
    texDesc.format = wgpuFmt;
    texDesc.mipLevelCount = validMips;
    texDesc.sampleCount = 1;
    texDesc.label = ToWgpuStringView("glTF_KTX2_CompressedTexture");
    WGPUTexture tex = wgpuDeviceCreateTexture(device, &texDesc);
    if (!tex) {
        BGL_LOG_WARN("[WebGPU Upload] Failed to create compressed 2D texture #{} ({}x{})", idx, img.width, img.height);
        return;
    }

    size_t compressedVramBytes{0};
    for (uint32_t level{0}; level < validMips; ++level) {
        const uint32_t mipW{(level < img.mipLevels.size()) ? img.mipLevels[level].width : img.width};
        const uint32_t mipH{(level < img.mipLevels.size()) ? img.mipLevels[level].height : img.height};
        const std::span<const uint8_t> mipBytes{
            (level < img.mipLevels.size())
                ? std::span<const uint8_t>{img.mipLevels[level].data.data(), img.mipLevels[level].data.size()}
                : std::span<const uint8_t>{img.rgba.data(), img.rgba.size()}};
        const uint32_t blocksX{std::max(1u, (mipW + 3u) / 4u)};
        const uint32_t blocksY{std::max(1u, (mipH + 3u) / 4u)};
        const uint64_t requiredBytes{static_cast<uint64_t>(blocksX) * blocksY * 16ULL};
        if (mipBytes.size() < requiredBytes) {
            break;
        }

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = tex;
        copyDst.mipLevel = level;
        copyDst.origin = WGPUOrigin3D{0, 0, 0};
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = blocksX * 16u;
        dataLayout.rowsPerImage = blocksY;

        WGPUExtent3D writeSize{mipW, mipH, 1};
        wgpuQueueWriteTexture(queue, &copyDst, mipBytes.data(), mipBytes.size(), &dataLayout, &writeSize);
        compressedVramBytes += static_cast<size_t>(requiredBytes);
    }

    WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
    if (!view) {
        wgpuTextureRelease(tex);
        return;
    }
    dstTex.texture = tex;
    dstTex.view = view;
    dstTex.sampler = defaultSampler;
    dstTex.width = img.width;
    dstTex.height = img.height;
    dstTex.vramBytes = compressedVramBytes;
    dstTex.format = img.compressionFormat;
    BGL_LOG_DEBUG(
        "[WebGPU Upload] Texture #{}: Native Compressed 2D Texture ({}x{}, {}, {} Mip Levels, {:.2f} KB VRAM)", idx,
        img.width, img.height, fmtName, validMips, static_cast<double>(compressedVramBytes) / 1024.0);
}

void UploadRgbaSceneTexture(WGPUDevice device, WGPUQueue queue, WGPUSampler defaultSampler, size_t idx,
                            const scene::CpuImage& img, bool isSrgb, bool isNormal, uint32_t mipLevels,
                            scene::TextureGpuResource& dstTex, std::vector<PendingGpuMipGen>& pendingMips) {
    WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_RenderAttachment;
    texDesc.dimension = WGPUTextureDimension_2D;
    texDesc.size = WGPUExtent3D{img.width, img.height, 1};
    texDesc.format = isSrgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm;
    texDesc.mipLevelCount = mipLevels;
    texDesc.sampleCount = 1;
    texDesc.label = ToWgpuStringView("glTF_Texture");
    const uint64_t requiredRgbaBytes{static_cast<uint64_t>(img.width) * static_cast<uint64_t>(img.height) * 4ULL};
    if (img.width == 0 || img.height == 0 || img.width > 16384u || img.height > 16384u ||
        requiredRgbaBytes > std::numeric_limits<size_t>::max() || img.rgba.size() < requiredRgbaBytes) {
        BGL_LOG_WARN("[WebGPU Upload] Skipped RGBA texture #{} due to invalid dimensions or buffer size", idx);
        return;
    }
    WGPUTexture tex = wgpuDeviceCreateTexture(device, &texDesc);
    if (!tex) {
        BGL_LOG_WARN("[WebGPU Upload] Failed to create 2D RGBA texture #{} ({}x{})", idx, img.width, img.height);
        return;
    }

    WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copyDst.texture = tex;
    copyDst.mipLevel = 0;
    copyDst.origin = WGPUOrigin3D{0, 0, 0};
    copyDst.aspect = WGPUTextureAspect_All;

    WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    dataLayout.offset = 0;
    dataLayout.bytesPerRow = img.width * 4u;
    dataLayout.rowsPerImage = img.height;

    WGPUExtent3D writeSize{img.width, img.height, 1};
    wgpuQueueWriteTexture(queue, &copyDst, img.rgba.data(), static_cast<size_t>(requiredRgbaBytes), &dataLayout,
                          &writeSize);

    WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
    if (!view) {
        wgpuTextureRelease(tex);
        return;
    }

    if (mipLevels > 1) {
        pendingMips.push_back(PendingGpuMipGen{
            .texture = tex,
            .width = img.width,
            .height = img.height,
            .mipLevels = mipLevels,
            .isSrgb = isSrgb,
            .isNormal = isNormal,
        });
    }

    uint64_t rgbaVramBytes{0};
    uint32_t mw{img.width};
    uint32_t mh{img.height};
    for (uint32_t m{0}; m < mipLevels; ++m) {
        rgbaVramBytes += static_cast<uint64_t>(mw) * static_cast<uint64_t>(mh) * 4ULL;
        mw = std::max(1u, mw / 2u);
        mh = std::max(1u, mh / 2u);
    }

    dstTex.texture = tex;
    dstTex.view = view;
    dstTex.sampler = defaultSampler;
    dstTex.width = img.width;
    dstTex.height = img.height;
    dstTex.vramBytes = static_cast<size_t>(std::min<uint64_t>(rgbaVramBytes, std::numeric_limits<size_t>::max()));
    dstTex.format = scene::KtxTranscodeTarget::Rgba32;
    BGL_LOG_DEBUG("[WebGPU Upload] Texture #{}: Created 2D Texture ({}x{}, {}, {} Mip Levels, NormalMap={}), wrote "
                  "{} bytes mip0 ({:.2f} KB VRAM)",
                  idx, img.width, img.height, isSrgb ? "RGBA8UnormSrgb" : "RGBA8Unorm", mipLevels,
                  isNormal ? "Yes" : "No", img.rgba.size(), static_cast<double>(rgbaVramBytes) / 1024.0);
}

void StageSinglePrimitive(scene::MeshPrimitive& prim, PrimitiveStaging& st) {
    if (prim.vertices.empty()) {
        return;
    }

    if (!prim.gpuIndexBuffer && !prim.indices.empty()) {
        if (prim.lodIndices[0].empty()) {
            prim.GenerateLODs();
        }
        std::vector<uint32_t> combinedIndices;
        size_t totalLodIndices{0};
        for (size_t lvl{0}; lvl < 4; ++lvl) {
            totalLodIndices += prim.lodIndices[lvl].empty() ? prim.indices.size() : prim.lodIndices[lvl].size();
        }
        combinedIndices.reserve(totalLodIndices);
        for (size_t lvl{0}; lvl < 4; ++lvl) {
            const auto& srcIndices = prim.lodIndices[lvl];
            const std::span<const uint32_t> slice =
                srcIndices.empty() ? std::span<const uint32_t>{prim.indices.data(), prim.indices.size()}
                                   : std::span<const uint32_t>{srcIndices.data(), srcIndices.size()};
            prim.lodLevels[lvl].firstIndex = static_cast<uint32_t>(combinedIndices.size());
            prim.lodLevels[lvl].indexCount = static_cast<uint32_t>(slice.size());
            combinedIndices.insert(combinedIndices.end(), slice.begin(), slice.end());
        }

        st.useUint16 = (prim.vertices.size() < 65536);
        prim.indexFormat = st.useUint16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
        if (st.useUint16) {
            std::vector<uint16_t> indices16;
            indices16.reserve(combinedIndices.size() + 1);
            for (uint32_t vIdx : combinedIndices) {
                indices16.push_back(static_cast<uint16_t>(vIdx));
            }
            if (indices16.size() % 2 != 0) {
                indices16.push_back(0);
            }
            st.indexBytes.resize(indices16.size() * sizeof(uint16_t));
            std::memcpy(st.indexBytes.data(), indices16.data(), st.indexBytes.size());
        } else {
            const size_t combinedBytes{combinedIndices.size() * sizeof(uint32_t)};
            st.indexBytes.resize((combinedBytes + 3u) & ~size_t{3});
            std::memcpy(st.indexBytes.data(), combinedIndices.data(), combinedBytes);
        }
    }

    if (!prim.gpuWireframeIndexBuffer) {
        auto emitTriEdges = [&](uint32_t a, uint32_t b, uint32_t c) {
            st.wfIndices.insert(st.wfIndices.end(), {a, b, b, c, c, a});
        };
        if (!prim.indices.empty()) {
            st.wfIndices.reserve(prim.indices.size() * 2);
            for (size_t k{0}; k + 2 < prim.indices.size(); k += 3) {
                emitTriEdges(prim.indices[k], prim.indices[k + 1], prim.indices[k + 2]);
            }
        } else {
            st.wfIndices.reserve(prim.vertices.size() * 2);
            for (size_t k{0}; k + 2 < prim.vertices.size(); k += 3) {
                emitTriEdges(static_cast<uint32_t>(k), static_cast<uint32_t>(k + 1), static_cast<uint32_t>(k + 2));
            }
        }
    }

    if (!prim.gpuMorphDeltaBuffer && !prim.morphTargets.empty() && !prim.vertices.empty()) {
        constexpr uint64_t kMaxMorphBufferBytes{128ULL * 1024ULL * 1024ULL};
        constexpr uint32_t kMaxGpuMorphTargets{8u};
        const uint32_t vCount{static_cast<uint32_t>(prim.vertices.size())};
        const uint64_t perTargetBytes{static_cast<uint64_t>(vCount) * 3ULL * sizeof(glm::vec4)};
        const uint32_t maxTargetsByBytes{
            perTargetBytes > 0u ? static_cast<uint32_t>(kMaxMorphBufferBytes / perTargetBytes) : 0u};
        const uint32_t tCount{std::min<uint32_t>(
            {static_cast<uint32_t>(prim.morphTargets.size()), kMaxGpuMorphTargets, maxTargetsByBytes})};
        if (tCount > 0u) {
            st.morphDeltas.resize(static_cast<size_t>(tCount) * vCount * 3u, glm::vec4{0.0f});
            for (uint32_t t{0}; t < tCount; ++t) {
                const auto& mt = prim.morphTargets[t];
                for (uint32_t v{0}; v < vCount; ++v) {
                    const size_t base{(static_cast<size_t>(t) * vCount + v) * 3u};
                    if (v < mt.positionDeltas.size()) {
                        st.morphDeltas[base + 0u] = glm::vec4{mt.positionDeltas[v], 0.0f};
                    }
                    if (v < mt.normalDeltas.size()) {
                        st.morphDeltas[base + 1u] = glm::vec4{mt.normalDeltas[v], 0.0f};
                    }
                    if (v < mt.tangentDeltas.size()) {
                        st.morphDeltas[base + 2u] = glm::vec4{mt.tangentDeltas[v], 0.0f};
                    }
                }
            }
            st.morphTargetCount = tCount;
            st.morphVertexCount = vCount;
        }
    }
}

void UploadSceneSkins(WGPUDevice device, WGPUQueue queue, WGPUBindGroupLayout skinBgl, scene::Scene& scene) {
    for (auto& skin : scene.skins) {
        skin.cachedFrameId = 0;
        skin.cachedUploadedFrameId = 0;
        skin.cachedMeshNode = nullptr;
        skin.SetGpuContext(device, skinBgl);
        if (skin.joints.empty() || skin.gpuJointBuffer) {
            continue;
        }
        const size_t jointCount{std::max<size_t>(1, skin.joints.size())};
        const size_t bufSize{jointCount * sizeof(glm::mat4)};
        const std::vector<glm::mat4> identityJoints(jointCount, glm::mat4{1.0f});
        skin.gpuJointBuffer = CreateAndUploadTrackedBuffer(
            device, queue, bufSize, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, "SkinJointMatrices_SSBO",
            identityJoints.data(), bufSize, skin.gpuJointBufferSize);
        if (skin.gpuJointBuffer) {
            WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntry.binding = 0;
            bgEntry.buffer = skin.gpuJointBuffer;
            bgEntry.offset = 0;
            bgEntry.size = bufSize;

            WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgDesc.layout = skinBgl;
            bgDesc.entryCount = 1;
            bgDesc.entries = &bgEntry;
            bgDesc.label = ToWgpuStringView("SkinStorage_BindGroup");
            skin.gpuBindGroup = wgpuDeviceCreateBindGroup(device, &bgDesc);
            if (!skin.gpuBindGroup) {
                skin.ReleaseGpu();
            }
        }
    }

    // Pre-allocate isolated per-node GPU joint buffers when multiple nodes share the same skin index.
    std::unordered_set<const scene::SceneNode*> visitedNodes;
    auto prebindNode = [&](const scene::SceneNode* nodePtr) {
        if (!nodePtr || !visitedNodes.insert(nodePtr).second) {
            return;
        }
        if (nodePtr->skinIndex >= 0 && static_cast<size_t>(nodePtr->skinIndex) < scene.skins.size()) {
            WGPUBuffer outBuf{nullptr};
            WGPUBindGroup outBg{nullptr};
            bool outNeeds{false};
            scene.skins[static_cast<size_t>(nodePtr->skinIndex)].ResolveNodeGpuBinding(nodePtr, 0, outBuf, outBg,
                                                                                       outNeeds);
        }
    };
    for (const auto& node : scene.allNodes) {
        prebindNode(node.get());
    }
    auto walkRoots = [&](auto& self, const scene::SceneNode* nodePtr, uint32_t depth) -> void {
        if (!nodePtr || depth >= 256u) {
            return;
        }
        prebindNode(nodePtr);
        for (const auto& ch : nodePtr->children) {
            self(self, ch.get(), depth + 1u);
        }
    };
    for (const auto& root : scene.rootNodes) {
        walkRoots(walkRoots, root.get(), 0u);
    }
}

} // namespace

void PipelineManager::SetMipmappingEnabled(bool enabled, scene::Scene* activeScene) {
    if (mipmappingEnabled_ == enabled)
        return;
    mipmappingEnabled_ = enabled;
    if (activeScene) {
        RebuildMaterialBindGroups(*activeScene);
    }
}

void PipelineManager::RebuildMaterialBindGroups(scene::Scene& scene) {
    if (!device_ || !materialBgl_) {
        return;
    }

    auto getTexView = [&](int texIdx, WGPUTextureView fallback) -> WGPUTextureView {
        if (texIdx >= 0 && texIdx < static_cast<int>(scene.gpuTextures.size())) {
            if (scene.gpuTextures[texIdx].view) {
                return scene.gpuTextures[texIdx].view;
            }
        }
        return fallback;
    };

    WGPUSampler activeSampler{mipmappingEnabled_ ? defaultSampler_.Get() : noMipSampler_.Get()};

    std::vector<scene::MeshPrimitive*> uniquePrims;
    CollectUniquePrimitives(scene, uniquePrims);

    for (scene::MeshPrimitive* primPtr : uniquePrims) {
        if (!primPtr || !primPtr->gpuUniformBuffer) {
            continue;
        }
        auto& prim = *primPtr;
        if (prim.gpuBindGroup) {
            wgpuBindGroupRelease(prim.gpuBindGroup);
            prim.gpuBindGroup = nullptr;
        }

        int baseColorTexIdx{-1};
        int mrTexIdx{-1};
        int normalTexIdx{-1};
        int emissiveTexIdx{-1};
        int occlusionTexIdx{-1};
        int clearcoatTexIdx{-1};
        int transmissionTexIdx{-1};
        int sheenTexIdx{-1};

        if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
            const auto& mat = scene.materials[prim.materialIndex];
            baseColorTexIdx = mat.baseColorTextureIndex;
            mrTexIdx = mat.metallicRoughnessTextureIndex;
            normalTexIdx = mat.normalTextureIndex;
            emissiveTexIdx = mat.emissiveTextureIndex;
            occlusionTexIdx = mat.occlusionTextureIndex;
            clearcoatTexIdx = mat.clearcoatTextureIndex;
            transmissionTexIdx = mat.transmissionTextureIndex;
            sheenTexIdx = mat.sheenColorTextureIndex;
        }

        const uint64_t morphBufferBytes =
            (prim.gpuMorphDeltaBuffer && prim.gpuMorphTargetCount > 0 && prim.gpuMorphVertexCount > 0)
                ? (static_cast<uint64_t>(prim.gpuMorphTargetCount) * prim.gpuMorphVertexCount * 3u * sizeof(glm::vec4))
                : (3u * sizeof(glm::vec4));

        std::array<WGPUBindGroupEntry, 11> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = prim.gpuUniformBuffer;
        bgEntries[0].offset = 0;
        bgEntries[0].size = sizeof(MaterialUniforms);

        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].sampler = activeSampler;

        bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[2].binding = 2;
        bgEntries[2].textureView = getTexView(baseColorTexIdx, defaultWhiteTextureView_);

        bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[3].binding = 3;
        bgEntries[3].textureView = getTexView(mrTexIdx, defaultWhiteTextureView_);

        bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[4].binding = 4;
        bgEntries[4].textureView = getTexView(normalTexIdx, defaultNormalTextureView_);

        bgEntries[5] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[5].binding = 5;
        bgEntries[5].textureView = getTexView(emissiveTexIdx, defaultBlackTextureView_);

        bgEntries[6] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[6].binding = 6;
        bgEntries[6].textureView = getTexView(occlusionTexIdx, defaultWhiteTextureView_);

        bgEntries[7] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[7].binding = 7;
        bgEntries[7].textureView = getTexView(clearcoatTexIdx, defaultWhiteTextureView_);

        bgEntries[8] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[8].binding = 8;
        bgEntries[8].textureView = getTexView(transmissionTexIdx, defaultWhiteTextureView_);

        bgEntries[9] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[9].binding = 9;
        bgEntries[9].textureView = getTexView(sheenTexIdx, defaultWhiteTextureView_);

        bgEntries[10] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[10].binding = 10;
        bgEntries[10].buffer = prim.gpuMorphDeltaBuffer ? prim.gpuMorphDeltaBuffer : dummyMorphDeltaBuffer_.Get();
        bgEntries[10].offset = 0;
        bgEntries[10].size = morphBufferBytes;

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = materialBgl_.Get();
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        bgDesc.label = ToWgpuStringView("MaterialStatic_BindGroup");
        prim.gpuBindGroup = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    }
}

void PipelineManager::PrepareScene(scene::Scene& scene) {
    if (!device_ || !queue_ || !materialBgl_ || !skinBgl_) {
        return;
    }

    std::vector<bool> isNormalTexture;
    std::vector<bool> isSrgbTexture;
    ClassifySceneTextureRoles(scene, isNormalTexture, isSrgbTexture);

    // 1. Upload CPU-decoded images (native block-compressed or RGBA8 + queued GPU mip generation)
    std::vector<PendingGpuMipGen> pendingMipGens;
    pendingMipGens.reserve(scene.cpuImages.size());
    for (size_t i{0}; i < scene.cpuImages.size(); ++i) {
        const auto& img = scene.cpuImages[i];
        if (img.width == 0 || img.height == 0 || img.rgba.empty() || i >= scene.gpuTextures.size() ||
            scene.gpuTextures[i].view != nullptr) {
            continue;
        }
        if (img.isCompressed && img.compressionFormat != scene::KtxTranscodeTarget::Rgba32) {
            UploadCompressedSceneTexture(device_, queue_, defaultSampler_.Get(), i, img, isSrgbTexture[i],
                                         scene.gpuTextures[i]);
        } else {
            const uint32_t mipLevels{CalculateMipLevels(img.width, img.height)};
            UploadRgbaSceneTexture(device_, queue_, defaultSampler_.Get(), i, img, isSrgbTexture[i], isNormalTexture[i],
                                   mipLevels, scene.gpuTextures[i], pendingMipGens);
        }
    }

    if (!pendingMipGens.empty()) {
        if (CommandEncoderHandle mipEnc{wgpuDeviceCreateCommandEncoder(device_, nullptr)}) {
            for (const auto& job : pendingMipGens) {
                GenerateTextureMipsGpu(mipEnc.Get(), job.texture, job.width, job.height, job.mipLevels, job.isSrgb,
                                       job.isNormal);
            }
            if (CommandBufferHandle mipCmd{wgpuCommandEncoderFinish(mipEnc.Get(), nullptr)}) {
                WGPUCommandBuffer rawCmd{mipCmd.Get()};
                wgpuQueueSubmit(queue_, 1, &rawCmd);
            }
        }
    }

    // 2. Stage Multi-LOD indices, wireframe edges, and morph deltas across unique primitives
    std::vector<scene::MeshPrimitive*> uniquePrims;
    CollectUniquePrimitives(scene, uniquePrims);
    std::vector<PrimitiveStaging> stagings(uniquePrims.size());

#if !defined(__EMSCRIPTEN__)
    if (uniquePrims.size() > 1) {
        core::ThreadPool::Instance().ParallelFor(
            0, uniquePrims.size(), [&](size_t idx) { StageSinglePrimitive(*uniquePrims[idx], stagings[idx]); });
    } else if (uniquePrims.size() == 1) {
        StageSinglePrimitive(*uniquePrims[0], stagings[0]);
    }
#else
    for (size_t idx{0}; idx < uniquePrims.size(); ++idx) {
        StageSinglePrimitive(*uniquePrims[idx], stagings[idx]);
    }
#endif

    // 3. Upload GPU vertex/index/wireframe/morph/material buffers
    for (size_t idx{0}; idx < uniquePrims.size(); ++idx) {
        scene::MeshPrimitive& prim = *uniquePrims[idx];
        const auto& st = stagings[idx];
        if (prim.vertices.empty()) {
            continue;
        }
        if (!prim.gpuVertexBuffer) {
            const auto& srcVerts = (!prim.morphTargets.empty() && prim.baseVertices.size() == prim.vertices.size())
                                       ? prim.baseVertices
                                       : prim.vertices;
            const size_t vbBytes{srcVerts.size() * sizeof(core::Vertex)};
            prim.gpuVertexBuffer =
                CreateAndUploadTrackedBuffer(device_, queue_, vbBytes, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                             "VertexBuffer", srcVerts.data(), vbBytes, prim.gpuVertexBufferSize);
            if (prim.gpuVertexBuffer) {
                prim.morphDirty = false;
            }
        }
        if (!prim.gpuIndexBuffer && !st.indexBytes.empty()) {
            prim.gpuIndexBuffer = CreateAndUploadTrackedBuffer(
                device_, queue_, st.indexBytes.size(), WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst,
                st.useUint16 ? "IndexBuffer_U16_MultiLOD" : "IndexBuffer_U32_MultiLOD", st.indexBytes.data(),
                st.indexBytes.size(), prim.gpuIndexBufferSize);
        }
        if (!prim.gpuWireframeIndexBuffer && !st.wfIndices.empty()) {
            prim.wireframeIndexCount = static_cast<uint32_t>(st.wfIndices.size());
            const size_t wfBytes{st.wfIndices.size() * sizeof(uint32_t)};
            prim.gpuWireframeIndexBuffer = CreateAndUploadTrackedBuffer(
                device_, queue_, wfBytes, WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst, "WireframeIndexBuffer",
                st.wfIndices.data(), wfBytes, prim.gpuWireframeIndexBufferSize);
        }
        if (!prim.gpuMorphDeltaBuffer && !st.morphDeltas.empty()) {
            const size_t morphBytes{st.morphDeltas.size() * sizeof(glm::vec4)};
            prim.gpuMorphDeltaBuffer = CreateAndUploadTrackedBuffer(
                device_, queue_, morphBytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, "MorphDeltas_SSBO",
                st.morphDeltas.data(), morphBytes, prim.gpuMorphDeltaBufferSize);
            if (prim.gpuMorphDeltaBuffer) {
                prim.gpuMorphTargetCount = st.morphTargetCount;
                prim.gpuMorphVertexCount = st.morphVertexCount;
            }
        }
        if (!prim.gpuUniformBuffer) {
            prim.gpuUniformBuffer = CreateAndUploadTrackedBuffer(
                device_, queue_, sizeof(MaterialUniforms), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                "MaterialUniforms", nullptr, 0, prim.gpuUniformBufferSize);
        }
        if (prim.gpuUniformBuffer) {
            const scene::Material* matPtr =
                (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size()))
                    ? &scene.materials[prim.materialIndex]
                    : nullptr;
            MaterialUniforms matUbo{};
            FillMaterialUniforms(matPtr, matUbo);
            wgpuQueueWriteBuffer(queue_, prim.gpuUniformBuffer, 0, &matUbo, sizeof(matUbo));
        }
    }

    // 4. Create persistent Skinning buffers & rebuild material + RT bind groups
    UploadSceneSkins(device_, queue_, skinBgl_.Get(), scene);
    RebuildMaterialBindGroups(scene);
    shadowPass_.MarkDirty();
    if (desktopRtSupported_ && rtScene_.IsInitialized()) {
        rtScene_.BuildFromScene(scene);
        rtPass_.ResetPathAccumulation();
    }
}

void PipelineManager::RetranscodeKtxTextures(scene::Scene& scene, scene::KtxTranscodeTarget target) {
    if (!device_ || !queue_) {
        return;
    }

    std::vector<std::byte> arenaBacking(2 * 1024 * 1024);
    std::pmr::monotonic_buffer_resource stagingArena{arenaBacking.data(), arenaBacking.size()};
    size_t retranscodedCount{0};

    for (size_t i{0}; i < scene.cpuImages.size(); ++i) {
        auto& cpuImg = scene.cpuImages[i];
        if (cpuImg.ktx2SourceBytes.empty()) {
            continue;
        }
        stagingArena.release();
        auto decoded = scene::TextureDecoder::DecodeKTX2(cpuImg.ktx2SourceBytes, target, &stagingArena);
        if (!decoded) {
            BGL_LOG_WARN("[KTX2] Failed to re-transcode texture #{} to {}: {}", i,
                         scene::TextureDecoder::GetKtxTargetName(target), decoded.error());
            continue;
        }

        cpuImg.width = decoded->width;
        cpuImg.height = decoded->height;
        cpuImg.rgba = std::move(decoded->rgbaData);
        cpuImg.isCompressed = decoded->isCompressed;
        cpuImg.compressionFormat = decoded->compressionFormat;
        cpuImg.mipLevels = std::move(decoded->mipLevels);

        if (i < scene.gpuTextures.size()) {
            if (scene.gpuTextures[i].view) {
                wgpuTextureViewRelease(scene.gpuTextures[i].view);
                scene.gpuTextures[i].view = nullptr;
            }
            if (scene.gpuTextures[i].texture) {
                wgpuTextureRelease(scene.gpuTextures[i].texture);
                scene.gpuTextures[i].texture = nullptr;
            }
            scene.gpuTextures[i].vramBytes = 0;
        }
        ++retranscodedCount;
    }

    if (retranscodedCount > 0) {
        PrepareScene(scene);
        BGL_LOG_INFO("[KTX2] Re-transcoded {} KTX2 texture(s) to {} using std::pmr staging arena", retranscodedCount,
                     scene::TextureDecoder::GetKtxTargetName(target));
    }
}

} // namespace bgl::gpu
