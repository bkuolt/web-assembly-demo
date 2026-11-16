/**
 * @file PostProcessPassTargets.cpp
 * @brief PostProcessPass shader pipeline compilation, resolution target resizing, and tonemap/DoF helpers.
 */
#include "core/Log.hpp"
#include "gpu/PostProcessHelpers.hpp"
#include "gpu/PostProcessPass.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace bgl::gpu {
using namespace detail;

namespace {

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