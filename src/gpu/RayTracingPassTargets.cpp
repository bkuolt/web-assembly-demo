/**
 * @file RayTracingPassTargets.cpp
 * @brief Offscreen G-Buffer, RT denoiser target allocation, release, and bind group management.
 */
#include "core/MemoryTracker.hpp"
#include "gpu/RayTracingPass.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <string>

#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

namespace {

#if defined(BGL_PLATFORM_NATIVE)
[[nodiscard]] size_t CalcRtTargetTrackedBytes(uint32_t fullW, uint32_t fullH, uint32_t traceW,
                                              uint32_t traceH) noexcept {
    const size_t fullPixels = static_cast<size_t>(fullW) * fullH;
    const size_t tracePixels = static_cast<size_t>(traceW) * traceH;
    // Full-res: GBuffer NormalRough(8) + AlbedoMetal(4) + Depth(4) + ShadowFinal(4) + AoFinal(4)
    // Trace-res: Shadow(4*4) + AO(4*4) + Reflections(8*4)
    return fullPixels * (8u + 4u + 4u + 4u + 4u) + tracePixels * (4u * 4u + 4u * 4u + 8u * 4u);
}

void CreateTex2D(WGPUDevice device, uint32_t w, uint32_t h, WGPUTextureFormat fmt, WGPUTextureUsage usage,
                 const char* label, TextureHandle& outTex, TextureViewHandle& outView) {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = usage;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{w, h, 1};
    desc.format = fmt;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    desc.label = ToWgpuStringView(label);
    outTex = wgpuDeviceCreateTexture(device, &desc);
    if (outTex) {
        outView = wgpuTextureCreateView(outTex.Get(), nullptr);
    }
}

[[nodiscard]] BindGroupHandle MakeBindGroup(WGPUDevice device, WGPUBindGroupLayout layout,
                                            std::span<const WGPUBindGroupEntry> entries, const char* label) {
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = layout;
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    desc.label = ToWgpuStringView(label);
    return BindGroupHandle{wgpuDeviceCreateBindGroup(device, &desc)};
}

[[nodiscard]] WGPUBindGroupEntry MakeUniformBgEntry(uint32_t binding, WGPUBuffer buf, uint64_t size) noexcept {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = binding;
    e.buffer = buf;
    e.offset = 0;
    e.size = size;
    return e;
}

[[nodiscard]] WGPUBindGroupEntry MakeTexBgEntry(uint32_t binding, WGPUTextureView view) noexcept {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = binding;
    e.textureView = view;
    return e;
}

[[nodiscard]] WGPUBindGroupEntry MakeSamplerBgEntry(uint32_t binding, WGPUSampler samp) noexcept {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = binding;
    e.sampler = samp;
    return e;
}
#endif

} // namespace

void RayTracingPass::EnsureResolutionTargets(uint32_t fullW, uint32_t fullH) {
#if defined(BGL_PLATFORM_NATIVE)
    if (!device_ || fullW == 0 || fullH == 0) {
        return;
    }
    const uint32_t nextTraceW = config_.halfRes ? std::max(1u, (fullW + 1u) / 2u) : fullW;
    const uint32_t nextTraceH = config_.halfRes ? std::max(1u, (fullH + 1u) / 2u) : fullH;
    if (fullW == fullWidth_ && fullH == fullHeight_ && nextTraceW == traceWidth_ && nextTraceH == traceHeight_ &&
        config_.halfRes == lastHalfRes_ && gbufferDepthView_ != nullptr) {
        return;
    }

    ReleaseResolutionTargets();
    fullWidth_ = fullW;
    fullHeight_ = fullH;
    traceWidth_ = nextTraceW;
    traceHeight_ = nextTraceH;
    lastHalfRes_ = config_.halfRes;
    frameIndex_ = 0;
    pathSampleIndex_ = 0;

    const WGPUTextureUsage rtStorageUsage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding;
    const WGPUTextureUsage gbufUsage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;

    // 1. G-Buffer (Full resolution 1x)
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA16Float, gbufUsage, "RT_GbufferNormalRough",
                gbufferNormalRoughnessTex_, gbufferNormalRoughnessView_);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA8Unorm, gbufUsage, "RT_GbufferAlbedoMetal",
                gbufferAlbedoMetallicTex_, gbufferAlbedoMetallicView_);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_Depth32Float, gbufUsage, "RT_GbufferDepth",
                gbufferDepthTex_, gbufferDepthView_);

    // 2. RT Shadows (Trace resolution + full-res output)
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_ShadowRaw",
                shadowRawTex_, shadowRawView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_ShadowScratch",
                shadowScratchTex_, shadowScratchView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_ShadowHist0",
                shadowHistoryTex_[0], shadowHistoryView_[0]);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_ShadowHist1",
                shadowHistoryTex_[1], shadowHistoryView_[1]);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_ShadowFinal",
                shadowFinalTex_, shadowFinalView_);

    // 3. RTAO (Trace resolution + full-res output)
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_AoRaw", aoRawTex_,
                aoRawView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_AoScratch",
                aoScratchTex_, aoScratchView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_AoHist0",
                aoHistoryTex_[0], aoHistoryView_[0]);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_AoHist1",
                aoHistoryTex_[1], aoHistoryView_[1]);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA8Unorm, rtStorageUsage, "RT_AoFinal", aoFinalTex_,
                aoFinalView_);

    // 4. RT Reflections (Trace resolution HDR)
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, rtStorageUsage, "RT_ReflRaw",
                reflRawTex_, reflRawView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, rtStorageUsage, "RT_ReflScratch",
                reflScratchTex_, reflScratchView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, rtStorageUsage, "RT_ReflHist0",
                reflHistoryTex_[0], reflHistoryView_[0]);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, rtStorageUsage, "RT_ReflHist1",
                reflHistoryTex_[1], reflHistoryView_[1]);

    // 5. Progressive Path Tracer FP32 Accumulation Buffer
    const uint64_t accumBytes = static_cast<uint64_t>(fullWidth_) * fullHeight_ * sizeof(glm::vec4);
    WGPUBufferDescriptor accDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    accDesc.size = accumBytes;
    accDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    accDesc.label = ToWgpuStringView("RT_PathAccum_SSBO");
    pathAccumBuffer_ = wgpuDeviceCreateBuffer(device_, &accDesc);
    if (pathAccumBuffer_) {
        pathAccumBytes_ = accumBytes;
        core::MemoryTracker::Instance().TrackBufferAlloc(pathAccumBytes_);
    }

    core::MemoryTracker::Instance().TrackTextureAlloc(
        CalcRtTargetTrackedBytes(fullWidth_, fullHeight_, traceWidth_, traceHeight_));

    RebuildPassBindGroups();
#else
    (void)fullW;
    (void)fullH;
#endif
}

void RayTracingPass::RebuildPassBindGroups() {
#if defined(BGL_PLATFORM_NATIVE)
    if (!device_ || !gbufferDepthView_ || !passUniformBuffer_ || !envCubemapView_ || !defaultSampler_ ||
        !linearSampler_ || !pathAccumBuffer_) {
        return;
    }

    const WGPUBindGroupEntry dynUbo = MakeUniformBgEntry(0, passUniformBuffer_.Get(), sizeof(RtPassUniforms));
    const WGPUBindGroupEntry depthEntry = MakeTexBgEntry(1, gbufferDepthView_.Get());
    const WGPUBindGroupEntry normRoughEntry = MakeTexBgEntry(2, gbufferNormalRoughnessView_.Get());

    // Trace8 bind groups (Shadow & AO)
    shadowTraceBg_ = MakeBindGroup(
        device_, trace8Bgl_.Get(),
        std::array<WGPUBindGroupEntry, 4>{dynUbo, depthEntry, normRoughEntry, MakeTexBgEntry(3, shadowRawView_.Get())},
        "RT_ShadowTrace_BG");
    aoTraceBg_ = MakeBindGroup(
        device_, trace8Bgl_.Get(),
        std::array<WGPUBindGroupEntry, 4>{dynUbo, depthEntry, normRoughEntry, MakeTexBgEntry(3, aoRawView_.Get())},
        "RT_AoTrace_BG");

    // Reflections trace bind group
    reflTraceBg_ = MakeBindGroup(device_, reflBgl_.Get(),
                                 std::array<WGPUBindGroupEntry, 7>{
                                     dynUbo, depthEntry, normRoughEntry,
                                     MakeTexBgEntry(3, gbufferAlbedoMetallicView_.Get()),
                                     MakeSamplerBgEntry(4, defaultSampler_), MakeTexBgEntry(5, envCubemapView_),
                                     MakeTexBgEntry(6, reflRawView_.Get())},
                                 "RT_ReflTrace_BG");

    // PathTrace bind group
    pathTraceBg_ = MakeBindGroup(device_, pathTraceBgl_.Get(),
                                 std::array<WGPUBindGroupEntry, 4>{
                                     dynUbo, MakeSamplerBgEntry(1, defaultSampler_), MakeTexBgEntry(2, envCubemapView_),
                                     MakeUniformBgEntry(3, pathAccumBuffer_.Get(), pathAccumBytes_)},
                                 "RT_PathTrace_BG");

    // Unified Denoise bind group builder for 8-bit (Shadow/AO) and 16-bit HDR (Reflections)
    auto buildDenoiseSet = [&](WGPUBindGroupLayout bgl, WGPUTextureView rawV, WGPUTextureView scratchV,
                               const std::array<TextureViewHandle, 2>& histV, WGPUTextureView finalV,
                               const char* tag, DenoiseBindGroupSet& outSet) {
        auto makeStep = [&](WGPUTextureView srcV, WGPUTextureView histPrevV, WGPUTextureView dstV, const char* step) {
            const std::string lbl = std::string{"RT_"} + tag + step;
            return MakeBindGroup(device_, bgl,
                                 std::array<WGPUBindGroupEntry, 6>{dynUbo, depthEntry, normRoughEntry,
                                                                   MakeTexBgEntry(3, srcV),
                                                                   MakeTexBgEntry(4, histPrevV),
                                                                   MakeTexBgEntry(5, dstV)},
                                 lbl.c_str());
        };
        for (uint32_t h = 0; h < 2u; ++h) {
            const uint32_t prevH = 1u - h;
            outSet.temporal[h] = makeStep(rawV, histV[prevH].Get(), histV[h].Get(), "_Temporal");
            outSet.atrousStep1[h] = makeStep(histV[h].Get(), histV[h].Get(), rawV, "_Atrous1");
        }
        outSet.atrousStep2 = makeStep(rawV, rawV, scratchV, "_Atrous2");
        outSet.atrousStep3 = makeStep(scratchV, scratchV, rawV, "_Atrous3");
        if (finalV) {
            outSet.upsample = makeStep(rawV, rawV, finalV, "_Upsample");
        } else {
            outSet.upsample.Reset();
        }
    };

    buildDenoiseSet(denoise8Bgl_.Get(), shadowRawView_.Get(), shadowScratchView_.Get(), shadowHistoryView_,
                    shadowFinalView_.Get(), "Shadow8", shadowBgs_);
    buildDenoiseSet(denoise8Bgl_.Get(), aoRawView_.Get(), aoScratchView_.Get(), aoHistoryView_, aoFinalView_.Get(),
                    "Ao8", aoBgs_);
    buildDenoiseSet(denoise16Bgl_.Get(), reflRawView_.Get(), reflScratchView_.Get(), reflHistoryView_, nullptr,
                    "Refl16", reflBgs_);

    // Composite & PathTrace Present bind groups
    const std::array<WGPUBindGroupEntry, 5> compEntries{
        MakeUniformBgEntry(0, compositeUniformBuffer_.Get(), sizeof(RtCompositeUniforms)),
        MakeSamplerBgEntry(1, linearSampler_.Get()),
        MakeTexBgEntry(2, reflRawView_.Get()),
        MakeTexBgEntry(3, gbufferDepthView_.Get()),
        MakeUniformBgEntry(4, pathAccumBuffer_.Get(), pathAccumBytes_),
    };
    compositeReflBg_ = MakeBindGroup(device_, compositeBgl_.Get(), compEntries, "RT_CompositeRefl_BG");
    pathPresentBg_ = MakeBindGroup(device_, compositeBgl_.Get(), compEntries, "RT_PathPresent_BG");
#endif
}

void RayTracingPass::ReleaseResolutionTargets() noexcept {
    shadowTraceBg_.Reset();
    aoTraceBg_.Reset();
    reflTraceBg_.Reset();
    pathTraceBg_.Reset();
    compositeReflBg_.Reset();
    pathPresentBg_.Reset();
    shadowBgs_.Reset();
    aoBgs_.Reset();
    reflBgs_.Reset();

#if defined(BGL_PLATFORM_NATIVE)
    if (fullWidth_ > 0 && fullHeight_ > 0) {
        core::MemoryTracker::Instance().TrackTextureFree(
            CalcRtTargetTrackedBytes(fullWidth_, fullHeight_, traceWidth_, traceHeight_));
    }
#endif

    gbufferNormalRoughnessView_.Reset();
    gbufferNormalRoughnessTex_.Reset();
    gbufferAlbedoMetallicView_.Reset();
    gbufferAlbedoMetallicTex_.Reset();
    gbufferDepthView_.Reset();
    gbufferDepthTex_.Reset();

    shadowRawView_.Reset();
    shadowRawTex_.Reset();
    shadowScratchView_.Reset();
    shadowScratchTex_.Reset();
    for (size_t i = 0; i < 2; ++i) {
        shadowHistoryView_[i].Reset();
        shadowHistoryTex_[i].Reset();
        aoHistoryView_[i].Reset();
        aoHistoryTex_[i].Reset();
        reflHistoryView_[i].Reset();
        reflHistoryTex_[i].Reset();
    }
    shadowFinalView_.Reset();
    shadowFinalTex_.Reset();
    aoRawView_.Reset();
    aoRawTex_.Reset();
    aoScratchView_.Reset();
    aoScratchTex_.Reset();
    aoFinalView_.Reset();
    aoFinalTex_.Reset();
    reflRawView_.Reset();
    reflRawTex_.Reset();
    reflScratchView_.Reset();
    reflScratchTex_.Reset();

    if (pathAccumBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(pathAccumBytes_);
        pathAccumBuffer_.Reset();
    }
    pathAccumBytes_ = 0;
    fullWidth_ = 0;
    fullHeight_ = 0;
    traceWidth_ = 0;
    traceHeight_ = 0;
}

} // namespace bgl::gpu
