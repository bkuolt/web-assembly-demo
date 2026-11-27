/**
 * @file AabbRenderer.cpp
 * @brief Implementation of batched 3D wireframe bounding box renderer for Scene and Quadtree spatial partitions.
 */
#include "gpu/AabbRenderer.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace bgl::gpu {

AabbRenderer::~AabbRenderer() {
    Shutdown();
}

AabbRenderer::AabbRenderer(AabbRenderer&& other) noexcept
    : device_{std::exchange(other.device_, nullptr)}, queue_{std::exchange(other.queue_, nullptr)},
      colorFormat_{other.colorFormat_}, sampleCount_{other.sampleCount_}, depthFormat_{other.depthFormat_},
      aabbPipelineLayout_{std::move(other.aabbPipelineLayout_)}, aabbPipeline_{std::move(other.aabbPipeline_)},
      aabbBindGroupLayout_{std::move(other.aabbBindGroupLayout_)},
      aabbVertexBuffer_{std::move(other.aabbVertexBuffer_)}, aabbUniformBuffer_{std::move(other.aabbUniformBuffer_)},
      aabbBindGroup_{std::move(other.aabbBindGroup_)},
      aabbVertexBufferSize_{std::exchange(other.aabbVertexBufferSize_, 0)},
      aabbFrameByteOffset_{std::exchange(other.aabbFrameByteOffset_, 0)},
      batchedAabbVertices_{std::move(other.batchedAabbVertices_)} {}

AabbRenderer& AabbRenderer::operator=(AabbRenderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        device_ = std::exchange(other.device_, nullptr);
        queue_ = std::exchange(other.queue_, nullptr);
        colorFormat_ = other.colorFormat_;
        sampleCount_ = other.sampleCount_;
        depthFormat_ = other.depthFormat_;
        aabbPipelineLayout_ = std::move(other.aabbPipelineLayout_);
        aabbPipeline_ = std::move(other.aabbPipeline_);
        aabbBindGroupLayout_ = std::move(other.aabbBindGroupLayout_);
        aabbVertexBuffer_ = std::move(other.aabbVertexBuffer_);
        aabbUniformBuffer_ = std::move(other.aabbUniformBuffer_);
        aabbBindGroup_ = std::move(other.aabbBindGroup_);
        aabbVertexBufferSize_ = std::exchange(other.aabbVertexBufferSize_, 0);
        aabbFrameByteOffset_ = std::exchange(other.aabbFrameByteOffset_, 0);
        batchedAabbVertices_ = std::move(other.batchedAabbVertices_);
    }
    return *this;
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
    aabbPipelineLayout_.Reset();
    aabbBindGroupLayout_.Reset();
    batchedAabbVertices_.clear();
    aabbVertexBufferSize_ = 0;
    aabbFrameByteOffset_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> AabbRenderer::CreateRenderPipeline(WGPUShaderModule shaderModule) {
    if (!device_ || !shaderModule || !aabbPipelineLayout_) {
        return std::unexpected("Cannot create AABB wireframe render pipeline with invalid state");
    }

    // Vertex Layout (Position 3x Float32 + Color 4x Float32)
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

    // Color Target & Blending
    WGPUBlendState aabbBlend = WGPU_BLEND_STATE_INIT;
    aabbBlend.color.operation = WGPUBlendOperation_Add;
    aabbBlend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    aabbBlend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    aabbBlend.alpha.operation = WGPUBlendOperation_Add;
    aabbBlend.alpha.srcFactor = WGPUBlendFactor_One;
    aabbBlend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUColorTargetState aabbColorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    aabbColorTarget.format = colorFormat_;
    aabbColorTarget.blend = &aabbBlend;
    aabbColorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState aabbFragState = WGPU_FRAGMENT_STATE_INIT;
    aabbFragState.module = shaderModule;
    aabbFragState.entryPoint = ToWgpuStringView("fs_aabb");
    aabbFragState.targetCount = 1;
    aabbFragState.targets = &aabbColorTarget;

    WGPUDepthStencilState aabbDepthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    aabbDepthStencil.format = depthFormat_;
    aabbDepthStencil.depthWriteEnabled = WGPUOptionalBool_False; // Wireframes don't overwrite depth
    aabbDepthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    WGPURenderPipelineDescriptor aabbRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    aabbRpDesc.label = ToWgpuStringView("AABB_WireframeRenderPipeline");
    aabbRpDesc.layout = aabbPipelineLayout_.Get();
    aabbRpDesc.vertex.module = shaderModule;
    aabbRpDesc.vertex.entryPoint = ToWgpuStringView("vs_aabb");
    aabbRpDesc.vertex.bufferCount = 1;
    aabbRpDesc.vertex.buffers = &aabbVbLayout;
    aabbRpDesc.fragment = &aabbFragState;
    aabbRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
    aabbRpDesc.primitive.cullMode = WGPUCullMode_None;
    aabbRpDesc.depthStencil = &aabbDepthStencil;
    aabbRpDesc.multisample.count = sampleCount_;
    aabbRpDesc.multisample.mask = 0xFFFFFFFF;
    aabbRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    RenderPipelineHandle nextPipe{wgpuDeviceCreateRenderPipeline(device_, &aabbRpDesc)};
    if (!nextPipe) {
        return std::unexpected("Failed to create AABB wireframe render pipeline");
    }
    aabbPipeline_ = std::move(nextPipe);
    return {};
}

core::Result<void> AabbRenderer::ReloadShader() {
    if (!device_ || !aabbPipelineLayout_) {
        return std::unexpected("AabbRenderer not initialized");
    }
    auto modRes = ShaderManager::Instance().LoadAndCompile(device_, "aabb.wgsl", "AABB_WireframeShader", true);
    if (!modRes) {
        return std::unexpected(modRes.error());
    }
    return CreateRenderPipeline(modRes->Get());
}

core::Result<void> AabbRenderer::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat colorFormat,
                                            uint32_t sampleCount, WGPUTextureFormat depthFormat) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize AabbRenderer with null WebGPU device or queue");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;
    colorFormat_ = colorFormat;
    sampleCount_ = sampleCount;
    depthFormat_ = depthFormat;

    // 1. Load & Compile WGSL Shader Module
    auto modRes = ShaderManager::Instance().LoadAndCompile(device_, "aabb.wgsl", "AABB_WireframeShader");
    if (!modRes) {
        Shutdown();
        return std::unexpected(modRes.error());
    }
    ShaderModuleHandle shaderModule{std::move(*modRes)};

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
    if (!aabbBindGroupLayout_) {
        Shutdown();
        return std::unexpected("Failed to create AABB bind group layout");
    }

    // 3. Create Pipeline Layout
    WGPUPipelineLayoutDescriptor aabbPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    aabbPlDesc.bindGroupLayoutCount = 1;
    aabbPlDesc.bindGroupLayouts = aabbBindGroupLayout_.AddressOf();
    aabbPlDesc.label = ToWgpuStringView("AABB_PipelineLayout");
    aabbPipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &aabbPlDesc);
    if (!aabbPipelineLayout_) {
        Shutdown();
        return std::unexpected("Failed to create AABB pipeline layout");
    }

    // 4. Assemble Render Pipeline
    if (auto pipeRes = CreateRenderPipeline(shaderModule.Get()); !pipeRes) {
        Shutdown();
        return std::unexpected(pipeRes.error());
    }

    // 7. Allocate Dynamic Vertex Buffer and Uniform Buffer
    constexpr size_t kInitialVertexBytes{512 * 1024};
    WGPUBufferDescriptor boxVbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    boxVbDesc.size = kInitialVertexBytes;
    boxVbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    boxVbDesc.label = ToWgpuStringView("AABB_DynamicVertexBuffer");
    aabbVertexBuffer_ = wgpuDeviceCreateBuffer(device_, &boxVbDesc);
    if (!aabbVertexBuffer_) {
        Shutdown();
        return std::unexpected("Failed to allocate AABB dynamic vertex buffer");
    }
    aabbVertexBufferSize_ = kInitialVertexBytes;
    core::MemoryTracker::Instance().TrackBufferAlloc(aabbVertexBufferSize_);

    WGPUBufferDescriptor aabbUbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    aabbUbDesc.size = sizeof(AabbUniforms);
    aabbUbDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    aabbUbDesc.label = ToWgpuStringView("AABB_Uniforms");
    aabbUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &aabbUbDesc);
    if (!aabbUniformBuffer_) {
        Shutdown();
        return std::unexpected("Failed to allocate AABB uniform buffer");
    }
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
    if (!aabbBindGroup_) {
        Shutdown();
        return std::unexpected("Failed to create AABB bind group");
    }

    BGL_LOG_INFO("AabbRenderer initialized successfully (Dynamic VBO: {} KB, LineList topology)",
                 aabbVertexBufferSize_ / 1024);
    return {};
}

void AabbRenderer::AppendAABBLines(const core::AABB& aabb, const glm::vec4& color) {
    if (!aabb.IsValid())
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
    if (!pass || !aabbPipeline_ || !aabbUniformBuffer_ || !aabbBindGroup_ || batchedAabbVertices_.empty()) {
        batchedAabbVertices_.clear();
        return;
    }

    const size_t requiredBytes{batchedAabbVertices_.size() * sizeof(AabbVertex)};
    size_t writeOffset{aabbFrameByteOffset_};
    const size_t totalNeeded{writeOffset + requiredBytes};

    if (totalNeeded > aabbVertexBufferSize_ || !aabbVertexBuffer_) {
        if (aabbVertexBuffer_) {
            core::MemoryTracker::Instance().TrackBufferFree(aabbVertexBufferSize_);
            aabbVertexBuffer_.Reset();
            aabbVertexBufferSize_ = 0;
        }
        const size_t newSize = std::max(totalNeeded * 2, size_t{512 * 1024});
        WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        vbDesc.size = newSize;
        vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        vbDesc.label = ToWgpuStringView("AABB_DynamicVertexBuffer");
        aabbVertexBuffer_.Reset(wgpuDeviceCreateBuffer(device_, &vbDesc));
        if (!aabbVertexBuffer_) {
            batchedAabbVertices_.clear();
            return;
        }
        aabbVertexBufferSize_ = newSize;
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
    if (showSceneAABB && scene.sceneAABB.IsValid()) {
        AppendAABBLines(scene.sceneAABB, glm::vec4{1.0f, 0.84f, 0.0f, 1.0f});
    }

    if (showPrimitiveAABBs) {
        std::unordered_set<const scene::SceneNode*> visitedNodes;
        constexpr uint32_t kMaxTraversalDepth{256u};
        auto renderNodeAABBs = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform,
                                   uint32_t depth) -> void {
            if (depth >= kMaxTraversalDepth || !visitedNodes.insert(&node).second) {
                return;
            }
            const glm::mat4 worldMat{parentTransform * node.localTransform};
            for (const auto& prim : node.GetPrimitives()) {
                if (prim.boundingBox.IsValid()) {
                    AppendAABBLines(prim.boundingBox.Transformed(worldMat), glm::vec4{0.2f, 0.85f, 1.0f, 0.8f});
                }
            }
            for (const auto& child : node.children) {
                if (child) {
                    self(self, *child, worldMat, depth + 1u);
                }
            }
        };

        for (const auto& rootNode : scene.rootNodes) {
            if (rootNode) {
                renderNodeAABBs(renderNodeAABBs, *rootNode, glm::mat4{1.0f}, 0u);
            }
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
