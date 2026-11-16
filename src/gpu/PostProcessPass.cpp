/**
 * @file PostProcessPass.cpp
 * @brief Implementation of Depth Linearization, Half-Res GTAO, 5-Mip Bloom Pyramid,
 *        GPU Auto-Exposure, SSR, Volumetric God Rays, Bokeh DoF, Filmic Tonemapping, and FXAA.
 */
#include "gpu/PostProcessPass.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/PostProcessHelpers.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include <vector>

namespace bgl::gpu {
using namespace detail;

namespace {

[[nodiscard]] BufferHandle CreateTrackedBuffer(WGPUDevice device, uint64_t size, WGPUBufferUsage usage,
                                               std::string_view label, const void* initialData = nullptr,
                                               WGPUQueue queue = nullptr) {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = size;
    desc.usage = usage;
    desc.label = ToWgpuStringView(label);
    BufferHandle buf{wgpuDeviceCreateBuffer(device, &desc)};
    if (buf) {
        core::MemoryTracker::Instance().TrackBufferAlloc(size);
        if (initialData && queue) {
            wgpuQueueWriteBuffer(queue, buf.Get(), 0, initialData, size);
        }
    }
    return buf;
}

void ReleaseTrackedBuffer(BufferHandle& buf, uint64_t size) noexcept {
    if (buf) {
        core::MemoryTracker::Instance().TrackBufferFree(size);
        buf.Reset();
    }
}

[[nodiscard]] core::Result<RenderPipelineHandle>
CreateFullscreenPipeline(WGPUDevice device, WGPUPipelineLayout layout, WGPUShaderModule mod, std::string_view vsEntry,
                         std::string_view fsEntry, WGPUTextureFormat colorFormat, std::string_view label,
                         bool additiveBlend = false) {
    WGPUBlendState blendState = WGPU_BLEND_STATE_INIT;
    blendState.color.srcFactor = WGPUBlendFactor_One;
    blendState.color.dstFactor = WGPUBlendFactor_One;
    blendState.color.operation = WGPUBlendOperation_Add;
    blendState.alpha.srcFactor = WGPUBlendFactor_One;
    blendState.alpha.dstFactor = WGPUBlendFactor_One;
    blendState.alpha.operation = WGPUBlendOperation_Add;

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = colorFormat;
    colorTarget.writeMask = WGPUColorWriteMask_All;
    colorTarget.blend = additiveBlend ? &blendState : nullptr;

    WGPUFragmentState fragState = WGPU_FRAGMENT_STATE_INIT;
    fragState.module = mod;
    fragState.entryPoint = ToWgpuStringView(fsEntry);
    fragState.targetCount = 1;
    fragState.targets = &colorTarget;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = layout;
    rpDesc.vertex.module = mod;
    rpDesc.vertex.entryPoint = ToWgpuStringView(vsEntry);
    rpDesc.fragment = &fragState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.multisample.count = 1;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.label = ToWgpuStringView(label);

    RenderPipelineHandle pipe{wgpuDeviceCreateRenderPipeline(device, &rpDesc)};
    if (!pipe) {
        return std::unexpected("Failed to create render pipeline: " + std::string{label});
    }
    return pipe;
}

} // namespace

PostProcessPass::~PostProcessPass() {
    Shutdown();
}

PostProcessPass::PostProcessPass(PostProcessPass&& other) noexcept {
    MoveFrom(other);
}

PostProcessPass& PostProcessPass::operator=(PostProcessPass&& other) noexcept {
    if (this != &other) {
        Shutdown();
        MoveFrom(other);
    }
    return *this;
}

void PostProcessPass::MoveFrom(PostProcessPass& other) noexcept {
    device_ = std::exchange(other.device_, nullptr);
    queue_ = std::exchange(other.queue_, nullptr);
    surfaceFormat_ = other.surfaceFormat_;

    tonemapBindGroupLayout_ = std::move(other.tonemapBindGroupLayout_);
    tonemapPipelineLayout_ = std::move(other.tonemapPipelineLayout_);
    tonemapPipeline_ = std::move(other.tonemapPipeline_);
    tonemapLdrPipeline_ = std::move(other.tonemapLdrPipeline_);
    tonemapUniformBuffer_ = std::move(other.tonemapUniformBuffer_);
    extraUniformBuffer_ = std::move(other.extraUniformBuffer_);
    linearSampler_ = std::move(other.linearSampler_);
    tonemapBindGroup_ = std::move(other.tonemapBindGroup_);
    cachedHdrResolveView_ = std::exchange(other.cachedHdrResolveView_, nullptr);
    cachedLinearDepthView_ = std::exchange(other.cachedLinearDepthView_, nullptr);
    cachedBloomView_ = std::exchange(other.cachedBloomView_, nullptr);
    cachedGtaoView_ = std::exchange(other.cachedGtaoView_, nullptr);
    cachedBloomHdrView_ = std::exchange(other.cachedBloomHdrView_, nullptr);

    fallbackBlackTex_ = std::move(other.fallbackBlackTex_);
    fallbackBlackView_ = std::move(other.fallbackBlackView_);
    fallbackWhiteTex_ = std::move(other.fallbackWhiteTex_);
    fallbackWhiteView_ = std::move(other.fallbackWhiteView_);
    fallbackDepthTex_ = std::move(other.fallbackDepthTex_);
    fallbackDepthView_ = std::move(other.fallbackDepthView_);
    exposureStateBuffer_ = std::move(other.exposureStateBuffer_);

    depthLinMsaaBgl_ = std::move(other.depthLinMsaaBgl_);
    depthLinMsaaPl_ = std::move(other.depthLinMsaaPl_);
    depthLinMsaaPipeline_ = std::move(other.depthLinMsaaPipeline_);
    depthLin1xBgl_ = std::move(other.depthLin1xBgl_);
    depthLin1xPl_ = std::move(other.depthLin1xPl_);
    depthLin1xPipeline_ = std::move(other.depthLin1xPipeline_);
    depthLinUniformBuffer_ = std::move(other.depthLinUniformBuffer_);
    depthLinBindGroup_ = std::move(other.depthLinBindGroup_);
    cachedDepthReadView_ = std::exchange(other.cachedDepthReadView_, nullptr);
    cachedDepthSampleCount_ = std::exchange(other.cachedDepthSampleCount_, 0u);

    gtaoBgl_ = std::move(other.gtaoBgl_);
    gtaoPl_ = std::move(other.gtaoPl_);
    gtaoPipeline_ = std::move(other.gtaoPipeline_);
    gtaoBlurBgl_ = std::move(other.gtaoBlurBgl_);
    gtaoBlurPl_ = std::move(other.gtaoBlurPl_);
    gtaoBlurPipeline_ = std::move(other.gtaoBlurPipeline_);
    gtaoUniformBuffer_ = std::move(other.gtaoUniformBuffer_);
    gtaoBindGroup_ = std::move(other.gtaoBindGroup_);
    gtaoBlurBindGroup_ = std::move(other.gtaoBlurBindGroup_);

    bloomBgl_ = std::move(other.bloomBgl_);
    bloomPl_ = std::move(other.bloomPl_);
    bloomDownPipeline_ = std::move(other.bloomDownPipeline_);
    bloomUpPipeline_ = std::move(other.bloomUpPipeline_);
    bloomUniformBuffer_ = std::move(other.bloomUniformBuffer_);
    bloomBindGroups_ = std::move(other.bloomBindGroups_);

    autoExpBgl_ = std::move(other.autoExpBgl_);
    autoExpPl_ = std::move(other.autoExpPl_);
    autoExpPipeline_ = std::move(other.autoExpPipeline_);
    autoExpUniformBuffer_ = std::move(other.autoExpUniformBuffer_);
    autoExpBindGroup_ = std::move(other.autoExpBindGroup_);
    cachedAutoExpHdrView_ = std::exchange(other.cachedAutoExpHdrView_, nullptr);

    fxaaBgl_ = std::move(other.fxaaBgl_);
    fxaaPl_ = std::move(other.fxaaPl_);
    fxaaPipeline_ = std::move(other.fxaaPipeline_);
    fxaaUniformBuffer_ = std::move(other.fxaaUniformBuffer_);
    fxaaBindGroup_ = std::move(other.fxaaBindGroup_);

    targetWidth_ = std::exchange(other.targetWidth_, 0u);
    targetHeight_ = std::exchange(other.targetHeight_, 0u);
    bloomMipCount_ = std::exchange(other.bloomMipCount_, 0u);
    linearDepthTex_ = std::move(other.linearDepthTex_);
    linearDepthView_ = std::move(other.linearDepthView_);
    gtaoRawTex_ = std::move(other.gtaoRawTex_);
    gtaoRawView_ = std::move(other.gtaoRawView_);
    gtaoBlurTex_ = std::move(other.gtaoBlurTex_);
    gtaoBlurView_ = std::move(other.gtaoBlurView_);
    bloomTex_ = std::move(other.bloomTex_);
    bloomMipViews_ = std::move(other.bloomMipViews_);
    ldrSceneTex_ = std::move(other.ldrSceneTex_);
    ldrSceneView_ = std::move(other.ldrSceneView_);

    projMatrix_ = other.projMatrix_;
    invProjMatrix_ = other.invProjMatrix_;
    elapsedTime_ = other.elapsedTime_;
    preparedLdrThisFrame_ = other.preparedLdrThisFrame_;

    exposure_ = other.exposure_;
    enableAutoExposure_ = other.enableAutoExposure_;
    toneMapOp_ = other.toneMapOp_;
    colorGradingPreset_ = other.colorGradingPreset_;
    enableLensFlare_ = other.enableLensFlare_;
    lensFlareIntensity_ = other.lensFlareIntensity_;
    enableGodRays_ = other.enableGodRays_;
    godRaysIntensity_ = other.godRaysIntensity_;
    enableSsao_ = other.enableSsao_;
    ssaoIntensity_ = other.ssaoIntensity_;
    ssaoRadius_ = other.ssaoRadius_;
    enableBloom_ = other.enableBloom_;
    bloomIntensity_ = other.bloomIntensity_;
    bloomThreshold_ = other.bloomThreshold_;
    enableDof_ = other.enableDof_;
    dofFocusDistance_ = other.dofFocusDistance_;
    dofFocusRange_ = other.dofFocusRange_;
    dofMaxBlurPx_ = other.dofMaxBlurPx_;
    enableSsr_ = other.enableSsr_;
    ssrIntensity_ = other.ssrIntensity_;
    enableFxaa_ = other.enableFxaa_;
    vignetteIntensity_ = other.vignetteIntensity_;
    chromaAberration_ = other.chromaAberration_;
    filmGrainIntensity_ = other.filmGrainIntensity_;
}

void PostProcessPass::InvalidateBindGroup() noexcept {
    tonemapBindGroup_.Reset();
    depthLinBindGroup_.Reset();
    autoExpBindGroup_.Reset();
    bloomBindGroups_[0].Reset();
    cachedHdrResolveView_ = nullptr;
    cachedLinearDepthView_ = nullptr;
    cachedBloomView_ = nullptr;
    cachedGtaoView_ = nullptr;
    cachedBloomHdrView_ = nullptr;
    cachedDepthReadView_ = nullptr;
    cachedAutoExpHdrView_ = nullptr;
}

void PostProcessPass::ReleaseResolutionTargets() noexcept {
    fxaaBindGroup_.Reset();
    gtaoBlurBindGroup_.Reset();
    gtaoBindGroup_.Reset();
    tonemapBindGroup_.Reset();
    for (auto& bg : bloomBindGroups_) {
        bg.Reset();
    }
    ldrSceneView_.Reset();
    ldrSceneTex_.Reset();
    for (auto& v : bloomMipViews_) {
        v.Reset();
    }
    bloomTex_.Reset();
    gtaoBlurView_.Reset();
    gtaoBlurTex_.Reset();
    gtaoRawView_.Reset();
    gtaoRawTex_.Reset();
    linearDepthView_.Reset();
    linearDepthTex_.Reset();
    cachedHdrResolveView_ = nullptr;
    cachedLinearDepthView_ = nullptr;
    cachedBloomView_ = nullptr;
    cachedGtaoView_ = nullptr;
    cachedBloomHdrView_ = nullptr;
    targetWidth_ = 0;
    targetHeight_ = 0;
    bloomMipCount_ = 0;
}

void PostProcessPass::Shutdown() noexcept {
    ReleaseResolutionTargets();

    fxaaBindGroup_.Reset();
    ReleaseTrackedBuffer(fxaaUniformBuffer_, sizeof(FxaaUniformData));
    fxaaPipeline_.Reset();
    fxaaPl_.Reset();
    fxaaBgl_.Reset();

    autoExpBindGroup_.Reset();
    ReleaseTrackedBuffer(autoExpUniformBuffer_, sizeof(AutoExposureUniformData));
    autoExpPipeline_.Reset();
    autoExpPl_.Reset();
    autoExpBgl_.Reset();

    ReleaseTrackedBuffer(bloomUniformBuffer_, static_cast<uint64_t>(kMaxBloomUniformSlots) * kPostUniformSlotStride);
    bloomUpPipeline_.Reset();
    bloomDownPipeline_.Reset();
    bloomPl_.Reset();
    bloomBgl_.Reset();

    gtaoBlurBindGroup_.Reset();
    gtaoBindGroup_.Reset();
    ReleaseTrackedBuffer(gtaoUniformBuffer_, sizeof(GtaoUniformData));
    gtaoBlurPipeline_.Reset();
    gtaoBlurPl_.Reset();
    gtaoBlurBgl_.Reset();
    gtaoPipeline_.Reset();
    gtaoPl_.Reset();
    gtaoBgl_.Reset();

    depthLinBindGroup_.Reset();
    ReleaseTrackedBuffer(depthLinUniformBuffer_, sizeof(DepthLinearizeUniformData));
    depthLin1xPipeline_.Reset();
    depthLin1xPl_.Reset();
    depthLin1xBgl_.Reset();
    depthLinMsaaPipeline_.Reset();
    depthLinMsaaPl_.Reset();
    depthLinMsaaBgl_.Reset();

    ReleaseTrackedBuffer(exposureStateBuffer_, sizeof(glm::vec4));
    fallbackDepthView_.Reset();
    fallbackDepthTex_.Reset();
    fallbackWhiteView_.Reset();
    fallbackWhiteTex_.Reset();
    fallbackBlackView_.Reset();
    fallbackBlackTex_.Reset();

    tonemapBindGroup_.Reset();
    linearSampler_.Reset();
    ReleaseTrackedBuffer(extraUniformBuffer_, sizeof(PostProcessExtraUniforms));
    ReleaseTrackedBuffer(tonemapUniformBuffer_, sizeof(TonemapUniforms));
    tonemapLdrPipeline_.Reset();
    tonemapPipeline_.Reset();
    tonemapPipelineLayout_.Reset();
    tonemapBindGroupLayout_.Reset();

    cachedDepthReadView_ = nullptr;
    cachedDepthSampleCount_ = 0;
    cachedAutoExpHdrView_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> PostProcessPass::CreateFallbackResources() {
    auto makeRgba1x1 = [&](const std::array<uint8_t, 4>& rgba, TextureHandle& tex, TextureViewHandle& view,
                           const char* label) {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        td.dimension = WGPUTextureDimension_2D;
        td.size = WGPUExtent3D{1, 1, 1};
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.label = ToWgpuStringView(label);
        tex = wgpuDeviceCreateTexture(device_, &td);
        if (!tex) {
            return;
        }
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = tex.Get();
        dst.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        const WGPUExtent3D sz{1, 1, 1};
        wgpuQueueWriteTexture(queue_, &dst, rgba.data(), rgba.size(), &layout, &sz);
        view = wgpuTextureCreateView(tex.Get(), nullptr);
    };

    makeRgba1x1({0, 0, 0, 255}, fallbackBlackTex_, fallbackBlackView_, "PostProcess_FallbackBlack");
    makeRgba1x1({255, 255, 255, 255}, fallbackWhiteTex_, fallbackWhiteView_, "PostProcess_FallbackWhite");

    // 1x1 R32Float fallback far-depth (1e6 = sky)
    WGPUTextureDescriptor depthTd = WGPU_TEXTURE_DESCRIPTOR_INIT;
    depthTd.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    depthTd.dimension = WGPUTextureDimension_2D;
    depthTd.size = WGPUExtent3D{1, 1, 1};
    depthTd.format = WGPUTextureFormat_R32Float;
    depthTd.mipLevelCount = 1;
    depthTd.sampleCount = 1;
    depthTd.label = ToWgpuStringView("PostProcess_FallbackDepth");
    fallbackDepthTex_ = wgpuDeviceCreateTexture(device_, &depthTd);
    if (fallbackDepthTex_) {
        const float skyZ = 1000000.0f;
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = fallbackDepthTex_.Get();
        dst.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        const WGPUExtent3D sz{1, 1, 1};
        wgpuQueueWriteTexture(queue_, &dst, &skyZ, sizeof(skyZ), &layout, &sz);
        fallbackDepthView_ = wgpuTextureCreateView(fallbackDepthTex_.Get(), nullptr);
    }

    const glm::vec4 initExposure{1.0f, 0.25f, 0.0f, 0.0f};
    exposureStateBuffer_ =
        CreateTrackedBuffer(device_, sizeof(glm::vec4), WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                            "PostProcess_ExposureState", &initExposure, queue_);

    if (!fallbackBlackView_ || !fallbackWhiteView_ || !fallbackDepthView_ || !exposureStateBuffer_) {
        return std::unexpected("Failed to allocate PostProcessPass fallback GPU resources");
    }
    return {};
}

core::Result<void> PostProcessPass::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize PostProcessPass with null WebGPU device or queue");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;

    // 1. Sampler
    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampDesc.maxAnisotropy = 1;
    sampDesc.label = ToWgpuStringView("PostProcess_LinearSampler");
    linearSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);
    if (!linearSampler_) {
        Shutdown();
        return std::unexpected("Failed to create PostProcessPass sampler");
    }

    if (auto res = CreateFallbackResources(); !res) {
        Shutdown();
        return res;
    }

    // 2. Main Tonemap Uniform Buffers & BGL (bindings 0..7)
    tonemapUniformBuffer_ = CreateTrackedBuffer(device_, sizeof(TonemapUniforms),
                                                WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "Tonemap_Uniforms");
    extraUniformBuffer_ =
        CreateTrackedBuffer(device_, sizeof(PostProcessExtraUniforms),
                            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "Tonemap_ExtraUniforms");

    std::array<WGPUBindGroupLayoutEntry, 8> tEntries{};
    for (uint32_t i = 0; i < 8; ++i) {
        tEntries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        tEntries[i].binding = i;
        tEntries[i].visibility = WGPUShaderStage_Fragment;
    }
    tEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    tEntries[0].buffer.minBindingSize = sizeof(TonemapUniforms);
    tEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    tEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    tEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    tEntries[3].texture.sampleType = WGPUTextureSampleType_Float;
    tEntries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    tEntries[4].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    tEntries[4].texture.viewDimension = WGPUTextureViewDimension_2D;
    tEntries[5].texture.sampleType = WGPUTextureSampleType_Float;
    tEntries[5].texture.viewDimension = WGPUTextureViewDimension_2D;
    tEntries[6].buffer.type = WGPUBufferBindingType_Uniform;
    tEntries[6].buffer.minBindingSize = sizeof(PostProcessExtraUniforms);
    tEntries[7].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    tEntries[7].buffer.minBindingSize = sizeof(glm::vec4);

    WGPUBindGroupLayoutDescriptor tbglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    tbglDesc.entryCount = tEntries.size();
    tbglDesc.entries = tEntries.data();
    tbglDesc.label = ToWgpuStringView("Tonemap_BindGroupLayout");
    tonemapBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &tbglDesc);

    const WGPUBindGroupLayout tBglRaw{tonemapBindGroupLayout_.Get()};
    WGPUPipelineLayoutDescriptor tplDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    tplDesc.bindGroupLayoutCount = 1;
    tplDesc.bindGroupLayouts = &tBglRaw;
    tonemapPipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &tplDesc);

    // 3. Depth Linearize Layouts & Buffer (MSAA + 1x)
    depthLinUniformBuffer_ =
        CreateTrackedBuffer(device_, sizeof(DepthLinearizeUniformData),
                            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "DepthLinearize_Uniforms");
    auto makeDepthBgl = [&](bool multisampled, BindGroupLayoutHandle& bgl, PipelineLayoutHandle& pl, const char* lbl) {
        std::array<WGPUBindGroupLayoutEntry, 2> e{};
        e[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[0].binding = 0;
        e[0].visibility = WGPUShaderStage_Fragment;
        e[0].buffer.type = WGPUBufferBindingType_Uniform;
        e[0].buffer.minBindingSize = sizeof(DepthLinearizeUniformData);

        e[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[1].binding = 1;
        e[1].visibility = WGPUShaderStage_Fragment;
        e[1].texture.sampleType = WGPUTextureSampleType_Depth;
        e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        e[1].texture.multisampled = multisampled ? WGPU_TRUE : WGPU_FALSE;

        WGPUBindGroupLayoutDescriptor bDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bDesc.entryCount = e.size();
        bDesc.entries = e.data();
        bDesc.label = ToWgpuStringView(lbl);
        bgl = wgpuDeviceCreateBindGroupLayout(device_, &bDesc);

        const WGPUBindGroupLayout raw{bgl.Get()};
        WGPUPipelineLayoutDescriptor pDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pDesc.bindGroupLayoutCount = 1;
        pDesc.bindGroupLayouts = &raw;
        pl = wgpuDeviceCreatePipelineLayout(device_, &pDesc);
    };
    makeDepthBgl(true, depthLinMsaaBgl_, depthLinMsaaPl_, "DepthLinearizeMsaa_BGL");
    makeDepthBgl(false, depthLin1xBgl_, depthLin1xPl_, "DepthLinearize1x_BGL");

    // 4. GTAO + Bilateral Blur Layouts & Buffer
    gtaoUniformBuffer_ = CreateTrackedBuffer(device_, sizeof(GtaoUniformData),
                                             WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "GTAO_Uniforms");
    {
        std::array<WGPUBindGroupLayoutEntry, 2> e{};
        e[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[0].binding = 0;
        e[0].visibility = WGPUShaderStage_Fragment;
        e[0].buffer.type = WGPUBufferBindingType_Uniform;
        e[0].buffer.minBindingSize = sizeof(GtaoUniformData);
        e[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[1].binding = 1;
        e[1].visibility = WGPUShaderStage_Fragment;
        e[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        WGPUBindGroupLayoutDescriptor bDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bDesc.entryCount = e.size();
        bDesc.entries = e.data();
        gtaoBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bDesc);
        const WGPUBindGroupLayout raw{gtaoBgl_.Get()};
        WGPUPipelineLayoutDescriptor pDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pDesc.bindGroupLayoutCount = 1;
        pDesc.bindGroupLayouts = &raw;
        gtaoPl_ = wgpuDeviceCreatePipelineLayout(device_, &pDesc);

        std::array<WGPUBindGroupLayoutEntry, 3> be{};
        be[0] = e[0];
        be[1] = e[1];
        be[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        be[2].binding = 2;
        be[2].visibility = WGPUShaderStage_Fragment;
        be[2].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        be[2].texture.viewDimension = WGPUTextureViewDimension_2D;
        WGPUBindGroupLayoutDescriptor bbDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bbDesc.entryCount = be.size();
        bbDesc.entries = be.data();
        gtaoBlurBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bbDesc);
        const WGPUBindGroupLayout rawBlur{gtaoBlurBgl_.Get()};
        WGPUPipelineLayoutDescriptor pbDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pbDesc.bindGroupLayoutCount = 1;
        pbDesc.bindGroupLayouts = &rawBlur;
        gtaoBlurPl_ = wgpuDeviceCreatePipelineLayout(device_, &pbDesc);
    }

    // 5. 5-Mip Bloom Dynamic-Offset Layout & Buffer
    bloomUniformBuffer_ =
        CreateTrackedBuffer(device_, static_cast<uint64_t>(kMaxBloomUniformSlots) * kPostUniformSlotStride,
                            WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "Bloom_DynamicUniforms");
    {
        std::array<WGPUBindGroupLayoutEntry, 3> e{};
        e[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[0].binding = 0;
        e[0].visibility = WGPUShaderStage_Fragment;
        e[0].buffer.type = WGPUBufferBindingType_Uniform;
        e[0].buffer.hasDynamicOffset = WGPU_TRUE;
        e[0].buffer.minBindingSize = sizeof(BloomUniformData);

        e[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[1].binding = 1;
        e[1].visibility = WGPUShaderStage_Fragment;
        e[1].sampler.type = WGPUSamplerBindingType_Filtering;

        e[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[2].binding = 2;
        e[2].visibility = WGPUShaderStage_Fragment;
        e[2].texture.sampleType = WGPUTextureSampleType_Float;
        e[2].texture.viewDimension = WGPUTextureViewDimension_2D;

        WGPUBindGroupLayoutDescriptor bDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bDesc.entryCount = e.size();
        bDesc.entries = e.data();
        bloomBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bDesc);
        const WGPUBindGroupLayout raw{bloomBgl_.Get()};
        WGPUPipelineLayoutDescriptor pDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pDesc.bindGroupLayoutCount = 1;
        pDesc.bindGroupLayouts = &raw;
        bloomPl_ = wgpuDeviceCreatePipelineLayout(device_, &pDesc);
    }

    // 6. Auto-Exposure Compute Layout & Buffer
    autoExpUniformBuffer_ =
        CreateTrackedBuffer(device_, sizeof(AutoExposureUniformData), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                            "AutoExposure_Uniforms");
    {
        std::array<WGPUBindGroupLayoutEntry, 3> e{};
        e[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[0].binding = 0;
        e[0].visibility = WGPUShaderStage_Compute;
        e[0].buffer.type = WGPUBufferBindingType_Uniform;
        e[0].buffer.minBindingSize = sizeof(AutoExposureUniformData);

        e[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[1].binding = 1;
        e[1].visibility = WGPUShaderStage_Compute;
        e[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        e[1].texture.viewDimension = WGPUTextureViewDimension_2D;

        e[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[2].binding = 2;
        e[2].visibility = WGPUShaderStage_Compute;
        e[2].buffer.type = WGPUBufferBindingType_Storage;
        e[2].buffer.minBindingSize = sizeof(glm::vec4);

        WGPUBindGroupLayoutDescriptor bDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bDesc.entryCount = e.size();
        bDesc.entries = e.data();
        autoExpBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bDesc);
        const WGPUBindGroupLayout raw{autoExpBgl_.Get()};
        WGPUPipelineLayoutDescriptor pDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pDesc.bindGroupLayoutCount = 1;
        pDesc.bindGroupLayouts = &raw;
        autoExpPl_ = wgpuDeviceCreatePipelineLayout(device_, &pDesc);
    }

    // 7. FXAA Layout & Buffer
    fxaaUniformBuffer_ = CreateTrackedBuffer(device_, sizeof(FxaaUniformData),
                                             WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "FXAA_Uniforms");
    {
        std::array<WGPUBindGroupLayoutEntry, 3> e{};
        e[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[0].binding = 0;
        e[0].visibility = WGPUShaderStage_Fragment;
        e[0].buffer.type = WGPUBufferBindingType_Uniform;
        e[0].buffer.minBindingSize = sizeof(FxaaUniformData);

        e[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[1].binding = 1;
        e[1].visibility = WGPUShaderStage_Fragment;
        e[1].sampler.type = WGPUSamplerBindingType_Filtering;

        e[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e[2].binding = 2;
        e[2].visibility = WGPUShaderStage_Fragment;
        e[2].texture.sampleType = WGPUTextureSampleType_Float;
        e[2].texture.viewDimension = WGPUTextureViewDimension_2D;

        WGPUBindGroupLayoutDescriptor bDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bDesc.entryCount = e.size();
        bDesc.entries = e.data();
        fxaaBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &bDesc);
        const WGPUBindGroupLayout raw{fxaaBgl_.Get()};
        WGPUPipelineLayoutDescriptor pDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pDesc.bindGroupLayoutCount = 1;
        pDesc.bindGroupLayouts = &raw;
        fxaaPl_ = wgpuDeviceCreatePipelineLayout(device_, &pDesc);
    }

    if (!tonemapUniformBuffer_ || !extraUniformBuffer_ || !tonemapPipelineLayout_ || !depthLinUniformBuffer_ ||
        !gtaoUniformBuffer_ || !bloomUniformBuffer_ || !autoExpUniformBuffer_ || !fxaaUniformBuffer_) {
        Shutdown();
        return std::unexpected("Failed to allocate PostProcessPass layouts or uniform buffers");
    }

    if (auto res = CreatePipelines(false); !res) {
        Shutdown();
        return res;
    }

    BGL_LOG_INFO("PostProcessPass initialized (LinearDepth + GTAO + 5-Mip Bloom + AutoExposure + SSR + DoF + GodRays + "
                 "FXAA)");
    return {};
}

core::Result<void> PostProcessPass::CreatePipelines(bool validateScope) {
    auto compile = [&](std::string_view file, const char* lbl) {
        return ShaderManager::Instance().LoadAndCompile(device_, file, lbl, validateScope);
    };

    auto tonemapMod = compile("tonemap.wgsl", "Tonemap_ShaderModule");
    auto depthMsaaMod = compile("depth_linearize.wgsl", "DepthLinearize_ShaderModule");
    auto depth1xMod = compile("depth_linearize_1x.wgsl", "DepthLinearize1x_ShaderModule");
    auto gtaoMod = compile("gtao.wgsl", "GTAO_ShaderModule");
    auto gtaoBlurMod = compile("gtao_blur.wgsl", "GTAOBlur_ShaderModule");
    auto bloomMod = compile("bloom.wgsl", "Bloom_ShaderModule");
    auto autoExpMod = compile("auto_exposure.wgsl", "AutoExposure_ShaderModule");
    auto fxaaMod = compile("fxaa.wgsl", "FXAA_ShaderModule");

    if (!tonemapMod || !depthMsaaMod || !depth1xMod || !gtaoMod || !gtaoBlurMod || !bloomMod || !autoExpMod ||
        !fxaaMod) {
        return std::unexpected("Failed to compile one or more PostProcessPass WGSL shaders");
    }

    auto tmSwap = CreateFullscreenPipeline(device_, tonemapPipelineLayout_.Get(), tonemapMod->Get(), "vs_tonemap",
                                           "fs_tonemap", surfaceFormat_, "Tonemap_SwapchainPipeline");
    auto tmLdr = CreateFullscreenPipeline(device_, tonemapPipelineLayout_.Get(), tonemapMod->Get(), "vs_tonemap",
                                          "fs_tonemap", WGPUTextureFormat_RGBA8Unorm, "Tonemap_LdrPipeline");
    auto dMsaa = CreateFullscreenPipeline(device_, depthLinMsaaPl_.Get(), depthMsaaMod->Get(), "vs_linearize",
                                          "fs_linearize", WGPUTextureFormat_R32Float, "DepthLinearizeMsaa_Pipeline");
    auto d1x = CreateFullscreenPipeline(device_, depthLin1xPl_.Get(), depth1xMod->Get(), "vs_linearize", "fs_linearize",
                                        WGPUTextureFormat_R32Float, "DepthLinearize1x_Pipeline");
    auto gtaoP = CreateFullscreenPipeline(device_, gtaoPl_.Get(), gtaoMod->Get(), "vs_gtao", "fs_gtao",
                                          WGPUTextureFormat_RGBA8Unorm, "GTAO_Pipeline");
    auto gtaoBlurP = CreateFullscreenPipeline(device_, gtaoBlurPl_.Get(), gtaoBlurMod->Get(), "vs_gtao_blur",
                                              "fs_gtao_blur", WGPUTextureFormat_RGBA8Unorm, "GTAOBlur_Pipeline");
    auto bloomDownP =
        CreateFullscreenPipeline(device_, bloomPl_.Get(), bloomMod->Get(), "vs_bloom", "fs_bloom_downsample",
                                 WGPUTextureFormat_RGBA16Float, "Bloom_DownsamplePipeline", false);
    auto bloomUpP = CreateFullscreenPipeline(device_, bloomPl_.Get(), bloomMod->Get(), "vs_bloom", "fs_bloom_upsample",
                                             WGPUTextureFormat_RGBA16Float, "Bloom_UpsamplePipeline", true);
    auto fxaaP = CreateFullscreenPipeline(device_, fxaaPl_.Get(), fxaaMod->Get(), "vs_fxaa", "fs_fxaa", surfaceFormat_,
                                          "FXAA_Pipeline");

    if (!tmSwap || !tmLdr || !dMsaa || !d1x || !gtaoP || !gtaoBlurP || !bloomDownP || !bloomUpP || !fxaaP) {
        return std::unexpected("Failed to build post-processing render pipelines");
    }

    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = autoExpPl_.Get();
    cpDesc.compute.module = autoExpMod->Get();
    cpDesc.compute.entryPoint = ToWgpuStringView("cs_auto_exposure");
    cpDesc.label = ToWgpuStringView("AutoExposure_ComputePipeline");
    ComputePipelineHandle autoExpP{wgpuDeviceCreateComputePipeline(device_, &cpDesc)};
    if (!autoExpP) {
        return std::unexpected("Failed to build AutoExposure compute pipeline");
    }

    tonemapPipeline_ = std::move(*tmSwap);
    tonemapLdrPipeline_ = std::move(*tmLdr);
    depthLinMsaaPipeline_ = std::move(*dMsaa);
    depthLin1xPipeline_ = std::move(*d1x);
    gtaoPipeline_ = std::move(*gtaoP);
    gtaoBlurPipeline_ = std::move(*gtaoBlurP);
    bloomDownPipeline_ = std::move(*bloomDownP);
    bloomUpPipeline_ = std::move(*bloomUpP);
    fxaaPipeline_ = std::move(*fxaaP);
    autoExpPipeline_ = std::move(autoExpP);
    return {};
}

core::Result<void> PostProcessPass::ReloadShader() {
    if (!device_ || !tonemapPipelineLayout_) {
        return std::unexpected("Cannot reload PostProcessPass shaders before initialization");
    }
    return CreatePipelines(true);
}

void PostProcessPass::EnsureResolutionTargets(uint32_t width, uint32_t height) {
    const uint32_t safeW = std::max(4u, width);
    const uint32_t safeH = std::max(4u, height);
    if (safeW == targetWidth_ && safeH == targetHeight_ && linearDepthTex_) {
        return;
    }
    ReleaseResolutionTargets();

    targetWidth_ = safeW;
    targetHeight_ = safeH;
    const uint32_t halfW = std::max(1u, safeW / 2u);
    const uint32_t halfH = std::max(1u, safeH / 2u);
    const uint32_t maxDim = std::max(halfW, halfH);
    const uint32_t possibleMips = static_cast<uint32_t>(std::bit_width(maxDim));
    bloomMipCount_ = std::clamp(possibleMips, 1u, kMaxBloomMips);

    auto make2d = [&](uint32_t w, uint32_t h, WGPUTextureFormat fmt, WGPUTextureUsage usage, uint32_t mips,
                      const char* lbl, TextureHandle& tex, TextureViewHandle& view) {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.usage = usage;
        td.dimension = WGPUTextureDimension_2D;
        td.size = WGPUExtent3D{w, h, 1};
        td.format = fmt;
        td.mipLevelCount = mips;
        td.sampleCount = 1;
        td.label = ToWgpuStringView(lbl);
        tex = wgpuDeviceCreateTexture(device_, &td);
        if (tex && mips == 1) {
            view = wgpuTextureCreateView(tex.Get(), nullptr);
        }
    };

    make2d(safeW, safeH, WGPUTextureFormat_R32Float,
           WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 1, "Post_LinearDepth", linearDepthTex_,
           linearDepthView_);
    make2d(halfW, halfH, WGPUTextureFormat_RGBA8Unorm,
           WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 1, "Post_GtaoRaw", gtaoRawTex_,
           gtaoRawView_);
    make2d(halfW, halfH, WGPUTextureFormat_RGBA8Unorm,
           WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 1, "Post_GtaoBlur", gtaoBlurTex_,
           gtaoBlurView_);
    make2d(safeW, safeH, WGPUTextureFormat_RGBA8Unorm,
           WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 1, "Post_LdrScene", ldrSceneTex_,
           ldrSceneView_);

    TextureViewHandle unusedBloomView{};
    make2d(halfW, halfH, WGPUTextureFormat_RGBA16Float,
           WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, bloomMipCount_, "Post_BloomPyramid",
           bloomTex_, unusedBloomView);

    if (bloomTex_) {
        for (uint32_t m = 0; m < bloomMipCount_; ++m) {
            WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            vd.format = WGPUTextureFormat_RGBA16Float;
            vd.dimension = WGPUTextureViewDimension_2D;
            vd.baseMipLevel = m;
            vd.mipLevelCount = 1;
            vd.baseArrayLayer = 0;
            vd.arrayLayerCount = 1;
            bloomMipViews_[m] = wgpuTextureCreateView(bloomTex_.Get(), &vd);
        }
    }

    // Build static resolution-dependent bind groups (GTAO, GTAO Blur, Bloom mip->mip, FXAA)
    if (linearDepthView_) {
        std::array<WGPUBindGroupEntry, 2> ge{};
        ge[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        ge[0].binding = 0;
        ge[0].buffer = gtaoUniformBuffer_.Get();
        ge[0].size = sizeof(GtaoUniformData);
        ge[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        ge[1].binding = 1;
        ge[1].textureView = linearDepthView_.Get();
        WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        desc.layout = gtaoBgl_.Get();
        desc.entryCount = ge.size();
        desc.entries = ge.data();
        gtaoBindGroup_ = wgpuDeviceCreateBindGroup(device_, &desc);

        if (gtaoRawView_) {
            std::array<WGPUBindGroupEntry, 3> be{};
            be[0] = ge[0];
            be[1] = ge[1];
            be[2] = WGPU_BIND_GROUP_ENTRY_INIT;
            be[2].binding = 2;
            be[2].textureView = gtaoRawView_.Get();
            WGPUBindGroupDescriptor bDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bDesc.layout = gtaoBlurBgl_.Get();
            bDesc.entryCount = be.size();
            bDesc.entries = be.data();
            gtaoBlurBindGroup_ = wgpuDeviceCreateBindGroup(device_, &bDesc);
        }
    }

    for (uint32_t m = 1; m < bloomMipCount_; ++m) {
        if (bloomMipViews_[m - 1]) {
            bloomBindGroups_[m] = CreateBloomPassBindGroup(device_, bloomBgl_.Get(), bloomUniformBuffer_.Get(),
                                                           linearSampler_.Get(), bloomMipViews_[m - 1].Get());
        }
    }

    // Bloom upsample bind groups at index kMaxBloomMips + dstMip read bloomMipViews_[dstMip + 1]
    for (uint32_t m = 0; m + 1 < bloomMipCount_; ++m) {
        if (bloomMipViews_[m + 1]) {
            bloomBindGroups_[kMaxBloomMips + m] = CreateBloomPassBindGroup(
                device_, bloomBgl_.Get(), bloomUniformBuffer_.Get(), linearSampler_.Get(), bloomMipViews_[m + 1].Get());
        }
    }

    if (ldrSceneView_) {
        std::array<WGPUBindGroupEntry, 3> fe{};
        fe[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        fe[0].binding = 0;
        fe[0].buffer = fxaaUniformBuffer_.Get();
        fe[0].size = sizeof(FxaaUniformData);
        fe[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        fe[1].binding = 1;
        fe[1].sampler = linearSampler_.Get();
        fe[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        fe[2].binding = 2;
        fe[2].textureView = ldrSceneView_.Get();
        WGPUBindGroupDescriptor fDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        fDesc.layout = fxaaBgl_.Get();
        fDesc.entryCount = fe.size();
        fDesc.entries = fe.data();
        fxaaBindGroup_ = wgpuDeviceCreateBindGroup(device_, &fDesc);
    }
}

WGPUTextureView PostProcessPass::GetUiBackdropView() const noexcept {
    if (bloomMipCount_ == 0) {
        return nullptr;
    }
    const uint32_t mipIdx = std::min(bloomMipCount_ - 1u, 2u);
    return bloomMipViews_[mipIdx].Get();
}

void PostProcessPass::CycleToneMapOperator() noexcept {
    switch (toneMapOp_) {
    case ToneMapOperator::KhronosPbrNeutral:
        toneMapOp_ = ToneMapOperator::AgX;
        break;
    case ToneMapOperator::AgX:
        toneMapOp_ = ToneMapOperator::ACES;
        break;
    case ToneMapOperator::ACES:
        toneMapOp_ = ToneMapOperator::KhronosPbrNeutral;
        break;
    }
}

void PostProcessPass::CycleColorGradingPreset() noexcept {
    const uint32_t next = (std::to_underlying(colorGradingPreset_) + 1u) % 5u;
    colorGradingPreset_ = static_cast<ColorGradingPreset>(next);
}

const char* PostProcessPass::GetColorGradingPresetName(ColorGradingPreset preset) noexcept {
    switch (preset) {
    case ColorGradingPreset::Neutral:
        return "Neutral";
    case ColorGradingPreset::WarmCinema:
        return "Warm Cinema";
    case ColorGradingPreset::CoolSciFi:
        return "Cool Sci-Fi";
    case ColorGradingPreset::BleachBypass:
        return "Bleach Bypass";
    case ColorGradingPreset::Noir:
        return "Noir";
    }
    return "Neutral";
}

float PostProcessPass::ComputeDoFCoc(float linearDepth, float focusDist, float focusRange) noexcept {
    const float safeDist = std::max(focusDist, 0.2f);
    const float safeRange = std::max(focusRange, 0.25f);
    return std::clamp((std::abs(linearDepth - safeDist) - 0.35f) / safeRange, 0.0f, 1.0f);
}

} // namespace bgl::gpu