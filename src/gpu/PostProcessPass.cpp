/**
 * @file PostProcessPass.cpp
 * @brief Implementation of fullscreen HDR Tonemapping, SSAO, Bloom, and Lens Flares.
 */
#include "gpu/PostProcessPass.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/Shaders.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>

namespace bgl::gpu {

namespace {

[[nodiscard]] bool IsSrgbSwapchainFormat(WGPUTextureFormat fmt) noexcept {
    return fmt == WGPUTextureFormat_BGRA8UnormSrgb || fmt == WGPUTextureFormat_RGBA8UnormSrgb;
}

} // namespace

PostProcessPass::~PostProcessPass() {
    Shutdown();
}

PostProcessPass::PostProcessPass(PostProcessPass&& other) noexcept
    : device_{other.device_}, queue_{other.queue_}, tonemapPipeline_{other.tonemapPipeline_},
      tonemapBindGroupLayout_{other.tonemapBindGroupLayout_}, tonemapUniformBuffer_{other.tonemapUniformBuffer_},
      tonemapVertexBuffer_{other.tonemapVertexBuffer_}, tonemapSampler_{other.tonemapSampler_},
      tonemapBindGroup_{other.tonemapBindGroup_}, cachedHdrResolveView_{other.cachedHdrResolveView_},
      exposure_{other.exposure_}, toneMapOp_{other.toneMapOp_}, enableLensFlare_{other.enableLensFlare_},
      lensFlareIntensity_{other.lensFlareIntensity_}, enableSsao_{other.enableSsao_},
      ssaoIntensity_{other.ssaoIntensity_}, enableBloom_{other.enableBloom_}, bloomIntensity_{other.bloomIntensity_} {
    other.device_ = nullptr;
    other.queue_ = nullptr;
    other.tonemapPipeline_ = nullptr;
    other.tonemapBindGroupLayout_ = nullptr;
    other.tonemapUniformBuffer_ = nullptr;
    other.tonemapVertexBuffer_ = nullptr;
    other.tonemapSampler_ = nullptr;
    other.tonemapBindGroup_ = nullptr;
    other.cachedHdrResolveView_ = nullptr;
}

PostProcessPass& PostProcessPass::operator=(PostProcessPass&& other) noexcept {
    if (this != &other) {
        Shutdown();
        device_ = other.device_;
        queue_ = other.queue_;
        tonemapPipeline_ = other.tonemapPipeline_;
        tonemapBindGroupLayout_ = other.tonemapBindGroupLayout_;
        tonemapUniformBuffer_ = other.tonemapUniformBuffer_;
        tonemapVertexBuffer_ = other.tonemapVertexBuffer_;
        tonemapSampler_ = other.tonemapSampler_;
        tonemapBindGroup_ = other.tonemapBindGroup_;
        cachedHdrResolveView_ = other.cachedHdrResolveView_;
        exposure_ = other.exposure_;
        toneMapOp_ = other.toneMapOp_;
        enableLensFlare_ = other.enableLensFlare_;
        lensFlareIntensity_ = other.lensFlareIntensity_;
        enableSsao_ = other.enableSsao_;
        ssaoIntensity_ = other.ssaoIntensity_;
        enableBloom_ = other.enableBloom_;
        bloomIntensity_ = other.bloomIntensity_;

        other.device_ = nullptr;
        other.queue_ = nullptr;
        other.tonemapPipeline_ = nullptr;
        other.tonemapBindGroupLayout_ = nullptr;
        other.tonemapUniformBuffer_ = nullptr;
        other.tonemapVertexBuffer_ = nullptr;
        other.tonemapSampler_ = nullptr;
        other.tonemapBindGroup_ = nullptr;
        other.cachedHdrResolveView_ = nullptr;
    }
    return *this;
}

void PostProcessPass::Shutdown() noexcept {
    if (tonemapBindGroup_) {
        wgpuBindGroupRelease(tonemapBindGroup_);
        tonemapBindGroup_ = nullptr;
    }
    if (tonemapSampler_) {
        wgpuSamplerRelease(tonemapSampler_);
        tonemapSampler_ = nullptr;
    }
    if (tonemapVertexBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(3 * sizeof(glm::vec2));
        wgpuBufferRelease(tonemapVertexBuffer_);
        tonemapVertexBuffer_ = nullptr;
    }
    if (tonemapUniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(TonemapUniforms));
        wgpuBufferRelease(tonemapUniformBuffer_);
        tonemapUniformBuffer_ = nullptr;
    }
    if (tonemapPipeline_) {
        wgpuRenderPipelineRelease(tonemapPipeline_);
        tonemapPipeline_ = nullptr;
    }
    if (tonemapBindGroupLayout_) {
        wgpuBindGroupLayoutRelease(tonemapBindGroupLayout_);
        tonemapBindGroupLayout_ = nullptr;
    }
    cachedHdrResolveView_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> PostProcessPass::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize PostProcessPass with null WebGPU device or queue");
    }
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;

    // 1. Compile WGSL Tonemap Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWgpuStringView(kTonemapShaderWGSL.data());

    WGPUShaderModuleDescriptor shaderDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDesc.nextInChain = &wgslDesc.chain;
    shaderDesc.label = ToWgpuStringView("Tonemap_ShaderModule");

    WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(device_, &shaderDesc);
    if (!shaderModule) {
        return std::unexpected("Failed to compile tonemap post-process WGSL shader module");
    }

    // 2. Bind Group Layout (Uniforms + Sampler + HDR Resolve View)
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(TonemapUniforms);

    entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    entries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].texture.sampleType = WGPUTextureSampleType_Float;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = entries.size();
    bglDesc.entries = entries.data();
    bglDesc.label = ToWgpuStringView("Tonemap_BindGroupLayout");
    tonemapBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    // 3. Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &tonemapBindGroupLayout_;
    plDesc.label = ToWgpuStringView("Tonemap_PipelineLayout");
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(device_, &plDesc);

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragState = WGPU_FRAGMENT_STATE_INIT;
    fragState.module = shaderModule;
    fragState.entryPoint = ToWgpuStringView("fs_tonemap");
    fragState.targetCount = 1;
    fragState.targets = &colorTarget;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pl;
    rpDesc.vertex.module = shaderModule;
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_tonemap");
    rpDesc.vertex.bufferCount = 0;
    rpDesc.vertex.buffers = nullptr;
    rpDesc.fragment = &fragState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = nullptr;
    rpDesc.multisample.count = 1;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;
    rpDesc.label = ToWgpuStringView("Tonemap_RenderPipeline");

    tonemapPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);
    wgpuPipelineLayoutRelease(pl);
    wgpuShaderModuleRelease(shaderModule);

    if (!tonemapPipeline_) {
        return std::unexpected("Failed to create tonemap post-process render pipeline");
    }

    // 4. Uniform Buffer
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(TonemapUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("Tonemap_Uniforms");
    tonemapUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(TonemapUniforms));

    // 5. Sampler
    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampDesc.maxAnisotropy = 1;
    sampDesc.label = ToWgpuStringView("Tonemap_Sampler");
    tonemapSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    BGL_LOG_INFO("PostProcessPass initialized successfully (Tonemapping + SSAO + Bloom + Lens Flares)");
    return {};
}

void PostProcessPass::Render(WGPURenderPassEncoder swapchainPass, WGPUTextureView hdrResolveView,
                             const glm::mat4& viewProj, const glm::vec3& cameraPos, const glm::vec3& lightDir,
                             float envRotation, uint32_t viewportW, uint32_t viewportH) {
    if (!swapchainPass || !hdrResolveView || !tonemapPipeline_)
        return;

    if (hdrResolveView != cachedHdrResolveView_ || !tonemapBindGroup_) {
        cachedHdrResolveView_ = hdrResolveView;
        if (tonemapBindGroup_)
            wgpuBindGroupRelease(tonemapBindGroup_);

        std::array<WGPUBindGroupEntry, 3> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = tonemapUniformBuffer_;
        bgEntries[0].offset = 0;
        bgEntries[0].size = sizeof(TonemapUniforms);

        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].sampler = tonemapSampler_;

        bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[2].binding = 2;
        bgEntries[2].textureView = hdrResolveView;

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = tonemapBindGroupLayout_;
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        bgDesc.label = ToWgpuStringView("Tonemap_BindGroup");
        tonemapBindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    }

    // Project sun vector to screen NDC for real-time lens flares
    const glm::vec3 rotatedSun = glm::rotate(glm::mat4{1.0f}, -envRotation, glm::vec3{0.0f, 1.0f, 0.0f}) *
                                 glm::vec4{glm::normalize(lightDir), 0.0f};
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

    TonemapUniforms uniforms{.exposure = exposure_,
                             .operatorId = std::to_underlying(toneMapOp_),
                             .isSrgbSurface = IsSrgbSwapchainFormat(surfaceFormat_) ? 1u : 0u,
                             .lensFlareEnabled = enableLensFlare_ ? 1u : 0u,
                             .sunScreenPos = glm::vec4{sunUV.x, sunUV.y, sunVisibility, aspect},
                             .flareParams = glm::vec4{lensFlareIntensity_, 0.38f, 1.0f, 0.0f},
                             .bloomParams =
                                 glm::vec4{bloomIntensity_, bloomThreshold_, 0.85f, enableBloom_ ? 1.0f : 0.0f},
                             .ssaoParams = glm::vec4{ssaoIntensity_, ssaoRadius_, 0.005f, enableSsao_ ? 1.0f : 0.0f}};

    wgpuQueueWriteBuffer(queue_, tonemapUniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(swapchainPass, tonemapPipeline_);
    wgpuRenderPassEncoderSetBindGroup(swapchainPass, 0, tonemapBindGroup_, 0, nullptr);
    wgpuRenderPassEncoderDraw(swapchainPass, 3, 1, 0, 0);
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

} // namespace bgl::gpu
