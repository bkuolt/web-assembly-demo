/**
 * @file RayTracingPassPipelines.cpp
 * @brief Pipeline compilation, bind group layouts, lifecycle, and shader hot-reload for RayTracingPass.
 */
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/RayTracingPass.hpp"
#include "gpu/ShaderManager.hpp"
#include <array>
#include <span>
#include <string>
#include <utility>

#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

namespace {

#if defined(BGL_PLATFORM_NATIVE)
[[nodiscard]] WGPUBindGroupLayoutEntry MakeUniformEntry(uint32_t binding, WGPUShaderStage visibility, uint64_t minSize,
                                                        bool dynamicOffset = false) noexcept {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = binding;
    e.visibility = visibility;
    e.buffer.type = WGPUBufferBindingType_Uniform;
    e.buffer.hasDynamicOffset = dynamicOffset ? WGPU_TRUE : WGPU_FALSE;
    e.buffer.minBindingSize = minSize;
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry MakeBufferEntry(uint32_t binding, WGPUShaderStage visibility,
                                                       WGPUBufferBindingType type, uint64_t minSize) noexcept {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = binding;
    e.visibility = visibility;
    e.buffer.type = type;
    e.buffer.minBindingSize = minSize;
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry MakeSamplerEntry(uint32_t binding, WGPUShaderStage visibility) noexcept {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = binding;
    e.visibility = visibility;
    e.sampler.type = WGPUSamplerBindingType_Filtering;
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry
MakeTexEntry(uint32_t binding, WGPUShaderStage visibility, WGPUTextureSampleType sampleType,
             WGPUTextureViewDimension dim = WGPUTextureViewDimension_2D) noexcept {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = binding;
    e.visibility = visibility;
    e.texture.sampleType = sampleType;
    e.texture.viewDimension = dim;
    e.texture.multisampled = WGPU_FALSE;
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry MakeStorageTexEntry(uint32_t binding, WGPUTextureFormat fmt) noexcept {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = binding;
    e.visibility = WGPUShaderStage_Compute;
    e.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
    e.storageTexture.format = fmt;
    e.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
    return e;
}

[[nodiscard]] BindGroupLayoutHandle MakeBgl(WGPUDevice device, std::span<const WGPUBindGroupLayoutEntry> entries,
                                            const char* label) {
    WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    desc.label = ToWgpuStringView(label);
    return BindGroupLayoutHandle{wgpuDeviceCreateBindGroupLayout(device, &desc)};
}

[[nodiscard]] PipelineLayoutHandle MakePl(WGPUDevice device, std::span<const WGPUBindGroupLayout> bgls,
                                          const char* label) {
    WGPUPipelineLayoutDescriptor desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    desc.bindGroupLayoutCount = bgls.size();
    desc.bindGroupLayouts = bgls.data();
    desc.label = ToWgpuStringView(label);
    return PipelineLayoutHandle{wgpuDeviceCreatePipelineLayout(device, &desc)};
}

[[nodiscard]] core::Result<ComputePipelineHandle> MakeComputePipe(WGPUDevice device, WGPUPipelineLayout layout,
                                                                  WGPUShaderModule mod, std::string_view entry,
                                                                  std::string_view label) {
    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = layout;
    cpDesc.compute.module = mod;
    cpDesc.compute.entryPoint = ToWgpuStringView(entry);
    cpDesc.label = ToWgpuStringView(label);
    ComputePipelineHandle pipe{wgpuDeviceCreateComputePipeline(device, &cpDesc)};
    if (!pipe) {
        return std::unexpected("Failed to create RT compute pipeline: " + std::string{label});
    }
    return pipe;
}
#endif

} // namespace

core::Result<void> RayTracingPass::CreateLayoutsAndBuffers(WGPUPipelineLayout mainPipelineLayout,
                                                           const RayTracingScene& rtScene) {
#if defined(BGL_PLATFORM_NATIVE)
    mainPipelineLayout_ = mainPipelineLayout;
    sceneBgl_ = rtScene.GetSceneBindGroupLayout();
    bindlessActive_ = rtScene.HasBindlessTextures() && (rtScene.GetBindlessTexturesBgl() != nullptr);

    auto makeBuffer = [&](uint64_t size, WGPUBufferUsage usage, const char* lbl, bool trackAlloc) -> BufferHandle {
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = size;
        desc.usage = usage;
        desc.label = ToWgpuStringView(lbl);
        BufferHandle buf{wgpuDeviceCreateBuffer(device_, &desc)};
        if (buf && trackAlloc) {
            core::MemoryTracker::Instance().TrackBufferAlloc(size);
        }
        return buf;
    };

    passUniformBuffer_ = makeBuffer(static_cast<uint64_t>(kMaxUniformSlots) * kUniformSlotStride,
                                    WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "RT_PassUniformRing", true);
    compositeUniformBuffer_ = makeBuffer(sizeof(RtCompositeUniforms), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                                         "RT_CompositeUniforms", true);
    if (!passUniformBuffer_ || !compositeUniformBuffer_) {
        return std::unexpected("Failed to create RayTracingPass uniform buffers");
    }

    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampDesc.maxAnisotropy = 1;
    linearSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    auto makeSecondaryPl = [&](WGPUBindGroupLayout passBgl, const char* stdLbl, const char* bindlessLbl) {
        if (bindlessActive_) {
            const std::array<WGPUBindGroupLayout, 3> pls{passBgl, sceneBgl_, rtScene.GetBindlessTexturesBgl()};
            return MakePl(device_, pls, bindlessLbl);
        }
        const std::array<WGPUBindGroupLayout, 2> pls{passBgl, sceneBgl_};
        return MakePl(device_, pls, stdLbl);
    };

    // 1. trace8Bgl (Shadows & AO): UBO(dyn) + Depth + NormalRoughness + StorageRGBA8
    {
        const std::array<WGPUBindGroupLayoutEntry, 4> e{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeTexEntry(1, WGPUShaderStage_Compute, WGPUTextureSampleType_Depth),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeStorageTexEntry(3, WGPUTextureFormat_RGBA8Unorm),
        };
        trace8Bgl_ = MakeBgl(device_, e, "RT_Trace8_BGL");
        const std::array<WGPUBindGroupLayout, 2> pls{trace8Bgl_.Get(), sceneBgl_};
        trace8Pl_ = MakePl(device_, pls, "RT_Trace8_PL");
    }

    // 2. reflBgl: UBO(dyn) + Depth + NormalRoughness + AlbedoMetallic + Sampler + EnvCube + StorageRGBA16F
    {
        const std::array<WGPUBindGroupLayoutEntry, 7> e{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeTexEntry(1, WGPUShaderStage_Compute, WGPUTextureSampleType_Depth),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(3, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeSamplerEntry(4, WGPUShaderStage_Compute),
            MakeTexEntry(5, WGPUShaderStage_Compute, WGPUTextureSampleType_Float, WGPUTextureViewDimension_Cube),
            MakeStorageTexEntry(6, WGPUTextureFormat_RGBA16Float),
        };
        reflBgl_ = MakeBgl(device_, e, "RT_Refl_BGL");
        reflPl_ = makeSecondaryPl(reflBgl_.Get(), "RT_Refl_PL", "RT_ReflBindless_PL");
    }

    // 3. denoise8Bgl & denoise16Bgl: UBO(dyn) + Depth + NormalRoughness + srcTex + historyTex + dstStorage
    auto makeDenoiseLayout = [&](WGPUTextureFormat storageFmt, const char* bglLbl, const char* plLbl,
                                 BindGroupLayoutHandle& outBgl, PipelineLayoutHandle& outPl) {
        const std::array<WGPUBindGroupLayoutEntry, 6> e{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeTexEntry(1, WGPUShaderStage_Compute, WGPUTextureSampleType_Depth),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(3, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(4, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeStorageTexEntry(5, storageFmt),
        };
        outBgl = MakeBgl(device_, e, bglLbl);
        const std::array<WGPUBindGroupLayout, 1> pls{outBgl.Get()};
        outPl = MakePl(device_, pls, plLbl);
    };
    makeDenoiseLayout(WGPUTextureFormat_RGBA8Unorm, "RT_Denoise8_BGL", "RT_Denoise8_PL", denoise8Bgl_, denoise8Pl_);
    makeDenoiseLayout(WGPUTextureFormat_RGBA16Float, "RT_Denoise16_BGL", "RT_Denoise16_PL", denoise16Bgl_,
                      denoise16Pl_);

    // 4. compositeBgl: UBO + Sampler + reflTex + Depth + pathAccum SSBO
    {
        const std::array<WGPUBindGroupLayoutEntry, 5> e{
            MakeUniformEntry(0, WGPUShaderStage_Fragment, sizeof(RtCompositeUniforms), false),
            MakeSamplerEntry(1, WGPUShaderStage_Fragment),
            MakeTexEntry(2, WGPUShaderStage_Fragment, WGPUTextureSampleType_Float),
            MakeTexEntry(3, WGPUShaderStage_Fragment, WGPUTextureSampleType_Depth),
            MakeBufferEntry(4, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage, sizeof(glm::vec4)),
        };
        compositeBgl_ = MakeBgl(device_, e, "RT_Composite_BGL");
        const std::array<WGPUBindGroupLayout, 1> plc{compositeBgl_.Get()};
        compositePl_ = MakePl(device_, plc, "RT_Composite_PL");
    }

    // 5. pathTraceBgl: UBO(dyn) + Sampler + EnvCube + pathAccum(rw)
    {
        const std::array<WGPUBindGroupLayoutEntry, 4> e{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeSamplerEntry(1, WGPUShaderStage_Compute),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float, WGPUTextureViewDimension_Cube),
            MakeBufferEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage, sizeof(glm::vec4)),
        };
        pathTraceBgl_ = MakeBgl(device_, e, "RT_PathTrace_BGL");
        pathTracePl_ = makeSecondaryPl(pathTraceBgl_.Get(), "RT_PathTrace_PL", "RT_PathTraceBindless_PL");
    }

    if (supportsTimestampQueries_) {
        WGPUQuerySetDescriptor qDesc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        qDesc.type = WGPUQueryType_Timestamp;
        qDesc.count = 4u;
        qDesc.label = ToWgpuStringView("RT_TimestampQuerySet");
        timestampQuerySet_ = wgpuDeviceCreateQuerySet(device_, &qDesc);

        timestampResolveBuf_ = makeBuffer(4u * sizeof(uint64_t), WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc,
                                          "RT_TimestampResolve", false);
        timestampReadbackBuf_ = makeBuffer(4u * sizeof(uint64_t), WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
                                           "RT_TimestampReadback", false);

        timestampPeriodNs_ = wgpuQueueGetTimestampPeriod(queue_);
        if (timestampPeriodNs_ <= 0.0f) {
            timestampPeriodNs_ = 1.0f;
        }
    }

    if (!trace8Bgl_ || !trace8Pl_ || !reflBgl_ || !reflPl_ || !denoise8Bgl_ || !denoise8Pl_ || !denoise16Bgl_ ||
        !denoise16Pl_ || !compositeBgl_ || !compositePl_ || !pathTraceBgl_ || !pathTracePl_) {
        return std::unexpected("Failed to create one or more RayTracingPass layouts");
    }
    return {};
#else
    (void)mainPipelineLayout;
    (void)rtScene;
    return {};
#endif
}

core::Result<void> RayTracingPass::CreatePipelines(bool validateScope, bool allowBindlessFallback) {
#if defined(BGL_PLATFORM_NATIVE)
    auto compile = [&](std::string_view file, const char* lbl) {
        return ShaderManager::Instance().LoadAndCompile(device_, file, lbl, validateScope);
    };

    auto gbufMod = compile("rt_gbuffer.wgsl", "RT_GBuffer_WGSL");
    auto shdMod = compile("rt_shadows.wgsl", "RT_Shadows_WGSL");
    auto aoMod = compile("rt_ao.wgsl", "RT_AO_WGSL");
    auto den8Mod = compile("rt_denoise.wgsl", "RT_Denoise8_WGSL");
    auto den16Mod = compile("rt_denoise_hdr.wgsl", "RT_Denoise16_WGSL");
    auto compMod = compile("rt_composite.wgsl", "RT_Composite_WGSL");

    if (!gbufMod || !shdMod || !aoMod || !den8Mod || !den16Mod || !compMod) {
        return std::unexpected("Failed to compile core desktop RayTracing WGSL shaders");
    }

    auto reflMod = compile(bindlessActive_ ? "rt_reflections_bindless.wgsl" : "rt_reflections.wgsl", "RT_Refl_WGSL");
    auto ptMod = compile(bindlessActive_ ? "rt_pathtrace_bindless.wgsl" : "rt_pathtrace.wgsl", "RT_PathTrace_WGSL");

    // Graceful fallback to non-bindless shaders if driver/naga rejects binding_array at compile time
    if ((!reflMod || !ptMod) && bindlessActive_ && allowBindlessFallback) {
        BGL_LOG_WARN("Bindless RT WGSL compilation failed; falling back to non-bindless secondary hit shading");
        bindlessActive_ = false;
        const std::array<WGPUBindGroupLayout, 2> rPls{reflBgl_.Get(), sceneBgl_};
        reflPl_ = MakePl(device_, rPls, "RT_Refl_PL");
        const std::array<WGPUBindGroupLayout, 2> pPls{pathTraceBgl_.Get(), sceneBgl_};
        pathTracePl_ = MakePl(device_, pPls, "RT_PathTrace_PL");
        reflMod = compile("rt_reflections.wgsl", "RT_Refl_WGSL");
        ptMod = compile("rt_pathtrace.wgsl", "RT_PathTrace_WGSL");
    }
    if (!reflMod || !ptMod) {
        return std::unexpected("Failed to compile RT reflection/pathtrace WGSL shaders");
    }

    // 1. G-Buffer pipelines (2 color attachments: RGBA16Float + RGBA8Unorm, Depth32Float)
    VertexAndInstanceLayoutStorage layoutStorage;
    FillVertexAndInstanceLayouts(layoutStorage);

    std::array<WGPUColorTargetState, 2> gbufTargets{};
    gbufTargets[0] = WGPU_COLOR_TARGET_STATE_INIT;
    gbufTargets[0].format = WGPUTextureFormat_RGBA16Float;
    gbufTargets[0].writeMask = WGPUColorWriteMask_All;
    gbufTargets[1] = WGPU_COLOR_TARGET_STATE_INIT;
    gbufTargets[1].format = WGPUTextureFormat_RGBA8Unorm;
    gbufTargets[1].writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState gbufFrag = WGPU_FRAGMENT_STATE_INIT;
    gbufFrag.module = gbufMod->Get();
    gbufFrag.entryPoint = ToWgpuStringView("fs_gbuffer");
    gbufFrag.targetCount = gbufTargets.size();
    gbufFrag.targets = gbufTargets.data();

    WGPUDepthStencilState gbufDepth = WGPU_DEPTH_STENCIL_STATE_INIT;
    gbufDepth.format = WGPUTextureFormat_Depth32Float;
    gbufDepth.depthWriteEnabled = WGPUOptionalBool_True;
    gbufDepth.depthCompare = WGPUCompareFunction_LessEqual;

    WGPURenderPipelineDescriptor gbufRp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    gbufRp.layout = mainPipelineLayout_;
    gbufRp.vertex.module = gbufMod->Get();
    gbufRp.vertex.entryPoint = ToWgpuStringView("vs_gbuffer");
    gbufRp.vertex.bufferCount = static_cast<uint32_t>(layoutStorage.layouts.size());
    gbufRp.vertex.buffers = layoutStorage.layouts.data();
    gbufRp.fragment = &gbufFrag;
    gbufRp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    gbufRp.primitive.frontFace = WGPUFrontFace_CCW;
    gbufRp.primitive.cullMode = WGPUCullMode_None;
    gbufRp.depthStencil = &gbufDepth;
    gbufRp.multisample.count = 1;
    gbufRp.multisample.mask = 0xFFFFFFFF;
    gbufRp.label = ToWgpuStringView("RT_GBufferPipeline");

    RenderPipelineHandle nextGbuf{wgpuDeviceCreateRenderPipeline(device_, &gbufRp)};
    gbufRp.primitive.cullMode = WGPUCullMode_Back;
    gbufRp.label = ToWgpuStringView("RT_GBufferBackCulledPipeline");
    RenderPipelineHandle nextGbufBack{wgpuDeviceCreateRenderPipeline(device_, &gbufRp)};
    if (!nextGbuf || !nextGbufBack) {
        return std::unexpected("Failed to create RT G-Buffer render pipelines");
    }

    // 2. Compute pipelines
    auto shdPipe = MakeComputePipe(device_, trace8Pl_.Get(), shdMod->Get(), "cs_rt_shadows", "RT_ShadowsPipeline");
    auto aoPipe = MakeComputePipe(device_, trace8Pl_.Get(), aoMod->Get(), "cs_rt_ao", "RT_AOPipeline");
    auto reflPipe = MakeComputePipe(device_, reflPl_.Get(), reflMod->Get(), "cs_rt_reflections", "RT_ReflPipeline");
    auto tmp8Pipe =
        MakeComputePipe(device_, denoise8Pl_.Get(), den8Mod->Get(), "cs_temporal_8", "RT_Temporal8Pipeline");
    auto atr8Pipe = MakeComputePipe(device_, denoise8Pl_.Get(), den8Mod->Get(), "cs_atrous_8", "RT_Atrous8Pipeline");
    auto up8Pipe = MakeComputePipe(device_, denoise8Pl_.Get(), den8Mod->Get(), "cs_upsample_8", "RT_Upsample8Pipeline");
    auto tmp16Pipe =
        MakeComputePipe(device_, denoise16Pl_.Get(), den16Mod->Get(), "cs_temporal_16", "RT_Temporal16Pipeline");
    auto atr16Pipe =
        MakeComputePipe(device_, denoise16Pl_.Get(), den16Mod->Get(), "cs_atrous_16", "RT_Atrous16Pipeline");
    auto ptPipe = MakeComputePipe(device_, pathTracePl_.Get(), ptMod->Get(), "cs_pathtrace", "RT_PathTracePipeline");

    if (!shdPipe || !aoPipe || !reflPipe || !tmp8Pipe || !atr8Pipe || !up8Pipe || !tmp16Pipe || !atr16Pipe || !ptPipe) {
        return std::unexpected("Failed to create one or more RT compute pipelines");
    }

    // 3. Fullscreen composite / path-trace present pipelines targeting HDR RGBA16Float
    WGPUBlendState addBlend = WGPU_BLEND_STATE_INIT;
    addBlend.color.srcFactor = WGPUBlendFactor_One;
    addBlend.color.dstFactor = WGPUBlendFactor_One;
    addBlend.color.operation = WGPUBlendOperation_Add;
    addBlend.alpha.srcFactor = WGPUBlendFactor_Zero;
    addBlend.alpha.dstFactor = WGPUBlendFactor_One;
    addBlend.alpha.operation = WGPUBlendOperation_Add;

    WGPUColorTargetState compTarget = WGPU_COLOR_TARGET_STATE_INIT;
    compTarget.format = WGPUTextureFormat_RGBA16Float;
    compTarget.writeMask = WGPUColorWriteMask_All;
    compTarget.blend = &addBlend;

    WGPUFragmentState compFrag = WGPU_FRAGMENT_STATE_INIT;
    compFrag.module = compMod->Get();
    compFrag.entryPoint = ToWgpuStringView("fs_rt_composite_refl");
    compFrag.targetCount = 1;
    compFrag.targets = &compTarget;

    WGPURenderPipelineDescriptor compRp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    compRp.layout = compositePl_.Get();
    compRp.vertex.module = compMod->Get();
    compRp.vertex.entryPoint = ToWgpuStringView("vs_rt_fullscreen");
    compRp.fragment = &compFrag;
    compRp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    compRp.primitive.cullMode = WGPUCullMode_None;
    compRp.multisample.count = 1;
    compRp.multisample.mask = 0xFFFFFFFF;
    compRp.label = ToWgpuStringView("RT_CompositeReflPipeline");
    RenderPipelineHandle nextCompRefl{wgpuDeviceCreateRenderPipeline(device_, &compRp)};

    WGPUColorTargetState ptTarget = WGPU_COLOR_TARGET_STATE_INIT;
    ptTarget.format = WGPUTextureFormat_RGBA16Float;
    ptTarget.writeMask = WGPUColorWriteMask_All;
    ptTarget.blend = nullptr;
    compFrag.entryPoint = ToWgpuStringView("fs_rt_pathtrace_present");
    compFrag.targets = &ptTarget;
    compRp.label = ToWgpuStringView("RT_PathTracePresentPipeline");
    RenderPipelineHandle nextPtPresent{wgpuDeviceCreateRenderPipeline(device_, &compRp)};

    if (!nextCompRefl || !nextPtPresent) {
        return std::unexpected("Failed to create RT composite/present render pipelines");
    }

    gbufferPipeline_ = std::move(nextGbuf);
    gbufferBackCulledPipeline_ = std::move(nextGbufBack);
    shadowPipeline_ = std::move(*shdPipe);
    aoPipeline_ = std::move(*aoPipe);
    reflPipeline_ = std::move(*reflPipe);
    temporal8Pipeline_ = std::move(*tmp8Pipe);
    atrous8Pipeline_ = std::move(*atr8Pipe);
    upsample8Pipeline_ = std::move(*up8Pipe);
    temporal16Pipeline_ = std::move(*tmp16Pipe);
    atrous16Pipeline_ = std::move(*atr16Pipe);
    pathTracePipeline_ = std::move(*ptPipe);
    compositeReflPipeline_ = std::move(nextCompRefl);
    pathTracePresentPipeline_ = std::move(nextPtPresent);
    return {};
#else
    (void)validateScope;
    (void)allowBindlessFallback;
    return {};
#endif
}

core::Result<void> RayTracingPass::Initialize(WGPUDevice device, WGPUQueue queue, WGPUPipelineLayout mainPipelineLayout,
                                              WGPUBindGroupLayout perDrawBgl, WGPUBindGroup dummySkinBindGroup,
                                              const RayTracingScene& rtScene, WGPUTextureView envCubemapView,
                                              WGPUSampler defaultSampler, WGPUTextureView fallbackWhiteView,
                                              bool supportsTimestampQueries) {
    Shutdown();
    device_ = device;
    queue_ = queue;
    mainPipelineLayout_ = mainPipelineLayout;
    dummySkinBindGroup_ = dummySkinBindGroup;
    envCubemapView_ = envCubemapView;
    defaultSampler_ = defaultSampler;
    fallbackWhiteView_ = fallbackWhiteView;
    supportsTimestampQueries_ = supportsTimestampQueries;

#if defined(BGL_PLATFORM_NATIVE)
    if (!device_ || !queue_ || !mainPipelineLayout || !perDrawBgl || !dummySkinBindGroup || !rtScene.IsInitialized()) {
        return std::unexpected("Cannot initialize RayTracingPass with invalid device or RT scene");
    }

    gbufferPerDrawRing_.Initialize(device_, queue_, perDrawBgl, "RT_GbufferPerDrawRing");
    if (auto res = gbufferInstanceBuffer_.Initialize(device_, 256, "RT_GbufferInstanceBuf"); !res) {
        Shutdown();
        return res;
    }

    if (auto res = CreateLayoutsAndBuffers(mainPipelineLayout, rtScene); !res) {
        Shutdown();
        return res;
    }
    if (auto res = CreatePipelines(false, true); !res) {
        Shutdown();
        return res;
    }
    initialized_ = true;
    BGL_LOG_INFO("Desktop RayTracingPass initialized (G-Buffer + RT Shadows + RTAO + RT Reflections + "
                 "Temporal/A-Trous Denoiser + Progressive Path Tracer, Bindless={})",
                 bindlessActive_ ? "YES" : "NO");
    return {};
#else
    (void)perDrawBgl;
    (void)rtScene;
    return {};
#endif
}

core::Result<void> RayTracingPass::ReloadShaders(std::span<const std::string> modifiedVirtualPaths) {
#if defined(BGL_PLATFORM_NATIVE)
    if (!initialized_ || !device_) {
        return {};
    }
    bool needReload = false;
    for (const auto& p : modifiedVirtualPaths) {
        if (p.starts_with("rt_") || p.starts_with("common/rt_")) {
            needReload = true;
            break;
        }
    }
    if (!needReload) {
        return {};
    }
    if (auto res = CreatePipelines(true, false); !res) {
        return res;
    }
    pathSampleIndex_ = 0;
    return {};
#else
    (void)modifiedVirtualPaths;
    return {};
#endif
}

void RayTracingPass::Shutdown() noexcept {
    if (timestampMapState_) {
        timestampMapState_->alive.store(false, std::memory_order_release);
        timestampMapState_->owner = nullptr;
    }
#if defined(BGL_PLATFORM_NATIVE)
    if (device_ && timestampMapState_ && timestampMapState_->inFlight.load(std::memory_order_acquire)) {
        wgpuDevicePoll(device_, WGPU_TRUE, nullptr);
    }
#endif
    ReleaseResolutionTargets();
    gbufferPerDrawRing_.Shutdown();
    gbufferInstanceBuffer_.Shutdown();
    gbufferDrawsScratch_.clear();
    gbufferPerDrawScratch_.clear();
    gbufferInstanceTransformsScratch_.clear();

    if (passUniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(static_cast<size_t>(kMaxUniformSlots) * kUniformSlotStride);
        passUniformBuffer_.Reset();
    }
    if (compositeUniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(RtCompositeUniforms));
        compositeUniformBuffer_.Reset();
    }

    timestampReadbackBuf_.Reset();
    timestampResolveBuf_.Reset();
    timestampQuerySet_.Reset();
    timestampMapState_.reset();
    timestampDataWritten_ = false;
    timestampCopyQueued_ = false;
    timestampHasPrePass_ = false;
    timestampHasPostPass_ = false;
    lastGpuMs_ = 0.0f;

    linearSampler_.Reset();
    pathTracePresentPipeline_.Reset();
    compositeReflPipeline_.Reset();
    pathTracePipeline_.Reset();
    atrous16Pipeline_.Reset();
    temporal16Pipeline_.Reset();
    upsample8Pipeline_.Reset();
    atrous8Pipeline_.Reset();
    temporal8Pipeline_.Reset();
    reflPipeline_.Reset();
    aoPipeline_.Reset();
    shadowPipeline_.Reset();
    gbufferBackCulledPipeline_.Reset();
    gbufferPipeline_.Reset();

    pathTracePl_.Reset();
    pathTraceBgl_.Reset();
    compositePl_.Reset();
    compositeBgl_.Reset();
    denoise16Pl_.Reset();
    denoise16Bgl_.Reset();
    denoise8Pl_.Reset();
    denoise8Bgl_.Reset();
    reflPl_.Reset();
    reflBgl_.Reset();
    trace8Pl_.Reset();
    trace8Bgl_.Reset();

    device_ = nullptr;
    queue_ = nullptr;
    mainPipelineLayout_ = nullptr;
    sceneBgl_ = nullptr;
    dummySkinBindGroup_ = nullptr;
    envCubemapView_ = nullptr;
    defaultSampler_ = nullptr;
    fallbackWhiteView_ = nullptr;
    initialized_ = false;
}

} // namespace bgl::gpu
