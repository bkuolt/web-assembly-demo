/**
 * @file ShadowPass.cpp
 * @brief Implementation of directional 2048x2048 PCF shadow map depth pre-pass with alpha testing.
 */
#include "gpu/ShadowPass.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/Shaders.hpp"
#include <algorithm>
#include <array>
#include <cstring>

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
    shadowPipeline_ = std::exchange(other.shadowPipeline_, nullptr);
    shadowBindGroupLayout_ = std::exchange(other.shadowBindGroupLayout_, nullptr);
    shadowTexture_ = std::exchange(other.shadowTexture_, nullptr);
    shadowTextureView_ = std::exchange(other.shadowTextureView_, nullptr);
    shadowSampler_ = std::exchange(other.shadowSampler_, nullptr);
    defaultSampler_ = std::exchange(other.defaultSampler_, nullptr);
    defaultWhiteView_ = std::exchange(other.defaultWhiteView_, nullptr);
    lightViewProj_ = other.lightViewProj_;
    dirty_ = std::exchange(other.dirty_, true);
    cleared_ = std::exchange(other.cleared_, false);
    lastScene_ = std::exchange(other.lastScene_, nullptr);
    lastLightDir_ = std::exchange(other.lastLightDir_, glm::vec3{0.0f});
}

void ShadowPass::Shutdown() noexcept {
    if (shadowSampler_) {
        wgpuSamplerRelease(shadowSampler_);
        shadowSampler_ = nullptr;
    }
    if (shadowTextureView_) {
        wgpuTextureViewRelease(shadowTextureView_);
        shadowTextureView_ = nullptr;
    }
    if (shadowTexture_) {
        core::MemoryTracker::Instance().TrackTextureFree(kShadowMapSize * kShadowMapSize * 4);
        wgpuTextureRelease(shadowTexture_);
        shadowTexture_ = nullptr;
    }
    if (shadowPipeline_) {
        wgpuRenderPipelineRelease(shadowPipeline_);
        shadowPipeline_ = nullptr;
    }
    if (shadowBindGroupLayout_) {
        wgpuBindGroupLayoutRelease(shadowBindGroupLayout_);
        shadowBindGroupLayout_ = nullptr;
    }
    defaultSampler_ = nullptr;
    defaultWhiteView_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
    dirty_ = true;
    cleared_ = false;
    lastScene_ = nullptr;
    lastLightDir_ = glm::vec3{0.0f};
}

core::Result<void> ShadowPass::Initialize(WGPUDevice device, WGPUQueue queue, WGPUSampler defaultSampler,
                                          WGPUTextureView defaultWhiteView) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize ShadowPass with null WebGPU device or queue");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;
    defaultSampler_ = defaultSampler;
    defaultWhiteView_ = defaultWhiteView;

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
    core::MemoryTracker::Instance().TrackTextureAlloc(kShadowMapSize * kShadowMapSize * 4);

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

    // 3. Shadow WGSL Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWgpuStringView(kShadowShaderWGSL.data());

    WGPUShaderModuleDescriptor shaderDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDesc.nextInChain = &wgslDesc.chain;
    shaderDesc.label = ToWgpuStringView("Shadow_ShaderModule");

    WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(device_, &shaderDesc);
    if (!shaderModule) {
        Shutdown();
        return std::unexpected("Failed to compile shadow WGSL shader module");
    }

    // 4. Bind Group Layout (0: Uniforms, 1: Sampler, 2: Texture for Alpha Discard)
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(ShadowUniforms);

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
    bglDesc.label = ToWgpuStringView("Shadow_BindGroupLayout");
    shadowBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!shadowBindGroupLayout_) {
        wgpuShaderModuleRelease(shaderModule);
        Shutdown();
        return std::unexpected("Failed to create shadow bind group layout");
    }

    // 5. Pipeline Layout & Render Pipeline
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &shadowBindGroupLayout_;
    plDesc.label = ToWgpuStringView("Shadow_PipelineLayout");
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!pl) {
        wgpuShaderModuleRelease(shaderModule);
        Shutdown();
        return std::unexpected("Failed to create shadow pipeline layout");
    }

    std::array<WGPUVertexAttribute, 6> shadowAttrs{};
    shadowAttrs[0].format = WGPUVertexFormat_Float32x3;
    shadowAttrs[0].offset = offsetof(core::Vertex, position);
    shadowAttrs[0].shaderLocation = 0;

    shadowAttrs[1].format = WGPUVertexFormat_Float32x3;
    shadowAttrs[1].offset = offsetof(core::Vertex, normal);
    shadowAttrs[1].shaderLocation = 1;

    shadowAttrs[2].format = WGPUVertexFormat_Float32x4;
    shadowAttrs[2].offset = offsetof(core::Vertex, tangent);
    shadowAttrs[2].shaderLocation = 2;

    shadowAttrs[3].format = WGPUVertexFormat_Float32x2;
    shadowAttrs[3].offset = offsetof(core::Vertex, uv);
    shadowAttrs[3].shaderLocation = 3;

    shadowAttrs[4].format = WGPUVertexFormat_Uint32x4;
    shadowAttrs[4].offset = offsetof(core::Vertex, joints);
    shadowAttrs[4].shaderLocation = 4;

    shadowAttrs[5].format = WGPUVertexFormat_Float32x4;
    shadowAttrs[5].offset = offsetof(core::Vertex, weights);
    shadowAttrs[5].shaderLocation = 5;

    WGPUVertexBufferLayout vbLayout{};
    vbLayout.arrayStride = sizeof(core::Vertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = shadowAttrs.size();
    vbLayout.attributes = shadowAttrs.data();

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth32Float;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_True;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;
    depthStencil.depthBias = 2;
    depthStencil.depthBiasSlopeScale = 2.0f;
    depthStencil.depthBiasClamp = 0.0f;

    WGPUFragmentState fragState = WGPU_FRAGMENT_STATE_INIT;
    fragState.module = shaderModule;
    fragState.entryPoint = ToWgpuStringView("fs_shadow");
    fragState.targetCount = 0;
    fragState.targets = nullptr;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.label = ToWgpuStringView("Shadow_RenderPipeline");
    rpDesc.layout = pl;
    rpDesc.vertex.module = shaderModule;
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_shadow");
    rpDesc.vertex.bufferCount = 1;
    rpDesc.vertex.buffers = &vbLayout;
    rpDesc.fragment = &fragState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;
    rpDesc.multisample.count = 1;
    rpDesc.multisample.mask = 0xFFFFFFFF;

    shadowPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);
    wgpuPipelineLayoutRelease(pl);
    wgpuShaderModuleRelease(shaderModule);

    if (!shadowPipeline_) {
        Shutdown();
        return std::unexpected("Failed to create shadow render pipeline");
    }

    // Initialize depth texture to 1.0 (unshadowed) so sampling is well-defined before first render.
    if (CommandEncoderHandle initEnc{wgpuDeviceCreateCommandEncoder(device_, nullptr)}) {
        Clear(initEnc.Get());
        if (CommandBufferHandle initCmd{wgpuCommandEncoderFinish(initEnc, nullptr)}) {
            WGPUCommandBuffer rawCmd{initCmd.Get()};
            wgpuQueueSubmit(queue_, 1, &rawCmd);
        }
    }

    BGL_LOG_INFO("ShadowPass initialized successfully (2048x2048 Depth32F PCF Pre-Pass)");
    return {};
}

glm::mat4 ShadowPass::ComputeDirectionalLightMatrix(const core::AABB& sceneBounds, const glm::vec3& lightDir) noexcept {
    const float dirLen{glm::length(lightDir)};
    const glm::vec3 d{(dirLen > 1e-4f) ? (lightDir / dirLen) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    if (std::abs(glm::dot(d, up)) > 0.99f) {
        up = glm::vec3{0.0f, 0.0f, 1.0f};
    }

    const bool validBounds{sceneBounds.minBound.x <= sceneBounds.maxBound.x &&
                           sceneBounds.minBound.y <= sceneBounds.maxBound.y &&
                           sceneBounds.minBound.z <= sceneBounds.maxBound.z};
    if (!validBounds) {
        const glm::vec3 center{0.0f};
        constexpr float kDefaultRadius{20.0f};
        const glm::vec3 eye{center + d * (kDefaultRadius * 1.5f)};
        const glm::mat4 lightView{glm::lookAtRH(eye, center, up)};
        const glm::mat4 lightProj{
            glm::orthoRH_ZO(-kDefaultRadius, kDefaultRadius, -kDefaultRadius, kDefaultRadius, 0.1f, kDefaultRadius * 3.5f)};
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

WGPUBindGroup ShadowPass::CreateShadowBindGroup(WGPUBuffer shadowUniformBuffer,
                                                 WGPUTextureView baseColorView) const noexcept {
    if (!device_ || !shadowBindGroupLayout_ || !shadowUniformBuffer || !defaultSampler_)
        return nullptr;

    WGPUTextureView view = baseColorView ? baseColorView : defaultWhiteView_;
    if (!view)
        return nullptr;

    std::array<WGPUBindGroupEntry, 3> bgEntries{};
    bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[0].binding = 0;
    bgEntries[0].buffer = shadowUniformBuffer;
    bgEntries[0].offset = 0;
    bgEntries[0].size = sizeof(ShadowUniforms);

    bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = defaultSampler_;

    bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntries[2].binding = 2;
    bgEntries[2].textureView = view;

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = shadowBindGroupLayout_;
    bgDesc.entryCount = bgEntries.size();
    bgDesc.entries = bgEntries.data();
    bgDesc.label = ToWgpuStringView("Shadow_BindGroup");
    return wgpuDeviceCreateBindGroup(device_, &bgDesc);
}

void ShadowPass::Clear(WGPUCommandEncoder encoder) {
    if (!encoder || !shadowTextureView_ || cleared_)
        return;

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
                        const scene::Quadtree* spatialIndex) {
    if (!encoder || !shadowPipeline_ || !shadowTextureView_)
        return;

    const float dirLen = glm::length(lightDir);
    const glm::vec3 safeDir =
        (dirLen > 1e-4f) ? (lightDir / dirLen) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f});
    lightViewProj_ = ComputeDirectionalLightMatrix(scene.sceneAABB, safeDir);

    if (!dirty_ && (!scene.animationPlaying || scene.activeAnimationIndex < 0) && lastScene_ == &scene &&
        glm::distance(lastLightDir_, safeDir) < 1e-4f) {
        return;
    }

    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depthAttachment.view = shadowTextureView_;
    depthAttachment.depthLoadOp = WGPULoadOp_Clear;
    depthAttachment.depthStoreOp = WGPUStoreOp_Store;
    depthAttachment.depthClearValue = 1.0f;

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.label = ToWgpuStringView("DirectionalShadowDepthPass");
    passDesc.depthStencilAttachment = &depthAttachment;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
    if (!pass)
        return;

    wgpuRenderPassEncoderSetPipeline(pass, shadowPipeline_);

    auto renderNodeShadow = [&](auto& self, scene::SceneNode& node, const glm::mat4& parentMat) -> void {
        const glm::mat4 worldMat{parentMat * node.localTransform};
        for (auto& prim : node.GetPrimitives()) {
            if (!prim.gpuVertexBuffer)
                continue;

            if (!prim.gpuShadowUniformBuffer) {
                WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ubDesc.size = sizeof(ShadowUniforms);
                ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                ubDesc.label = ToWgpuStringView("ShadowPrimitiveUniforms");
                prim.gpuShadowUniformBuffer = wgpuDeviceCreateBuffer(device_, &ubDesc);
                if (!prim.gpuShadowUniformBuffer)
                    continue;
            }

            if (prim.morphDirty) {
                wgpuQueueWriteBuffer(queue_, prim.gpuVertexBuffer, 0, prim.vertices.data(),
                                     prim.vertices.size() * sizeof(core::Vertex));
                prim.morphDirty = false;
            }

            ShadowUniforms uniforms{};
            uniforms.model = worldMat;
            uniforms.lightViewProj = lightViewProj_;
            if (prim.materialIndex >= 0 && static_cast<size_t>(prim.materialIndex) < scene.materials.size()) {
                const auto& mat = scene.materials[prim.materialIndex];
                uniforms.baseColorFactor = mat.baseColorFactor;
                uniforms.alphaCutoff = mat.alphaCutoff;
                uniforms.alphaMode = static_cast<uint32_t>(mat.alphaMode);
                if (mat.baseColorTextureIndex >= 0) {
                    uniforms.flags |= (1u << 0);
                }
            }

            if (node.skinIndex >= 0 && static_cast<size_t>(node.skinIndex) < scene.skins.size()) {
                const auto& skin = scene.skins[static_cast<size_t>(node.skinIndex)];
                uniforms.jointCount = skin.ComputeJointMatrices(worldMat, scene.allNodes, uniforms.jointMatrices);
            }

            if (!node.morphWeights.empty()) {
                const uint32_t count = std::min(static_cast<uint32_t>(node.morphWeights.size()), 4u);
                uniforms.morphTargetCount = count;
                for (uint32_t m = 0; m < count; ++m) {
                    uniforms.morphWeights[m] = node.morphWeights[m];
                }
            }

            if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                const auto& mat = scene.materials[prim.materialIndex];
                uniforms.uvScale = mat.uvScale;
                uniforms.uvOffset = mat.uvOffset;
            }

            wgpuQueueWriteBuffer(queue_, prim.gpuShadowUniformBuffer, 0, &uniforms, sizeof(uniforms));

            if (!prim.gpuShadowBindGroup) {
                WGPUTextureView baseView = defaultWhiteView_;
                if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                    const auto& mat = scene.materials[prim.materialIndex];
                    if (mat.baseColorTextureIndex >= 0 &&
                        mat.baseColorTextureIndex < static_cast<int>(scene.gpuTextures.size()) &&
                        scene.gpuTextures[mat.baseColorTextureIndex].view) {
                        baseView = scene.gpuTextures[mat.baseColorTextureIndex].view;
                    }
                }
                prim.gpuShadowBindGroup = CreateShadowBindGroup(prim.gpuShadowUniformBuffer, baseView);
                if (!prim.gpuShadowBindGroup)
                    continue;
            }

            wgpuRenderPassEncoderSetBindGroup(pass, 0, prim.gpuShadowBindGroup, 0, nullptr);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, prim.gpuVertexBuffer, 0,
                                                 prim.vertices.size() * sizeof(core::Vertex));

            if (prim.gpuIndexBuffer && !prim.indices.empty()) {
                const auto slice = prim.GetLodSlice(1u);
                wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuIndexBuffer, prim.indexFormat, 0,
                                                    wgpuBufferGetSize(prim.gpuIndexBuffer));
                wgpuRenderPassEncoderDrawIndexed(pass, slice.indexCount, 1, slice.firstIndex, 0, 0);
            } else {
                wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(prim.vertices.size()), 1, 0, 0);
            }
        }

        for (const auto& child : node.children) {
            if (child)
                self(self, *child, worldMat);
        }
    };

    (void)spatialIndex;
    for (const auto& root : scene.rootNodes) {
        if (root)
            renderNodeShadow(renderNodeShadow, *root, glm::mat4{1.0f});
    }

    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    dirty_ = false;
    cleared_ = false;
    lastScene_ = &scene;
    lastLightDir_ = safeDir;
}

} // namespace bgl::gpu
