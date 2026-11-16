/**
 * @file PostProcessPassRender.cpp
 * @brief Per-frame pass execution and uniform upload for PostProcessPass (Depth, GTAO, Bloom, Auto-Exposure, FXAA).
 */
#include "gpu/PostProcessHelpers.hpp"
#include "gpu/PostProcessPass.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>

namespace bgl::gpu {
using namespace detail;

void PostProcessPass::UploadTonemapUniforms(const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                            const glm::vec3& lightDir, float envRotation, uint32_t viewportW,
                                            uint32_t viewportH) {
    const float lightLen = glm::length(lightDir);
    const glm::vec3 safeLightDir = (lightLen > 1e-4f) ? (lightDir / lightLen) : glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 rotatedSun =
        glm::rotate(glm::mat4{1.0f}, -envRotation, glm::vec3{0.0f, 1.0f, 0.0f}) * glm::vec4{safeLightDir, 0.0f};
    const glm::vec4 sunWorldPos = glm::vec4{cameraPos + rotatedSun * 1000.0f, 1.0f};
    const glm::vec4 sunClip = viewProj * sunWorldPos;

    float sunVisibility = 0.0f;
    glm::vec2 sunUV{0.5f, 0.5f};
    const float aspect =
        (viewportH > 0) ? (static_cast<float>(viewportW) / static_cast<float>(viewportH)) : (16.0f / 9.0f);

    if (sunClip.w > 0.001f) {
        const glm::vec3 sunNdc = glm::vec3{sunClip} / sunClip.w;
        if (sunNdc.z > 0.0f && sunNdc.z < 1.0f) {
            sunUV = glm::vec2{sunNdc.x * 0.5f + 0.5f, 0.5f - sunNdc.y * 0.5f};
            if (sunUV.x >= -0.2f && sunUV.x <= 1.2f && sunUV.y >= -0.2f && sunUV.y <= 1.2f) {
                const float distCenter = glm::length(sunUV - glm::vec2{0.5f});
                sunVisibility = std::clamp(1.2f - distCenter, 0.0f, 1.0f);
            }
        }
    }

    const TonemapUniforms uniforms{
        .exposure = exposure_,
        .operatorId = std::to_underlying(toneMapOp_),
        .isSrgbSurface = (!enableFxaa_ && IsSrgbSwapchainFormat(surfaceFormat_)) ? 1u : 0u,
        .lensFlareEnabled = enableLensFlare_ ? 1u : 0u,
        .sunScreenPos = glm::vec4{sunUV.x, sunUV.y, sunVisibility, aspect},
        .flareParams = glm::vec4{lensFlareIntensity_, 0.38f, 1.0f, 0.0f},
        .bloomParams = glm::vec4{bloomIntensity_, bloomThreshold_, 0.85f, enableBloom_ ? 1.0f : 0.0f},
        .ssaoParams = glm::vec4{ssaoIntensity_, ssaoRadius_, 0.005f, enableSsao_ ? 1.0f : 0.0f},
    };
    wgpuQueueWriteBuffer(queue_, tonemapUniformBuffer_.Get(), 0, &uniforms, sizeof(uniforms));

    const PostProcessExtraUniforms fxUniforms{
        .proj = projMatrix_,
        .invProj = invProjMatrix_,
        .dofParams = glm::vec4{enableDof_ ? 1.0f : 0.0f, dofFocusDistance_, dofFocusRange_, dofMaxBlurPx_},
        .ssrParams = glm::vec4{enableSsr_ ? 1.0f : 0.0f, ssrIntensity_, 14.0f, 0.35f},
        .godRayParams = glm::vec4{enableGodRays_ ? 1.0f : 0.0f, godRaysIntensity_, 0.94f, 0.85f},
        .gradingParams = glm::vec4{static_cast<float>(std::to_underlying(colorGradingPreset_)), vignetteIntensity_,
                                   chromaAberration_, filmGrainIntensity_},
        .screenParams =
            glm::vec4{static_cast<float>(std::max(1u, viewportW)), static_cast<float>(std::max(1u, viewportH)),
                      elapsedTime_, enableAutoExposure_ ? 1.0f : 0.0f},
    };
    wgpuQueueWriteBuffer(queue_, extraUniformBuffer_.Get(), 0, &fxUniforms, sizeof(fxUniforms));
}

void PostProcessPass::EnsureTonemapBindGroup(WGPUTextureView hdrResolveView, WGPUTextureView depthView,
                                             WGPUTextureView bloomView, WGPUTextureView gtaoView) {
    if (hdrResolveView == cachedHdrResolveView_ && depthView == cachedLinearDepthView_ &&
        bloomView == cachedBloomView_ && gtaoView == cachedGtaoView_ && tonemapBindGroup_) {
        return;
    }
    cachedHdrResolveView_ = hdrResolveView;
    cachedLinearDepthView_ = depthView;
    cachedBloomView_ = bloomView;
    cachedGtaoView_ = gtaoView;
    tonemapBindGroup_.Reset();

    std::array<WGPUBindGroupEntry, 8> bgEntries{};
    bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[0].binding = 0;
    bgEntries[0].buffer = tonemapUniformBuffer_.Get();
    bgEntries[0].size = sizeof(TonemapUniforms);
    bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = linearSampler_.Get();
    bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[2].binding = 2;
    bgEntries[2].textureView = hdrResolveView;
    bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[3].binding = 3;
    bgEntries[3].textureView = bloomView;
    bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[4].binding = 4;
    bgEntries[4].textureView = depthView;
    bgEntries[5] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[5].binding = 5;
    bgEntries[5].textureView = gtaoView;
    bgEntries[6] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[6].binding = 6;
    bgEntries[6].buffer = extraUniformBuffer_.Get();
    bgEntries[6].size = sizeof(PostProcessExtraUniforms);
    bgEntries[7] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[7].binding = 7;
    bgEntries[7].buffer = exposureStateBuffer_.Get();
    bgEntries[7].size = sizeof(glm::vec4);

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = tonemapBindGroupLayout_.Get();
    bgDesc.entryCount = bgEntries.size();
    bgDesc.entries = bgEntries.data();
    bgDesc.label = ToWgpuStringView("Tonemap_BindGroup");
    tonemapBindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
}

void PostProcessPass::PrepareDepthAndGtaoPasses(WGPUCommandEncoder encoder, WGPUTextureView depthReadView,
                                                uint32_t sampleCount) {
    if (!depthReadView || !linearDepthView_) {
        return;
    }

    if (depthReadView != cachedDepthReadView_ || sampleCount != cachedDepthSampleCount_ || !depthLinBindGroup_) {
        cachedDepthReadView_ = depthReadView;
        cachedDepthSampleCount_ = sampleCount;
        std::array<WGPUBindGroupEntry, 2> e{};
        e[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        e[0].binding = 0;
        e[0].buffer = depthLinUniformBuffer_.Get();
        e[0].size = sizeof(DepthLinearizeUniformData);
        e[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        e[1].binding = 1;
        e[1].textureView = depthReadView;
        WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        desc.layout = (sampleCount > 1) ? depthLinMsaaBgl_.Get() : depthLin1xBgl_.Get();
        desc.entryCount = e.size();
        desc.entries = e.data();
        depthLinBindGroup_ = wgpuDeviceCreateBindGroup(device_, &desc);
    }

    const DepthLinearizeUniformData dlData{
        .invProj = invProjMatrix_,
        .params = glm::vec4{0.02f, 2000.0f, static_cast<float>(targetWidth_), static_cast<float>(targetHeight_)},
    };
    wgpuQueueWriteBuffer(queue_, depthLinUniformBuffer_.Get(), 0, &dlData, sizeof(dlData));
    WGPURenderPipeline dlPipe = (sampleCount > 1) ? depthLinMsaaPipeline_.Get() : depthLin1xPipeline_.Get();
    DrawFullscreenPass(encoder, linearDepthView_.Get(), dlPipe, depthLinBindGroup_.Get(), "Pass_DepthLinearize");

    if (enableSsao_ && gtaoRawView_ && gtaoBlurView_ && gtaoBindGroup_ && gtaoBlurBindGroup_) {
        const GtaoUniformData gtaoUbo{
            .invProj = invProjMatrix_,
            .params = glm::vec4{ssaoRadius_, ssaoIntensity_, 0.005f, 2.0f},
            .texelInfo = glm::vec4{static_cast<float>(targetWidth_), static_cast<float>(targetHeight_),
                                   static_cast<float>(std::max(1u, targetWidth_ / 2u)),
                                   static_cast<float>(std::max(1u, targetHeight_ / 2u))},
        };
        wgpuQueueWriteBuffer(queue_, gtaoUniformBuffer_.Get(), 0, &gtaoUbo, sizeof(gtaoUbo));
        DrawFullscreenPass(encoder, gtaoRawView_.Get(), gtaoPipeline_.Get(), gtaoBindGroup_.Get(), "Pass_GTAO");
        DrawFullscreenPass(encoder, gtaoBlurView_.Get(), gtaoBlurPipeline_.Get(), gtaoBlurBindGroup_.Get(),
                           "Pass_GTAOBlur");
    }
}

void PostProcessPass::PrepareBloomAndBackdropPasses(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView) {
    if (bloomMipCount_ == 0 || !bloomMipViews_[0]) {
        return;
    }

    if (hdrResolveView != cachedBloomHdrView_ || !bloomBindGroups_[0]) {
        cachedBloomHdrView_ = hdrResolveView;
        bloomBindGroups_[0] = CreateBloomPassBindGroup(device_, bloomBgl_.Get(), bloomUniformBuffer_.Get(),
                                                       linearSampler_.Get(), hdrResolveView);
    }

    std::array<uint8_t, kMaxBloomUniformSlots * kPostUniformSlotStride> slotBytes{};
    uint32_t slotIdx = 0;
    const float activeThresh = enableBloom_ ? bloomThreshold_ : 0.0f;
    // Always run at least 3 downsamples so Mip 2 gives ImmediateUI its blurred frosted glass backdrop
    const uint32_t downMips = enableBloom_ ? bloomMipCount_ : std::min(bloomMipCount_, 3u);

    uint32_t srcW = targetWidth_;
    uint32_t srcH = targetHeight_;
    for (uint32_t m = 0; m < downMips; ++m) {
        const BloomUniformData data{
            .params0 =
                glm::vec4{1.0f / static_cast<float>(std::max(1u, srcW)), 1.0f / static_cast<float>(std::max(1u, srcH)),
                          activeThresh, std::max(0.1f, activeThresh * 0.5f)},
            .params1 = glm::vec4{(m == 0 && enableBloom_) ? 1.0f : 0.0f, 1.0f, 1.0f, 0.0f},
        };
        std::memcpy(slotBytes.data() + slotIdx * kPostUniformSlotStride, &data, sizeof(data));
        srcW = std::max(1u, srcW / 2u);
        srcH = std::max(1u, srcH / 2u);
        ++slotIdx;
    }

    if (enableBloom_ && bloomMipCount_ > 1) {
        for (int m = static_cast<int>(bloomMipCount_) - 2; m >= 0; --m) {
            const uint32_t upW = std::max(1u, targetWidth_ >> (static_cast<uint32_t>(m) + 2u));
            const uint32_t upH = std::max(1u, targetHeight_ >> (static_cast<uint32_t>(m) + 2u));
            const BloomUniformData data{
                .params0 = glm::vec4{1.0f / static_cast<float>(upW), 1.0f / static_cast<float>(upH), 0.0f, 0.0f},
                .params1 = glm::vec4{0.0f, 1.0f, 1.0f, 0.0f},
            };
            std::memcpy(slotBytes.data() + slotIdx * kPostUniformSlotStride, &data, sizeof(data));
            ++slotIdx;
        }
    }
    wgpuQueueWriteBuffer(queue_, bloomUniformBuffer_.Get(), 0, slotBytes.data(),
                         static_cast<uint64_t>(slotIdx) * kPostUniformSlotStride);

    uint32_t dispatchSlot = 0;
    for (uint32_t m = 0; m < downMips; ++m) {
        const uint32_t dynOffset = dispatchSlot * kPostUniformSlotStride;
        DrawFullscreenPass(encoder, bloomMipViews_[m].Get(), bloomDownPipeline_.Get(), bloomBindGroups_[m].Get(),
                           "Pass_BloomDownsample", false, &dynOffset);
        ++dispatchSlot;
    }
    if (enableBloom_ && bloomMipCount_ > 1) {
        for (int m = static_cast<int>(bloomMipCount_) - 2; m >= 0; --m) {
            const uint32_t dynOffset = dispatchSlot * kPostUniformSlotStride;
            DrawFullscreenPass(encoder, bloomMipViews_[static_cast<uint32_t>(m)].Get(), bloomUpPipeline_.Get(),
                               bloomBindGroups_[kMaxBloomMips + static_cast<uint32_t>(m)].Get(), "Pass_BloomUpsample",
                               true, &dynOffset);
            ++dispatchSlot;
        }
    }
}

void PostProcessPass::PrepareAutoExposurePass(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView,
                                              float deltaTime) {
    if (!autoExpPipeline_) {
        return;
    }

    if (hdrResolveView != cachedAutoExpHdrView_ || !autoExpBindGroup_) {
        cachedAutoExpHdrView_ = hdrResolveView;
        std::array<WGPUBindGroupEntry, 3> ae{};
        ae[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        ae[0].binding = 0;
        ae[0].buffer = autoExpUniformBuffer_.Get();
        ae[0].size = sizeof(AutoExposureUniformData);
        ae[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        ae[1].binding = 1;
        ae[1].textureView = hdrResolveView;
        ae[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        ae[2].binding = 2;
        ae[2].buffer = exposureStateBuffer_.Get();
        ae[2].size = sizeof(glm::vec4);
        WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        desc.layout = autoExpBgl_.Get();
        desc.entryCount = ae.size();
        desc.entries = ae.data();
        autoExpBindGroup_ = wgpuDeviceCreateBindGroup(device_, &desc);
    }

    const AutoExposureUniformData aeData{
        .params = glm::vec4{std::clamp(deltaTime, 0.001f, 0.25f), 2.5f, 0.25f, 4.0f},
        .keyParams = glm::vec4{0.24f, enableAutoExposure_ ? 1.0f : 0.0f, exposure_, 0.0f},
    };
    wgpuQueueWriteBuffer(queue_, autoExpUniformBuffer_.Get(), 0, &aeData, sizeof(aeData));

    if (!autoExpBindGroup_) {
        return;
    }
    WGPUComputePassDescriptor cpDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    cpDesc.label = ToWgpuStringView("Pass_AutoExposure");
    if (ComputePassEncoderHandle cpass{wgpuCommandEncoderBeginComputePass(encoder, &cpDesc)}) {
        wgpuComputePassEncoderSetPipeline(cpass.Get(), autoExpPipeline_.Get());
        wgpuComputePassEncoderSetBindGroup(cpass.Get(), 0, autoExpBindGroup_.Get(), 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(cpass.Get(), 1, 1, 1);
        wgpuComputePassEncoderEnd(cpass.Get());
    }
}

void PostProcessPass::PrepareFxaaTonemapPass(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView,
                                             WGPUTextureView depthReadView, const glm::mat4& viewProj,
                                             const glm::vec3& cameraPos, const glm::vec3& lightDir, float envRotation,
                                             uint32_t viewportW, uint32_t viewportH) {
    if (!enableFxaa_ || !ldrSceneView_ || !tonemapLdrPipeline_) {
        return;
    }

    WGPUTextureView activeDepthView =
        (depthReadView && linearDepthView_) ? linearDepthView_.Get() : fallbackDepthView_.Get();
    WGPUTextureView activeBloomView =
        (enableBloom_ && bloomMipViews_[0]) ? bloomMipViews_[0].Get() : fallbackBlackView_.Get();
    WGPUTextureView activeGtaoView =
        (enableSsao_ && depthReadView && gtaoBlurView_) ? gtaoBlurView_.Get() : fallbackWhiteView_.Get();

    EnsureTonemapBindGroup(hdrResolveView, activeDepthView, activeBloomView, activeGtaoView);
    UploadTonemapUniforms(viewProj, cameraPos, lightDir, envRotation, viewportW, viewportH);
    if (tonemapBindGroup_) {
        DrawFullscreenPass(encoder, ldrSceneView_.Get(), tonemapLdrPipeline_.Get(), tonemapBindGroup_.Get(),
                           "Pass_TonemapToLdr");
        preparedLdrThisFrame_ = true;
    }
}

void PostProcessPass::Prepare(WGPUCommandEncoder encoder, WGPUTextureView hdrResolveView, WGPUTextureView depthReadView,
                              uint32_t sampleCount, const glm::mat4& viewProj, const glm::mat4& projMatrix,
                              const glm::vec3& cameraPos, const glm::vec3& lightDir, float envRotation,
                              uint32_t viewportW, uint32_t viewportH, float deltaTime, float elapsedTime) {
    preparedLdrThisFrame_ = false;
    if (!encoder || !hdrResolveView || !tonemapPipeline_ || viewportW == 0 || viewportH == 0) {
        return;
    }

    projMatrix_ = projMatrix;
    invProjMatrix_ = glm::inverse(projMatrix);
    elapsedTime_ = elapsedTime;
    EnsureResolutionTargets(viewportW, viewportH);

    PrepareDepthAndGtaoPasses(encoder, depthReadView, sampleCount);
    PrepareBloomAndBackdropPasses(encoder, hdrResolveView);
    PrepareAutoExposurePass(encoder, hdrResolveView, deltaTime);
    PrepareFxaaTonemapPass(encoder, hdrResolveView, depthReadView, viewProj, cameraPos, lightDir, envRotation,
                           viewportW, viewportH);
}

void PostProcessPass::Render(WGPURenderPassEncoder swapchainPass, WGPUTextureView hdrResolveView,
                             const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec3& lightDir,
                             float envRotation, uint32_t viewportW, uint32_t viewportH) {
    if (!swapchainPass || !hdrResolveView || !tonemapPipeline_ || !tonemapUniformBuffer_ || !linearSampler_) {
        return;
    }

    if (preparedLdrThisFrame_ && enableFxaa_ && fxaaPipeline_ && fxaaBindGroup_) {
        const FxaaUniformData fxData{
            .invScreenSize = glm::vec4{1.0f / static_cast<float>(std::max(1u, targetWidth_)),
                                       1.0f / static_cast<float>(std::max(1u, targetHeight_)), 0.75f, 0.125f},
            .flags = glm::vec4{IsSrgbSwapchainFormat(surfaceFormat_) ? 1.0f : 0.0f, 0.0312f, 1.0f, 0.0f},
        };
        wgpuQueueWriteBuffer(queue_, fxaaUniformBuffer_.Get(), 0, &fxData, sizeof(fxData));
        wgpuRenderPassEncoderSetPipeline(swapchainPass, fxaaPipeline_.Get());
        wgpuRenderPassEncoderSetBindGroup(swapchainPass, 0, fxaaBindGroup_.Get(), 0, nullptr);
        wgpuRenderPassEncoderDraw(swapchainPass, 3, 1, 0, 0);
        return;
    }

    WGPUTextureView activeDepthView = linearDepthView_ ? linearDepthView_.Get() : fallbackDepthView_.Get();
    WGPUTextureView activeBloomView =
        (enableBloom_ && bloomMipViews_[0]) ? bloomMipViews_[0].Get() : fallbackBlackView_.Get();
    WGPUTextureView activeGtaoView = (enableSsao_ && gtaoBlurView_) ? gtaoBlurView_.Get() : fallbackWhiteView_.Get();

    EnsureTonemapBindGroup(hdrResolveView, activeDepthView, activeBloomView, activeGtaoView);
    if (!tonemapBindGroup_) {
        return;
    }

    UploadTonemapUniforms(viewProj, cameraPos, lightDir, envRotation, viewportW, viewportH);

    wgpuRenderPassEncoderSetPipeline(swapchainPass, tonemapPipeline_.Get());
    wgpuRenderPassEncoderSetBindGroup(swapchainPass, 0, tonemapBindGroup_.Get(), 0, nullptr);
    wgpuRenderPassEncoderDraw(swapchainPass, 3, 1, 0, 0);
}

} // namespace bgl::gpu
