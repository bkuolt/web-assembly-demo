/**
 * @file RayTracingPassPipelines.cpp
 * @brief Pipeline compilation, bind group layouts, resolution targets, and shader hot-reload for RayTracingPass.
 */
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/RayTracingPass.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#if defined(BGL_PLATFORM_NATIVE)
#include <webgpu/wgpu.h>
#endif

namespace bgl::gpu {

namespace {

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

} // namespace

core::Result<void> RayTracingPass::CreateLayoutsAndBuffers(WGPUPipelineLayout mainPipelineLayout,
                                                           const RayTracingScene& rtScene) {
#if defined(BGL_PLATFORM_NATIVE)
    mainPipelineLayout_ = mainPipelineLayout;
    bindlessActive_ = rtScene.HasBindlessTextures() && (rtScene.GetBindlessTexturesBgl() != nullptr);

    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = static_cast<uint64_t>(kMaxUniformSlots) * kUniformSlotStride;
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("RT_PassUniformRing");
    passUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    if (!passUniformBuffer_) {
        return std::unexpected("Failed to create RT pass uniform buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(ubDesc.size);

    WGPUBufferDescriptor cbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    cbDesc.size = sizeof(RtCompositeUniforms);
    cbDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    cbDesc.label = ToWgpuStringView("RT_CompositeUniforms");
    compositeUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &cbDesc);
    if (!compositeUniformBuffer_) {
        return std::unexpected("Failed to create RT composite uniform buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(cbDesc.size);

    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampDesc.maxAnisotropy = 1;
    linearSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    auto makeBgl = [&](std::span<const WGPUBindGroupLayoutEntry> entries, const char* lbl) -> BindGroupLayoutHandle {
        WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        d.entryCount = entries.size();
        d.entries = entries.data();
        d.label = ToWgpuStringView(lbl);
        return BindGroupLayoutHandle{wgpuDeviceCreateBindGroupLayout(device_, &d)};
    };
    auto makePl = [&](std::span<const WGPUBindGroupLayout> bgls, const char* lbl) -> PipelineLayoutHandle {
        WGPUPipelineLayoutDescriptor d = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        d.bindGroupLayoutCount = bgls.size();
        d.bindGroupLayouts = bgls.data();
        d.label = ToWgpuStringView(lbl);
        return PipelineLayoutHandle{wgpuDeviceCreatePipelineLayout(device_, &d)};
    };

    const WGPUBindGroupLayout sceneBgl = rtScene.GetSceneBindGroupLayout();

    // 1. trace8Bgl (Shadows & AO): UBO(dyn) + Depth + NormalRoughness + StorageRGBA8
    {
        const std::array<WGPUBindGroupLayoutEntry, 4> e{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeTexEntry(1, WGPUShaderStage_Compute, WGPUTextureSampleType_Depth),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeStorageTexEntry(3, WGPUTextureFormat_RGBA8Unorm),
        };
        trace8Bgl_ = makeBgl(e, "RT_Trace8_BGL");
        const std::array<WGPUBindGroupLayout, 2> pls{trace8Bgl_.Get(), sceneBgl};
        trace8Pl_ = makePl(pls, "RT_Trace8_PL");
    }

    // 2. reflBgl: UBO(dyn) + Depth + NormalRoughness + AlbedoMetallic + Sampler + EnvCube + StorageRGBA16F
    {
        WGPUBindGroupLayoutEntry sampE = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        sampE.binding = 4;
        sampE.visibility = WGPUShaderStage_Compute;
        sampE.sampler.type = WGPUSamplerBindingType_Filtering;

        const std::array<WGPUBindGroupLayoutEntry, 7> e{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeTexEntry(1, WGPUShaderStage_Compute, WGPUTextureSampleType_Depth),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(3, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            sampE,
            MakeTexEntry(5, WGPUShaderStage_Compute, WGPUTextureSampleType_Float, WGPUTextureViewDimension_Cube),
            MakeStorageTexEntry(6, WGPUTextureFormat_RGBA16Float),
        };
        reflBgl_ = makeBgl(e, "RT_Refl_BGL");
        if (bindlessActive_) {
            const std::array<WGPUBindGroupLayout, 3> pls{reflBgl_.Get(), sceneBgl, rtScene.GetBindlessTexturesBgl()};
            reflPl_ = makePl(pls, "RT_ReflBindless_PL");
        } else {
            const std::array<WGPUBindGroupLayout, 2> pls{reflBgl_.Get(), sceneBgl};
            reflPl_ = makePl(pls, "RT_Refl_PL");
        }
    }

    // 3. denoise8Bgl & denoise16Bgl: UBO(dyn) + Depth + NormalRoughness + srcTex + historyTex + dstStorage
    {
        const std::array<WGPUBindGroupLayoutEntry, 6> e8{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeTexEntry(1, WGPUShaderStage_Compute, WGPUTextureSampleType_Depth),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(3, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(4, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeStorageTexEntry(5, WGPUTextureFormat_RGBA8Unorm),
        };
        denoise8Bgl_ = makeBgl(e8, "RT_Denoise8_BGL");
        const std::array<WGPUBindGroupLayout, 1> pl8{denoise8Bgl_.Get()};
        denoise8Pl_ = makePl(pl8, "RT_Denoise8_PL");

        const std::array<WGPUBindGroupLayoutEntry, 6> e16{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            MakeTexEntry(1, WGPUShaderStage_Compute, WGPUTextureSampleType_Depth),
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(3, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeTexEntry(4, WGPUShaderStage_Compute, WGPUTextureSampleType_Float),
            MakeStorageTexEntry(5, WGPUTextureFormat_RGBA16Float),
        };
        denoise16Bgl_ = makeBgl(e16, "RT_Denoise16_BGL");
        const std::array<WGPUBindGroupLayout, 1> pl16{denoise16Bgl_.Get()};
        denoise16Pl_ = makePl(pl16, "RT_Denoise16_PL");
    }

    // 4. compositeBgl: UBO + Sampler + reflTex + Depth + pathAccum SSBO
    {
        WGPUBindGroupLayoutEntry sampE = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        sampE.binding = 1;
        sampE.visibility = WGPUShaderStage_Fragment;
        sampE.sampler.type = WGPUSamplerBindingType_Filtering;

        WGPUBindGroupLayoutEntry ssboE = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        ssboE.binding = 4;
        ssboE.visibility = WGPUShaderStage_Fragment;
        ssboE.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        ssboE.buffer.minBindingSize = sizeof(glm::vec4);

        const std::array<WGPUBindGroupLayoutEntry, 5> e{
            MakeUniformEntry(0, WGPUShaderStage_Fragment, sizeof(RtCompositeUniforms), false),
            sampE,
            MakeTexEntry(2, WGPUShaderStage_Fragment, WGPUTextureSampleType_Float),
            MakeTexEntry(3, WGPUShaderStage_Fragment, WGPUTextureSampleType_Depth),
            ssboE,
        };
        compositeBgl_ = makeBgl(e, "RT_Composite_BGL");
        const std::array<WGPUBindGroupLayout, 1> plc{compositeBgl_.Get()};
        compositePl_ = makePl(plc, "RT_Composite_PL");
    }

    // 5. pathTraceBgl: UBO(dyn) + Sampler + EnvCube + pathAccum(rw)
    {
        WGPUBindGroupLayoutEntry sampE = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        sampE.binding = 1;
        sampE.visibility = WGPUShaderStage_Compute;
        sampE.sampler.type = WGPUSamplerBindingType_Filtering;

        WGPUBindGroupLayoutEntry accumE = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        accumE.binding = 3;
        accumE.visibility = WGPUShaderStage_Compute;
        accumE.buffer.type = WGPUBufferBindingType_Storage;
        accumE.buffer.minBindingSize = sizeof(glm::vec4);

        const std::array<WGPUBindGroupLayoutEntry, 4> e{
            MakeUniformEntry(0, WGPUShaderStage_Compute, sizeof(RtPassUniforms), true),
            sampE,
            MakeTexEntry(2, WGPUShaderStage_Compute, WGPUTextureSampleType_Float, WGPUTextureViewDimension_Cube),
            accumE,
        };
        pathTraceBgl_ = makeBgl(e, "RT_PathTrace_BGL");
        if (bindlessActive_) {
            const std::array<WGPUBindGroupLayout, 3> plp{pathTraceBgl_.Get(), sceneBgl,
                                                         rtScene.GetBindlessTexturesBgl()};
            pathTracePl_ = makePl(plp, "RT_PathTraceBindless_PL");
        } else {
            const std::array<WGPUBindGroupLayout, 2> plp{pathTraceBgl_.Get(), sceneBgl};
            pathTracePl_ = makePl(plp, "RT_PathTrace_PL");
        }
    }

    if (supportsTimestampQueries_) {
        WGPUQuerySetDescriptor qDesc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        qDesc.type = WGPUQueryType_Timestamp;
        qDesc.count = 4u;
        qDesc.label = ToWgpuStringView("RT_TimestampQuerySet");
        timestampQuerySet_ = wgpuDeviceCreateQuerySet(device_, &qDesc);

        WGPUBufferDescriptor resDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        resDesc.size = 4u * sizeof(uint64_t);
        resDesc.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
        resDesc.label = ToWgpuStringView("RT_TimestampResolve");
        timestampResolveBuf_ = wgpuDeviceCreateBuffer(device_, &resDesc);

        WGPUBufferDescriptor rbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        rbDesc.size = 4u * sizeof(uint64_t);
        rbDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        rbDesc.label = ToWgpuStringView("RT_TimestampReadback");
        timestampReadbackBuf_ = wgpuDeviceCreateBuffer(device_, &rbDesc);

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

core::Result<void> RayTracingPass::CreatePipelines(bool validateScope, WGPUPipelineLayout mainPipelineLayout,
                                                   const RayTracingScene& rtScene) {
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
    if ((!reflMod || !ptMod) && bindlessActive_) {
        BGL_LOG_WARN("Bindless RT WGSL compilation failed; falling back to non-bindless secondary hit shading");
        bindlessActive_ = false;
        const WGPUBindGroupLayout sceneBgl = rtScene.GetSceneBindGroupLayout();
        const std::array<WGPUBindGroupLayout, 2> rPls{reflBgl_.Get(), sceneBgl};
        WGPUPipelineLayoutDescriptor rd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        rd.bindGroupLayoutCount = rPls.size();
        rd.bindGroupLayouts = rPls.data();
        reflPl_ = wgpuDeviceCreatePipelineLayout(device_, &rd);

        const std::array<WGPUBindGroupLayout, 2> pPls{pathTraceBgl_.Get(), sceneBgl};
        WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pd.bindGroupLayoutCount = pPls.size();
        pd.bindGroupLayouts = pPls.data();
        pathTracePl_ = wgpuDeviceCreatePipelineLayout(device_, &pd);

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
    gbufRp.layout = mainPipelineLayout;
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
    (void)mainPipelineLayout;
    (void)rtScene;
    return {};
#endif
}

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

    const WGPUTextureUsage tex8Usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding;
    const WGPUTextureUsage tex16Usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding;

    // 1. G-Buffer (Full resolution 1x)
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA16Float,
                WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, "RT_GbufferNormalRough",
                gbufferNormalRoughnessTex_, gbufferNormalRoughnessView_);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA8Unorm,
                WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, "RT_GbufferAlbedoMetal",
                gbufferAlbedoMetallicTex_, gbufferAlbedoMetallicView_);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_Depth32Float,
                WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, "RT_GbufferDepth",
                gbufferDepthTex_, gbufferDepthView_);

    // 2. RT Shadows (Trace resolution + full-res output)
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_ShadowRaw",
                shadowRawTex_, shadowRawView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_ShadowScratch",
                shadowScratchTex_, shadowScratchView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_ShadowHist0",
                shadowHistoryTex_[0], shadowHistoryView_[0]);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_ShadowHist1",
                shadowHistoryTex_[1], shadowHistoryView_[1]);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_ShadowFinal",
                shadowFinalTex_, shadowFinalView_);

    // 3. RTAO (Trace resolution + full-res output)
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_AoRaw", aoRawTex_,
                aoRawView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_AoScratch",
                aoScratchTex_, aoScratchView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_AoHist0",
                aoHistoryTex_[0], aoHistoryView_[0]);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_AoHist1",
                aoHistoryTex_[1], aoHistoryView_[1]);
    CreateTex2D(device_, fullWidth_, fullHeight_, WGPUTextureFormat_RGBA8Unorm, tex8Usage, "RT_AoFinal", aoFinalTex_,
                aoFinalView_);

    // 4. RT Reflections (Trace resolution HDR)
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, tex16Usage, "RT_ReflRaw",
                reflRawTex_, reflRawView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, tex16Usage, "RT_ReflScratch",
                reflScratchTex_, reflScratchView_);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, tex16Usage, "RT_ReflHist0",
                reflHistoryTex_[0], reflHistoryView_[0]);
    CreateTex2D(device_, traceWidth_, traceHeight_, WGPUTextureFormat_RGBA16Float, tex16Usage, "RT_ReflHist1",
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

    const size_t fullPixels = static_cast<size_t>(fullWidth_) * fullHeight_;
    const size_t tracePixels = static_cast<size_t>(traceWidth_) * traceHeight_;
    core::MemoryTracker::Instance().TrackTextureAlloc(fullPixels * (8u + 4u + 4u + 4u + 4u) +
                                                      tracePixels * (4u * 4u + 4u * 4u + 8u * 4u));

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

    auto makeBg = [&](WGPUBindGroupLayout layout, std::span<const WGPUBindGroupEntry> entries,
                      const char* lbl) -> BindGroupHandle {
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        d.layout = layout;
        d.entryCount = entries.size();
        d.entries = entries.data();
        d.label = ToWgpuStringView(lbl);
        return BindGroupHandle{wgpuDeviceCreateBindGroup(device_, &d)};
    };

    auto dynUbo = [&]() {
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = 0;
        e.buffer = passUniformBuffer_.Get();
        e.offset = 0;
        e.size = sizeof(RtPassUniforms);
        return e;
    };
    auto texEntry = [&](uint32_t b, WGPUTextureView v) {
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = b;
        e.textureView = v;
        return e;
    };

    // Trace8 bind groups (Shadow & AO)
    shadowTraceBg_ = makeBg(trace8Bgl_.Get(),
                            std::array<WGPUBindGroupEntry, 4>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                              texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                              texEntry(3, shadowRawView_.Get())},
                            "RT_ShadowTrace_BG");
    aoTraceBg_ = makeBg(trace8Bgl_.Get(),
                        std::array<WGPUBindGroupEntry, 4>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                          texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                          texEntry(3, aoRawView_.Get())},
                        "RT_AoTrace_BG");

    // Reflections trace bind group
    {
        WGPUBindGroupEntry sampE = WGPU_BIND_GROUP_ENTRY_INIT;
        sampE.binding = 4;
        sampE.sampler = defaultSampler_;
        reflTraceBg_ =
            makeBg(reflBgl_.Get(),
                   std::array<WGPUBindGroupEntry, 7>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                     texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                     texEntry(3, gbufferAlbedoMetallicView_.Get()), sampE,
                                                     texEntry(5, envCubemapView_), texEntry(6, reflRawView_.Get())},
                   "RT_ReflTrace_BG");
    }

    // PathTrace bind group
    {
        WGPUBindGroupEntry sampE = WGPU_BIND_GROUP_ENTRY_INIT;
        sampE.binding = 1;
        sampE.sampler = defaultSampler_;
        WGPUBindGroupEntry accE = WGPU_BIND_GROUP_ENTRY_INIT;
        accE.binding = 3;
        accE.buffer = pathAccumBuffer_.Get();
        accE.offset = 0;
        accE.size = pathAccumBytes_;
        pathTraceBg_ = makeBg(pathTraceBgl_.Get(),
                              std::array<WGPUBindGroupEntry, 4>{dynUbo(), sampE, texEntry(2, envCubemapView_), accE},
                              "RT_PathTrace_BG");
    }

    // Denoise8 (Shadows & AO) bind groups for ping-pong temporal + 3x A-Trous + upsample
    auto buildDenoise8Set = [&](WGPUTextureView rawV, WGPUTextureView scratchV,
                                const std::array<TextureViewHandle, 2>& histV, WGPUTextureView finalV,
                                std::array<BindGroupHandle, 2>& outTemporal, std::array<BindGroupHandle, 2>& outA1,
                                BindGroupHandle& outA2, BindGroupHandle& outA3, BindGroupHandle& outUp) {
        for (uint32_t h = 0; h < 2u; ++h) {
            const uint32_t prevH = 1u - h;
            outTemporal[h] = makeBg(
                denoise8Bgl_.Get(),
                std::array<WGPUBindGroupEntry, 6>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                  texEntry(2, gbufferNormalRoughnessView_.Get()), texEntry(3, rawV),
                                                  texEntry(4, histV[prevH].Get()), texEntry(5, histV[h].Get())},
                "RT_Temporal8_BG");
            outA1[h] = makeBg(denoise8Bgl_.Get(),
                              std::array<WGPUBindGroupEntry, 6>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                                texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                                texEntry(3, histV[h].Get()),
                                                                texEntry(4, histV[h].Get()), texEntry(5, rawV)},
                              "RT_Atrous8_1_BG");
        }
        outA2 = makeBg(denoise8Bgl_.Get(),
                       std::array<WGPUBindGroupEntry, 6>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                         texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                         texEntry(3, rawV), texEntry(4, rawV), texEntry(5, scratchV)},
                       "RT_Atrous8_2_BG");
        outA3 =
            makeBg(denoise8Bgl_.Get(),
                   std::array<WGPUBindGroupEntry, 6>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                     texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                     texEntry(3, scratchV), texEntry(4, scratchV), texEntry(5, rawV)},
                   "RT_Atrous8_3_BG");
        outUp = makeBg(denoise8Bgl_.Get(),
                       std::array<WGPUBindGroupEntry, 6>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                         texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                         texEntry(3, rawV), texEntry(4, rawV), texEntry(5, finalV)},
                       "RT_Upsample8_BG");
    };

    buildDenoise8Set(shadowRawView_.Get(), shadowScratchView_.Get(), shadowHistoryView_, shadowFinalView_.Get(),
                     shadowTemporalBg_, shadowAtrousStep1Bg_, shadowAtrousStep2Bg_, shadowAtrousStep3Bg_,
                     shadowUpsampleBg_);
    buildDenoise8Set(aoRawView_.Get(), aoScratchView_.Get(), aoHistoryView_, aoFinalView_.Get(), aoTemporalBg_,
                     aoAtrousStep1Bg_, aoAtrousStep2Bg_, aoAtrousStep3Bg_, aoUpsampleBg_);

    // Denoise16 (Reflections) bind groups: Temporal -> hist[h], A1: hist[h]->raw, A2: raw->scratch, A3: scratch->raw
    for (uint32_t h = 0; h < 2u; ++h) {
        const uint32_t prevH = 1u - h;
        reflTemporalBg_[h] = makeBg(denoise16Bgl_.Get(),
                                    std::array<WGPUBindGroupEntry, 6>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                                      texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                                      texEntry(3, reflRawView_.Get()),
                                                                      texEntry(4, reflHistoryView_[prevH].Get()),
                                                                      texEntry(5, reflHistoryView_[h].Get())},
                                    "RT_Temporal16_BG");
        reflAtrousStep1Bg_[h] = makeBg(denoise16Bgl_.Get(),
                                       std::array<WGPUBindGroupEntry, 6>{dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                                                         texEntry(2, gbufferNormalRoughnessView_.Get()),
                                                                         texEntry(3, reflHistoryView_[h].Get()),
                                                                         texEntry(4, reflHistoryView_[h].Get()),
                                                                         texEntry(5, reflRawView_.Get())},
                                       "RT_Atrous16_1_BG");
    }
    reflAtrousStep2Bg_ = makeBg(denoise16Bgl_.Get(),
                                std::array<WGPUBindGroupEntry, 6>{
                                    dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                    texEntry(2, gbufferNormalRoughnessView_.Get()), texEntry(3, reflRawView_.Get()),
                                    texEntry(4, reflRawView_.Get()), texEntry(5, reflScratchView_.Get())},
                                "RT_Atrous16_2_BG");
    reflAtrousStep3Bg_ = makeBg(denoise16Bgl_.Get(),
                                std::array<WGPUBindGroupEntry, 6>{
                                    dynUbo(), texEntry(1, gbufferDepthView_.Get()),
                                    texEntry(2, gbufferNormalRoughnessView_.Get()), texEntry(3, reflScratchView_.Get()),
                                    texEntry(4, reflScratchView_.Get()), texEntry(5, reflRawView_.Get())},
                                "RT_Atrous16_3_BG");

    // Composite & PathTrace Present bind groups
    {
        WGPUBindGroupEntry compUboE = WGPU_BIND_GROUP_ENTRY_INIT;
        compUboE.binding = 0;
        compUboE.buffer = compositeUniformBuffer_.Get();
        compUboE.offset = 0;
        compUboE.size = sizeof(RtCompositeUniforms);

        WGPUBindGroupEntry sampE = WGPU_BIND_GROUP_ENTRY_INIT;
        sampE.binding = 1;
        sampE.sampler = linearSampler_.Get();

        WGPUBindGroupEntry accE = WGPU_BIND_GROUP_ENTRY_INIT;
        accE.binding = 4;
        accE.buffer = pathAccumBuffer_.Get();
        accE.offset = 0;
        accE.size = pathAccumBytes_;

        compositeReflBg_ = makeBg(compositeBgl_.Get(),
                                  std::array<WGPUBindGroupEntry, 5>{compUboE, sampE, texEntry(2, reflRawView_.Get()),
                                                                    texEntry(3, gbufferDepthView_.Get()), accE},
                                  "RT_CompositeRefl_BG");
        pathPresentBg_ = makeBg(compositeBgl_.Get(),
                                std::array<WGPUBindGroupEntry, 5>{compUboE, sampE, texEntry(2, reflRawView_.Get()),
                                                                  texEntry(3, gbufferDepthView_.Get()), accE},
                                "RT_PathPresent_BG");
    }
#endif
}

void RayTracingPass::ReleaseResolutionTargets() noexcept {
    shadowTraceBg_.Reset();
    aoTraceBg_.Reset();
    reflTraceBg_.Reset();
    pathTraceBg_.Reset();
    compositeReflBg_.Reset();
    pathPresentBg_.Reset();
    for (auto& bg : shadowTemporalBg_) {
        bg.Reset();
    }
    for (auto& bg : shadowAtrousStep1Bg_) {
        bg.Reset();
    }
    shadowAtrousStep2Bg_.Reset();
    shadowAtrousStep3Bg_.Reset();
    shadowUpsampleBg_.Reset();
    for (auto& bg : aoTemporalBg_) {
        bg.Reset();
    }
    for (auto& bg : aoAtrousStep1Bg_) {
        bg.Reset();
    }
    aoAtrousStep2Bg_.Reset();
    aoAtrousStep3Bg_.Reset();
    aoUpsampleBg_.Reset();
    for (auto& bg : reflTemporalBg_) {
        bg.Reset();
    }
    for (auto& bg : reflAtrousStep1Bg_) {
        bg.Reset();
    }
    reflAtrousStep2Bg_.Reset();
    reflAtrousStep3Bg_.Reset();

    if (fullWidth_ > 0 && fullHeight_ > 0) {
        const size_t fullPixels = static_cast<size_t>(fullWidth_) * fullHeight_;
        const size_t tracePixels = static_cast<size_t>(traceWidth_) * traceHeight_;
        core::MemoryTracker::Instance().TrackTextureFree(fullPixels * (8u + 4u + 4u + 4u + 4u) +
                                                         tracePixels * (4u * 4u + 4u * 4u + 8u * 4u));
    }

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
    if (auto res = CreatePipelines(false, mainPipelineLayout, rtScene); !res) {
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
    // Note: pass dummy empty check — CreatePipelines uses stored mainPipelineLayout_ and existing layouts
    // For full hot-reload we recompile shaders directly using existing layouts:
    auto compile = [&](std::string_view file, const char* lbl) {
        return ShaderManager::Instance().LoadAndCompile(device_, file, lbl, true);
    };
    auto gbufMod = compile("rt_gbuffer.wgsl", "RT_GBuffer_WGSL");
    auto shdMod = compile("rt_shadows.wgsl", "RT_Shadows_WGSL");
    auto aoMod = compile("rt_ao.wgsl", "RT_AO_WGSL");
    auto den8Mod = compile("rt_denoise.wgsl", "RT_Denoise8_WGSL");
    auto den16Mod = compile("rt_denoise_hdr.wgsl", "RT_Denoise16_WGSL");
    auto compMod = compile("rt_composite.wgsl", "RT_Composite_WGSL");
    auto reflMod = compile(bindlessActive_ ? "rt_reflections_bindless.wgsl" : "rt_reflections.wgsl", "RT_Refl_WGSL");
    auto ptMod = compile(bindlessActive_ ? "rt_pathtrace_bindless.wgsl" : "rt_pathtrace.wgsl", "RT_PathTrace_WGSL");

    if (!gbufMod || !shdMod || !aoMod || !den8Mod || !den16Mod || !compMod || !reflMod || !ptMod) {
        return std::unexpected("Failed to hot-reload one or more RayTracing WGSL shaders");
    }

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
        return std::unexpected("Failed to rebuild RayTracing compute pipelines during hot-reload");
    }
    shadowPipeline_ = std::move(*shdPipe);
    aoPipeline_ = std::move(*aoPipe);
    reflPipeline_ = std::move(*reflPipe);
    temporal8Pipeline_ = std::move(*tmp8Pipe);
    atrous8Pipeline_ = std::move(*atr8Pipe);
    upsample8Pipeline_ = std::move(*up8Pipe);
    temporal16Pipeline_ = std::move(*tmp16Pipe);
    atrous16Pipeline_ = std::move(*atr16Pipe);
    pathTracePipeline_ = std::move(*ptPipe);
    pathSampleIndex_ = 0;
    return {};
#else
    (void)modifiedVirtualPaths;
    return {};
#endif
}

void RayTracingPass::Shutdown() noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    if (device_ && timestampReadInFlight_) {
        wgpuDevicePoll(device_, WGPU_TRUE, nullptr);
    }
#endif
    ReleaseResolutionTargets();
    gbufferPerDrawRing_.Shutdown();
    gbufferInstanceBuffer_.Shutdown();
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
    timestampReadInFlight_ = false;
    timestampDataWritten_ = false;
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
    dummySkinBindGroup_ = nullptr;
    envCubemapView_ = nullptr;
    defaultSampler_ = nullptr;
    fallbackWhiteView_ = nullptr;
    initialized_ = false;
}

} // namespace bgl::gpu
