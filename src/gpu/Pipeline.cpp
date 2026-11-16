/**
 * @file Pipeline.cpp
 * @brief Core PipelineManager lifecycle, main/tonemap/mipgen pipelines, and frame render encoding.
 */
#include "gpu/Pipeline.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/PipelineHelpers.hpp"
#include "gpu/ShaderManager.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
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
    outlinePerDrawRing_.Shutdown();
    outlineInstanceBuffer_.Shutdown();
    perDrawRing_.Shutdown();
    transparentPerDrawRing_.Shutdown();
    instanceBuffer_.Shutdown();

    outlineCompBindGroup_.Reset();
    selectionMaskView_.Reset();
    selectionMaskTexture_.Reset();
    selectionMaskWidth_ = 0;
    selectionMaskHeight_ = 0;
    if (outlineCompUniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(glm::vec4) * 3u);
        outlineCompUniformBuffer_.Reset();
    }
    outlineCompPipeline_.Reset();
    outlineCompPl_.Reset();
    outlineCompBgl_.Reset();
    outlineMaskPipeline_.Reset();

    mipGenHdrPipeline_.Reset();
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
    opaqueCopyMipCount_ = 1;

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
    outlinePerDrawRing_.Initialize(device_, queue_, perDrawBgl_.Get(), "Outline_PerDrawRing");
    if (auto ibRes = outlineInstanceBuffer_.Initialize(device_, 64, "Outline_InstanceBuffer"); !ibRes) {
        BGL_LOG_WARN("Selection outline instance buffer initialization note: {}", ibRes.error());
    }
    if (auto outRes = CreateSelectionOutlinePipelines(false); !outRes) {
        BGL_LOG_WARN("Selection outline initialization note: {}", outRes.error());
    }
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
    bool needOutline{false};

    for (const auto& path : modifiedVirtualPaths) {
        if (path == "pbr.wgsl" || path == "wireframe.wgsl") {
            needMain = true;
        } else if (path == "outline.wgsl" || path == "outline_comp.wgsl") {
            needOutline = true;
        } else if (path == "mipgen.wgsl") {
            needMipGen = true;
        } else if (path == "shadow.wgsl") {
            needShadow = true;
        } else if (path == "tonemap.wgsl" || path == "depth_linearize.wgsl" || path == "depth_linearize_1x.wgsl" ||
                   path == "gtao.wgsl" || path == "gtao_blur.wgsl" || path == "bloom.wgsl" ||
                   path == "auto_exposure.wgsl" || path == "fxaa.wgsl") {
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
    if (needOutline) {
        checkRes(CreateSelectionOutlinePipelines(true), "outline");
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
