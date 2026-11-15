/**
 * @file Skybox.cpp
 * @brief Implementation of HDR Environment Cubemap & Procedural Atmospheric Skybox with WebGPU 4x MSAA pipeline.
 */
#include "gpu/Skybox.hpp"
#include "core/Log.hpp"
#include "gpu/ShaderManager.hpp"
#include <array>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>

namespace bgl::gpu {

Skybox::~Skybox() {
    Shutdown();
}

void Skybox::Shutdown() noexcept {
    bindGroup_.Reset();
    fallbackSampler_.Reset();
    fallbackCubeView_.Reset();
    fallbackCubeTexture_.Reset();
    uniformBuffer_.Reset();
    pipeline_.Reset();
    pipelineLayout_.Reset();
    bindGroupLayout_.Reset();
    device_ = nullptr;
    queue_ = nullptr;
    initialized_ = false;
    hasCustomCubemap_ = false;
}

core::Result<void> Skybox::CreateRenderPipeline(WGPUShaderModule shaderModule) {
    if (!device_ || !shaderModule || !pipelineLayout_) {
        return std::unexpected("Cannot create Skybox render pipeline with invalid state");
    }

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = hdrFormat_;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = shaderModule;
    fragmentState.entryPoint = ToWgpuStringView("fs_sky");
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth24Plus;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pipelineLayout_;
    rpDesc.vertex.module = shaderModule;
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_sky");
    rpDesc.vertex.bufferCount = 0;
    rpDesc.vertex.buffers = nullptr;
    rpDesc.fragment = &fragmentState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;
    rpDesc.multisample.count = sampleCount_;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    RenderPipelineHandle nextPipe{wgpuDeviceCreateRenderPipeline(device_, &rpDesc)};
    if (!nextPipe) {
        return std::unexpected("Failed to create Skybox render pipeline");
    }
    pipeline_ = std::move(nextPipe);
    return {};
}

core::Result<void> Skybox::ReloadShader() {
    if (!initialized_ || !device_ || !pipelineLayout_) {
        return std::unexpected("Skybox not initialized");
    }
    auto modRes = ShaderManager::Instance().LoadAndCompile(device_, "skybox.wgsl", "Skybox_WGSL", true);
    if (!modRes) {
        return std::unexpected(modRes.error());
    }
    return CreateRenderPipeline(modRes->Get());
}

core::Result<void> Skybox::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat hdrFormat,
                                      uint32_t sampleCount) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize Skybox without valid WGPUDevice and WGPUQueue");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;
    hdrFormat_ = hdrFormat;
    sampleCount_ = sampleCount;

    // 1. Load & Compile WGSL Shader
    auto modRes = ShaderManager::Instance().LoadAndCompile(device_, "skybox.wgsl", "Skybox_WGSL");
    if (!modRes) {
        Shutdown();
        return std::unexpected(modRes.error());
    }
    ShaderModuleHandle shaderModule{std::move(*modRes)};

    // 2. Bind Group Layout (0: SkyboxUniforms, 1: Sampler, 2: Environment Cubemap)
    std::array<WGPUBindGroupLayoutEntry, 3> bglEntries{};
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(SkyboxUniforms);

    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    bglEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[2].binding = 2;
    bglEntries[2].visibility = WGPUShaderStage_Fragment;
    bglEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[2].texture.viewDimension = WGPUTextureViewDimension_Cube;
    bglEntries[2].texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = bglEntries.size();
    bglDesc.entries = bglEntries.data();
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!bindGroupLayout_) {
        Shutdown();
        return std::unexpected("Failed to create Skybox bind group layout");
    }

    // 3. Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = bindGroupLayout_.AddressOf();
    pipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!pipelineLayout_) {
        Shutdown();
        return std::unexpected("Failed to create Skybox pipeline layout");
    }

    // 4. Create Render Pipeline (renders into RGBA16Float 4x MSAA HDR target)
    if (auto pipeRes = CreateRenderPipeline(shaderModule.Get()); !pipeRes) {
        Shutdown();
        return std::unexpected(pipeRes.error());
    }

    // 6. Uniform Buffer
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(SkyboxUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("SkyboxUniformBuffer");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    if (!uniformBuffer_) {
        Shutdown();
        return std::unexpected("Failed to allocate Skybox uniform buffer");
    }

    // 7. Create 1x1x6 fallback cubemap and trilinear sampler
    WGPUTextureDescriptor cubeDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    cubeDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    cubeDesc.dimension = WGPUTextureDimension_2D;
    cubeDesc.size = WGPUExtent3D{1, 1, 6};
    cubeDesc.format = WGPUTextureFormat_RGBA8Unorm;
    cubeDesc.mipLevelCount = 1;
    cubeDesc.sampleCount = 1;
    cubeDesc.label = ToWgpuStringView("SkyboxFallbackCubemap");
    fallbackCubeTexture_ = wgpuDeviceCreateTexture(device_, &cubeDesc);
    if (!fallbackCubeTexture_) {
        Shutdown();
        return std::unexpected("Failed to allocate Skybox fallback cubemap");
    }

    WGPUTextureViewDescriptor cubeViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    cubeViewDesc.format = WGPUTextureFormat_RGBA8Unorm;
    cubeViewDesc.dimension = WGPUTextureViewDimension_Cube;
    cubeViewDesc.baseMipLevel = 0;
    cubeViewDesc.mipLevelCount = 1;
    cubeViewDesc.baseArrayLayer = 0;
    cubeViewDesc.arrayLayerCount = 6;
    cubeViewDesc.aspect = WGPUTextureAspect_All;
    fallbackCubeView_ = wgpuTextureCreateView(fallbackCubeTexture_, &cubeViewDesc);

    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampDesc.lodMinClamp = 0.0f;
    sampDesc.lodMaxClamp = 16.0f;
    sampDesc.maxAnisotropy = 1;
    fallbackSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    hasCustomCubemap_ = false;
    RebuildBindGroup(fallbackCubeView_, fallbackSampler_);

    initialized_ = (pipeline_ != nullptr && uniformBuffer_ != nullptr && bindGroup_ != nullptr);
    if (!initialized_) {
        Shutdown();
        return std::unexpected("Failed to finalize Skybox resources");
    }
    BGL_LOG_INFO("HDR Skybox pipeline initialized successfully (RGBA16Float, 4x MSAA, Sharp/Blurred/Hidden modes)!");
    return {};
}

void Skybox::RebuildBindGroup(WGPUTextureView cubemapView, WGPUSampler sampler) {
    if (!device_ || !bindGroupLayout_ || !uniformBuffer_)
        return;

    std::array<WGPUBindGroupEntry, 3> entries{};
    entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].buffer = uniformBuffer_;
    entries[0].offset = 0;
    entries[0].size = sizeof(SkyboxUniforms);

    entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].sampler = sampler ? sampler : fallbackSampler_.Get();

    entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[2].binding = 2;
    entries[2].textureView = cubemapView ? cubemapView : fallbackCubeView_.Get();

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = bindGroupLayout_;
    bgDesc.entryCount = entries.size();
    bgDesc.entries = entries.data();
    bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
}

void Skybox::SetEnvironmentCubemap(WGPUTextureView cubemapView, WGPUSampler sampler) {
    hasCustomCubemap_ = (cubemapView != nullptr);
    RebuildBindGroup(cubemapView, sampler);
}

void Skybox::Render(WGPURenderPassEncoder pass, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                    const glm::vec3& sunDir, float solarElevation, uint32_t weatherMode, float weatherIntensity) {
    if (!pass || !initialized_ || !enabled_ || mode_ == SkyboxMode::Hidden)
        return;

    SkyboxUniforms uniforms{};
    uniforms.invViewProj = glm::inverse(viewProj);
    const float sunLen = glm::length(sunDir);
    const glm::vec3 safeSun = (sunLen > 1e-4f) ? (sunDir / sunLen) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f});
    uniforms.sunDir = glm::vec4{safeSun, solarElevation};
    uniforms.cameraPos = glm::vec4{cameraPos, static_cast<float>(weatherMode)};
    const float blurLod{(mode_ == SkyboxMode::Blurred) ? (blurRoughness_ * 5.0f) : 0.0f};
    uniforms.params = glm::vec4{rotation_, blurLod, hasCustomCubemap_ ? 1.0f : 0.0f, weatherIntensity};

    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
}

} // namespace bgl::gpu
