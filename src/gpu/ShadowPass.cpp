/**
 * @file ShadowPass.cpp
 * @brief Implementation of directional 2048x2048 PCF shadow map depth pre-pass with alpha testing.
 */
#include "gpu/ShadowPass.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/DrawHelpers.hpp"
#include "gpu/Handle.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

namespace bgl::gpu {

ShadowPass::~ShadowPass() {
    Shutdown();
}

ShadowPass::ShadowPass(ShadowPass&& other) noexcept {
    MoveFrom(other);
}

ShadowPass& ShadowPass::operator=(ShadowPass&& other) noexcept {
    if (this != &other) {
        Shutdown();
        MoveFrom(other);
    }
    return *this;
}

void ShadowPass::MoveFrom(ShadowPass& other) noexcept {
    device_ = std::exchange(other.device_, nullptr);
    queue_ = std::exchange(other.queue_, nullptr);
    shadowPipeline_ = std::move(other.shadowPipeline_);
    pipelineLayout_ = std::move(other.pipelineLayout_);
    shadowTexture_ = std::move(other.shadowTexture_);
    shadowTextureView_ = std::move(other.shadowTextureView_);
    shadowSampler_ = std::move(other.shadowSampler_);
    dummySkinBindGroup_ = std::exchange(other.dummySkinBindGroup_, nullptr);
    perDrawRing_ = std::move(other.perDrawRing_);
    instanceBuffer_ = std::move(other.instanceBuffer_);
    visibleItemsScratch_ = std::move(other.visibleItemsScratch_);
    drawItemsScratch_ = std::move(other.drawItemsScratch_);
    perDrawScratch_ = std::move(other.perDrawScratch_);
    instanceTransformsScratch_ = std::move(other.instanceTransformsScratch_);
    nodeInstanceStartScratch_ = std::move(other.nodeInstanceStartScratch_);
    lightViewProj_ = other.lightViewProj_;
    dirty_ = std::exchange(other.dirty_, true);
    cleared_ = std::exchange(other.cleared_, false);
    lastScene_ = std::exchange(other.lastScene_, nullptr);
    lastTransformVersion_ = std::exchange(other.lastTransformVersion_, std::numeric_limits<uint64_t>::max());
    lastLightDir_ = std::exchange(other.lastLightDir_, glm::vec3{0.0f});
}

void ShadowPass::Shutdown() noexcept {
    perDrawRing_.Shutdown();
    instanceBuffer_.Shutdown();
    drawItemsScratch_.clear();
    perDrawScratch_.clear();
    visibleItemsScratch_.clear();
    instanceTransformsScratch_.clear();
    nodeInstanceStartScratch_.clear();

    shadowSampler_.Reset();
    shadowTextureView_.Reset();
    if (shadowTexture_) {
        core::MemoryTracker::Instance().TrackTextureFree(static_cast<size_t>(kShadowMapSize) * kShadowMapSize * 4);
        shadowTexture_.Reset();
    }
    shadowPipeline_.Reset();
    pipelineLayout_.Reset();
    dummySkinBindGroup_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
    dirty_ = true;
    cleared_ = false;
    lastScene_ = nullptr;
    lastTransformVersion_ = std::numeric_limits<uint64_t>::max();
    lastLightDir_ = glm::vec3{0.0f};
}

core::Result<void> ShadowPass::Initialize(WGPUDevice device, WGPUQueue queue, WGPUBindGroupLayout perDrawBgl,
                                          WGPUBindGroupLayout materialBgl, WGPUBindGroupLayout skinBgl,
                                          WGPUBindGroup dummySkinBindGroup) {
    if (!device || !queue || !perDrawBgl || !materialBgl || !skinBgl || !dummySkinBindGroup) {
        return std::unexpected(
            "Cannot initialize ShadowPass with null WebGPU device, queue, or shared bind group layouts");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;
    dummySkinBindGroup_ = dummySkinBindGroup;

    // 1. Create 2048x2048 Depth32Float Shadow Texture
    WGPUTextureDescriptor shadowTexDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    shadowTexDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    shadowTexDesc.dimension = WGPUTextureDimension_2D;
    shadowTexDesc.size = WGPUExtent3D{kShadowMapSize, kShadowMapSize, 1};
    shadowTexDesc.format = WGPUTextureFormat_Depth32Float;
    shadowTexDesc.mipLevelCount = 1;
    shadowTexDesc.sampleCount = 1;
    shadowTexDesc.label = ToWgpuStringView("DirectionalShadowMap");
    shadowTexture_ = wgpuDeviceCreateTexture(device_, &shadowTexDesc);

    if (!shadowTexture_) {
        Shutdown();
        return std::unexpected("Failed to allocate 2048x2048 Depth32Float shadow map texture");
    }
    core::MemoryTracker::Instance().TrackTextureAlloc(static_cast<size_t>(kShadowMapSize) * kShadowMapSize * 4);

    WGPUTextureViewDescriptor shadowViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    shadowViewDesc.format = WGPUTextureFormat_Depth32Float;
    shadowViewDesc.dimension = WGPUTextureViewDimension_2D;
    shadowViewDesc.baseMipLevel = 0;
    shadowViewDesc.mipLevelCount = 1;
    shadowViewDesc.baseArrayLayer = 0;
    shadowViewDesc.arrayLayerCount = 1;
    shadowViewDesc.aspect = WGPUTextureAspect_DepthOnly;
    shadowViewDesc.label = ToWgpuStringView("DirectionalShadowMapView");
    shadowTextureView_ = wgpuTextureCreateView(shadowTexture_, &shadowViewDesc);
    if (!shadowTextureView_) {
        Shutdown();
        return std::unexpected("Failed to create directional shadow map texture view");
    }

    // 2. Hardware PCF Comparison Sampler
    WGPUSamplerDescriptor compSamplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    compSamplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    compSamplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    compSamplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    compSamplerDesc.magFilter = WGPUFilterMode_Linear;
    compSamplerDesc.minFilter = WGPUFilterMode_Linear;
    compSamplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    compSamplerDesc.compare = WGPUCompareFunction_LessEqual;
    compSamplerDesc.label = ToWgpuStringView("ShadowPcfComparisonSampler");
    shadowSampler_ = wgpuDeviceCreateSampler(device_, &compSamplerDesc);
    if (!shadowSampler_) {
        Shutdown();
        return std::unexpected("Failed to create PCF shadow comparison sampler");
    }

    // 3. Shared 3-Group Pipeline Layout (0: PerDrawDynamic, 1: MaterialStatic, 2: SkinStorage)
    const std::array<WGPUBindGroupLayout, 3> layouts{perDrawBgl, materialBgl, skinBgl};
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = layouts.size();
    plDesc.bindGroupLayouts = layouts.data();
    plDesc.label = ToWgpuStringView("Shadow_PipelineLayout");
    pipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!pipelineLayout_) {
        Shutdown();
        return std::unexpected("Failed to create shadow pipeline layout");
    }

    if (auto res = instanceBuffer_.Initialize(device_, 256, "Shadow_InstanceBuffer"); !res) {
        Shutdown();
        return res;
    }

    if (auto res = CreatePipeline(false); !res) {
        Shutdown();
        return res;
    }

    perDrawRing_.Initialize(device_, queue_, perDrawBgl, "Shadow_PerDrawRing");

    // Initialize depth texture to 1.0 (unshadowed) so sampling is well-defined before first render.
    if (CommandEncoderHandle initEnc{wgpuDeviceCreateCommandEncoder(device_, nullptr)}) {
        Clear(initEnc.Get());
        if (CommandBufferHandle initCmd{wgpuCommandEncoderFinish(initEnc, nullptr)}) {
            WGPUCommandBuffer rawCmd{initCmd.Get()};
            wgpuQueueSubmit(queue_, 1, &rawCmd);
        }
    }

    BGL_LOG_INFO("ShadowPass initialized successfully (2048x2048 Depth32F PCF Pre-Pass, Dynamic PerDraw Ring)");
    return {};
}

glm::mat4 ShadowPass::ComputeDirectionalLightMatrix(const core::AABB& sceneBounds, const glm::vec3& lightDir) noexcept {
    const float dirLen{glm::length(lightDir)};
    const glm::vec3 d{(dirLen > 1e-4f) ? (lightDir / dirLen) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    if (std::abs(glm::dot(d, up)) > 0.99f) {
        up = glm::vec3{0.0f, 0.0f, 1.0f};
    }

    const bool validBounds{sceneBounds.IsValid()};
    if (!validBounds) {
        const glm::vec3 center{0.0f};
        constexpr float kDefaultRadius{20.0f};
        const glm::vec3 eye{center + d * (kDefaultRadius * 1.5f)};
        const glm::mat4 lightView{glm::lookAtRH(eye, center, up)};
        const glm::mat4 lightProj{glm::orthoRH_ZO(-kDefaultRadius, kDefaultRadius, -kDefaultRadius, kDefaultRadius,
                                                  0.1f, kDefaultRadius * 3.5f)};
        return lightProj * lightView;
    }

    const glm::vec3 center{sceneBounds.Center()};
    const float radius{std::max(glm::length(sceneBounds.Extents()), 5.0f)};
    const glm::vec3 eye{center + d * radius * 1.5f};
    const glm::mat4 lightView{glm::lookAtRH(eye, center, up)};

    glm::vec3 minLight{1e30f};
    glm::vec3 maxLight{-1e30f};
    const std::array<glm::vec3, 8> corners{{sceneBounds.minBound,
                                            {sceneBounds.maxBound.x, sceneBounds.minBound.y, sceneBounds.minBound.z},
                                            {sceneBounds.minBound.x, sceneBounds.maxBound.y, sceneBounds.minBound.z},
                                            {sceneBounds.maxBound.x, sceneBounds.maxBound.y, sceneBounds.minBound.z},
                                            {sceneBounds.minBound.x, sceneBounds.minBound.y, sceneBounds.maxBound.z},
                                            {sceneBounds.maxBound.x, sceneBounds.minBound.y, sceneBounds.maxBound.z},
                                            {sceneBounds.minBound.x, sceneBounds.maxBound.y, sceneBounds.maxBound.z},
                                            sceneBounds.maxBound}};

    for (const auto& corner : corners) {
        const glm::vec4 lightSpace{lightView * glm::vec4{corner, 1.0f}};
        minLight = glm::min(minLight, glm::vec3{lightSpace});
        maxLight = glm::max(maxLight, glm::vec3{lightSpace});
    }

    const float margin{radius * 0.15f};
    const glm::mat4 lightProj{glm::orthoRH_ZO(minLight.x - margin, maxLight.x + margin, minLight.y - margin,
                                              maxLight.y + margin, std::max(-maxLight.z - margin, 0.1f),
                                              -minLight.z + margin * 2.0f)};

    return lightProj * lightView;
}

void ShadowPass::Clear(WGPUCommandEncoder encoder) {
    if (!encoder || !shadowTextureView_ || cleared_) {
        return;
    }

    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depthAttachment.view = shadowTextureView_;
    depthAttachment.depthLoadOp = WGPULoadOp_Clear;
    depthAttachment.depthStoreOp = WGPUStoreOp_Store;
    depthAttachment.depthClearValue = 1.0f;

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.label = ToWgpuStringView("DirectionalShadowClearPass");
    passDesc.depthStencilAttachment = &depthAttachment;

    if (RenderPassEncoderHandle clearPass{wgpuCommandEncoderBeginRenderPass(encoder, &passDesc)}) {
        wgpuRenderPassEncoderEnd(clearPass);
    }
    cleared_ = true;
    dirty_ = true;
}

void ShadowPass::Render(WGPUCommandEncoder encoder, scene::Scene& scene, const glm::vec3& lightDir,
                        const scene::Quadtree* spatialIndex, uint64_t skinFrameId) {
    if (!encoder || !shadowPipeline_ || !shadowTextureView_) {
        return;
    }

    const float dirLen = glm::length(lightDir);
    const glm::vec3 safeDir = (dirLen > 1e-4f) ? (lightDir / dirLen) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f});
    lightViewProj_ = ComputeDirectionalLightMatrix(scene.sceneAABB, safeDir);

    if (!dirty_ && (!scene.animationPlaying || scene.activeAnimationIndex < 0) && lastScene_ == &scene &&
        lastTransformVersion_ == scene.transformVersion_ && glm::distance(lastLightDir_, safeDir) < 1e-4f) {
        return;
    }

    drawItemsScratch_.clear();
    perDrawScratch_.clear();
    instanceTransformsScratch_.clear();
    bool hasInstanced{false};

    const core::Frustum lightFrustum{core::Frustum::FromViewProjection(lightViewProj_)};
    const bool isAnimating{scene.animationPlaying && scene.activeAnimationIndex >= 0};

    auto pushPrimitiveDraw = [&](scene::SceneNode& node, scene::MeshPrimitive& prim, const glm::mat4& worldMat) {
        if (!prim.gpuVertexBuffer || !prim.gpuBindGroup) {
            return;
        }
        uint32_t instStart{0};
        uint32_t instCount{1};
        if (!node.instanceTransforms.empty()) {
            if (!hasInstanced) {
                nodeInstanceStartScratch_.clear();
                hasInstanced = true;
            }
            const auto [it, inserted] = nodeInstanceStartScratch_.try_emplace(
                &node, static_cast<uint32_t>(1u + instanceTransformsScratch_.size()));
            if (inserted) {
                instanceTransformsScratch_.insert(instanceTransformsScratch_.end(), node.instanceTransforms.begin(),
                                                  node.instanceTransforms.end());
            }
            instStart = it->second;
            instCount = static_cast<uint32_t>(node.instanceTransforms.size());
        }

        PreparedDrawBindings prepared{};
        PopulatePerDrawMorphAndSkin(queue_, prim, &node, worldMat, lightViewProj_, lightViewProj_, 0u, 0.0f,
                                    scene.skins, scene.allNodes, skinFrameId, dummySkinBindGroup_, prepared);
        const uint32_t dynOffset{static_cast<uint32_t>(perDrawScratch_.size() * sizeof(PerDrawUniforms))};
        perDrawScratch_.push_back(prepared.uniforms);
        drawItemsScratch_.push_back(ShadowDrawItem{
            .prim = &prim,
            .skinBindGroup = prepared.skinBindGroup,
            .dynamicOffset = dynOffset,
            .instanceStart = instStart,
            .instanceCount = instCount,
        });
    };

    if (spatialIndex && spatialIndex->GetNodeCount() > 0 && !isAnimating) {
        const glm::vec3 lightEye{scene.sceneAABB.Center() +
                                 safeDir * std::max(glm::length(scene.sceneAABB.Extents()), 5.0f) * 1.5f};
        spatialIndex->QueryVisible(lightFrustum, lightEye, visibleItemsScratch_, 1.0f);
        drawItemsScratch_.reserve(visibleItemsScratch_.size());
        perDrawScratch_.reserve(visibleItemsScratch_.size());

        for (const auto& vis : visibleItemsScratch_) {
            if (!vis.node) {
                continue;
            }
            auto prims = vis.node->GetPrimitives();
            if (vis.primitiveIndex >= prims.size()) {
                continue;
            }
            pushPrimitiveDraw(*vis.node, prims[vis.primitiveIndex], vis.node->worldTransform);
        }
    } else {
        std::unordered_set<const scene::SceneNode*> visitedNodes{};
        constexpr size_t kMaxTraversalDepth{256u};
        auto collectNodeShadow = [&](auto& self, scene::SceneNode& node, const glm::mat4& parentMat,
                                     size_t depth) -> void {
            if (depth >= kMaxTraversalDepth || !visitedNodes.insert(&node).second) {
                return;
            }
            const glm::mat4 worldMat{parentMat * node.localTransform};
            for (auto& prim : node.GetPrimitives()) {
                const core::AABB worldBox =
                    !node.instanceTransforms.empty() ? node.worldAABB : prim.boundingBox.Transformed(worldMat);
                if (worldBox.IsValid() && !lightFrustum.IntersectsAABB(worldBox)) {
                    continue;
                }
                pushPrimitiveDraw(node, prim, worldMat);
            }
            for (const auto& child : node.children) {
                if (child) {
                    self(self, *child, worldMat, depth + 1u);
                }
            }
        };

        for (const auto& root : scene.rootNodes) {
            if (root) {
                collectNodeShadow(collectNodeShadow, *root, glm::mat4{1.0f}, 0u);
            }
        }
    }

    WGPUBindGroup perDrawBg{nullptr};
    if (!perDrawScratch_.empty()) {
        perDrawBg = perDrawRing_.Upload(perDrawScratch_);
        instanceBuffer_.Upload(queue_, instanceTransformsScratch_);
        if (!perDrawBg || !instanceBuffer_.GetBuffer()) {
            drawItemsScratch_.clear();
        }
    }

    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depthAttachment.view = shadowTextureView_;
    depthAttachment.depthLoadOp = WGPULoadOp_Clear;
    depthAttachment.depthStoreOp = WGPUStoreOp_Store;
    depthAttachment.depthClearValue = 1.0f;

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.label = ToWgpuStringView("DirectionalShadowDepthPass");
    passDesc.depthStencilAttachment = &depthAttachment;

    RenderPassEncoderHandle pass{wgpuCommandEncoderBeginRenderPass(encoder, &passDesc)};
    if (!pass) {
        return;
    }

    WGPUBuffer instBuf{instanceBuffer_.GetBuffer()};
    if (!drawItemsScratch_.empty() && perDrawBg && instBuf) {
        wgpuRenderPassEncoderSetPipeline(pass.Get(), shadowPipeline_.Get());

        WGPUBindGroup boundMatBg{nullptr};
        WGPUBindGroup boundSkinBg{nullptr};
        WGPUBuffer boundVb{nullptr};
        WGPUBuffer boundIb{nullptr};
        uint32_t boundInstStart{std::numeric_limits<uint32_t>::max()};
        uint32_t boundInstCount{0};

        for (const auto& draw : drawItemsScratch_) {
            const scene::MeshPrimitive* prim = draw.prim;
            wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, perDrawBg, 1, &draw.dynamicOffset);
            if (prim->gpuBindGroup != boundMatBg) {
                wgpuRenderPassEncoderSetBindGroup(pass.Get(), 1, prim->gpuBindGroup, 0, nullptr);
                boundMatBg = prim->gpuBindGroup;
            }
            if (draw.skinBindGroup != boundSkinBg) {
                wgpuRenderPassEncoderSetBindGroup(pass.Get(), 2, draw.skinBindGroup, 0, nullptr);
                boundSkinBg = draw.skinBindGroup;
            }
            if (prim->gpuVertexBuffer != boundVb) {
                wgpuRenderPassEncoderSetVertexBuffer(pass.Get(), 0, prim->gpuVertexBuffer, 0,
                                                     prim->gpuVertexBufferSize);
                boundVb = prim->gpuVertexBuffer;
            }
            if (draw.instanceStart != boundInstStart || draw.instanceCount != boundInstCount) {
                wgpuRenderPassEncoderSetVertexBuffer(pass.Get(), 1, instBuf,
                                                     DynamicInstanceBuffer::GetByteOffset(draw.instanceStart),
                                                     DynamicInstanceBuffer::GetSliceSize(draw.instanceCount));
                boundInstStart = draw.instanceStart;
                boundInstCount = draw.instanceCount;
            }

            if (prim->gpuIndexBuffer && !prim->indices.empty()) {
                const auto slice = prim->GetLodSlice(1u);
                if (prim->gpuIndexBuffer != boundIb) {
                    wgpuRenderPassEncoderSetIndexBuffer(pass.Get(), prim->gpuIndexBuffer, prim->indexFormat, 0,
                                                        prim->gpuIndexBufferSize);
                    boundIb = prim->gpuIndexBuffer;
                }
                wgpuRenderPassEncoderDrawIndexed(pass.Get(), slice.indexCount, draw.instanceCount, slice.firstIndex, 0,
                                                 0);
            } else if (!prim->vertices.empty()) {
                wgpuRenderPassEncoderDraw(pass.Get(), static_cast<uint32_t>(prim->vertices.size()), draw.instanceCount,
                                          0, 0);
            }
        }
    }

    wgpuRenderPassEncoderEnd(pass.Get());

    dirty_ = false;
    cleared_ = false;
    lastScene_ = &scene;
    lastTransformVersion_ = scene.transformVersion_;
    lastLightDir_ = safeDir;
}

core::Result<void> ShadowPass::CreatePipeline(bool validateScope) {
    auto shaderModuleRes =
        ShaderManager::Instance().LoadAndCompile(device_, "shadow.wgsl", "Shadow_ShaderModule", validateScope);
    if (!shaderModuleRes) {
        return std::unexpected(shaderModuleRes.error());
    }

    VertexAndInstanceLayoutStorage layoutStorage;
    FillVertexAndInstanceLayouts(layoutStorage);

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth32Float;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_True;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;
    depthStencil.depthBias = 2;
    depthStencil.depthBiasSlopeScale = 2.0f;
    depthStencil.depthBiasClamp = 0.0f;

    WGPUFragmentState fragState = WGPU_FRAGMENT_STATE_INIT;
    fragState.module = shaderModuleRes->Get();
    fragState.entryPoint = ToWgpuStringView("fs_shadow");
    fragState.targetCount = 0;
    fragState.targets = nullptr;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.label = ToWgpuStringView("Shadow_RenderPipeline");
    rpDesc.layout = pipelineLayout_;
    rpDesc.vertex.module = shaderModuleRes->Get();
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_shadow");
    rpDesc.vertex.bufferCount = static_cast<uint32_t>(layoutStorage.layouts.size());
    rpDesc.vertex.buffers = layoutStorage.layouts.data();
    rpDesc.fragment = &fragState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;
    rpDesc.multisample.count = 1;
    rpDesc.multisample.mask = 0xFFFFFFFF;

    RenderPipelineHandle nextPipeline{wgpuDeviceCreateRenderPipeline(device_, &rpDesc)};
    if (!nextPipeline) {
        return std::unexpected("Failed to create shadow render pipeline");
    }

    shadowPipeline_ = std::move(nextPipeline);
    return {};
}

core::Result<void> ShadowPass::ReloadShader() {
    if (!device_ || !pipelineLayout_) {
        return std::unexpected("Cannot reload ShadowPass shader before initialization");
    }
    return CreatePipeline(true);
}

} // namespace bgl::gpu
