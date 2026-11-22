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
#include <memory_resource>
#include <span>
#include <utility>

namespace bgl::gpu {
using namespace detail;

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

    std::vector<bool> isNormalTexture(scene.cpuImages.size(), false);
    std::vector<bool> isSrgbTexture(scene.cpuImages.size(), false);
    for (const auto& mat : scene.materials) {
        if (mat.baseColorTextureIndex >= 0 && static_cast<size_t>(mat.baseColorTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.baseColorTextureIndex] = true;
        }
        if (mat.emissiveTextureIndex >= 0 && static_cast<size_t>(mat.emissiveTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.emissiveTextureIndex] = true;
        }
        if (mat.sheenColorTextureIndex >= 0 && static_cast<size_t>(mat.sheenColorTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.sheenColorTextureIndex] = true;
        }
        if (mat.normalTextureIndex >= 0 && static_cast<size_t>(mat.normalTextureIndex) < isNormalTexture.size()) {
            isNormalTexture[mat.normalTextureIndex] = true;
            isSrgbTexture[mat.normalTextureIndex] = false;
        }
        if (mat.metallicRoughnessTextureIndex >= 0 &&
            static_cast<size_t>(mat.metallicRoughnessTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.metallicRoughnessTextureIndex] = false;
        }
        if (mat.occlusionTextureIndex >= 0 && static_cast<size_t>(mat.occlusionTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.occlusionTextureIndex] = false;
        }
    }

    struct PendingGpuMipGen {
        WGPUTexture texture{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t mipLevels{1};
        bool isSrgb{false};
        bool isNormal{false};
    };
    std::vector<PendingGpuMipGen> pendingMipGens;
    pendingMipGens.reserve(scene.cpuImages.size());

    // 1. Upload CPU decoded images (either native block-compressed BC7/ASTC/ETC2 or RGBA8 mip0) to WebGPU Textures
    for (size_t i{0}; i < scene.cpuImages.size(); ++i) {
        const auto& img = scene.cpuImages[i];
        if (img.width == 0 || img.height == 0 || img.rgba.empty()) {
            continue;
        }
        if (i >= scene.gpuTextures.size() || scene.gpuTextures[i].view != nullptr) {
            continue;
        }

        if (img.isCompressed && img.compressionFormat != scene::KtxTranscodeTarget::Rgba32) {
            WGPUTextureFormat wgpuFmt{WGPUTextureFormat_BC7RGBAUnorm};
            std::string_view fmtName{"BC7"};
            switch (static_cast<scene::KtxTranscodeTarget>(img.compressionFormat)) {
            case scene::KtxTranscodeTarget::Bc7:
                wgpuFmt = isSrgbTexture[i] ? WGPUTextureFormat_BC7RGBAUnormSrgb : WGPUTextureFormat_BC7RGBAUnorm;
                fmtName = isSrgbTexture[i] ? "BC7RGBAUnormSrgb" : "BC7RGBAUnorm";
                break;
            case scene::KtxTranscodeTarget::Astc4x4:
                wgpuFmt = isSrgbTexture[i] ? WGPUTextureFormat_ASTC4x4UnormSrgb : WGPUTextureFormat_ASTC4x4Unorm;
                fmtName = isSrgbTexture[i] ? "ASTC4x4UnormSrgb" : "ASTC4x4Unorm";
                break;
            case scene::KtxTranscodeTarget::Etc2Rgba:
                wgpuFmt = isSrgbTexture[i] ? WGPUTextureFormat_ETC2RGBA8UnormSrgb : WGPUTextureFormat_ETC2RGBA8Unorm;
                fmtName = isSrgbTexture[i] ? "ETC2RGBA8UnormSrgb" : "ETC2RGBA8Unorm";
                break;
            default:
                break;
            }

            // Count valid 4x4 block-aligned mip levels
            uint32_t validCompressedMips{0};
            for (const auto& mip : img.mipLevels) {
                if ((mip.width % 4u) == 0u && (mip.height % 4u) == 0u && !mip.data.empty()) {
                    ++validCompressedMips;
                } else {
                    break;
                }
            }
            validCompressedMips = std::max(1u, validCompressedMips);

            WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.size = WGPUExtent3D{img.width, img.height, 1};
            texDesc.format = wgpuFmt;
            texDesc.mipLevelCount = validCompressedMips;
            texDesc.sampleCount = 1;
            texDesc.label = ToWgpuStringView("glTF_KTX2_CompressedTexture");
            WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

            size_t compressedVramBytes{0};
            for (uint32_t level{0}; level < validCompressedMips; ++level) {
                const uint32_t mipW{(level < img.mipLevels.size()) ? img.mipLevels[level].width : img.width};
                const uint32_t mipH{(level < img.mipLevels.size()) ? img.mipLevels[level].height : img.height};
                std::span<const uint8_t> mipBytes{
                    (level < img.mipLevels.size())
                        ? std::span<const uint8_t>{img.mipLevels[level].data.data(), img.mipLevels[level].data.size()}
                        : std::span<const uint8_t>{img.rgba.data(), img.rgba.size()}};

                const uint32_t blocksX{std::max(1u, (mipW + 3u) / 4u)};
                const uint32_t blocksY{std::max(1u, (mipH + 3u) / 4u)};

                WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                copyDst.texture = tex;
                copyDst.mipLevel = level;
                copyDst.origin = WGPUOrigin3D{0, 0, 0};
                copyDst.aspect = WGPUTextureAspect_All;

                WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                dataLayout.offset = 0;
                dataLayout.bytesPerRow = blocksX * 16u; // BC7, ASTC 4x4, and ETC2 RGBA8 all use 16 bytes per 4x4 block
                dataLayout.rowsPerImage = blocksY;

                WGPUExtent3D writeSize{mipW, mipH, 1};
                wgpuQueueWriteTexture(queue_, &copyDst, mipBytes.data(), mipBytes.size(), &dataLayout, &writeSize);
                compressedVramBytes += static_cast<size_t>(blocksX) * blocksY * 16u;
            }

            WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
            scene.gpuTextures[i].texture = tex;
            scene.gpuTextures[i].view = view;
            scene.gpuTextures[i].sampler = defaultSampler_;
            scene.gpuTextures[i].width = img.width;
            scene.gpuTextures[i].height = img.height;
            scene.gpuTextures[i].vramBytes = compressedVramBytes;
            scene.gpuTextures[i].format = img.compressionFormat;
            BGL_LOG_DEBUG(
                "[WebGPU Upload] Texture #{}: Native Compressed 2D Texture ({}x{}, {}, {} Mip Levels, {:.2f} KB VRAM)",
                i, img.width, img.height, fmtName, validCompressedMips,
                static_cast<double>(compressedVramBytes) / 1024.0);
            continue;
        }

        const uint32_t mipLevels{CalculateMipLevels(img.width, img.height)};

        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_RenderAttachment;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.size = WGPUExtent3D{img.width, img.height, 1};
        texDesc.format = isSrgbTexture[i] ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm;
        texDesc.mipLevelCount = mipLevels;
        texDesc.sampleCount = 1;
        texDesc.label = ToWgpuStringView("glTF_Texture");
        WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = tex;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{0, 0, 0};
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = img.width * 4;
        dataLayout.rowsPerImage = img.height;

        WGPUExtent3D writeSize{img.width, img.height, 1};
        std::span<const uint8_t> mip0Span{img.rgba};
        wgpuQueueWriteTexture(queue_, &copyDst, mip0Span.data(), mip0Span.size(), &dataLayout, &writeSize);

        if (mipLevels > 1) {
            pendingMipGens.push_back(PendingGpuMipGen{
                .texture = tex,
                .width = img.width,
                .height = img.height,
                .mipLevels = mipLevels,
                .isSrgb = isSrgbTexture[i],
                .isNormal = isNormalTexture[i],
            });
        }

        WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

        size_t rgbaVramBytes{0};
        uint32_t mw{img.width};
        uint32_t mh{img.height};
        for (uint32_t m{0}; m < mipLevels; ++m) {
            rgbaVramBytes += static_cast<size_t>(mw) * mh * 4u;
            mw = std::max(1u, mw / 2u);
            mh = std::max(1u, mh / 2u);
        }

        scene.gpuTextures[i].texture = tex;
        scene.gpuTextures[i].view = view;
        scene.gpuTextures[i].sampler = defaultSampler_;
        scene.gpuTextures[i].width = img.width;
        scene.gpuTextures[i].height = img.height;
        scene.gpuTextures[i].vramBytes = rgbaVramBytes;
        scene.gpuTextures[i].format = scene::KtxTranscodeTarget::Rgba32;
        BGL_LOG_DEBUG("[WebGPU Upload] Texture #{}: Created 2D Texture ({}x{}, {}, {} Mip Levels, NormalMap={}), wrote "
                      "{} bytes mip0 ({:.2f} KB VRAM)",
                      i, img.width, img.height, isSrgbTexture[i] ? "RGBA8UnormSrgb" : "RGBA8Unorm", mipLevels,
                      isNormalTexture[i] ? "Yes" : "No", img.rgba.size(), static_cast<double>(rgbaVramBytes) / 1024.0);
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

    // 2. Collect unique primitives and build multi-LOD, wireframe, and morph-delta staging buffers in parallel
    std::vector<scene::MeshPrimitive*> uniquePrims;
    CollectUniquePrimitives(scene, uniquePrims);

    struct PrimStaging {
        std::vector<uint8_t> indexBytes{};
        bool useUint16{false};
        std::vector<uint32_t> wfIndices{};
        std::vector<glm::vec4> morphDeltas{};
        uint32_t morphTargetCount{0};
        uint32_t morphVertexCount{0};
    };
    std::vector<PrimStaging> stagings(uniquePrims.size());

    auto stagePrimitive = [&](size_t idx) {
        scene::MeshPrimitive& prim = *uniquePrims[idx];
        auto& st = stagings[idx];
        if (prim.vertices.empty()) {
            return;
        }

        if (!prim.gpuIndexBuffer && !prim.indices.empty()) {
            if (prim.lodIndices[0].empty()) {
                prim.GenerateLODs();
            }
            std::vector<uint32_t> combinedIndices{};
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
            if (!prim.indices.empty()) {
                st.wfIndices.reserve(prim.indices.size() * 2);
                for (size_t k{0}; k + 2 < prim.indices.size(); k += 3) {
                    const uint32_t a{prim.indices[k]};
                    const uint32_t b{prim.indices[k + 1]};
                    const uint32_t c{prim.indices[k + 2]};
                    st.wfIndices.push_back(a);
                    st.wfIndices.push_back(b);
                    st.wfIndices.push_back(b);
                    st.wfIndices.push_back(c);
                    st.wfIndices.push_back(c);
                    st.wfIndices.push_back(a);
                }
            } else {
                st.wfIndices.reserve(prim.vertices.size() * 2);
                for (size_t k{0}; k + 2 < prim.vertices.size(); k += 3) {
                    const uint32_t a{static_cast<uint32_t>(k)};
                    const uint32_t b{static_cast<uint32_t>(k + 1)};
                    const uint32_t c{static_cast<uint32_t>(k + 2)};
                    st.wfIndices.push_back(a);
                    st.wfIndices.push_back(b);
                    st.wfIndices.push_back(b);
                    st.wfIndices.push_back(c);
                    st.wfIndices.push_back(c);
                    st.wfIndices.push_back(a);
                }
            }
        }

        if (!prim.gpuMorphDeltaBuffer && !prim.morphTargets.empty()) {
            const uint32_t tCount{static_cast<uint32_t>(prim.morphTargets.size())};
            const uint32_t vCount{static_cast<uint32_t>(prim.vertices.size())};
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
    };

#if !defined(__EMSCRIPTEN__)
    if (uniquePrims.size() > 1) {
        core::ThreadPool::Instance().ParallelFor(0, uniquePrims.size(), stagePrimitive);
    } else if (uniquePrims.size() == 1) {
        stagePrimitive(0);
    }
#else
    for (size_t idx{0}; idx < uniquePrims.size(); ++idx) {
        stagePrimitive(idx);
    }
#endif

    // 3. Create & upload WebGPU geometry buffers, morph SSBOs, and static MaterialUniforms buffers
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
            const size_t vbSize{srcVerts.size() * sizeof(core::Vertex)};
            WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            vbDesc.size = (vbSize + 3u) & ~size_t{3};
            vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            vbDesc.label = ToWgpuStringView("VertexBuffer");
            prim.gpuVertexBuffer = wgpuDeviceCreateBuffer(device_, &vbDesc);
            if (prim.gpuVertexBuffer) {
                prim.gpuVertexBufferSize = vbDesc.size;
                core::MemoryTracker::Instance().TrackBufferAlloc(prim.gpuVertexBufferSize);
                wgpuQueueWriteBuffer(queue_, prim.gpuVertexBuffer, 0, srcVerts.data(), vbSize);
                prim.morphDirty = false;
            }
        }

        if (!prim.gpuIndexBuffer && !st.indexBytes.empty()) {
            WGPUBufferDescriptor ibDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            ibDesc.size = st.indexBytes.size();
            ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
            ibDesc.label = ToWgpuStringView(st.useUint16 ? "IndexBuffer_U16_MultiLOD" : "IndexBuffer_U32_MultiLOD");
            prim.gpuIndexBuffer = wgpuDeviceCreateBuffer(device_, &ibDesc);
            if (prim.gpuIndexBuffer) {
                prim.gpuIndexBufferSize = ibDesc.size;
                core::MemoryTracker::Instance().TrackBufferAlloc(prim.gpuIndexBufferSize);
                wgpuQueueWriteBuffer(queue_, prim.gpuIndexBuffer, 0, st.indexBytes.data(), st.indexBytes.size());
            }
        }

        if (!prim.gpuWireframeIndexBuffer && !st.wfIndices.empty()) {
            prim.wireframeIndexCount = static_cast<uint32_t>(st.wfIndices.size());
            const size_t wfIbSize{st.wfIndices.size() * sizeof(uint32_t)};
            WGPUBufferDescriptor wfIbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            wfIbDesc.size = (wfIbSize + 3u) & ~size_t{3};
            wfIbDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
            wfIbDesc.label = ToWgpuStringView("WireframeIndexBuffer");
            prim.gpuWireframeIndexBuffer = wgpuDeviceCreateBuffer(device_, &wfIbDesc);
            if (prim.gpuWireframeIndexBuffer) {
                prim.gpuWireframeIndexBufferSize = wfIbDesc.size;
                core::MemoryTracker::Instance().TrackBufferAlloc(prim.gpuWireframeIndexBufferSize);
                wgpuQueueWriteBuffer(queue_, prim.gpuWireframeIndexBuffer, 0, st.wfIndices.data(), wfIbSize);
            }
        }

        if (!prim.gpuMorphDeltaBuffer && !st.morphDeltas.empty()) {
            const uint64_t morphBytes{st.morphDeltas.size() * sizeof(glm::vec4)};
            WGPUBufferDescriptor mbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            mbDesc.size = morphBytes;
            mbDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
            mbDesc.label = ToWgpuStringView("MorphDeltas_SSBO");
            prim.gpuMorphDeltaBuffer = wgpuDeviceCreateBuffer(device_, &mbDesc);
            if (prim.gpuMorphDeltaBuffer) {
                prim.gpuMorphTargetCount = st.morphTargetCount;
                prim.gpuMorphVertexCount = st.morphVertexCount;
                prim.gpuMorphDeltaBufferSize = morphBytes;
                core::MemoryTracker::Instance().TrackBufferAlloc(prim.gpuMorphDeltaBufferSize);
                wgpuQueueWriteBuffer(queue_, prim.gpuMorphDeltaBuffer, 0, st.morphDeltas.data(), morphBytes);
            }
        }

        if (!prim.gpuUniformBuffer) {
            WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            ubDesc.size = sizeof(MaterialUniforms);
            ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            ubDesc.label = ToWgpuStringView("MaterialUniforms");
            prim.gpuUniformBuffer = wgpuDeviceCreateBuffer(device_, &ubDesc);
            if (prim.gpuUniformBuffer) {
                prim.gpuUniformBufferSize = sizeof(MaterialUniforms);
                core::MemoryTracker::Instance().TrackBufferAlloc(prim.gpuUniformBufferSize);
            }
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

    // 4. Create persistent Skinning Storage Buffers & BindGroups once per Skin
    for (auto& skin : scene.skins) {
        skin.cachedFrameId = 0;
        skin.cachedUploadedFrameId = 0;
        skin.cachedMeshNode = nullptr;
        if (skin.joints.empty() || skin.gpuJointBuffer) {
            continue;
        }
        const size_t jointCount{std::max<size_t>(1, skin.joints.size())};
        const uint64_t bufSize{jointCount * sizeof(glm::mat4)};
        WGPUBufferDescriptor sbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        sbDesc.size = bufSize;
        sbDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        sbDesc.label = ToWgpuStringView("SkinJointMatrices_SSBO");
        skin.gpuJointBuffer = wgpuDeviceCreateBuffer(device_, &sbDesc);
        if (skin.gpuJointBuffer) {
            skin.gpuJointBufferSize = bufSize;
            core::MemoryTracker::Instance().TrackBufferAlloc(skin.gpuJointBufferSize);
            const std::vector<glm::mat4> identityJoints(jointCount, glm::mat4{1.0f});
            wgpuQueueWriteBuffer(queue_, skin.gpuJointBuffer, 0, identityJoints.data(), bufSize);

            WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntry.binding = 0;
            bgEntry.buffer = skin.gpuJointBuffer;
            bgEntry.offset = 0;
            bgEntry.size = bufSize;

            WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgDesc.layout = skinBgl_.Get();
            bgDesc.entryCount = 1;
            bgDesc.entries = &bgEntry;
            bgDesc.label = ToWgpuStringView("SkinStorage_BindGroup");
            skin.gpuBindGroup = wgpuDeviceCreateBindGroup(device_, &bgDesc);
        }
    }

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
