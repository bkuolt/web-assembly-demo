/**
 * @file SpriteRenderer.cpp
 * @brief Batched WebGPU implementation of SpriteRenderer for 2D sprites and 3D HDR billboards.
 */
#include "gpu/SpriteRenderer.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>

namespace bgl::gpu {

namespace {

[[nodiscard]] bool IsSrgbTextureFormat(WGPUTextureFormat fmt) noexcept {
    return fmt == WGPUTextureFormat_BGRA8UnormSrgb || fmt == WGPUTextureFormat_RGBA8UnormSrgb;
}

void AppendQuadIndices(std::vector<uint32_t>& idxs, uint32_t baseVertex) {
    idxs.push_back(baseVertex + 0u);
    idxs.push_back(baseVertex + 1u);
    idxs.push_back(baseVertex + 2u);
    idxs.push_back(baseVertex + 0u);
    idxs.push_back(baseVertex + 2u);
    idxs.push_back(baseVertex + 3u);
}

template <typename BatchVec>
void AppendCoalescedBatch(BatchVec& batches, WGPURenderPipeline pipe, uint64_t texKey, bool nearest,
                          uint32_t firstIndex) {
    if (!batches.empty()) {
        auto& back = batches.back();
        if (back.pipeline == pipe && back.textureKey == texKey && back.nearestFilter == nearest) {
            back.indexCount += 6u;
            return;
        }
    }
    batches.push_back(
        {.pipeline = pipe, .textureKey = texKey, .nearestFilter = nearest, .firstIndex = firstIndex, .indexCount = 6u});
}

} // namespace

void SpriteRenderer::PassBuffers::Release() noexcept {
    if (vbBytes > 0) {
        core::MemoryTracker::Instance().TrackBufferFree(vbBytes);
        vbBytes = 0;
    }
    if (ibBytes > 0) {
        core::MemoryTracker::Instance().TrackBufferFree(ibBytes);
        ibBytes = 0;
    }
    if (uboTracked) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(SpriteUniforms));
        uboTracked = false;
    }
    uboBg.Reset();
    ubo.Reset();
    ib.Reset();
    vb.Reset();
    vertexCap = 0;
    indexCap = 0;
}

SpriteRenderer::~SpriteRenderer() {
    Shutdown();
}

SpriteRenderer::SpriteRenderer(SpriteRenderer&& other) noexcept
    : device_{std::exchange(other.device_, nullptr)}, queue_{std::exchange(other.queue_, nullptr)},
      hdrFormat_{other.hdrFormat_}, swapchainFormat_{other.swapchainFormat_}, hdrSampleCount_{other.hdrSampleCount_},
      depthFormat_{other.depthFormat_}, gpuInitialized_{std::exchange(other.gpuInitialized_, false)},
      uboBgl_{std::move(other.uboBgl_)}, texBgl_{std::move(other.texBgl_)},
      pipelineLayout_{std::move(other.pipelineLayout_)}, linearSampler_{std::move(other.linearSampler_)},
      nearestSampler_{std::move(other.nearestSampler_)}, hdrAlphaDepth_{std::move(other.hdrAlphaDepth_)},
      hdrAdditiveDepth_{std::move(other.hdrAdditiveDepth_)}, hdrAlphaNoDepth_{std::move(other.hdrAlphaNoDepth_)},
      hdrAdditiveNoDepth_{std::move(other.hdrAdditiveNoDepth_)}, swapAlpha_{std::move(other.swapAlpha_)},
      swapAdditive_{std::move(other.swapAdditive_)}, pass3d_{std::move(other.pass3d_)},
      pass2d_{std::move(other.pass2d_)}, textureCache_{std::move(other.textureCache_)},
      pendingSprites2D_{std::move(other.pendingSprites2D_)},
      pendingBillboards3D_{std::move(other.pendingBillboards3D_)}, scratchVerts_{std::move(other.scratchVerts_)},
      scratchIndices_{std::move(other.scratchIndices_)}, scratchBatches_{std::move(other.scratchBatches_)},
      last3DBatchCount_{other.last3DBatchCount_}, last2DBatchCount_{other.last2DBatchCount_} {}

SpriteRenderer& SpriteRenderer::operator=(SpriteRenderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        device_ = std::exchange(other.device_, nullptr);
        queue_ = std::exchange(other.queue_, nullptr);
        hdrFormat_ = other.hdrFormat_;
        swapchainFormat_ = other.swapchainFormat_;
        hdrSampleCount_ = other.hdrSampleCount_;
        depthFormat_ = other.depthFormat_;
        gpuInitialized_ = std::exchange(other.gpuInitialized_, false);
        uboBgl_ = std::move(other.uboBgl_);
        texBgl_ = std::move(other.texBgl_);
        pipelineLayout_ = std::move(other.pipelineLayout_);
        linearSampler_ = std::move(other.linearSampler_);
        nearestSampler_ = std::move(other.nearestSampler_);
        hdrAlphaDepth_ = std::move(other.hdrAlphaDepth_);
        hdrAdditiveDepth_ = std::move(other.hdrAdditiveDepth_);
        hdrAlphaNoDepth_ = std::move(other.hdrAlphaNoDepth_);
        hdrAdditiveNoDepth_ = std::move(other.hdrAdditiveNoDepth_);
        swapAlpha_ = std::move(other.swapAlpha_);
        swapAdditive_ = std::move(other.swapAdditive_);
        pass3d_ = std::move(other.pass3d_);
        pass2d_ = std::move(other.pass2d_);
        textureCache_ = std::move(other.textureCache_);
        pendingSprites2D_ = std::move(other.pendingSprites2D_);
        pendingBillboards3D_ = std::move(other.pendingBillboards3D_);
        scratchVerts_ = std::move(other.scratchVerts_);
        scratchIndices_ = std::move(other.scratchIndices_);
        scratchBatches_ = std::move(other.scratchBatches_);
        last3DBatchCount_ = other.last3DBatchCount_;
        last2DBatchCount_ = other.last2DBatchCount_;
    }
    return *this;
}

core::Result<void> SpriteRenderer::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat hdrFormat,
                                              WGPUTextureFormat swapchainFormat, uint32_t hdrSampleCount,
                                              WGPUTextureFormat depthFormat) {
    Shutdown();
    if (!device || !queue) {
        return std::unexpected("SpriteRenderer requires valid WebGPU device and queue");
    }

    device_ = device;
    queue_ = queue;
    hdrFormat_ = hdrFormat;
    swapchainFormat_ = swapchainFormat;
    hdrSampleCount_ = std::max(1u, hdrSampleCount);
    depthFormat_ = depthFormat;

    // @group(0): SpriteUniforms
    WGPUBindGroupLayoutEntry uboEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    uboEntry.binding = 0;
    uboEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    uboEntry.buffer.type = WGPUBufferBindingType_Uniform;
    uboEntry.buffer.minBindingSize = sizeof(SpriteUniforms);

    WGPUBindGroupLayoutDescriptor uboBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    uboBglDesc.entryCount = 1;
    uboBglDesc.entries = &uboEntry;
    uboBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &uboBglDesc);

    // @group(1): Sampler + Texture2D
    std::array<WGPUBindGroupLayoutEntry, 2> texEntries{};
    texEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    texEntries[0].binding = 0;
    texEntries[0].visibility = WGPUShaderStage_Fragment;
    texEntries[0].sampler.type = WGPUSamplerBindingType_Filtering;

    texEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    texEntries[1].binding = 1;
    texEntries[1].visibility = WGPUShaderStage_Fragment;
    texEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
    texEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor texBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    texBglDesc.entryCount = texEntries.size();
    texBglDesc.entries = texEntries.data();
    texBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &texBglDesc);

    std::array<WGPUBindGroupLayout, 2> bgls{uboBgl_.Get(), texBgl_.Get()};
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = bgls.size();
    plDesc.bindGroupLayouts = bgls.data();
    pipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);

    WGPUSamplerDescriptor linDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    linDesc.minFilter = WGPUFilterMode_Linear;
    linDesc.magFilter = WGPUFilterMode_Linear;
    linDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    linDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    linDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    linDesc.maxAnisotropy = 1;
    linearSampler_ = wgpuDeviceCreateSampler(device_, &linDesc);

    WGPUSamplerDescriptor nearDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    nearDesc.minFilter = WGPUFilterMode_Nearest;
    nearDesc.magFilter = WGPUFilterMode_Nearest;
    nearDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    nearDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    nearDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    nearDesc.maxAnisotropy = 1;
    nearestSampler_ = wgpuDeviceCreateSampler(device_, &nearDesc);

    if (!uboBgl_ || !texBgl_ || !pipelineLayout_ || !linearSampler_ || !nearestSampler_) {
        Shutdown();
        return std::unexpected("Failed to create SpriteRenderer layouts or samplers");
    }

    if (auto res = CreatePassUniformBuffer(pass3d_, "Sprite3DUbo"); !res) {
        Shutdown();
        return res;
    }
    if (auto res = CreatePassUniformBuffer(pass2d_, "Sprite2DUbo"); !res) {
        Shutdown();
        return res;
    }

    auto shaderRes = ReloadShader();
    if (!shaderRes) {
        Shutdown();
        return shaderRes;
    }

    // Seed built-in 1x1 white fallback texture under ID 0
    (void)ResolveTextureBindGroup(nullptr);

    gpuInitialized_ = true;
    BGL_LOG_INFO("SpriteRenderer initialized cleanly (HDR MSAA={}x, Swapchain={})", hdrSampleCount_,
                 static_cast<int>(swapchainFormat_));
    return {};
}

core::Result<void> SpriteRenderer::CreatePassUniformBuffer(PassBuffers& pass, const char* label) {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = sizeof(SpriteUniforms);
    desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    desc.label = ToWgpuStringView(label);
    pass.ubo = wgpuDeviceCreateBuffer(device_, &desc);
    if (!pass.ubo) {
        return std::unexpected("Failed to create SpriteRenderer uniform buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(SpriteUniforms));
    pass.uboTracked = true;

    WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntry.binding = 0;
    bgEntry.buffer = pass.ubo.Get();
    bgEntry.offset = 0;
    bgEntry.size = sizeof(SpriteUniforms);

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = uboBgl_.Get();
    bgDesc.entryCount = 1;
    bgDesc.entries = &bgEntry;
    pass.uboBg = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    if (!pass.uboBg) {
        return std::unexpected("Failed to create SpriteRenderer uniform bind group");
    }
    return {};
}

core::Result<void> SpriteRenderer::ReloadShader() {
    if (!device_ || !pipelineLayout_) {
        return std::unexpected("Cannot reload sprite shader before SpriteRenderer initialization");
    }
    auto smRes = ShaderManager::Instance().LoadAndCompile(device_, "sprite.wgsl", "SpriteRendererWGSL");
    if (!smRes) {
        return std::unexpected(smRes.error());
    }
    return BuildAllPipelines(smRes->Get());
}

RenderPipelineHandle SpriteRenderer::CreateSinglePipeline(WGPUShaderModule shaderModule, WGPUTextureFormat colorFormat,
                                                          uint32_t sampleCount, bool useDepth, bool depthTest,
                                                          core::SpriteBlendMode blendMode, const char* label) {
    std::array<WGPUVertexAttribute, 3> attrs{};
    attrs[0].format = WGPUVertexFormat_Float32x3;
    attrs[0].offset = offsetof(SpriteVertex, pos);
    attrs[0].shaderLocation = 0;

    attrs[1].format = WGPUVertexFormat_Float32x2;
    attrs[1].offset = offsetof(SpriteVertex, uv);
    attrs[1].shaderLocation = 1;

    attrs[2].format = WGPUVertexFormat_Float32x4;
    attrs[2].offset = offsetof(SpriteVertex, color);
    attrs[2].shaderLocation = 2;

    WGPUVertexBufferLayout vbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbLayout.arrayStride = sizeof(SpriteVertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = attrs.size();
    vbLayout.attributes = attrs.data();

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor =
        (blendMode == core::SpriteBlendMode::Additive) ? WGPUBlendFactor_One : WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor =
        (blendMode == core::SpriteBlendMode::Additive) ? WGPUBlendFactor_One : WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = colorFormat;
    target.blend = &blend;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT;
    frag.module = shaderModule;
    frag.entryPoint = ToWgpuStringView("fs_sprite");
    frag.targetCount = 1;
    frag.targets = &target;

    WGPUDepthStencilState depthState = WGPU_DEPTH_STENCIL_STATE_INIT;
    if (useDepth) {
        depthState.format = depthFormat_;
        depthState.depthWriteEnabled = WGPUOptionalBool_False;
        depthState.depthCompare = depthTest ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Always;
    }

    WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rp.label = ToWgpuStringView(label);
    rp.layout = pipelineLayout_.Get();
    rp.vertex.module = shaderModule;
    rp.vertex.entryPoint = ToWgpuStringView("vs_sprite");
    rp.vertex.bufferCount = 1;
    rp.vertex.buffers = &vbLayout;
    rp.fragment = &frag;
    rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rp.primitive.cullMode = WGPUCullMode_None;
    rp.depthStencil = useDepth ? &depthState : nullptr;
    rp.multisample.count = sampleCount;
    rp.multisample.mask = ~0u;

    return RenderPipelineHandle{wgpuDeviceCreateRenderPipeline(device_, &rp)};
}

core::Result<void> SpriteRenderer::BuildAllPipelines(WGPUShaderModule shaderModule) {
    auto pHdrAlphaDepth = CreateSinglePipeline(shaderModule, hdrFormat_, hdrSampleCount_, true, true,
                                               core::SpriteBlendMode::Alpha, "Sprite_Hdr_Alpha_Depth");
    auto pHdrAddDepth = CreateSinglePipeline(shaderModule, hdrFormat_, hdrSampleCount_, true, true,
                                             core::SpriteBlendMode::Additive, "Sprite_Hdr_Add_Depth");
    auto pHdrAlphaNoDepth = CreateSinglePipeline(shaderModule, hdrFormat_, hdrSampleCount_, true, false,
                                                 core::SpriteBlendMode::Alpha, "Sprite_Hdr_Alpha_NoDepth");
    auto pHdrAddNoDepth = CreateSinglePipeline(shaderModule, hdrFormat_, hdrSampleCount_, true, false,
                                               core::SpriteBlendMode::Additive, "Sprite_Hdr_Add_NoDepth");
    auto pSwapAlpha = CreateSinglePipeline(shaderModule, swapchainFormat_, 1u, false, false,
                                           core::SpriteBlendMode::Alpha, "Sprite_Swap_Alpha");
    auto pSwapAdd = CreateSinglePipeline(shaderModule, swapchainFormat_, 1u, false, false,
                                         core::SpriteBlendMode::Additive, "Sprite_Swap_Add");

    if (!pHdrAlphaDepth || !pHdrAddDepth || !pHdrAlphaNoDepth || !pHdrAddNoDepth || !pSwapAlpha || !pSwapAdd) {
        return std::unexpected("Failed to create one or more SpriteRenderer WebGPU pipelines");
    }

    hdrAlphaDepth_ = std::move(pHdrAlphaDepth);
    hdrAdditiveDepth_ = std::move(pHdrAddDepth);
    hdrAlphaNoDepth_ = std::move(pHdrAlphaNoDepth);
    hdrAdditiveNoDepth_ = std::move(pHdrAddNoDepth);
    swapAlpha_ = std::move(pSwapAlpha);
    swapAdditive_ = std::move(pSwapAdd);
    return {};
}

void SpriteRenderer::Shutdown() noexcept {
    pass3d_.Release();
    pass2d_.Release();

    for (auto& [id, entry] : textureCache_) {
        if (entry.trackedBytes > 0) {
            core::MemoryTracker::Instance().TrackTextureFree(entry.trackedBytes);
        }
    }
    textureCache_.clear();

    hdrAlphaDepth_.Reset();
    hdrAdditiveDepth_.Reset();
    hdrAlphaNoDepth_.Reset();
    hdrAdditiveNoDepth_.Reset();
    swapAlpha_.Reset();
    swapAdditive_.Reset();
    nearestSampler_.Reset();
    linearSampler_.Reset();
    pipelineLayout_.Reset();
    texBgl_.Reset();
    uboBgl_.Reset();

    pendingSprites2D_.clear();
    pendingBillboards3D_.clear();
    scratchVerts_.clear();
    scratchIndices_.clear();
    scratchBatches_.clear();
    last3DBatchCount_ = 0;
    last2DBatchCount_ = 0;
    gpuInitialized_ = false;
    device_ = nullptr;
    queue_ = nullptr;
}

void SpriteRenderer::SubmitSprite(const core::Sprite& sprite) {
    if (!sprite.visible || sprite.color.a <= 0.001f || sprite.size.x <= 0.0f || sprite.size.y <= 0.0f) {
        return;
    }
    pendingSprites2D_.push_back(sprite);
}

void SpriteRenderer::SubmitBillboard(const core::Billboard& billboard) {
    if (!billboard.visible || billboard.color.a <= 0.001f || billboard.size.x <= 0.0f || billboard.size.y <= 0.0f) {
        return;
    }
    pendingBillboards3D_.push_back(billboard);
}

void SpriteRenderer::BuildSpriteQuad(const core::Sprite& sprite, std::vector<SpriteVertex>& outVerts) {
    const float lx0 = -sprite.anchor.x * sprite.size.x;
    const float lx1 = (1.0f - sprite.anchor.x) * sprite.size.x;
    const float ly0 = -sprite.anchor.y * sprite.size.y;
    const float ly1 = (1.0f - sprite.anchor.y) * sprite.size.y;

    const float c = std::cos(sprite.rotationRad);
    const float s = std::sin(sprite.rotationRad);
    auto transformPt = [&](float lx, float ly) -> glm::vec3 {
        return glm::vec3{sprite.position.x + (lx * c - ly * s), sprite.position.y + (lx * s + ly * c), 0.0f};
    };

    glm::vec2 uvMin{};
    glm::vec2 uvMax{};
    sprite.ComputeEffectiveUVs(uvMin, uvMax);

    outVerts.push_back(SpriteVertex{transformPt(lx0, ly0), {uvMin.x, uvMin.y}, sprite.color});
    outVerts.push_back(SpriteVertex{transformPt(lx1, ly0), {uvMax.x, uvMin.y}, sprite.color});
    outVerts.push_back(SpriteVertex{transformPt(lx1, ly1), {uvMax.x, uvMax.y}, sprite.color});
    outVerts.push_back(SpriteVertex{transformPt(lx0, ly1), {uvMin.x, uvMax.y}, sprite.color});
}

void SpriteRenderer::BuildBillboardQuad(const core::Billboard& billboard, const glm::vec3& cameraPos,
                                        const glm::mat4& viewMatrix, std::vector<SpriteVertex>& outVerts) {
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    billboard.ComputeBasis(cameraPos, viewMatrix, right, up);

    const float minX = -billboard.anchor.x * billboard.size.x;
    const float maxX = (1.0f - billboard.anchor.x) * billboard.size.x;
    const float minY = -(1.0f - billboard.anchor.y) * billboard.size.y;
    const float maxY = billboard.anchor.y * billboard.size.y;

    glm::vec2 uvMin{};
    glm::vec2 uvMax{};
    billboard.ComputeEffectiveUVs(uvMin, uvMax);

    const glm::vec3& p = billboard.position;
    outVerts.push_back(SpriteVertex{p + right * minX + up * maxY, {uvMin.x, uvMin.y}, billboard.color});
    outVerts.push_back(SpriteVertex{p + right * maxX + up * maxY, {uvMax.x, uvMin.y}, billboard.color});
    outVerts.push_back(SpriteVertex{p + right * maxX + up * minY, {uvMax.x, uvMax.y}, billboard.color});
    outVerts.push_back(SpriteVertex{p + right * minX + up * minY, {uvMin.x, uvMax.y}, billboard.color});
}

WGPUBindGroup SpriteRenderer::ResolveTextureBindGroup(const std::shared_ptr<const core::SpriteTexture>& tex) {
    const uint64_t key = (tex && tex->IsValid()) ? tex->id : 0ULL;
    const bool nearest = tex ? tex->nearestFilter : true;

    if (const auto it = textureCache_.find(key); it != textureCache_.end()) {
        if (it->second.bindGroup && (key == 0ULL || it->second.nearestFilter == nearest)) {
            return it->second.bindGroup.Get();
        }
    }

    CachedTexture entry;
    uint32_t w = 1;
    uint32_t h = 1;
    WGPUTextureFormat fmt = WGPUTextureFormat_RGBA8Unorm;
    std::array<uint8_t, 4> whitePx{255, 255, 255, 255};
    const uint8_t* pxPtr = whitePx.data();
    size_t pxSize = whitePx.size();

    if (key != 0ULL) {
        w = tex->width;
        h = tex->height;
        fmt = tex->srgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm;
        pxPtr = tex->rgbaPixels.data();
        pxSize = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        entry.owner = tex;
    }

    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = WGPUExtent3D{w, h, 1};
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = WGPUTextureDimension_2D;
    td.format = fmt;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    entry.texture = wgpuDeviceCreateTexture(device_, &td);
    if (!entry.texture) {
        return nullptr;
    }

    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = entry.texture.Get();
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = w * 4u;
    layout.rowsPerImage = h;
    const WGPUExtent3D writeExtent{w, h, 1};
    wgpuQueueWriteTexture(queue_, &dst, pxPtr, pxSize, &layout, &writeExtent);

    entry.view = wgpuTextureCreateView(entry.texture.Get(), nullptr);
    entry.nearestFilter = nearest;
    entry.trackedBytes = pxSize;
    core::MemoryTracker::Instance().TrackTextureAlloc(entry.trackedBytes);

    std::array<WGPUBindGroupEntry, 2> bgEntries{};
    bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[0].binding = 0;
    bgEntries[0].sampler = nearest ? nearestSampler_.Get() : linearSampler_.Get();

    bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[1].binding = 1;
    bgEntries[1].textureView = entry.view.Get();

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = texBgl_.Get();
    bgDesc.entryCount = bgEntries.size();
    bgDesc.entries = bgEntries.data();
    entry.bindGroup = wgpuDeviceCreateBindGroup(device_, &bgDesc);

    auto& stored = textureCache_[key];
    if (stored.trackedBytes > 0) {
        core::MemoryTracker::Instance().TrackTextureFree(stored.trackedBytes);
    }
    stored = std::move(entry);
    return stored.bindGroup.Get();
}

bool SpriteRenderer::EnsurePassGeometryCapacity(PassBuffers& pass, size_t vertexCount, size_t indexCount) {
    if (vertexCount > pass.vertexCap) {
        const size_t nextCap = std::max(vertexCount * 2u, size_t{256});
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = nextCap * sizeof(SpriteVertex);
        desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        desc.label = ToWgpuStringView("SpriteVertexBuffer");
        BufferHandle nextVb{wgpuDeviceCreateBuffer(device_, &desc)};
        if (!nextVb) {
            return false;
        }
        if (pass.vbBytes > 0) {
            core::MemoryTracker::Instance().TrackBufferFree(pass.vbBytes);
        }
        pass.vb = std::move(nextVb);
        pass.vertexCap = nextCap;
        pass.vbBytes = desc.size;
        core::MemoryTracker::Instance().TrackBufferAlloc(pass.vbBytes);
    }
    if (indexCount > pass.indexCap) {
        const size_t nextCap = std::max(indexCount * 2u, size_t{384});
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = ((nextCap * sizeof(uint32_t)) + 3u) & ~size_t{3u};
        desc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        desc.label = ToWgpuStringView("SpriteIndexBuffer");
        BufferHandle nextIb{wgpuDeviceCreateBuffer(device_, &desc)};
        if (!nextIb) {
            return false;
        }
        if (pass.ibBytes > 0) {
            core::MemoryTracker::Instance().TrackBufferFree(pass.ibBytes);
        }
        pass.ib = std::move(nextIb);
        pass.indexCap = nextCap;
        pass.ibBytes = desc.size;
        core::MemoryTracker::Instance().TrackBufferAlloc(pass.ibBytes);
    }
    return true;
}

void SpriteRenderer::EmitBatches(WGPURenderPassEncoder pass, const PassBuffers& gpuBufs,
                                 const std::vector<SpriteVertex>& verts, const std::vector<uint32_t>& idxs,
                                 const SpriteUniforms& uboData, const std::vector<DrawBatch>& batches) {
    if (!pass || verts.empty() || idxs.empty() || batches.empty() || !gpuBufs.vb || !gpuBufs.ib || !gpuBufs.ubo ||
        !gpuBufs.uboBg) {
        return;
    }

    const size_t vBytes = verts.size() * sizeof(SpriteVertex);
    const size_t iBytes = (idxs.size() * sizeof(uint32_t) + 3u) & ~size_t{3u};
    wgpuQueueWriteBuffer(queue_, gpuBufs.ubo.Get(), 0, &uboData, sizeof(uboData));
    wgpuQueueWriteBuffer(queue_, gpuBufs.vb.Get(), 0, verts.data(), vBytes);
    wgpuQueueWriteBuffer(queue_, gpuBufs.ib.Get(), 0, idxs.data(), iBytes);

    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpuBufs.vb.Get(), 0, vBytes);
    wgpuRenderPassEncoderSetIndexBuffer(pass, gpuBufs.ib.Get(), WGPUIndexFormat_Uint32, 0, iBytes);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, gpuBufs.uboBg.Get(), 0, nullptr);

    WGPURenderPipeline boundPipeline = nullptr;
    WGPUBindGroup boundTexBg = nullptr;

    for (const auto& batch : batches) {
        if (!batch.pipeline || batch.indexCount == 0) {
            continue;
        }
        WGPUBindGroup texBg = nullptr;
        if (const auto it = textureCache_.find(batch.textureKey); it != textureCache_.end()) {
            texBg = it->second.bindGroup.Get();
        }
        if (!texBg) {
            continue;
        }
        if (batch.pipeline != boundPipeline) {
            wgpuRenderPassEncoderSetPipeline(pass, batch.pipeline);
            boundPipeline = batch.pipeline;
        }
        if (texBg != boundTexBg) {
            wgpuRenderPassEncoderSetBindGroup(pass, 1, texBg, 0, nullptr);
            boundTexBg = texBg;
        }
        wgpuRenderPassEncoderDrawIndexed(pass, batch.indexCount, 1, batch.firstIndex, 0, 0);
    }
}

void SpriteRenderer::Render3D(WGPURenderPassEncoder hdrTransparentPass, const glm::mat4& viewProj,
                              const glm::vec3& cameraPos, const glm::mat4& viewMatrix) {
    last3DBatchCount_ = 0;
    if (!gpuInitialized_ || !hdrTransparentPass || pendingBillboards3D_.empty()) {
        return;
    }

    std::stable_sort(pendingBillboards3D_.begin(), pendingBillboards3D_.end(),
                     [&cameraPos](const core::Billboard& a, const core::Billboard& b) {
                         if (a.depthTest != b.depthTest) {
                             return a.depthTest; // Depth-tested billboards first, non-depth markers after
                         }
                         const glm::vec3 da = a.position - cameraPos;
                         const glm::vec3 db = b.position - cameraPos;
                         return glm::dot(da, da) > glm::dot(db, db); // Back-to-front sorting
                     });

    scratchVerts_.clear();
    scratchIndices_.clear();
    scratchBatches_.clear();
    scratchVerts_.reserve(pendingBillboards3D_.size() * 4u);
    scratchIndices_.reserve(pendingBillboards3D_.size() * 6u);

    for (const auto& bb : pendingBillboards3D_) {
        (void)ResolveTextureBindGroup(bb.texture);
        const uint64_t texKey = (bb.texture && bb.texture->IsValid()) ? bb.texture->id : 0ULL;
        const bool nearest = bb.texture ? bb.texture->nearestFilter : true;

        WGPURenderPipeline pipe = nullptr;
        if (bb.depthTest) {
            pipe = (bb.blend == core::SpriteBlendMode::Additive) ? hdrAdditiveDepth_.Get() : hdrAlphaDepth_.Get();
        } else {
            pipe = (bb.blend == core::SpriteBlendMode::Additive) ? hdrAdditiveNoDepth_.Get() : hdrAlphaNoDepth_.Get();
        }

        const uint32_t firstIndex = static_cast<uint32_t>(scratchIndices_.size());
        const uint32_t baseVertex = static_cast<uint32_t>(scratchVerts_.size());
        BuildBillboardQuad(bb, cameraPos, viewMatrix, scratchVerts_);
        AppendQuadIndices(scratchIndices_, baseVertex);
        AppendCoalescedBatch(scratchBatches_, pipe, texKey, nearest, firstIndex);
    }

    if (!EnsurePassGeometryCapacity(pass3d_, scratchVerts_.size(), scratchIndices_.size())) {
        return;
    }

    SpriteUniforms ubo{};
    ubo.viewProj = viewProj;
    ubo.params = glm::vec4{0.0f, 0.0f, 0.0f, 0.0f}; // HDR target stays linear
    EmitBatches(hdrTransparentPass, pass3d_, scratchVerts_, scratchIndices_, ubo, scratchBatches_);
    last3DBatchCount_ = scratchBatches_.size();
}

void SpriteRenderer::Render2D(WGPURenderPassEncoder swapchainPass, uint32_t viewportWidth, uint32_t viewportHeight) {
    last2DBatchCount_ = 0;
    if (!gpuInitialized_ || !swapchainPass || pendingSprites2D_.empty()) {
        return;
    }

    std::stable_sort(pendingSprites2D_.begin(), pendingSprites2D_.end(),
                     [](const core::Sprite& a, const core::Sprite& b) {
                         if (a.layer != b.layer) {
                             return a.layer < b.layer;
                         }
                         if (a.blend != b.blend) {
                             return a.blend < b.blend;
                         }
                         const uint64_t ta = (a.texture && a.texture->IsValid()) ? a.texture->id : 0ULL;
                         const uint64_t tb = (b.texture && b.texture->IsValid()) ? b.texture->id : 0ULL;
                         return ta < tb;
                     });

    scratchVerts_.clear();
    scratchIndices_.clear();
    scratchBatches_.clear();
    scratchVerts_.reserve(pendingSprites2D_.size() * 4u);
    scratchIndices_.reserve(pendingSprites2D_.size() * 6u);

    for (const auto& sp : pendingSprites2D_) {
        (void)ResolveTextureBindGroup(sp.texture);
        const uint64_t texKey = (sp.texture && sp.texture->IsValid()) ? sp.texture->id : 0ULL;
        const bool nearest = sp.texture ? sp.texture->nearestFilter : true;
        WGPURenderPipeline pipe =
            (sp.blend == core::SpriteBlendMode::Additive) ? swapAdditive_.Get() : swapAlpha_.Get();

        const uint32_t firstIndex = static_cast<uint32_t>(scratchIndices_.size());
        const uint32_t baseVertex = static_cast<uint32_t>(scratchVerts_.size());
        BuildSpriteQuad(sp, scratchVerts_);
        AppendQuadIndices(scratchIndices_, baseVertex);
        AppendCoalescedBatch(scratchBatches_, pipe, texKey, nearest, firstIndex);
    }

    if (!EnsurePassGeometryCapacity(pass2d_, scratchVerts_.size(), scratchIndices_.size())) {
        return;
    }

    const float w = static_cast<float>(std::max(1u, viewportWidth));
    const float h = static_cast<float>(std::max(1u, viewportHeight));
    SpriteUniforms ubo{};
    ubo.viewProj = glm::ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f);
    ubo.params = glm::vec4{IsSrgbTextureFormat(swapchainFormat_) ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f};
    EmitBatches(swapchainPass, pass2d_, scratchVerts_, scratchIndices_, ubo, scratchBatches_);
    last2DBatchCount_ = scratchBatches_.size();
}

void SpriteRenderer::PruneExpiredTextures() noexcept {
    for (auto it = textureCache_.begin(); it != textureCache_.end();) {
        if (it->first != 0ULL && it->second.owner.expired()) {
            if (it->second.trackedBytes > 0) {
                core::MemoryTracker::Instance().TrackTextureFree(it->second.trackedBytes);
            }
            it = textureCache_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpriteRenderer::ResetFrame() noexcept {
    pendingSprites2D_.clear();
    pendingBillboards3D_.clear();
    PruneExpiredTextures();
}

} // namespace bgl::gpu
