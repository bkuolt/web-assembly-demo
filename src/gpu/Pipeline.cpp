/**
 * @file Pipeline.cpp
 * @brief Implementation of hardware native WebGPU RenderPipeline and glTF mesh rendering.
 */
#include "gpu/Pipeline.hpp"
#include "gpu/Shaders.hpp"
#include "scene/Scene.hpp"
#include "scene/Quadtree.hpp"
#include "core/Log.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace bgl::gpu {

PipelineManager::~PipelineManager() {
    if (defaultSampler_) {
        wgpuSamplerRelease(defaultSampler_);
        defaultSampler_ = nullptr;
    }
    if (defaultBlackTextureView_) {
        wgpuTextureViewRelease(defaultBlackTextureView_);
        defaultBlackTextureView_ = nullptr;
    }
    if (defaultBlackTexture_) {
        wgpuTextureRelease(defaultBlackTexture_);
        defaultBlackTexture_ = nullptr;
    }
    if (defaultNormalTextureView_) {
        wgpuTextureViewRelease(defaultNormalTextureView_);
        defaultNormalTextureView_ = nullptr;
    }
    if (defaultNormalTexture_) {
        wgpuTextureRelease(defaultNormalTexture_);
        defaultNormalTexture_ = nullptr;
    }
    if (defaultWhiteTextureView_) {
        wgpuTextureViewRelease(defaultWhiteTextureView_);
        defaultWhiteTextureView_ = nullptr;
    }
    if (defaultWhiteTexture_) {
        wgpuTextureRelease(defaultWhiteTexture_);
        defaultWhiteTexture_ = nullptr;
    }
    if (aabbBindGroup_) {
        wgpuBindGroupRelease(aabbBindGroup_);
        aabbBindGroup_ = nullptr;
    }
    if (aabbUniformBuffer_) {
        wgpuBufferRelease(aabbUniformBuffer_);
        aabbUniformBuffer_ = nullptr;
    }
    if (aabbVertexBuffer_) {
        wgpuBufferRelease(aabbVertexBuffer_);
        aabbVertexBuffer_ = nullptr;
    }
    if (aabbPipeline_) {
        wgpuRenderPipelineRelease(aabbPipeline_);
        aabbPipeline_ = nullptr;
    }
    if (aabbBindGroupLayout_) {
        wgpuBindGroupLayoutRelease(aabbBindGroupLayout_);
        aabbBindGroupLayout_ = nullptr;
    }
    if (wireframePipeline_) {
        wgpuRenderPipelineRelease(wireframePipeline_);
        wireframePipeline_ = nullptr;
    }
    if (opaquePipeline_) {
        wgpuRenderPipelineRelease(opaquePipeline_);
        opaquePipeline_ = nullptr;
    }
    if (bindGroupLayout_) {
        wgpuBindGroupLayoutRelease(bindGroupLayout_);
        bindGroupLayout_ = nullptr;
    }
}

core::Result<void> PipelineManager::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat, uint32_t sampleCount) {
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;
    sampleCount_ = sampleCount;
    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize PipelineManager without valid WGPUDevice and WGPUQueue");
    }

    // 1. Compile WGSL PBR Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.code = WGPUStringView{ kPbrShaderWGSL.data(), kPbrShaderWGSL.size() };

    WGPUShaderModuleDescriptor smDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgslDesc);
    smDesc.label = WGPUStringView{ "PBR_WGSL", strlen("PBR_WGSL") };

    WGPUShaderModule pbrShaderModule = wgpuDeviceCreateShaderModule(device_, &smDesc);
    if (!pbrShaderModule) {
        return std::unexpected("Failed to compile embedded WGSL PBR shader module");
    }

    // 2. Compile WGSL Wireframe Shader Module
    WGPUShaderSourceWGSL wfWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wfWgslDesc.code = WGPUStringView{ kWireframeShaderWGSL.data(), kWireframeShaderWGSL.size() };

    WGPUShaderModuleDescriptor wfSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    wfSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wfWgslDesc);
    wfSmDesc.label = WGPUStringView{ "Wireframe_WGSL", strlen("Wireframe_WGSL") };

    WGPUShaderModule wfShaderModule = wgpuDeviceCreateShaderModule(device_, &wfSmDesc);

    // 3. Bind Group Layout:
    // 0: Uniform buffer
    // 1: Sampler
    // 2: BaseColor texture
    // 3: MetallicRoughness texture
    // 4: Normal texture
    // 5: Emissive texture
    // 6: Occlusion texture
    // 7: Clearcoat texture
    // 8: Transmission texture
    // 9: Sheen texture
    WGPUBindGroupLayoutEntry bglEntries[10] = {};

    // Binding 0: Uniform buffer
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(PrimitiveUniforms);

    // Binding 1: Sampler
    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    // Bindings 2..9: 2D Sampled Textures
    for (uint32_t b = 2; b < 10; ++b) {
        bglEntries[b] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        bglEntries[b].binding = b;
        bglEntries[b].visibility = WGPUShaderStage_Fragment;
        bglEntries[b].texture.sampleType = WGPUTextureSampleType_Float;
        bglEntries[b].texture.viewDimension = WGPUTextureViewDimension_2D;
        bglEntries[b].texture.multisampled = WGPU_FALSE;
    }

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = 10;
    bglDesc.entries = bglEntries;
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!bindGroupLayout_) {
        return std::unexpected("Failed to create WebGPU BindGroupLayout for PBR material");
    }

    // 4. Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout_;
    WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(device_, &plDesc);

    // 5. Vertex Layout (Position, Normal, Tangent, UV)
    WGPUVertexAttribute vertAttrs[4] = {};
    // Position (vec3<f32>)
    vertAttrs[0].format = WGPUVertexFormat_Float32x3;
    vertAttrs[0].offset = offsetof(core::Vertex, position);
    vertAttrs[0].shaderLocation = 0;
    // Normal (vec3<f32>)
    vertAttrs[1].format = WGPUVertexFormat_Float32x3;
    vertAttrs[1].offset = offsetof(core::Vertex, normal);
    vertAttrs[1].shaderLocation = 1;
    // Tangent (vec4<f32>)
    vertAttrs[2].format = WGPUVertexFormat_Float32x4;
    vertAttrs[2].offset = offsetof(core::Vertex, tangent);
    vertAttrs[2].shaderLocation = 2;
    // UV (vec2<f32>)
    vertAttrs[3].format = WGPUVertexFormat_Float32x2;
    vertAttrs[3].offset = offsetof(core::Vertex, uv);
    vertAttrs[3].shaderLocation = 3;

    WGPUVertexBufferLayout vbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbLayout.arrayStride = sizeof(core::Vertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = 4;
    vbLayout.attributes = vertAttrs;

    // 6. Color Target & Depth State
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = pbrShaderModule;
    fragmentState.entryPoint = WGPUStringView{ "fs_main", strlen("fs_main") };
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth24Plus;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_True;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    // 7. Create Opaque Render Pipeline
    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.module = pbrShaderModule;
    rpDesc.vertex.entryPoint = WGPUStringView{ "vs_main", strlen("vs_main") };
    rpDesc.vertex.bufferCount = 1;
    rpDesc.vertex.buffers = &vbLayout;
    rpDesc.fragment = &fragmentState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;
    rpDesc.multisample.count = sampleCount_;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    opaquePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);

    // 8. Create Wireframe Render Pipeline
    if (wfShaderModule) {
        WGPUFragmentState wfFragState = WGPU_FRAGMENT_STATE_INIT;
        wfFragState.module = wfShaderModule;
        wfFragState.entryPoint = WGPUStringView{ "fs_main", strlen("fs_main") };
        wfFragState.targetCount = 1;
        wfFragState.targets = &colorTarget;

        WGPUDepthStencilState wfDepthStencil = depthStencil;
        wfDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;

        WGPURenderPipelineDescriptor wfRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        wfRpDesc.layout = pipelineLayout;
        wfRpDesc.vertex.module = wfShaderModule;
        wfRpDesc.vertex.entryPoint = WGPUStringView{ "vs_main", strlen("vs_main") };
        wfRpDesc.vertex.bufferCount = 1;
        wfRpDesc.vertex.buffers = &vbLayout;
        wfRpDesc.fragment = &wfFragState;
        wfRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
        wfRpDesc.primitive.cullMode = WGPUCullMode_None;
        wfRpDesc.depthStencil = &wfDepthStencil;
        wfRpDesc.multisample.count = sampleCount_;
        wfRpDesc.multisample.mask = 0xFFFFFFFF;
        wfRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

        wireframePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &wfRpDesc);
        wgpuShaderModuleRelease(wfShaderModule);
    }

    // 8b. Create AABB Wireframe Render Pipeline
    WGPUShaderSourceWGSL aabbWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    aabbWgslDesc.code = WGPUStringView{ kAabbShaderWGSL.data(), kAabbShaderWGSL.size() };

    WGPUShaderModuleDescriptor aabbSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    aabbSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&aabbWgslDesc);
    aabbSmDesc.label = WGPUStringView{ "AABB_WGSL", strlen("AABB_WGSL") };
    WGPUShaderModule aabbShaderModule = wgpuDeviceCreateShaderModule(device_, &aabbSmDesc);

    // AABB Bind Group Layout (Binding 0: AabbUniforms)
    WGPUBindGroupLayoutEntry aabbBglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    aabbBglEntry.binding = 0;
    aabbBglEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    aabbBglEntry.buffer.type = WGPUBufferBindingType_Uniform;
    aabbBglEntry.buffer.minBindingSize = sizeof(AabbUniforms);

    WGPUBindGroupLayoutDescriptor aabbBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    aabbBglDesc.entryCount = 1;
    aabbBglDesc.entries = &aabbBglEntry;
    aabbBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &aabbBglDesc);

    // AABB Pipeline Layout
    WGPUPipelineLayoutDescriptor aabbPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    aabbPlDesc.bindGroupLayoutCount = 1;
    aabbPlDesc.bindGroupLayouts = &aabbBindGroupLayout_;
    WGPUPipelineLayout aabbPlLayout = wgpuDeviceCreatePipelineLayout(device_, &aabbPlDesc);

    // AABB Vertex Layout: pos vec3<f32>
    WGPUVertexAttribute aabbAttr = {};
    aabbAttr.format = WGPUVertexFormat_Float32x3;
    aabbAttr.offset = 0;
    aabbAttr.shaderLocation = 0;

    WGPUVertexBufferLayout aabbVbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    aabbVbLayout.arrayStride = sizeof(float) * 3;
    aabbVbLayout.stepMode = WGPUVertexStepMode_Vertex;
    aabbVbLayout.attributeCount = 1;
    aabbVbLayout.attributes = &aabbAttr;

    WGPUFragmentState aabbFragState = WGPU_FRAGMENT_STATE_INIT;
    aabbFragState.module = aabbShaderModule;
    aabbFragState.entryPoint = WGPUStringView{ "fs_aabb", strlen("fs_aabb") };
    aabbFragState.targetCount = 1;
    aabbFragState.targets = &colorTarget;

    WGPUDepthStencilState aabbDepthStencil = depthStencil;
    aabbDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    aabbDepthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    WGPURenderPipelineDescriptor aabbRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    aabbRpDesc.layout = aabbPlLayout;
    aabbRpDesc.vertex.module = aabbShaderModule;
    aabbRpDesc.vertex.entryPoint = WGPUStringView{ "vs_aabb", strlen("vs_aabb") };
    aabbRpDesc.vertex.bufferCount = 1;
    aabbRpDesc.vertex.buffers = &aabbVbLayout;
    aabbRpDesc.fragment = &aabbFragState;
    aabbRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
    aabbRpDesc.primitive.cullMode = WGPUCullMode_None;
    aabbRpDesc.depthStencil = &aabbDepthStencil;
    aabbRpDesc.multisample.count = sampleCount_;
    aabbRpDesc.multisample.mask = 0xFFFFFFFF;
    aabbRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    aabbPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &aabbRpDesc);

    wgpuPipelineLayoutRelease(aabbPlLayout);
    wgpuShaderModuleRelease(aabbShaderModule);

    // AABB Unit Box Vertex Buffer
    static constexpr float kUnitBoxVertices[] = {
        // Bottom 4 lines (Y = 0)
        0.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f,
        // Top 4 lines (Y = 1)
        0.0f, 1.0f, 0.0f,   1.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 0.0f,   1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,   0.0f, 1.0f, 1.0f,
        0.0f, 1.0f, 1.0f,   0.0f, 1.0f, 0.0f,
        // 4 Vertical pillars
        0.0f, 0.0f, 0.0f,   0.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 1.0f,   1.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 1.0f,   0.0f, 1.0f, 1.0f,
    };
    WGPUBufferDescriptor boxVbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    boxVbDesc.size = sizeof(kUnitBoxVertices);
    boxVbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    boxVbDesc.label = WGPUStringView{ "AABB_BoxLines", strlen("AABB_BoxLines") };
    aabbVertexBuffer_ = wgpuDeviceCreateBuffer(device_, &boxVbDesc);
    wgpuQueueWriteBuffer(queue_, aabbVertexBuffer_, 0, kUnitBoxVertices, sizeof(kUnitBoxVertices));

    // AABB Uniform Buffer
    WGPUBufferDescriptor aabbUbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    aabbUbDesc.size = sizeof(AabbUniforms);
    aabbUbDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    aabbUbDesc.label = WGPUStringView{ "AABB_Uniforms", strlen("AABB_Uniforms") };
    aabbUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &aabbUbDesc);

    // AABB Bind Group
    WGPUBindGroupEntry aabbBgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
    aabbBgEntry.binding = 0;
    aabbBgEntry.buffer = aabbUniformBuffer_;
    aabbBgEntry.offset = 0;
    aabbBgEntry.size = sizeof(AabbUniforms);

    WGPUBindGroupDescriptor aabbBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    aabbBgDesc.layout = aabbBindGroupLayout_;
    aabbBgDesc.entryCount = 1;
    aabbBgDesc.entries = &aabbBgEntry;
    aabbBindGroup_ = wgpuDeviceCreateBindGroup(device_, &aabbBgDesc);

    wgpuPipelineLayoutRelease(pipelineLayout);
    wgpuShaderModuleRelease(pbrShaderModule);

    // 9. Helper to create 1x1 fallback textures
    auto create1x1Texture = [&](const uint8_t rgba[4], const char* label) -> std::pair<WGPUTexture, WGPUTextureView> {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.size = WGPUExtent3D{ 1, 1, 1 };
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.label = WGPUStringView{ label, strlen(label) };
        WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = tex;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = 4;
        dataLayout.rowsPerImage = 1;

        WGPUExtent3D writeSize = { 1, 1, 1 };
        wgpuQueueWriteTexture(queue_, &copyDst, rgba, 4, &dataLayout, &writeSize);

        WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
        return { tex, view };
    };

    // 1x1 White Texture (Default for BaseColor, MetallicRoughness, Occlusion, Clearcoat, Transmission, Sheen)
    const uint8_t whitePixels[4] = { 255, 255, 255, 255 };
    auto [whiteTex, whiteView] = create1x1Texture(whitePixels, "DefaultWhiteTexture");
    defaultWhiteTexture_ = whiteTex;
    defaultWhiteTextureView_ = whiteView;

    // 1x1 Flat Normal Texture (0.5, 0.5, 1.0 => Normal 0, 0, 1)
    const uint8_t normalPixels[4] = { 128, 128, 255, 255 };
    auto [normTex, normView] = create1x1Texture(normalPixels, "DefaultNormalTexture");
    defaultNormalTexture_ = normTex;
    defaultNormalTextureView_ = normView;

    // 1x1 Black Texture (Default for Emissive)
    const uint8_t blackPixels[4] = { 0, 0, 0, 255 };
    auto [blackTex, blackView] = create1x1Texture(blackPixels, "DefaultBlackTexture");
    defaultBlackTexture_ = blackTex;
    defaultBlackTextureView_ = blackView;

    // 10. Create Default Filtering Sampler (16x Anisotropic + Trilinear Mipmap Filtering)
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

    initialized_ = (opaquePipeline_ != nullptr);
    BGL_LOG_INFO("Hardware WebGPU RenderPipeline initialized with full glTF 2.0 PBR material shaders!");
    return {};
}

static std::vector<uint8_t> GenerateNextMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) {
    std::vector<uint8_t> dst(dstW * dstH * 4);
    for (uint32_t y = 0; y < dstH; ++y) {
        for (uint32_t x = 0; x < dstW; ++x) {
            uint32_t srcX0 = std::min(x * 2, srcW - 1);
            uint32_t srcX1 = std::min(x * 2 + 1, srcW - 1);
            uint32_t srcY0 = std::min(y * 2, srcH - 1);
            uint32_t srcY1 = std::min(y * 2 + 1, srcH - 1);

            const uint8_t* p00 = src + (srcY0 * srcW + srcX0) * 4;
            const uint8_t* p10 = src + (srcY0 * srcW + srcX1) * 4;
            const uint8_t* p01 = src + (srcY1 * srcW + srcX0) * 4;
            const uint8_t* p11 = src + (srcY1 * srcW + srcX1) * 4;

            size_t dstIdx = (y * dstW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                uint32_t sum = static_cast<uint32_t>(p00[c]) +
                               static_cast<uint32_t>(p10[c]) +
                               static_cast<uint32_t>(p01[c]) +
                               static_cast<uint32_t>(p11[c]);
                dst[dstIdx + c] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
    return dst;
}

void PipelineManager::PrepareScene(scene::Scene& scene) {
    if (!device_ || !queue_ || !bindGroupLayout_) return;

    // 1. Upload CPU decoded images to WebGPU Textures with full mipmap pyramid
    for (size_t i = 0; i < scene.cpuImages.size(); ++i) {
        const auto& img = scene.cpuImages[i];
        if (img.width == 0 || img.height == 0 || img.rgba.empty()) continue;

        if (i < scene.gpuTextures.size() && scene.gpuTextures[i].view == nullptr) {
            uint32_t maxDim = std::max(img.width, img.height);
            uint32_t mipLevels = 1;
            while ((maxDim >> mipLevels) > 0) {
                mipLevels++;
            }

            WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.size = WGPUExtent3D{ img.width, img.height, 1 };
            texDesc.format = WGPUTextureFormat_RGBA8Unorm;
            texDesc.mipLevelCount = mipLevels;
            texDesc.sampleCount = 1;
            texDesc.label = WGPUStringView{ "glTF_Texture", strlen("glTF_Texture") };
            WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

            // Mip 0 upload
            WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            copyDst.texture = tex;
            copyDst.mipLevel = 0;
            copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
            copyDst.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            dataLayout.offset = 0;
            dataLayout.bytesPerRow = img.width * 4;
            dataLayout.rowsPerImage = img.height;

            WGPUExtent3D writeSize = { img.width, img.height, 1 };
            wgpuQueueWriteTexture(queue_, &copyDst, img.rgba.data(), img.rgba.size(), &dataLayout, &writeSize);

            // Generate downsampled mips and upload to GPU
            std::vector<uint8_t> prevMip = img.rgba;
            uint32_t prevW = img.width;
            uint32_t prevH = img.height;

            for (uint32_t level = 1; level < mipLevels; ++level) {
                uint32_t curW = std::max(1u, prevW / 2);
                uint32_t curH = std::max(1u, prevH / 2);

                std::vector<uint8_t> curMip = GenerateNextMipLevel(prevMip.data(), prevW, prevH, curW, curH);

                WGPUTexelCopyTextureInfo mipCopyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                mipCopyDst.texture = tex;
                mipCopyDst.mipLevel = level;
                mipCopyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
                mipCopyDst.aspect = WGPUTextureAspect_All;

                WGPUTexelCopyBufferLayout mipDataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                mipDataLayout.offset = 0;
                mipDataLayout.bytesPerRow = curW * 4;
                mipDataLayout.rowsPerImage = curH;

                WGPUExtent3D mipWriteSize = { curW, curH, 1 };
                wgpuQueueWriteTexture(queue_, &mipCopyDst, curMip.data(), curMip.size(), &mipDataLayout, &mipWriteSize);

                prevMip = std::move(curMip);
                prevW = curW;
                prevH = curH;
            }

            WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

            scene.gpuTextures[i].texture = tex;
            scene.gpuTextures[i].view = view;
            scene.gpuTextures[i].sampler = defaultSampler_;
            scene.gpuTextures[i].width = img.width;
            scene.gpuTextures[i].height = img.height;
            BGL_LOG_INFO("[WebGPU Upload] Texture #{}: Created 2D Texture ({}x{}, RGBA8Unorm, {} Mip Levels, 16x Aniso), wrote {} bytes to GPU (Handle: {:p})",
                         i, img.width, img.height, mipLevels, img.rgba.size(), static_cast<void*>(tex));
        }
    }

    auto getTexView = [&](int texIdx, WGPUTextureView fallback) -> WGPUTextureView {
        if (texIdx >= 0 && texIdx < static_cast<int>(scene.gpuTextures.size())) {
            if (scene.gpuTextures[texIdx].view) {
                return scene.gpuTextures[texIdx].view;
            }
        }
        return fallback;
    };

    // 2. Prepare vertex buffers, index buffers, uniform buffers, and bind groups for each primitive
    auto prepareNode = [&](auto& self, scene::SceneNode& node) -> void {
        for (size_t pIdx = 0; pIdx < node.primitives.size(); ++pIdx) {
            auto& prim = node.primitives[pIdx];
            if (prim.vertices.empty()) continue;

            size_t vbSize = prim.vertices.size() * sizeof(core::Vertex);
            size_t ibSize = prim.indices.size() * sizeof(uint32_t);

            // Vertex Buffer
            if (!prim.gpuVertexBuffer) {
                WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                vbDesc.size = (vbSize + 3) & ~3;
                vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
                vbDesc.label = WGPUStringView{ "VertexBuffer", strlen("VertexBuffer") };
                prim.gpuVertexBuffer = wgpuDeviceCreateBuffer(device_, &vbDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuVertexBuffer, 0, prim.vertices.data(), vbSize);
            }

            // Index Buffer
            if (!prim.gpuIndexBuffer && !prim.indices.empty()) {
                WGPUBufferDescriptor ibDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ibDesc.size = (ibSize + 3) & ~3;
                ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
                ibDesc.label = WGPUStringView{ "IndexBuffer", strlen("IndexBuffer") };
                prim.gpuIndexBuffer = wgpuDeviceCreateBuffer(device_, &ibDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuIndexBuffer, 0, prim.indices.data(), ibSize);
            }

            // Uniform Buffer (272 bytes)
            if (!prim.gpuUniformBuffer) {
                WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ubDesc.size = sizeof(PrimitiveUniforms);
                ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                ubDesc.label = WGPUStringView{ "PrimitiveUniforms", strlen("PrimitiveUniforms") };
                prim.gpuUniformBuffer = wgpuDeviceCreateBuffer(device_, &ubDesc);
            }

            BGL_LOG_INFO("[WebGPU Upload] Node '{}' Prim #{}: VB={} bytes ({} verts), IB={} bytes ({} indices), UB={} bytes",
                         node.name, pIdx, vbSize, prim.vertices.size(), ibSize, prim.indices.size(), sizeof(PrimitiveUniforms));

            // Bind Group (10 entries)
            if (!prim.gpuBindGroup) {
                int baseColorTexIdx = -1;
                int mrTexIdx = -1;
                int normalTexIdx = -1;
                int emissiveTexIdx = -1;
                int occlusionTexIdx = -1;
                int clearcoatTexIdx = -1;
                int transmissionTexIdx = -1;
                int sheenTexIdx = -1;

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

                WGPUBindGroupEntry bgEntries[10] = {};
                // 0: Uniform buffer
                bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[0].binding = 0;
                bgEntries[0].buffer = prim.gpuUniformBuffer;
                bgEntries[0].offset = 0;
                bgEntries[0].size = sizeof(PrimitiveUniforms);

                // 1: Sampler
                bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[1].binding = 1;
                bgEntries[1].sampler = defaultSampler_;

                // 2: BaseColor
                bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[2].binding = 2;
                bgEntries[2].textureView = getTexView(baseColorTexIdx, defaultWhiteTextureView_);

                // 3: MetallicRoughness
                bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[3].binding = 3;
                bgEntries[3].textureView = getTexView(mrTexIdx, defaultWhiteTextureView_);

                // 4: Normal
                bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[4].binding = 4;
                bgEntries[4].textureView = getTexView(normalTexIdx, defaultNormalTextureView_);

                // 5: Emissive
                bgEntries[5] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[5].binding = 5;
                bgEntries[5].textureView = getTexView(emissiveTexIdx, defaultBlackTextureView_);

                // 6: Occlusion
                bgEntries[6] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[6].binding = 6;
                bgEntries[6].textureView = getTexView(occlusionTexIdx, defaultWhiteTextureView_);

                // 7: Clearcoat
                bgEntries[7] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[7].binding = 7;
                bgEntries[7].textureView = getTexView(clearcoatTexIdx, defaultWhiteTextureView_);

                // 8: Transmission
                bgEntries[8] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[8].binding = 8;
                bgEntries[8].textureView = getTexView(transmissionTexIdx, defaultWhiteTextureView_);

                // 9: Sheen
                bgEntries[9] = WGPU_BIND_GROUP_ENTRY_INIT;
                bgEntries[9].binding = 9;
                bgEntries[9].textureView = getTexView(sheenTexIdx, defaultWhiteTextureView_);

                WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                bgDesc.layout = bindGroupLayout_;
                bgDesc.entryCount = 10;
                bgDesc.entries = bgEntries;
                prim.gpuBindGroup = wgpuDeviceCreateBindGroup(device_, &bgDesc);
            }
        }

        for (auto& child : node.children) {
            if (child) {
                self(self, *child);
            }
        }
    };

    for (auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            prepareNode(prepareNode, *rootNode);
        }
    }
}

void PipelineManager::RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe, const scene::Quadtree* spatialIndex) {
    if (!pass || !initialized_) return;

    WGPURenderPipeline activePipeline = wireframe && wireframePipeline_ ? wireframePipeline_ : opaquePipeline_;
    wgpuRenderPassEncoderSetPipeline(pass, activePipeline);

    core::Frustum frustum = core::Frustum::FromViewProjection(viewProj);
    lastVisiblePrimitiveCount_ = 0;

    if (spatialIndex) {
        auto visibleItems = spatialIndex->QueryVisible(frustum, cameraPos);
        lastVisiblePrimitiveCount_ = visibleItems.size();
        for (const auto& item : visibleItems) {
            if (!item.node || item.primitiveIndex >= item.node->primitives.size()) continue;
            auto& prim = item.node->primitives[item.primitiveIndex];
            RenderPrimitive(pass, scene, prim, item.node->worldTransform, viewProj, cameraPos, wireframe);
        }
    } else {
        for (const auto& rootNode : scene.rootNodes) {
            if (rootNode) {
                RenderNode(pass, scene, *rootNode, viewProj, glm::mat4(1.0f), cameraPos, wireframe, frustum);
            }
        }
    }
}

void PipelineManager::RenderNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, const glm::vec3& cameraPos, bool wireframe, const core::Frustum& frustum) {
    glm::mat4 worldMat = parentTransform * node.localTransform;

    if (node.worldAABB.minBound.x <= node.worldAABB.maxBound.x && !frustum.IntersectsAABB(node.worldAABB)) {
        return; // Hierarchical frustum culling
    }

    lastVisiblePrimitiveCount_ += node.primitives.size();
    for (auto& prim : node.primitives) {
        RenderPrimitive(pass, scene, prim, worldMat, viewProj, cameraPos, wireframe);
    }

    for (const auto& child : node.children) {
        if (child) {
            RenderNode(pass, scene, *child, viewProj, worldMat, cameraPos, wireframe, frustum);
        }
    }
}

void PipelineManager::RenderPrimitive(WGPURenderPassEncoder pass, scene::Scene& scene, scene::MeshPrimitive& prim, const glm::mat4& worldMat, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe) {
    if (!prim.gpuVertexBuffer || !prim.gpuBindGroup || !prim.gpuUniformBuffer) return;

    PrimitiveUniforms uniforms;
    uniforms.viewProj = viewProj;
    uniforms.model = worldMat;
    uniforms.cameraPos = glm::vec4(cameraPos, 1.0f);
    uniforms.lightDir = glm::normalize(glm::vec4(0.5f, 1.0f, 0.8f, 0.0f));

    if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
        const auto& mat = scene.materials[prim.materialIndex];
        uniforms.baseColorFactor = mat.baseColorFactor;
        uniforms.metallicFactor = mat.metallicFactor;
        uniforms.roughnessFactor = mat.roughnessFactor;
        uniforms.normalScale = mat.normalScale;
        uniforms.occlusionStrength = mat.occlusionStrength;
        uniforms.emissiveFactor = glm::vec4(mat.emissiveFactor, mat.emissiveStrength);
        uniforms.alphaCutoff = mat.alphaCutoff;
        uniforms.alphaMode = static_cast<uint32_t>(mat.alphaMode);
        uniforms.ior = mat.ior;
        uniforms.clearcoatFactor = mat.clearcoatFactor;
        uniforms.clearcoatRoughness = mat.clearcoatRoughnessFactor;
        uniforms.transmissionFactor = mat.transmissionFactor;
        uniforms.specularFactor = mat.specularFactor;
        uniforms.sheenColorFactor = glm::vec4(mat.sheenColorFactor, mat.sheenRoughnessFactor);
        uniforms.specularColorFactor = glm::vec4(mat.specularColorFactor, 0.0f);

        uint32_t flags = 0;
        if (mat.baseColorTextureIndex >= 0) flags |= MaterialFlags::HasBaseColorTex;
        if (mat.metallicRoughnessTextureIndex >= 0) flags |= MaterialFlags::HasMetallicRoughnessTex;
        if (mat.normalTextureIndex >= 0) flags |= MaterialFlags::HasNormalTex;
        if (mat.emissiveTextureIndex >= 0) flags |= MaterialFlags::HasEmissiveTex;
        if (mat.occlusionTextureIndex >= 0) flags |= MaterialFlags::HasOcclusionTex;
        if (mat.clearcoatTextureIndex >= 0) flags |= MaterialFlags::HasClearcoatTex;
        if (mat.transmissionTextureIndex >= 0) flags |= MaterialFlags::HasTransmissionTex;
        if (mat.sheenColorTextureIndex >= 0) flags |= MaterialFlags::HasSheenTex;
        if (mat.unlit) flags |= MaterialFlags::Unlit;
        if (mat.doubleSided) flags |= MaterialFlags::DoubleSided;
        uniforms.flags = flags;
    }

    wgpuQueueWriteBuffer(queue_, prim.gpuUniformBuffer, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetBindGroup(pass, 0, prim.gpuBindGroup, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, prim.gpuVertexBuffer, 0, prim.vertices.size() * sizeof(core::Vertex));

    if (prim.gpuIndexBuffer && !prim.indices.empty()) {
        wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuIndexBuffer, WGPUIndexFormat_Uint32, 0, prim.indices.size() * sizeof(uint32_t));
        wgpuRenderPassEncoderDrawIndexed(pass, static_cast<uint32_t>(prim.indices.size()), 1, 0, 0, 0);
    } else {
        wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(prim.vertices.size()), 1, 0, 0);
    }
}

void PipelineManager::RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj, const glm::vec4& color) {
    if (!pass || !aabbPipeline_ || !aabbVertexBuffer_ || !aabbUniformBuffer_ || !aabbBindGroup_) return;
    if (aabb.minBound.x > aabb.maxBound.x) return;

    glm::vec3 minB = aabb.minBound;
    glm::vec3 size = glm::max(aabb.maxBound - aabb.minBound, glm::vec3(0.001f));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), minB) * glm::scale(glm::mat4(1.0f), size);

    AabbUniforms uniforms;
    uniforms.viewProj = viewProj;
    uniforms.model = model;
    uniforms.color = color;

    wgpuQueueWriteBuffer(queue_, aabbUniformBuffer_, 0, &uniforms, sizeof(uniforms));

    static constexpr size_t kVertexSize = 24 * 3 * sizeof(float);
    wgpuRenderPassEncoderSetPipeline(pass, aabbPipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, aabbBindGroup_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, aabbVertexBuffer_, 0, kVertexSize);
    wgpuRenderPassEncoderDraw(pass, 24, 1, 0, 0);
}

void PipelineManager::RenderSceneAABBs(WGPURenderPassEncoder pass, const scene::Scene& scene, const glm::mat4& viewProj, bool showSceneAABB, bool showPrimitiveAABBs) {
    if (!pass || !aabbPipeline_) return;

    if (showSceneAABB && scene.sceneAABB.minBound.x <= scene.sceneAABB.maxBound.x) {
        // Bright Gold for root Scene AABB
        RenderAABB(pass, scene.sceneAABB, viewProj, glm::vec4(1.0f, 0.84f, 0.0f, 1.0f));
    }

    if (showPrimitiveAABBs) {
        auto renderNodeAABBs = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform) -> void {
            glm::mat4 worldMat = parentTransform * node.localTransform;
            for (const auto& prim : node.primitives) {
                if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                    core::AABB primWorldAABB;
                    glm::vec3 c = prim.boundingBox.Center();
                    glm::vec3 e = prim.boundingBox.Extents();
                    glm::vec3 corners[8] = {
                        c + glm::vec3(-e.x, -e.y, -e.z),
                        c + glm::vec3( e.x, -e.y, -e.z),
                        c + glm::vec3(-e.x,  e.y, -e.z),
                        c + glm::vec3( e.x,  e.y, -e.z),
                        c + glm::vec3(-e.x, -e.y,  e.z),
                        c + glm::vec3( e.x, -e.y,  e.z),
                        c + glm::vec3(-e.x,  e.y,  e.z),
                        c + glm::vec3( e.x,  e.y,  e.z)
                    };
                    for (const auto& corner : corners) {
                        glm::vec4 wc = worldMat * glm::vec4(corner, 1.0f);
                        primWorldAABB.Expand(glm::vec3(wc));
                    }
                    // Vivid Cyan for mesh primitives
                    RenderAABB(pass, primWorldAABB, viewProj, glm::vec4(0.2f, 0.85f, 1.0f, 0.8f));
                }
            }
            for (const auto& child : node.children) {
                if (child) self(self, *child, worldMat);
            }
        };

        for (const auto& rootNode : scene.rootNodes) {
            if (rootNode) renderNodeAABBs(renderNodeAABBs, *rootNode, glm::mat4(1.0f));
        }
    }
}

void PipelineManager::RenderQuadtreeBounds(WGPURenderPassEncoder pass, const scene::Quadtree& quadtree, const glm::mat4& viewProj) {
    if (!pass || !aabbPipeline_) return;

    std::vector<core::AABB> bounds;
    quadtree.GetNodeBounds(bounds);
    for (const auto& b : bounds) {
        // Emerald Green for Quadtree spatial partitions
        RenderAABB(pass, b, viewProj, glm::vec4(0.25f, 0.95f, 0.45f, 0.7f));
    }
}

} // namespace bgl::gpu
