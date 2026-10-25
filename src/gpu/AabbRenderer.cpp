/**
 * @file AabbRenderer.cpp
 * @brief Implementation of batched 3D wireframe bounding box renderer for Scene and Quadtree spatial partitions.
 */
#include "gpu/AabbRenderer.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/Shaders.hpp"
#include <algorithm>
#include <cstring>

namespace bgl::gpu {

AabbRenderer::~AabbRenderer() {
    Shutdown();
}

void AabbRenderer::Shutdown() noexcept {
    if (aabbUniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(AabbUniforms));
    }
    if (aabbVertexBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(aabbVertexBufferSize_);
    }
    aabbBindGroup_.Reset();
    aabbUniformBuffer_.Reset();
    aabbVertexBuffer_.Reset();
    aabbPipeline_.Reset();
    aabbBindGroupLayout_.Reset();
    batchedAabbVertices_.clear();
    cachedSceneAabbVertices_.clear();
    aabbVertexBufferSize_ = 0;
    aabbFrameByteOffset_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> AabbRenderer::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat colorFormat,
                                            uint32_t sampleCount) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize AabbRenderer with null WebGPU device or queue");
    }
    device_ = device;
    queue_ = queue;

    // 1. Compile WGSL Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWgpuStringView(kAabbShaderWGSL.data());

    WGPUShaderModuleDescriptor shaderDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDesc.nextInChain = &wgslDesc.chain;
    shaderDesc.label = ToWgpuStringView("AABB_WireframeShader");

    WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(device_, &shaderDesc);
    if (!shaderModule) {
        return std::unexpected("Failed to compile AABB wireframe WGSL shader module");
    }

    // 2. Create Bind Group Layout
    WGPUBindGroupLayoutEntry aabbBglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    aabbBglEntry.binding = 0;
    aabbBglEntry.visibility = WGPUShaderStage_Vertex;
    aabbBglEntry.buffer.type = WGPUBufferBindingType_Uniform;
    aabbBglEntry.buffer.hasDynamicOffset = WGPU_FALSE;
    aabbBglEntry.buffer.minBindingSize = sizeof(AabbUniforms);

    WGPUBindGroupLayoutDescriptor aabbBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    aabbBglDesc.entryCount = 1;
    aabbBglDesc.entries = &aabbBglEntry;
    aabbBglDesc.label = ToWgpuStringView("AABB_BindGroupLayout");
    aabbBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &aabbBglDesc);

    // 3. Create Pipeline Layout
    WGPUPipelineLayoutDescriptor aabbPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    aabbPlDesc.bindGroupLayoutCount = 1;
    aabbPlDesc.bindGroupLayouts = aabbBindGroupLayout_.AddressOf();
    aabbPlDesc.label = ToWgpuStringView("AABB_PipelineLayout");
    WGPUPipelineLayout aabbPl = wgpuDeviceCreatePipelineLayout(device_, &aabbPlDesc);

    // 4. Vertex Layout (Position 3x Float32 + Color 4x Float32)
    std::array<WGPUVertexAttribute, 2> aabbAttrs{};
    aabbAttrs[0].format = WGPUVertexFormat_Float32x3;
    aabbAttrs[0].offset = offsetof(AabbVertex, pos);
    aabbAttrs[0].shaderLocation = 0;

    aabbAttrs[1].format = WGPUVertexFormat_Float32x4;
    aabbAttrs[1].offset = offsetof(AabbVertex, color);
    aabbAttrs[1].shaderLocation = 1;

    WGPUVertexBufferLayout aabbVbLayout{};
    aabbVbLayout.arrayStride = sizeof(AabbVertex);
    aabbVbLayout.stepMode = WGPUVertexStepMode_Vertex;
    aabbVbLayout.attributeCount = aabbAttrs.size();
    aabbVbLayout.attributes = aabbAttrs.data();

    // 5. Color Target & Blending
    WGPUBlendState aabbBlend = WGPU_BLEND_STATE_INIT;
    aabbBlend.color.operation = WGPUBlendOperation_Add;
    aabbBlend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    aabbBlend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    aabbBlend.alpha.operation = WGPUBlendOperation_Add;
    aabbBlend.alpha.srcFactor = WGPUBlendFactor_One;
    aabbBlend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUColorTargetState aabbColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    aabbColorTarget.format = colorFormat;
    aabbColorTarget.blend = &aabbBlend;
    aabbColorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState aabbFragState = WGPU_FRAGMENT_STATE_INIT;
    aabbFragState.module = shaderModule;
    aabbFragState.entryPoint = ToWgpuStringView("fs_aabb");
    aabbFragState.targetCount = 1;
    aabbFragState.targets = &aabbColorTarget;

    WGPUDepthStencilState aabbDepthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    aabbDepthStencil.format = WGPUTextureFormat_Depth32Float;
    aabbDepthStencil.depthWriteEnabled = WGPUOptionalBool_False; // Wireframes don't overwrite depth
    aabbDepthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    // 6. Assemble Render Pipeline
    WGPURenderPipelineDescriptor aabbRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    aabbRpDesc.label = ToWgpuStringView("AABB_WireframeRenderPipeline");
    aabbRpDesc.layout = aabbPl;
    aabbRpDesc.vertex.module = shaderModule;
    aabbRpDesc.vertex.entryPoint = ToWgpuStringView("vs_aabb");
    aabbRpDesc.vertex.bufferCount = 1;
    aabbRpDesc.vertex.buffers = &aabbVbLayout;
    aabbRpDesc.fragment = &aabbFragState;
    aabbRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
    aabbRpDesc.primitive.cullMode = WGPUCullMode_None;
    aabbRpDesc.depthStencil = &aabbDepthStencil;
    aabbRpDesc.multisample.count = sampleCount;
    aabbRpDesc.multisample.mask = 0xFFFFFFFF;
    aabbRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    aabbPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &aabbRpDesc);
    wgpuPipelineLayoutRelease(aabbPl);
    wgpuShaderModuleRelease(shaderModule);

    if (!aabbPipeline_) {
        return std::unexpected("Failed to create AABB wireframe render pipeline");
    }

    // 7. Allocate Dynamic Vertex Buffer and Uniform Buffer
    aabbVertexBufferSize_ = 512 * 1024;
    WGPUBufferDescriptor boxVbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    boxVbDesc.size = aabbVertexBufferSize_;
    boxVbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    boxVbDesc.label = ToWgpuStringView("AABB_DynamicVertexBuffer");
    aabbVertexBuffer_ = wgpuDeviceCreateBuffer(device_, &boxVbDesc);
    core::MemoryTracker::Instance().TrackBufferAlloc(aabbVertexBufferSize_);

    WGPUBufferDescriptor aabbUbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    aabbUbDesc.size = sizeof(AabbUniforms);
    aabbUbDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    aabbUbDesc.label = ToWgpuStringView("AABB_Uniforms");
    aabbUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &aabbUbDesc);
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(AabbUniforms));

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

    BGL_LOG_INFO("AabbRenderer initialized successfully (Dynamic VBO: {} KB, LineList topology)",
                 aabbVertexBufferSize_ / 1024);
    return {};
}

void AabbRenderer::AppendAABBLines(const core::AABB& aabb, const glm::vec4& color) {
    if (aabb.minBound.x > aabb.maxBound.x)
        return;
    const glm::vec3 minB{aabb.minBound};
    const glm::vec3 maxB{aabb.maxBound};

    const std::array<glm::vec3, 8> c{{glm::vec3{minB.x, minB.y, minB.z}, glm::vec3{maxB.x, minB.y, minB.z},
                                      glm::vec3{maxB.x, maxB.y, minB.z}, glm::vec3{minB.x, maxB.y, minB.z},
                                      glm::vec3{minB.x, minB.y, maxB.z}, glm::vec3{maxB.x, minB.y, maxB.z},
                                      glm::vec3{maxB.x, maxB.y, maxB.z}, glm::vec3{minB.x, maxB.y, maxB.z}}};

    static constexpr std::array<size_t, 24> kIndices{
        {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7}};

    for (size_t idx : kIndices) {
        batchedAabbVertices_.push_back(AabbVertex{.pos = c[idx], ._pad = 0.0f, .color = color});
    }
}

void AabbRenderer::FlushAABBLines(WGPURenderPassEncoder pass, const glm::mat4& viewProj) {
    if (!pass || !aabbPipeline_ || batchedAabbVertices_.empty()) {
        batchedAabbVertices_.clear();
        return;
    }

    const size_t requiredBytes{batchedAabbVertices_.size() * sizeof(AabbVertex)};
    size_t writeOffset{aabbFrameByteOffset_};
    const size_t totalNeeded{writeOffset + requiredBytes};

    if (totalNeeded > aabbVertexBufferSize_ || !aabbVertexBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(aabbVertexBufferSize_);
        aabbVertexBufferSize_ = std::max(totalNeeded * 2, size_t{512 * 1024});
        WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        vbDesc.size = aabbVertexBufferSize_;
        vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        vbDesc.label = ToWgpuStringView("AABB_DynamicVertexBuffer");
        aabbVertexBuffer_.Reset(wgpuDeviceCreateBuffer(device_, &vbDesc));
        core::MemoryTracker::Instance().TrackBufferAlloc(aabbVertexBufferSize_);
        writeOffset = 0;
    }

    wgpuQueueWriteBuffer(queue_, aabbVertexBuffer_, writeOffset, batchedAabbVertices_.data(), requiredBytes);

    AabbUniforms uniforms{};
    uniforms.viewProj = viewProj;
    wgpuQueueWriteBuffer(queue_, aabbUniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(pass, aabbPipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, aabbBindGroup_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, aabbVertexBuffer_, writeOffset, requiredBytes);
    wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(batchedAabbVertices_.size()), 1, 0, 0);

    aabbFrameByteOffset_ = (writeOffset + requiredBytes + 255u) & ~size_t{255u};
    batchedAabbVertices_.clear();
}

void AabbRenderer::RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj,
                              const glm::vec4& color) {
    AppendAABBLines(aabb, color);
    FlushAABBLines(pass, viewProj);
}

void AabbRenderer::BatchSceneAABBs(const scene::Scene& scene, bool showSceneAABB, bool showPrimitiveAABBs) {
    if (showSceneAABB && showPrimitiveAABBs && !cachedSceneAabbVertices_.empty()) {
        batchedAabbVertices_.insert(batchedAabbVertices_.end(), cachedSceneAabbVertices_.begin(),
                                    cachedSceneAabbVertices_.end());
        return;
    }

    if (showSceneAABB && scene.sceneAABB.minBound.x <= scene.sceneAABB.maxBound.x) {
        AppendAABBLines(scene.sceneAABB, glm::vec4{1.0f, 0.84f, 0.0f, 1.0f});
    }

    if (showPrimitiveAABBs) {
        auto renderNodeAABBs = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform) -> void {
            const glm::mat4 worldMat{parentTransform * node.localTransform};
            for (const auto& prim : node.GetPrimitives()) {
                if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                    AppendAABBLines(prim.boundingBox.Transformed(worldMat), glm::vec4{0.2f, 0.85f, 1.0f, 0.8f});
                }
            }
            for (const auto& child : node.children) {
                if (child)
                    self(self, *child, worldMat);
            }
        };

        for (const auto& rootNode : scene.rootNodes) {
            if (rootNode)
                renderNodeAABBs(renderNodeAABBs, *rootNode, glm::mat4{1.0f});
        }
    }
}

void AabbRenderer::BatchQuadtreeBounds(const scene::Quadtree& quadtree) {
    std::vector<core::AABB> bounds{};
    quadtree.GetNodeBounds(bounds);
    for (const auto& b : bounds) {
        AppendAABBLines(b, glm::vec4{0.25f, 0.95f, 0.45f, 0.7f});
    }
}

} // namespace bgl::gpu
