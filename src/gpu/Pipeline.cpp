/**
 * @file Pipeline.cpp
 * @brief Core PipelineManager lifecycle, main/tonemap/mipgen pipelines, and frame render encoding.
 */
#include "gpu/Pipeline.hpp"
#include "core/Log.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/PipelineHelpers.hpp"
#include "gpu/ShaderManager.hpp"
#include "scene/Quadtree.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <span>
#include <utility>

namespace bgl::gpu {
using namespace detail;

PipelineManager::~PipelineManager() {
    Shutdown();
}

void PipelineManager::Shutdown() noexcept {
    postProcessPass_.Shutdown();
    shadowPass_.Shutdown();
    aabbRenderer_.Shutdown();
    gpuCuller_.Shutdown();
    gpuRayPicker_.Shutdown();
    perDrawRing_.Shutdown();
    transparentPerDrawRing_.Shutdown();
    instanceBuffer_.Shutdown();

    mipGenNormalPipeline_.Reset();
    mipGenSrgbPipeline_.Reset();
    mipGenUnormPipeline_.Reset();
    mipGenPipelineLayout_.Reset();
    mipGenBgl_.Reset();

    dummySkinBindGroup_.Reset();
    dummySkinBuffer_.Reset();
    dummyMorphDeltaBuffer_.Reset();
    sceneGlobalsBindGroup_.Reset();

    sceneLightingBuffer_.Reset();
    ggxPrefilterUniformBuffer_.Reset();
    ggxPrefilterComputePipeline_.Reset();
    ggxPrefilterComputeBgl_.Reset();
    brdfLutComputePipeline_.Reset();
    brdfLutComputeBgl_.Reset();
    brdfLutView_.Reset();
    brdfLutTexture_.Reset();

    noMipSampler_.Reset();
    defaultSampler_.Reset();
    envCubemapView_.Reset();
    envCubemapTexture_.Reset();
    defaultBlackTextureView_.Reset();
    defaultBlackTexture_.Reset();
    defaultNormalTextureView_.Reset();
    defaultNormalTexture_.Reset();
    defaultWhiteTextureView_.Reset();
    defaultWhiteTexture_.Reset();
    wireframePipeline_.Reset();
    blendBackCulledPipeline_.Reset();
    blendPipeline_.Reset();
    opaqueBackCulledPipeline_.Reset();
    opaquePipeline_.Reset();
    mainPipelineLayout_.Reset();
    skinBgl_.Reset();
    materialBgl_.Reset();
    perDrawBgl_.Reset();
    sceneGlobalsBgl_.Reset();

    opaqueColorCopyView_.Reset();
    opaqueColorCopyTexture_.Reset();
    opaqueCopyWidth_ = 0;
    opaqueCopyHeight_ = 0;

    visibleItemsScratch_.clear();
    renderItemsScratch_.clear();
    drawCallsScratch_.clear();
    mainPerDrawScratch_.clear();
    frameInstanceTransformsScratch_.clear();
    nodeInstanceStartScratch_.clear();
    transparentStartIndex_ = 0;
    skinFrameId_ = 1;
    skinFrameAdvanced_ = false;

    device_ = nullptr;
    queue_ = nullptr;
    initialized_ = false;
}

GpuCuller& PipelineManager::GetGpuCuller() noexcept {
    if (!gpuCuller_.IsInitialized() && device_ && queue_) {
        if (auto res = gpuCuller_.Initialize(device_, queue_); !res) {
            BGL_LOG_WARN("Failed to initialize GpuCuller lazily: {}", res.error());
        }
    }
    return gpuCuller_;
}

GpuRayPicker& PipelineManager::GetGpuRayPicker() noexcept {
    if (!gpuRayPicker_.IsInitialized() && device_ && queue_) {
        if (auto res = gpuRayPicker_.Initialize(device_, queue_); !res) {
            BGL_LOG_WARN("Failed to initialize GpuRayPicker lazily: {}", res.error());
        }
    }
    return gpuRayPicker_;
}

uint32_t PipelineManager::CalculateMipLevels(uint32_t width, uint32_t height) noexcept {
    const uint32_t maxDim{std::max(width, height)};
    return std::max(1u, static_cast<uint32_t>(std::bit_width(maxDim)));
}

std::vector<uint8_t> PipelineManager::GenerateNextMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                                                           uint32_t dstW, uint32_t dstH, bool isNormalMap) {
    if (!src || srcW == 0 || srcH == 0) {
        return std::vector<uint8_t>(static_cast<size_t>(dstW) * dstH * 4);
    }
    return GenerateNextMipLevel(std::span<const uint8_t>{src, static_cast<size_t>(srcW) * srcH * 4}, srcW, srcH, dstW,
                                dstH, isNormalMap);
}

std::vector<uint8_t> PipelineManager::GenerateNextMipLevel(std::span<const uint8_t> src, uint32_t srcW, uint32_t srcH,
                                                           uint32_t dstW, uint32_t dstH, bool isNormalMap) {
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * 4);
    if (src.empty() || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0 ||
        src.size() < static_cast<size_t>(srcW) * srcH * 4) {
        return dst;
    }

    for (uint32_t y{0}; y < dstH; ++y) {
        for (uint32_t x{0}; x < dstW; ++x) {
            const uint32_t srcX0{std::min(x * 2, srcW - 1)};
            const uint32_t srcX1{std::min(x * 2 + 1, srcW - 1)};
            const uint32_t srcY0{std::min(y * 2, srcH - 1)};
            const uint32_t srcY1{std::min(y * 2 + 1, srcH - 1)};

            std::span<const uint8_t> p00{src.subspan((static_cast<size_t>(srcY0) * srcW + srcX0) * 4, 4)};
            std::span<const uint8_t> p10{src.subspan((static_cast<size_t>(srcY0) * srcW + srcX1) * 4, 4)};
            std::span<const uint8_t> p01{src.subspan((static_cast<size_t>(srcY1) * srcW + srcX0) * 4, 4)};
            std::span<const uint8_t> p11{src.subspan((static_cast<size_t>(srcY1) * srcW + srcX1) * 4, 4)};

            const size_t dstIdx{(static_cast<size_t>(y) * dstW + x) * 4};
            if (isNormalMap) {
                auto unpackN = [](std::span<const uint8_t> p) -> glm::vec3 {
                    return glm::vec3{(static_cast<float>(p[0]) / 255.0f) * 2.0f - 1.0f,
                                     (static_cast<float>(p[1]) / 255.0f) * 2.0f - 1.0f,
                                     (static_cast<float>(p[2]) / 255.0f) * 2.0f - 1.0f};
                };
                glm::vec3 avgN{unpackN(p00) + unpackN(p10) + unpackN(p01) + unpackN(p11)};
                const float len{glm::length(avgN)};
                avgN = (len > 1e-5f) ? (avgN / len) : glm::vec3{0.0f, 0.0f, 1.0f};
                dst[dstIdx + 0] =
                    static_cast<uint8_t>(std::clamp((avgN.x * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                dst[dstIdx + 1] =
                    static_cast<uint8_t>(std::clamp((avgN.y * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                dst[dstIdx + 2] =
                    static_cast<uint8_t>(std::clamp((avgN.z * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                const uint32_t sumA{static_cast<uint32_t>(p00[3]) + p10[3] + p01[3] + p11[3]};
                dst[dstIdx + 3] = static_cast<uint8_t>((sumA + 2) / 4);
            } else {
                for (int c{0}; c < 4; ++c) {
                    const uint32_t sum{static_cast<uint32_t>(p00[c]) + static_cast<uint32_t>(p10[c]) +
                                       static_cast<uint32_t>(p01[c]) + static_cast<uint32_t>(p11[c])};
                    dst[dstIdx + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
    }
    return dst;
}

glm::mat4 PipelineManager::ComputeDirectionalLightMatrix(const core::AABB& sceneBounds,
                                                         const glm::vec3& lightDir) noexcept {
    return ShadowPass::ComputeDirectionalLightMatrix(sceneBounds, lightDir);
}

void PipelineManager::BuildClusteredLightGrid(std::span<const GpuPunctualLight> lights, uint32_t lightCount,
                                              const glm::mat4& viewMatrix, const glm::mat4& projMatrix, float zNear,
                                              float zFar, std::array<glm::uvec4, 256>& outClusterMasks) noexcept {
    outClusterMasks.fill(glm::uvec4{0u});
    const uint32_t count{std::min<uint32_t>(lightCount, static_cast<uint32_t>(lights.size()))};
    if (count == 0)
        return;

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
            // Directional light affects all clusters
            for (auto& vecMask : outClusterMasks) {
                vecMask = vecMask | glm::uvec4{bitMask};
            }
            continue;
        }

        // Point or Spot light: transform world position into view space
        const glm::vec3 viewPos{glm::vec3{viewMatrix * glm::vec4{glm::vec3{light.posOrDir}, 1.0f}}};
        // Effective cull radius (use explicit range or fallback radius derived from intensity)
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

        // Project sphere bounds onto NDC [-1, 1] -> Tile [0..15] x [0..7]
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
            // WebGPU framebuffer Y is top-down (0 = top, 1 = bottom)
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

void PipelineManager::RenderTonemapPass(WGPURenderPassEncoder swapchainPass, WGPUTextureView hdrResolveView,
                                        const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                        const glm::vec3& lightDir, float envRotation, uint32_t viewportW,
                                        uint32_t viewportH) {
    postProcessPass_.Render(swapchainPass, hdrResolveView, viewProj, cameraPos, lightDir, envRotation, viewportW,
                            viewportH);
}

core::Result<void> PipelineManager::CreateSharedBindGroupLayouts() {
    // Shared 4-BindGroup Architecture:
    // @group(0) SceneGlobals: Lighting + IBL + ShadowMap + BRDF LUT + Opaque Copy
    // @group(1) PerDrawDynamic: 256-byte dynamic-offset PerDrawUniforms ring
    // @group(2) MaterialStatic: 256-byte MaterialUniforms + Sampler + 8 Textures + MorphDeltas SSBO
    // @group(3) SkinStorage: JointMatrices SSBO (reused across Shadow & Main passes)
    std::array<WGPUBindGroupLayoutEntry, 6> sgEntries{};
    sgEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    sgEntries[0].binding = 0;
    sgEntries[0].visibility = WGPUShaderStage_Fragment;
    sgEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    sgEntries[0].buffer.minBindingSize = sizeof(SceneLightingUniforms);

    sgEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    sgEntries[1].binding = 1;
    sgEntries[1].visibility = WGPUShaderStage_Fragment;
    sgEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
    sgEntries[1].texture.viewDimension = WGPUTextureViewDimension_Cube;
    sgEntries[1].texture.multisampled = WGPU_FALSE;

    sgEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    sgEntries[2].binding = 2;
    sgEntries[2].visibility = WGPUShaderStage_Fragment;
    sgEntries[2].texture.sampleType = WGPUTextureSampleType_Depth;
    sgEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    sgEntries[2].texture.multisampled = WGPU_FALSE;

    sgEntries[3] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    sgEntries[3].binding = 3;
    sgEntries[3].visibility = WGPUShaderStage_Fragment;
    sgEntries[3].sampler.type = WGPUSamplerBindingType_Comparison;

    sgEntries[4] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    sgEntries[4].binding = 4;
    sgEntries[4].visibility = WGPUShaderStage_Fragment;
    sgEntries[4].texture.sampleType = WGPUTextureSampleType_Float;
    sgEntries[4].texture.viewDimension = WGPUTextureViewDimension_2D;
    sgEntries[4].texture.multisampled = WGPU_FALSE;

    sgEntries[5] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    sgEntries[5].binding = 5;
    sgEntries[5].visibility = WGPUShaderStage_Fragment;
    sgEntries[5].texture.sampleType = WGPUTextureSampleType_Float;
    sgEntries[5].texture.viewDimension = WGPUTextureViewDimension_2D;
    sgEntries[5].texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor sgBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    sgBglDesc.entryCount = sgEntries.size();
    sgBglDesc.entries = sgEntries.data();
    sgBglDesc.label = ToWgpuStringView("SceneGlobals_BGL");
    sceneGlobalsBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &sgBglDesc);

    WGPUBindGroupLayoutEntry pdEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    pdEntry.binding = 0;
    pdEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    pdEntry.buffer.type = WGPUBufferBindingType_Uniform;
    pdEntry.buffer.hasDynamicOffset = WGPU_TRUE;
    pdEntry.buffer.minBindingSize = sizeof(PerDrawUniforms);

    WGPUBindGroupLayoutDescriptor pdBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    pdBglDesc.entryCount = 1;
    pdBglDesc.entries = &pdEntry;
    pdBglDesc.label = ToWgpuStringView("PerDrawDynamic_BGL");
    perDrawBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &pdBglDesc);

    std::array<WGPUBindGroupLayoutEntry, 11> matEntries{};
    matEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    matEntries[0].binding = 0;
    matEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    matEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    matEntries[0].buffer.minBindingSize = sizeof(MaterialUniforms);

    matEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    matEntries[1].binding = 1;
    matEntries[1].visibility = WGPUShaderStage_Fragment;
    matEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    for (uint32_t b{2}; b < 10; ++b) {
        matEntries[b] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        matEntries[b].binding = b;
        matEntries[b].visibility = WGPUShaderStage_Fragment;
        matEntries[b].texture.sampleType = WGPUTextureSampleType_Float;
        matEntries[b].texture.viewDimension = WGPUTextureViewDimension_2D;
        matEntries[b].texture.multisampled = WGPU_FALSE;
    }

    matEntries[10] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    matEntries[10].binding = 10;
    matEntries[10].visibility = WGPUShaderStage_Vertex;
    matEntries[10].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    matEntries[10].buffer.minBindingSize = 3u * sizeof(glm::vec4);

    WGPUBindGroupLayoutDescriptor matBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    matBglDesc.entryCount = matEntries.size();
    matBglDesc.entries = matEntries.data();
    matBglDesc.label = ToWgpuStringView("MaterialStatic_BGL");
    materialBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &matBglDesc);

    WGPUBindGroupLayoutEntry skinEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    skinEntry.binding = 0;
    skinEntry.visibility = WGPUShaderStage_Vertex;
    skinEntry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    skinEntry.buffer.minBindingSize = sizeof(glm::mat4);

    WGPUBindGroupLayoutDescriptor skinBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    skinBglDesc.entryCount = 1;
    skinBglDesc.entries = &skinEntry;
    skinBglDesc.label = ToWgpuStringView("SkinStorage_BGL");
    skinBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &skinBglDesc);

    if (!sceneGlobalsBgl_ || !perDrawBgl_ || !materialBgl_ || !skinBgl_) {
        return std::unexpected("Failed to create shared WebGPU BindGroupLayouts");
    }
    return {};
}

core::Result<void> PipelineManager::CreateDummyStorageBuffers() {
    constexpr std::array<glm::vec4, 3> kZeroMorphDeltas{};
    WGPUBufferDescriptor dmDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    dmDesc.size = sizeof(kZeroMorphDeltas);
    dmDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    dmDesc.label = ToWgpuStringView("DummyMorphDeltas");
    dummyMorphDeltaBuffer_ = wgpuDeviceCreateBuffer(device_, &dmDesc);
    if (dummyMorphDeltaBuffer_) {
        wgpuQueueWriteBuffer(queue_, dummyMorphDeltaBuffer_, 0, kZeroMorphDeltas.data(), sizeof(kZeroMorphDeltas));
    }

    const glm::mat4 kIdentityMat{1.0f};
    WGPUBufferDescriptor dsDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    dsDesc.size = sizeof(glm::mat4);
    dsDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    dsDesc.label = ToWgpuStringView("DummySkinMatrices");
    dummySkinBuffer_ = wgpuDeviceCreateBuffer(device_, &dsDesc);
    if (dummySkinBuffer_) {
        wgpuQueueWriteBuffer(queue_, dummySkinBuffer_, 0, &kIdentityMat, sizeof(glm::mat4));
        WGPUBindGroupEntry dsEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        dsEntry.binding = 0;
        dsEntry.buffer = dummySkinBuffer_;
        dsEntry.offset = 0;
        dsEntry.size = sizeof(glm::mat4);
        WGPUBindGroupDescriptor dsBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        dsBgDesc.layout = skinBgl_;
        dsBgDesc.entryCount = 1;
        dsBgDesc.entries = &dsEntry;
        dsBgDesc.label = ToWgpuStringView("DummySkin_BindGroup");
        dummySkinBindGroup_ = wgpuDeviceCreateBindGroup(device_, &dsBgDesc);
    }

    if (!dummyMorphDeltaBuffer_ || !dummySkinBuffer_ || !dummySkinBindGroup_) {
        return std::unexpected("Failed to create dummy storage buffers for skinning/morphing");
    }
    return {};
}

void PipelineManager::CreateSamplersAndLightingBuffer() {
    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_Repeat;
    sampDesc.addressModeV = WGPUAddressMode_Repeat;
    sampDesc.addressModeW = WGPUAddressMode_Repeat;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampDesc.lodMinClamp = 0.0f;
    sampDesc.lodMaxClamp = 32.0f;
    sampDesc.maxAnisotropy = 16;
    defaultSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    WGPUSamplerDescriptor noMipDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    noMipDesc.addressModeU = WGPUAddressMode_Repeat;
    noMipDesc.addressModeV = WGPUAddressMode_Repeat;
    noMipDesc.addressModeW = WGPUAddressMode_Repeat;
    noMipDesc.magFilter = WGPUFilterMode_Linear;
    noMipDesc.minFilter = WGPUFilterMode_Linear;
    noMipDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    noMipDesc.lodMinClamp = 0.0f;
    noMipDesc.lodMaxClamp = 0.0f;
    noMipDesc.maxAnisotropy = 1;
    noMipSampler_ = wgpuDeviceCreateSampler(device_, &noMipDesc);

    WGPUBufferDescriptor slDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    slDesc.size = sizeof(SceneLightingUniforms);
    slDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    slDesc.label = ToWgpuStringView("SceneLightingUniforms");
    sceneLightingBuffer_ = wgpuDeviceCreateBuffer(device_, &slDesc);
}

void PipelineManager::CreateFallbackTextures() {
    auto create1x1Texture = [&](std::span<const uint8_t, 4> rgba,
                                std::string_view label) -> std::pair<WGPUTexture, WGPUTextureView> {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.size = WGPUExtent3D{1, 1, 1};
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.label = WGPUStringView{label.data(), label.size()};
        WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = tex;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{0, 0, 0};
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = 4;
        dataLayout.rowsPerImage = 1;

        WGPUExtent3D writeSize{1, 1, 1};
        wgpuQueueWriteTexture(queue_, &copyDst, rgba.data(), rgba.size(), &dataLayout, &writeSize);

        WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
        return {tex, view};
    };

    constexpr std::array<uint8_t, 4> whitePixels{255, 255, 255, 255};
    auto [whiteTex, whiteView] = create1x1Texture(whitePixels, "DefaultWhiteTexture");
    defaultWhiteTexture_ = whiteTex;
    defaultWhiteTextureView_ = whiteView;

    constexpr std::array<uint8_t, 4> normalPixels{128, 128, 255, 255};
    auto [normTex, normView] = create1x1Texture(normalPixels, "DefaultNormalTexture");
    defaultNormalTexture_ = normTex;
    defaultNormalTextureView_ = normView;

    constexpr std::array<uint8_t, 4> blackPixels{0, 0, 0, 255};
    auto [blackTex, blackView] = create1x1Texture(blackPixels, "DefaultBlackTexture");
    defaultBlackTexture_ = blackTex;
    defaultBlackTextureView_ = blackView;
}

core::Result<void> PipelineManager::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                               uint32_t sampleCount) {
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;
    hdrFormat_ = WGPUTextureFormat_RGBA16Float;
    sampleCount_ = sampleCount;
    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize PipelineManager without valid WGPUDevice and WGPUQueue");
    }

    if (auto res = CreateSharedBindGroupLayouts(); !res) {
        Shutdown();
        return res;
    }

    if (auto res = CreateDummyStorageBuffers(); !res) {
        Shutdown();
        return res;
    }

    const std::array<WGPUBindGroupLayout, 4> plLayouts{sceneGlobalsBgl_.Get(), perDrawBgl_.Get(), materialBgl_.Get(),
                                                       skinBgl_.Get()};
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = plLayouts.size();
    plDesc.bindGroupLayouts = plLayouts.data();
    mainPipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!mainPipelineLayout_) {
        Shutdown();
        return std::unexpected("Failed to create main PBR pipeline layout");
    }

    if (auto res = instanceBuffer_.Initialize(device_, 256, "Main_InstanceBuffer"); !res) {
        Shutdown();
        return res;
    }

    if (auto res = CreateMainRenderPipelines(false); !res) {
        Shutdown();
        return res;
    }

    CreateSamplersAndLightingBuffer();

    InitializeComputeIBLPipelines();
    SetEnvironmentPreset(EnvironmentPreset::MediterraneanCourtyard, nullptr);

    CreateFallbackTextures();

    if (auto res = postProcessPass_.Initialize(device_, queue_, surfaceFormat_); !res) {
        Shutdown();
        return std::unexpected("Failed to initialize PostProcessPass: " + res.error());
    }
    if (auto res = shadowPass_.Initialize(device_, queue_, perDrawBgl_.Get(), materialBgl_.Get(), skinBgl_.Get(),
                                          dummySkinBindGroup_.Get());
        !res) {
        Shutdown();
        return std::unexpected("Failed to initialize ShadowPass: " + res.error());
    }

    perDrawRing_.Initialize(device_, queue_, perDrawBgl_.Get(), "Main_PerDrawRing");
    transparentPerDrawRing_.Initialize(device_, queue_, perDrawBgl_.Get(), "Transparent_PerDrawRing");
    InitMipGenPipelines();
    RebuildSceneGlobalsBindGroup();

    initialized_ =
        (opaquePipeline_ != nullptr && blendPipeline_ != nullptr && postProcessPass_.IsInitialized() &&
         shadowPass_.IsInitialized() && sceneGlobalsBindGroup_ != nullptr && instanceBuffer_.GetBuffer() != nullptr);

    if (auto res = aabbRenderer_.Initialize(device_, queue_, hdrFormat_, sampleCount_, WGPUTextureFormat_Depth24Plus);
        !res) {
        BGL_LOG_WARN("Failed to initialize AabbRenderer: {}", res.error());
    }

    BGL_LOG_INFO("Hardware WebGPU RenderPipeline initialized (RGBA16F HDR + Tonemapping + Compute IBL + Forward+ "
                 "Lights + Dynamic PerDraw UBO Ring + GPU Mipmaps)!");
    return {};
}

void PipelineManager::InitMipGenPipelines() {
    if (!device_) {
        return;
    }

    WGPUBindGroupLayoutEntry bglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntry.binding = 0;
    bglEntry.visibility = WGPUShaderStage_Fragment;
    bglEntry.texture.sampleType = WGPUTextureSampleType_Float;
    bglEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntry.texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = 1;
    bglDesc.entries = &bglEntry;
    bglDesc.label = ToWgpuStringView("MipGen_BGL");
    mipGenBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!mipGenBgl_) {
        return;
    }

    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = mipGenBgl_.AddressOf();
    plDesc.label = ToWgpuStringView("MipGen_PL");
    mipGenPipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!mipGenPipelineLayout_) {
        return;
    }

    if (auto res = CreateMipGenPipelines(false); !res) {
        BGL_LOG_WARN("Failed to compile MipGen WGSL module ({}); uncompressed textures will skip GPU mips",
                     res.error());
    }
}

void PipelineManager::GenerateTextureMipsGpu(WGPUCommandEncoder encoder, WGPUTexture texture, uint32_t width,
                                             uint32_t height, uint32_t mipLevels, bool isSrgb, bool isNormalMap) {
    (void)width;
    (void)height;
    if (!encoder || !device_ || !mipGenBgl_ || !texture || mipLevels <= 1) {
        return;
    }

    WGPURenderPipeline pipeline{nullptr};
    if (isNormalMap) {
        pipeline = mipGenNormalPipeline_.Get();
    } else if (isSrgb) {
        pipeline = mipGenSrgbPipeline_.Get();
    } else {
        pipeline = mipGenUnormPipeline_.Get();
    }
    if (!pipeline) {
        return;
    }

    const WGPUTextureFormat fmt{isSrgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm};
    for (uint32_t level{1}; level < mipLevels; ++level) {
        WGPUTextureViewDescriptor srcDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        srcDesc.format = fmt;
        srcDesc.dimension = WGPUTextureViewDimension_2D;
        srcDesc.baseMipLevel = level - 1;
        srcDesc.mipLevelCount = 1;
        srcDesc.baseArrayLayer = 0;
        srcDesc.arrayLayerCount = 1;
        srcDesc.aspect = WGPUTextureAspect_All;
        TextureViewHandle srcView{wgpuTextureCreateView(texture, &srcDesc)};

        WGPUTextureViewDescriptor dstDesc = srcDesc;
        dstDesc.baseMipLevel = level;
        TextureViewHandle dstView{wgpuTextureCreateView(texture, &dstDesc)};
        if (!srcView || !dstView) {
            break;
        }

        WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntry.binding = 0;
        bgEntry.textureView = srcView.Get();

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = mipGenBgl_.Get();
        bgDesc.entryCount = 1;
        bgDesc.entries = &bgEntry;
        BindGroupHandle srcBg{wgpuDeviceCreateBindGroup(device_, &bgDesc)};
        if (!srcBg) {
            break;
        }

        WGPURenderPassColorAttachment colorAtt = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colorAtt.view = dstView.Get();
        colorAtt.loadOp = WGPULoadOp_Clear;
        colorAtt.storeOp = WGPUStoreOp_Store;
        colorAtt.clearValue = WGPUColor{0.0, 0.0, 0.0, 0.0};

        WGPURenderPassDescriptor rpDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &colorAtt;
        rpDesc.label = ToWgpuStringView("GpuMipGenPass");

        if (RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &rpDesc)}) {
            wgpuRenderPassEncoderSetPipeline(pass.Get(), pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, srcBg.Get(), 0, nullptr);
            wgpuRenderPassEncoderDraw(pass.Get(), 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass.Get());
        }
    }
}

void PipelineManager::RebuildSceneGlobalsBindGroup() {
    if (!device_ || !sceneGlobalsBgl_ || !sceneLightingBuffer_ || !envCubemapView_ || !shadowPass_.IsInitialized() ||
        !defaultWhiteTextureView_) {
        return;
    }

    std::array<WGPUBindGroupEntry, 6> sgEntries{};
    sgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    sgEntries[0].binding = 0;
    sgEntries[0].buffer = sceneLightingBuffer_.Get();
    sgEntries[0].offset = 0;
    sgEntries[0].size = sizeof(SceneLightingUniforms);

    sgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    sgEntries[1].binding = 1;
    sgEntries[1].textureView = envCubemapView_.Get();

    sgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    sgEntries[2].binding = 2;
    sgEntries[2].textureView = shadowPass_.GetShadowMapView();

    sgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
    sgEntries[3].binding = 3;
    sgEntries[3].sampler = shadowPass_.GetShadowSampler();

    sgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
    sgEntries[4].binding = 4;
    sgEntries[4].textureView = brdfLutView_ ? brdfLutView_.Get() : defaultWhiteTextureView_.Get();

    sgEntries[5] = WGPU_BIND_GROUP_ENTRY_INIT;
    sgEntries[5].binding = 5;
    sgEntries[5].textureView = opaqueColorCopyView_ ? opaqueColorCopyView_.Get() : defaultWhiteTextureView_.Get();

    WGPUBindGroupDescriptor sgBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    sgBgDesc.layout = sceneGlobalsBgl_.Get();
    sgBgDesc.entryCount = sgEntries.size();
    sgBgDesc.entries = sgEntries.data();
    sgBgDesc.label = ToWgpuStringView("SceneGlobals_BindGroup");
    sceneGlobalsBindGroup_ = wgpuDeviceCreateBindGroup(device_, &sgBgDesc);
}

void PipelineManager::UpdateSceneLightingUniforms(scene::Scene& scene, const glm::mat4& viewMatrix,
                                                  const glm::mat4& projMatrix, const glm::vec3& cameraPos,
                                                  uint32_t viewportW, uint32_t viewportH, float zNear, float zFar) {
    if (!sceneLightingBuffer_)
        return;

    SceneLightingUniforms sl{};
    sl.shIrradiance = shIrradiance_;
    sl.viewMatrix = viewMatrix;
    sl.gridParams = glm::vec4{static_cast<float>(std::max(1u, viewportW)), static_cast<float>(std::max(1u, viewportH)),
                              zNear, zFar};
    sl.cameraPos = glm::vec4{cameraPos, 1.0f};
    sl.lightDir = glm::vec4{currentLightDir_, solarElevation_};
    sl.renderParams = glm::vec4{static_cast<float>(envMipLevels_), 0.0015f, aoIntensity_, 0.0f};

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
            } else if (srcLight.type == scene::LightType::Point) {
                dstLight.posOrDir = glm::vec4{worldPos, 1.0f};
            } else {
                dstLight.posOrDir = glm::vec4{worldPos, 2.0f};
            }

            dstLight.colorInt = glm::vec4{srcLight.color * srcLight.intensity, srcLight.range};
            dstLight.spotParams = glm::vec4{lightToTargetDir, 0.0f};

            const float cosInner{std::cos(srcLight.innerConeAngle)};
            const float cosOuter{std::cos(srcLight.outerConeAngle)};
            const float angleScale{1.0f / std::max(0.001f, cosInner - cosOuter)};
            const float angleOffset{-cosOuter * angleScale};
            sl.spotAngles[lightCount] = glm::vec4{angleScale, angleOffset,
                                                  (srcLight.type == scene::LightType::Directional) ? 1.0f : 0.0f, 0.0f};
            ++lightCount;
        }

        for (const auto& child : node.children) {
            if (child)
                self(self, *child, worldMat);
        }
    };

    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode)
            collectNodeLights(collectNodeLights, *rootNode, glm::mat4{1.0f});
    }

    // Also include any unattached scene.lights if none were referenced by nodes
    if (lightCount == 0 && !scene.lights.empty()) {
        for (const auto& srcLight : scene.lights) {
            if (lightCount >= 16u)
                break;
            auto& dstLight = sl.lights[lightCount];
            if (srcLight.type == scene::LightType::Directional) {
                dstLight.posOrDir = glm::vec4{currentLightDir_, 0.0f};
                hasExplicitDirectional = true;
            } else {
                dstLight.posOrDir =
                    glm::vec4{0.0f, 2.0f, 0.0f, (srcLight.type == scene::LightType::Point) ? 1.0f : 2.0f};
            }
            dstLight.colorInt = glm::vec4{srcLight.color * srcLight.intensity, srcLight.range};
            dstLight.spotParams = glm::vec4{0.0f, -1.0f, 0.0f, 0.0f};
            const float cosInner{std::cos(srcLight.innerConeAngle)};
            const float cosOuter{std::cos(srcLight.outerConeAngle)};
            const float angleScale{1.0f / std::max(0.001f, cosInner - cosOuter)};
            const float angleOffset{-cosOuter * angleScale};
            sl.spotAngles[lightCount] = glm::vec4{angleScale, angleOffset, 1.0f, 0.0f};
            ++lightCount;
        }
    }

    activePunctualLightCount_ = lightCount;
    BuildClusteredLightGrid(std::span<const GpuPunctualLight>{sl.lights.data(), lightCount}, lightCount, viewMatrix,
                            projMatrix, zNear, zFar, sl.clusterMasks);

    sl.envParams = glm::vec4{envRotation_, static_cast<float>(lightCount),
                             1.0f, // Enable Clustered Forward+ 3D light culling
                             hasExplicitDirectional ? 0.0f : 1.0f};
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

    // Sort draw calls: Opaques front-to-back (minimizing overdraw and grouping by material),
    // Transparents/Transmissives back-to-front (for correct alpha blending)
    std::sort(renderItemsScratch_.begin(), renderItemsScratch_.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.isTransparent != b.isTransparent) {
            return !a.isTransparent; // opaques first
        }
        if (!a.isTransparent) {
            if (a.materialIndex != b.materialIndex && std::abs(a.distance - b.distance) < 0.2f) {
                return a.materialIndex < b.materialIndex;
            }
            return a.distance < b.distance; // nearest first
        }
        return a.distance > b.distance; // farthest first
    });

    auto firstTransIt =
        std::ranges::find_if(renderItemsScratch_, [](const RenderItem& it) { return it.isTransparent; });
    transparentStartIndex_ = static_cast<size_t>(std::distance(renderItemsScratch_.begin(), firstTransIt));

    frameInstanceTransformsScratch_.clear();
    bool hasInstanced{false};
    for (const auto& item : renderItemsScratch_) {
        if (item.node && !item.node->instanceTransforms.empty()) {
            hasInstanced = true;
            break;
        }
    }
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
    if (bumpMappingEnabled_)
        baseGlobalFlags |= MaterialFlags::EnableBumpMapping;
    if (mipmappingEnabled_)
        baseGlobalFlags |= MaterialFlags::EnableMipmapping;
    if (shadowsEnabled_)
        baseGlobalFlags |= MaterialFlags::EnableShadows;
    if (reflectionsEnabled_)
        baseGlobalFlags |= MaterialFlags::EnableReflections;
    if (ambientOcclusionEnabled_)
        baseGlobalFlags |= MaterialFlags::EnableAmbientOcclusion;
    if (parallaxEnabled_)
        baseGlobalFlags |= MaterialFlags::EnableParallaxOcclusion;
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

void PipelineManager::EnsureOpaqueCopyTexture(uint32_t width, uint32_t height) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (opaqueColorCopyTexture_ && opaqueCopyWidth_ == width && opaqueCopyHeight_ == height) {
        return;
    }
    opaqueCopyWidth_ = width;
    opaqueCopyHeight_ = height;
    opaqueColorCopyView_.Reset();
    opaqueColorCopyTexture_.Reset();

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{opaqueCopyWidth_, opaqueCopyHeight_, 1};
    desc.format = WGPUTextureFormat_RGBA16Float;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    desc.label = ToWgpuStringView("OpaqueColorCopyTexture");
    opaqueColorCopyTexture_ = wgpuDeviceCreateTexture(device_, &desc);
    if (opaqueColorCopyTexture_) {
        opaqueColorCopyView_ = wgpuTextureCreateView(opaqueColorCopyTexture_, nullptr);
    }
    RebuildSceneGlobalsBindGroup();
}

glm::vec3 PipelineManager::EvaluateBeerLambertVolumeAttenuation(const glm::vec3& attenuationColor,
                                                                float attenuationDistance, float thickness,
                                                                float NdotV) noexcept {
    if (thickness <= 0.0f || attenuationDistance >= 1e8f) {
        return glm::vec3{1.0f};
    }
    const float attDist = std::max(attenuationDistance, 1e-4f);
    const glm::vec3 safeColor = glm::clamp(attenuationColor, glm::vec3{1e-4f}, glm::vec3{1.0f});
    const glm::vec3 extinction = -glm::log(safeColor) / attDist;
    const float opticalPath = thickness / std::max(NdotV, 0.1f);
    return glm::exp(-extinction * opticalPath);
}

glm::vec3 PipelineManager::EvaluateBloomThreshold(const glm::vec3& color, float exposure, float threshold) noexcept {
    const glm::vec3 exposed = color * exposure;
    const float lum = glm::dot(exposed, glm::vec3{0.2126f, 0.7152f, 0.0722f});
    if (lum <= threshold) {
        return glm::vec3{0.0f};
    }
    const float factor = (lum - threshold) / std::max(lum, 1e-4f);
    return exposed * factor;
}

float PipelineManager::EvaluateScreenSpaceOcclusionFactor(float depthCenter, float depthNeighbor, float radius,
                                                          float bias) noexcept {
    const float diff = depthCenter - depthNeighbor;
    if (diff > bias && diff < radius) {
        const float occ = (diff - bias) / std::max(radius - bias, 1e-4f);
        return std::clamp(1.0f - occ, 0.0f, 1.0f);
    }
    return 1.0f;
}

glm::vec3 PipelineManager::EvaluateRainWetAlbedo(const glm::vec3& dryAlbedo, float metallic, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f)
        return dryAlbedo;
    const float wetWeight{std::clamp(clampedInt * 0.8f, 0.0f, 0.75f) * (1.0f - std::clamp(metallic, 0.0f, 1.0f))};
    return glm::mix(dryAlbedo, dryAlbedo * 0.65f, wetWeight);
}

float PipelineManager::EvaluateRainWetRoughness(float dryRoughness, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f)
        return dryRoughness;
    const float glazeWeight{std::clamp(clampedInt * 0.85f, 0.0f, 0.88f)};
    return glm::mix(dryRoughness, 0.08f, glazeWeight);
}

float PipelineManager::EvaluateSnowAccumulationFactor(float normalY, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f || normalY <= 0.2f)
        return 0.0f;
    const float t{std::clamp((normalY - 0.2f) / (0.75f - 0.2f), 0.0f, 1.0f)};
    const float smooth{t * t * (3.0f - 2.0f * t)};
    return smooth * std::clamp(clampedInt, 0.0f, 1.5f);
}

namespace {

[[nodiscard]] inline float WrapHour24(float hour) noexcept {
    float wrapped = std::fmod(hour, 24.0f);
    return (wrapped < 0.0f) ? (wrapped + 24.0f) : wrapped;
}

[[nodiscard]] inline float ComputeSolarElevation(float hour) noexcept {
    return -std::cos((WrapHour24(hour) / 24.0f) * 2.0f * std::numbers::pi_v<float>);
}

} // namespace

void PipelineManager::SetTimeOfDay(float hour) noexcept {
    timeOfDay_ = WrapHour24(hour);
    solarElevation_ = ComputeSolarElevation(timeOfDay_);
}

float PipelineManager::EvaluateFogFactor(float distance, float worldY, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f || distance <= 0.0f) {
        return 0.0f;
    }
    const float heightDensity{std::exp(-std::max(worldY + 2.0f, 0.0f) * 0.12f)};
    const float opticalDepth{distance * (0.045f + 0.055f * heightDensity) * clampedInt};
    return std::clamp(1.0f - std::exp(-opticalDepth), 0.0f, 0.96f);
}

float PipelineManager::EvaluateSmokeDensity(float worldY, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f) {
        return 0.0f;
    }
    const float plumeEnvelope{std::exp(-std::max(worldY + 3.0f, 0.0f) * 0.08f)};
    return std::clamp(plumeEnvelope * clampedInt * 0.75f, 0.0f, 0.95f);
}

glm::vec3 PipelineManager::EvaluateDiurnalSunRadiance(float timeOfDayHours) noexcept {
    const float solarElev = ComputeSolarElevation(timeOfDayHours);
    const glm::vec3 noonSun{1.0f, 0.98f, 0.95f};
    const glm::vec3 goldenSun{1.0f, 0.48f, 0.18f};
    const glm::vec3 nightMoon{0.28f, 0.44f, 0.78f};

    if (solarElev >= 0.35f) {
        const float t = std::clamp((solarElev - 0.35f) / 0.65f, 0.0f, 1.0f);
        return glm::mix(goldenSun * 2.2f, noonSun * 3.14159265f, t);
    } else if (solarElev >= -0.10f) {
        const float t = std::clamp((solarElev + 0.10f) / 0.45f, 0.0f, 1.0f);
        return glm::mix(nightMoon * 0.55f, goldenSun * 2.2f, t);
    }
    return nightMoon * 0.55f;
}

glm::vec3 PipelineManager::EvaluateDiurnalAmbientScale(float timeOfDayHours) noexcept {
    const float solarElev = ComputeSolarElevation(timeOfDayHours);
    const glm::vec3 dayScale{1.0f, 1.0f, 1.0f};
    const glm::vec3 duskScale{0.82f, 0.60f, 0.52f};
    const glm::vec3 nightScale{0.16f, 0.22f, 0.38f};

    if (solarElev >= 0.25f) {
        const float t = std::clamp((solarElev - 0.25f) / 0.75f, 0.0f, 1.0f);
        return glm::mix(duskScale, dayScale, t);
    } else if (solarElev >= -0.15f) {
        const float t = std::clamp((solarElev + 0.15f) / 0.40f, 0.0f, 1.0f);
        return glm::mix(nightScale, duskScale, t);
    }
    return nightScale;
}

void PipelineManager::UpdateAutoLod(float deltaTime) noexcept {
    if (!autoLodEnabled_ || deltaTime <= 0.0f) {
        return;
    }
    // Target 60 FPS (16.67 ms): if frame time exceeds 18.0 ms, increase LOD bias to simplify earlier;
    // if frame time is comfortably below 14.0 ms, relax LOD bias back toward 1.0f.
    constexpr float kSlowFrameSec{1.0f / 55.0f};
    constexpr float kFastFrameSec{1.0f / 72.0f};
    if (deltaTime > kSlowFrameSec) {
        lodBias_ = std::clamp(lodBias_ + deltaTime * 0.85f, 0.25f, 4.0f);
    } else if (deltaTime < kFastFrameSec && lodBias_ > 1.0f) {
        lodBias_ = std::max(1.0f, lodBias_ - deltaTime * 0.45f);
    }
}

glm::vec3 PipelineManager::EvaluateLodHeatmapColor(uint32_t lodLevel, float transition) noexcept {
    constexpr std::array<glm::vec3, 4> kPalette{{
        {0.10f, 0.92f, 0.38f}, // LOD0: Emerald Green
        {0.96f, 0.82f, 0.12f}, // LOD1: Amber Yellow
        {0.98f, 0.45f, 0.10f}, // LOD2: Deep Orange
        {0.95f, 0.16f, 0.22f}  // LOD3: Crimson Red
    }};
    const uint32_t cur = std::min(lodLevel, 3u);
    const uint32_t next = std::min(cur + 1u, 3u);
    return glm::mix(kPalette[cur], kPalette[next], std::clamp(transition, 0.0f, 1.0f));
}

float PipelineManager::EvaluateBayerDither4x4(uint32_t x, uint32_t y) noexcept {
    constexpr std::array<float, 16> kBayer{{0.0f / 16.0f, 8.0f / 16.0f, 2.0f / 16.0f, 10.0f / 16.0f, 12.0f / 16.0f,
                                            4.0f / 16.0f, 14.0f / 16.0f, 6.0f / 16.0f, 3.0f / 16.0f, 11.0f / 16.0f,
                                            1.0f / 16.0f, 9.0f / 16.0f, 15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f,
                                            5.0f / 16.0f}};
    const uint32_t idx = ((y & 3u) * 4u) + (x & 3u);
    return kBayer[idx];
}

bool PipelineManager::IsMaterialTransparentOrTransmissive(const scene::Material& mat) noexcept {
    return mat.alphaMode == scene::AlphaMode::Blend || mat.transmissionFactor > 0.001f;
}

WGPUBlendState PipelineManager::GetTransparentBlendState() noexcept {
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    return blend;
}

core::Result<void> PipelineManager::CreateMainRenderPipelines(bool validateScope) {
    if (!device_ || !mainPipelineLayout_) {
        return std::unexpected("Cannot create main PBR render pipelines without device or pipeline layout");
    }

    auto pbrModuleRes = ShaderManager::Instance().LoadAndCompile(device_, "pbr.wgsl", "PBR_WGSL", validateScope);
    if (!pbrModuleRes) {
        return std::unexpected(pbrModuleRes.error());
    }

    auto wfModuleRes =
        ShaderManager::Instance().LoadAndCompile(device_, "wireframe.wgsl", "Wireframe_WGSL", validateScope);
    if (!wfModuleRes) {
        if (validateScope) {
            return std::unexpected(wfModuleRes.error());
        }
        BGL_LOG_WARN("Wireframe shader compilation failed: {}", wfModuleRes.error());
    }

    VertexAndInstanceLayoutStorage layoutStorage;
    FillVertexAndInstanceLayouts(layoutStorage);

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = hdrFormat_;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = pbrModuleRes->Get();
    fragmentState.entryPoint = ToWgpuStringView("fs_main");
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth24Plus;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_True;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = mainPipelineLayout_.Get();
    rpDesc.vertex.module = pbrModuleRes->Get();
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_main");
    rpDesc.vertex.bufferCount = static_cast<uint32_t>(layoutStorage.layouts.size());
    rpDesc.vertex.buffers = layoutStorage.layouts.data();
    rpDesc.fragment = &fragmentState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;
    rpDesc.multisample.count = sampleCount_;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    RenderPipelineHandle nextOpaque{wgpuDeviceCreateRenderPipeline(device_, &rpDesc)};
    rpDesc.primitive.cullMode = WGPUCullMode_Back;
    RenderPipelineHandle nextOpaqueBackCulled{wgpuDeviceCreateRenderPipeline(device_, &rpDesc)};

    const WGPUBlendState blendState = GetTransparentBlendState();
    WGPUColorTargetState blendColorTarget = colorTarget;
    blendColorTarget.blend = &blendState;

    WGPUFragmentState blendFragState = fragmentState;
    blendFragState.targets = &blendColorTarget;

    WGPUDepthStencilState blendDepthStencil = depthStencil;
    blendDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;

    WGPURenderPipelineDescriptor blendRpDesc = rpDesc;
    blendRpDesc.fragment = &blendFragState;
    blendRpDesc.depthStencil = &blendDepthStencil;
    blendRpDesc.primitive.cullMode = WGPUCullMode_None;
    RenderPipelineHandle nextBlend{wgpuDeviceCreateRenderPipeline(device_, &blendRpDesc)};
    blendRpDesc.primitive.cullMode = WGPUCullMode_Back;
    RenderPipelineHandle nextBlendBackCulled{wgpuDeviceCreateRenderPipeline(device_, &blendRpDesc)};

    if (!nextOpaque || !nextOpaqueBackCulled || !nextBlend || !nextBlendBackCulled) {
        return std::unexpected("Failed to create PBR opaque/blend render pipelines");
    }

    RenderPipelineHandle nextWireframe{};
    if (wfModuleRes) {
        WGPUFragmentState wfFragState = WGPU_FRAGMENT_STATE_INIT;
        wfFragState.module = wfModuleRes->Get();
        wfFragState.entryPoint = ToWgpuStringView("fs_main");
        wfFragState.targetCount = 1;
        wfFragState.targets = &colorTarget;

        WGPUDepthStencilState wfDepthStencil = depthStencil;
        wfDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;

        WGPURenderPipelineDescriptor wfRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        wfRpDesc.layout = mainPipelineLayout_.Get();
        wfRpDesc.vertex.module = wfModuleRes->Get();
        wfRpDesc.vertex.entryPoint = ToWgpuStringView("vs_main");
        wfRpDesc.vertex.bufferCount = static_cast<uint32_t>(layoutStorage.layouts.size());
        wfRpDesc.vertex.buffers = layoutStorage.layouts.data();
        wfRpDesc.fragment = &wfFragState;
        wfRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
        wfRpDesc.primitive.cullMode = WGPUCullMode_None;
        wfRpDesc.depthStencil = &wfDepthStencil;
        wfRpDesc.multisample.count = sampleCount_;
        wfRpDesc.multisample.mask = 0xFFFFFFFF;
        wfRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

        nextWireframe = wgpuDeviceCreateRenderPipeline(device_, &wfRpDesc);
        if (!nextWireframe && validateScope) {
            return std::unexpected("Failed to create wireframe render pipeline");
        }
    }

    opaquePipeline_ = std::move(nextOpaque);
    opaqueBackCulledPipeline_ = std::move(nextOpaqueBackCulled);
    blendPipeline_ = std::move(nextBlend);
    blendBackCulledPipeline_ = std::move(nextBlendBackCulled);
    if (nextWireframe) {
        wireframePipeline_ = std::move(nextWireframe);
    }
    return {};
}

core::Result<void> PipelineManager::CreateMipGenPipelines(bool validateScope) {
    if (!device_ || !mipGenPipelineLayout_) {
        return std::unexpected("Cannot create MipGen pipelines without device or pipeline layout");
    }

    auto shaderModuleRes =
        ShaderManager::Instance().LoadAndCompile(device_, "mipgen.wgsl", "MipGen_WGSL", validateScope);
    if (!shaderModuleRes) {
        return std::unexpected(shaderModuleRes.error());
    }

    auto createMipPipeline = [&](WGPUTextureFormat fmt, std::string_view fsEntry,
                                 std::string_view label) -> RenderPipelineHandle {
        WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
        colorTarget.format = fmt;
        colorTarget.writeMask = WGPUColorWriteMask_All;

        WGPUFragmentState fragState = WGPU_FRAGMENT_STATE_INIT;
        fragState.module = shaderModuleRes->Get();
        fragState.entryPoint = ToWgpuStringView(fsEntry);
        fragState.targetCount = 1;
        fragState.targets = &colorTarget;

        WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rpDesc.layout = mipGenPipelineLayout_.Get();
        rpDesc.vertex.module = shaderModuleRes->Get();
        rpDesc.vertex.entryPoint = ToWgpuStringView("vs_mip");
        rpDesc.vertex.bufferCount = 0;
        rpDesc.vertex.buffers = nullptr;
        rpDesc.fragment = &fragState;
        rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        rpDesc.primitive.cullMode = WGPUCullMode_None;
        rpDesc.depthStencil = nullptr;
        rpDesc.multisample.count = 1;
        rpDesc.multisample.mask = 0xFFFFFFFF;
        rpDesc.label = ToWgpuStringView(label);
        return RenderPipelineHandle{wgpuDeviceCreateRenderPipeline(device_, &rpDesc)};
    };

    RenderPipelineHandle nextUnorm = createMipPipeline(WGPUTextureFormat_RGBA8Unorm, "fs_mip", "MipGen_RGBA8Unorm");
    RenderPipelineHandle nextSrgb =
        createMipPipeline(WGPUTextureFormat_RGBA8UnormSrgb, "fs_mip", "MipGen_RGBA8UnormSrgb");
    RenderPipelineHandle nextNormal = createMipPipeline(WGPUTextureFormat_RGBA8Unorm, "fs_mip_normal", "MipGen_Normal");

    if (!nextUnorm || !nextSrgb || !nextNormal) {
        return std::unexpected("Failed to create MipGen render pipelines");
    }

    mipGenUnormPipeline_ = std::move(nextUnorm);
    mipGenSrgbPipeline_ = std::move(nextSrgb);
    mipGenNormalPipeline_ = std::move(nextNormal);
    return {};
}

core::Result<void> PipelineManager::ReloadShaders(std::span<const std::string> modifiedVirtualPaths) {
    if (!initialized_ || !device_) {
        return std::unexpected("Cannot reload shaders before PipelineManager initialization");
    }

    bool needMain{false};
    bool needMipGen{false};
    bool needShadow{false};
    bool needTonemap{false};
    bool needAabb{false};
    bool needGpuCull{false};
    bool needGpuRayPick{false};

    for (const auto& path : modifiedVirtualPaths) {
        if (path == "pbr.wgsl" || path == "wireframe.wgsl") {
            needMain = true;
        } else if (path == "mipgen.wgsl") {
            needMipGen = true;
        } else if (path == "shadow.wgsl") {
            needShadow = true;
        } else if (path == "tonemap.wgsl") {
            needTonemap = true;
        } else if (path == "aabb.wgsl") {
            needAabb = true;
        } else if (path == "gpu_cull.wgsl") {
            needGpuCull = true;
        } else if (path == "gpu_ray_pick.wgsl") {
            needGpuRayPick = true;
        }
    }

    std::string firstError{};
    auto checkRes = [&](const core::Result<void>& res, std::string_view tag) {
        if (!res) {
            BGL_LOG_ERROR("WGSL hot-reload failed for {}: {}", tag, res.error());
            if (firstError.empty()) {
                firstError = res.error();
            }
        } else {
            BGL_LOG_INFO("WGSL hot-reloaded: {}", tag);
        }
    };

    if (needMain) {
        checkRes(CreateMainRenderPipelines(true), "pbr/wireframe");
    }
    if (needMipGen) {
        checkRes(CreateMipGenPipelines(true), "mipgen.wgsl");
    }
    if (needShadow) {
        checkRes(shadowPass_.ReloadShader(), "shadow.wgsl");
    }
    if (needTonemap) {
        checkRes(postProcessPass_.ReloadShader(), "tonemap.wgsl");
    }
    if (needAabb) {
        checkRes(aabbRenderer_.ReloadShader(), "aabb.wgsl");
    }
    if (needGpuCull) {
        checkRes(gpuCuller_.ReloadShader(), "gpu_cull.wgsl");
    }
    if (needGpuRayPick) {
        checkRes(gpuRayPicker_.ReloadShader(), "gpu_ray_pick.wgsl");
    }

    if (!firstError.empty()) {
        return std::unexpected(std::move(firstError));
    }
    return {};
}

} // namespace bgl::gpu
