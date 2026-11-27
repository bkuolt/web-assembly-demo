/**
 * @file GpuCuller.cpp
 * @brief Implementation of WebGPU Compute-shader-driven Frustum & Spatial Bounding Culling.
 */
#include "gpu/GpuCuller.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace bgl::gpu {

namespace {

void FreeTrackedBuffer(BufferHandle& buffer, size_t& trackedBytes) noexcept {
    if (buffer) {
        core::MemoryTracker::Instance().TrackBufferFree(trackedBytes);
        buffer.Reset();
    }
    trackedBytes = 0;
}

bool RecreateTrackedBuffer(WGPUDevice device, BufferHandle& buffer, size_t& trackedBytes, size_t newBytes,
                           WGPUBufferUsage usage, const char* label) {
    FreeTrackedBuffer(buffer, trackedBytes);
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = newBytes;
    desc.usage = usage;
    desc.label = ToWgpuStringView(label);
    buffer = wgpuDeviceCreateBuffer(device, &desc);
    if (!buffer) {
        return false;
    }
    trackedBytes = newBytes;
    core::MemoryTracker::Instance().TrackBufferAlloc(trackedBytes);
    return true;
}

} // namespace

GpuCuller::~GpuCuller() {
    Shutdown();
}

GpuCuller::GpuCuller(GpuCuller&& other) noexcept {
    MoveFrom(other);
}

GpuCuller& GpuCuller::operator=(GpuCuller&& other) noexcept {
    if (this != &other) {
        Shutdown();
        MoveFrom(other);
    }
    return *this;
}

void GpuCuller::MoveFrom(GpuCuller& other) noexcept {
    device_ = std::exchange(other.device_, nullptr);
    queue_ = std::exchange(other.queue_, nullptr);
    computePipeline_ = std::move(other.computePipeline_);
    pipelineLayout_ = std::move(other.pipelineLayout_);
    bindGroupLayout_ = std::move(other.bindGroupLayout_);
    bindGroup_ = std::move(other.bindGroup_);
    uniformBuffer_ = std::move(other.uniformBuffer_);
    boundsStorageBuffer_ = std::move(other.boundsStorageBuffer_);
    visibilityStorageBuffer_ = std::move(other.visibilityStorageBuffer_);
    atomicCounterBuffer_ = std::move(other.atomicCounterBuffer_);
    indirectArgsBuffer_ = std::move(other.indirectArgsBuffer_);
    primitiveCount_ = std::exchange(other.primitiveCount_, 0);
    boundsBufferSize_ = std::exchange(other.boundsBufferSize_, 0);
    visibilityBufferSize_ = std::exchange(other.visibilityBufferSize_, 0);
    indirectArgsBufferSize_ = std::exchange(other.indirectArgsBufferSize_, 0);
    cpuBounds_ = std::move(other.cpuBounds_);
    cpuIndirectArgs_ = std::move(other.cpuIndirectArgs_);
}

void GpuCuller::Shutdown() noexcept {
    bindGroup_.Reset();
    FreeTrackedBuffer(indirectArgsBuffer_, indirectArgsBufferSize_);
    if (atomicCounterBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(uint32_t));
        atomicCounterBuffer_.Reset();
    }
    FreeTrackedBuffer(visibilityStorageBuffer_, visibilityBufferSize_);
    FreeTrackedBuffer(boundsStorageBuffer_, boundsBufferSize_);
    if (uniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(GpuCullUniforms));
        uniformBuffer_.Reset();
    }
    computePipeline_.Reset();
    pipelineLayout_.Reset();
    bindGroupLayout_.Reset();
    cpuBounds_.clear();
    cpuIndirectArgs_.clear();
    primitiveCount_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> GpuCuller::CreateComputePipeline(WGPUShaderModule shaderModule) {
    if (!device_ || !shaderModule || !pipelineLayout_) {
        return std::unexpected("Cannot create GPU culler compute pipeline with invalid state");
    }

    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = pipelineLayout_.Get();
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = ToWgpuStringView("cs_cull");
    cpDesc.label = ToWgpuStringView("GpuCull_ComputePipeline");

    ComputePipelineHandle nextPipe{wgpuDeviceCreateComputePipeline(device_, &cpDesc)};
    if (!nextPipe) {
        return std::unexpected("Failed to create GPU culler compute pipeline");
    }
    computePipeline_ = std::move(nextPipe);
    return {};
}

core::Result<void> GpuCuller::ReloadShader() {
    if (!device_ || !pipelineLayout_) {
        return std::unexpected("GpuCuller not initialized");
    }
    auto modRes = ShaderManager::Instance().LoadAndCompile(device_, "gpu_cull.wgsl", "GpuCull_ComputeModule", true);
    if (!modRes) {
        return std::unexpected(modRes.error());
    }
    return CreateComputePipeline(modRes->Get());
}

core::Result<void> GpuCuller::Initialize(WGPUDevice device, WGPUQueue queue) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize GpuCuller with null WebGPU device or queue");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;

    // 1. Load & Compile WGSL Compute Shader Module
    auto modRes = ShaderManager::Instance().LoadAndCompile(device_, "gpu_cull.wgsl", "GpuCull_ComputeModule");
    if (!modRes) {
        Shutdown();
        return std::unexpected(modRes.error());
    }
    ShaderModuleHandle shaderModule{std::move(*modRes)};

    // 2. Bind Group Layout
    std::array<WGPUBindGroupLayoutEntry, 5> entries{};
    // Uniforms
    entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Compute;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(GpuCullUniforms);

    // Bounds Storage (read)
    entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    entries[1].buffer.minBindingSize = sizeof(GpuBoundingBox);

    // Visibility Storage (read_write)
    entries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Compute;
    entries[2].buffer.type = WGPUBufferBindingType_Storage;
    entries[2].buffer.minBindingSize = sizeof(uint32_t);

    // Atomic Visible Count (read_write)
    entries[3] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Compute;
    entries[3].buffer.type = WGPUBufferBindingType_Storage;
    entries[3].buffer.minBindingSize = sizeof(uint32_t);

    // Indirect Draw Args Storage (read_write)
    entries[4] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[4].binding = 4;
    entries[4].visibility = WGPUShaderStage_Compute;
    entries[4].buffer.type = WGPUBufferBindingType_Storage;
    entries[4].buffer.minBindingSize = sizeof(DrawIndexedIndirectArgs);

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = entries.size();
    bglDesc.entries = entries.data();
    bglDesc.label = ToWgpuStringView("GpuCull_BindGroupLayout");
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!bindGroupLayout_) {
        Shutdown();
        return std::unexpected("Failed to create GPU culler bind group layout");
    }

    // 3. Compute Pipeline Layout & Pipeline
    const WGPUBindGroupLayout bglRaw{bindGroupLayout_.Get()};
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bglRaw;
    plDesc.label = ToWgpuStringView("GpuCull_PipelineLayout");
    pipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!pipelineLayout_) {
        Shutdown();
        return std::unexpected("Failed to create GPU culler pipeline layout");
    }

    if (auto pipeRes = CreateComputePipeline(shaderModule.Get()); !pipeRes) {
        Shutdown();
        return std::unexpected(pipeRes.error());
    }

    // 4. Uniform & Counter Buffers
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(GpuCullUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("GpuCull_Uniforms");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    if (!uniformBuffer_) {
        Shutdown();
        return std::unexpected("Failed to create GPU culler uniform buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(GpuCullUniforms));

    WGPUBufferDescriptor countDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    countDesc.size = sizeof(uint32_t);
    countDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    countDesc.label = ToWgpuStringView("GpuCull_AtomicCounter");
    atomicCounterBuffer_ = wgpuDeviceCreateBuffer(device_, &countDesc);
    if (!atomicCounterBuffer_) {
        Shutdown();
        return std::unexpected("Failed to create GPU culler atomic counter buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(uint32_t));

    BGL_LOG_INFO("GpuCuller initialized successfully (WebGPU Compute Shader Frustum Culling)");
    return {};
}

void GpuCuller::UploadSceneBounds(const scene::Scene& scene) {
    cpuBounds_.clear();
    cpuIndirectArgs_.clear();
    std::unordered_set<const scene::SceneNode*> visitedBounds{};
    auto collectBounds = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentMat,
                             size_t depth) -> void {
        if (depth > 256u || !visitedBounds.insert(&node).second) {
            return;
        }
        const glm::mat4 worldMat = parentMat * node.localTransform;
        auto prims = node.GetPrimitives();
        for (size_t pIdx = 0; pIdx < prims.size(); ++pIdx) {
            const auto& prim = prims[pIdx];
            if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                core::AABB worldBox{};
                if (node.instanceTransforms.empty()) {
                    worldBox = prim.boundingBox.Transformed(worldMat);
                } else {
                    for (const glm::mat4& instMat : node.instanceTransforms) {
                        worldBox.Expand(prim.boundingBox.Transformed(worldMat * instMat));
                    }
                }
                const uint32_t instCount =
                    node.instanceTransforms.empty() ? 1u : static_cast<uint32_t>(node.instanceTransforms.size());
                GpuBoundingBox box{};
                box.minBound = glm::vec4{worldBox.minBound, 1.0f};
                box.maxBound = glm::vec4{worldBox.maxBound, 1.0f};
                box.nodeIndex = static_cast<uint32_t>(node.nodeIndex >= 0 ? node.nodeIndex : 0);
                box.primitiveIndex = static_cast<uint32_t>(pIdx);
                box.instanceCount = instCount;
                cpuBounds_.push_back(box);

                DrawIndexedIndirectArgs args{};
                if (prim.lodLevels[0].indexCount > 0) {
                    args.indexCount = prim.lodLevels[0].indexCount;
                    args.firstIndex = prim.lodLevels[0].firstIndex;
                } else if (!prim.indices.empty()) {
                    args.indexCount = static_cast<uint32_t>(prim.indices.size());
                } else {
                    args.indexCount = static_cast<uint32_t>(prim.vertices.size());
                }
                args.instanceCount = instCount;
                cpuIndirectArgs_.push_back(args);
            }
        }
        for (const auto& child : node.children) {
            if (child)
                self(self, *child, worldMat, depth + 1u);
        }
    };

    for (const auto& root : scene.rootNodes) {
        if (root)
            collectBounds(collectBounds, *root, glm::mat4{1.0f}, 0u);
    }

    primitiveCount_ = cpuBounds_.size();
    if (primitiveCount_ == 0 || !device_ || !queue_)
        return;

    const size_t neededBytes = primitiveCount_ * sizeof(GpuBoundingBox);
    const size_t visNeededBytes = primitiveCount_ * sizeof(uint32_t);
    const size_t indirectNeededBytes = primitiveCount_ * sizeof(DrawIndexedIndirectArgs);

    if (neededBytes > boundsBufferSize_ || visNeededBytes > visibilityBufferSize_ ||
        indirectNeededBytes > indirectArgsBufferSize_ || !boundsStorageBuffer_ || !visibilityStorageBuffer_ ||
        !indirectArgsBuffer_ || !bindGroup_) {
        const size_t newBoundsSize = std::max(neededBytes * 2, size_t{64 * 1024});
        const size_t newVisSize = std::max(visNeededBytes * 2, size_t{16 * 1024});
        const size_t newIndirectSize = std::max(indirectNeededBytes * 2, size_t{32 * 1024});

        if (!RecreateTrackedBuffer(device_, boundsStorageBuffer_, boundsBufferSize_, newBoundsSize,
                                   WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, "GpuCull_BoundsStorage") ||
            !RecreateTrackedBuffer(device_, visibilityStorageBuffer_, visibilityBufferSize_, newVisSize,
                                   WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc,
                                   "GpuCull_VisibilityStorage") ||
            !RecreateTrackedBuffer(device_, indirectArgsBuffer_, indirectArgsBufferSize_, newIndirectSize,
                                   WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst |
                                       WGPUBufferUsage_CopySrc,
                                   "GpuCull_IndirectArgs")) {
            primitiveCount_ = 0;
            return;
        }

        bindGroup_.Reset();

        std::array<WGPUBindGroupEntry, 5> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = uniformBuffer_.Get();
        bgEntries[0].offset = 0;
        bgEntries[0].size = sizeof(GpuCullUniforms);

        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].buffer = boundsStorageBuffer_.Get();
        bgEntries[1].offset = 0;
        bgEntries[1].size = boundsBufferSize_;

        bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[2].binding = 2;
        bgEntries[2].buffer = visibilityStorageBuffer_.Get();
        bgEntries[2].offset = 0;
        bgEntries[2].size = visibilityBufferSize_;

        bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[3].binding = 3;
        bgEntries[3].buffer = atomicCounterBuffer_.Get();
        bgEntries[3].offset = 0;
        bgEntries[3].size = sizeof(uint32_t);

        bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[4].binding = 4;
        bgEntries[4].buffer = indirectArgsBuffer_.Get();
        bgEntries[4].offset = 0;
        bgEntries[4].size = indirectArgsBufferSize_;

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = bindGroupLayout_.Get();
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        bgDesc.label = ToWgpuStringView("GpuCull_BindGroup");
        bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    }

    wgpuQueueWriteBuffer(queue_, boundsStorageBuffer_.Get(), 0, cpuBounds_.data(), neededBytes);
    wgpuQueueWriteBuffer(queue_, indirectArgsBuffer_.Get(), 0, cpuIndirectArgs_.data(), indirectNeededBytes);
}

void GpuCuller::Dispatch(WGPUCommandEncoder encoder, const core::Frustum& frustum) {
    if (!encoder || !computePipeline_ || !bindGroup_ || primitiveCount_ == 0)
        return;

    GpuCullUniforms uniforms{};
    for (size_t i = 0; i < 6; ++i) {
        uniforms.frustumPlanes[i] = glm::vec4{frustum.planes[i].normal, frustum.planes[i].distance};
    }
    uniforms.primitiveCount = static_cast<uint32_t>(primitiveCount_);

    wgpuQueueWriteBuffer(queue_, uniformBuffer_.Get(), 0, &uniforms, sizeof(uniforms));
    const uint32_t zeroCount = 0;
    wgpuQueueWriteBuffer(queue_, atomicCounterBuffer_.Get(), 0, &zeroCount, sizeof(uint32_t));

    WGPUComputePassDescriptor cpDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    cpDesc.label = ToWgpuStringView("GpuCull_Pass");
    ComputePassEncoderHandle pass{wgpuCommandEncoderBeginComputePass(encoder, &cpDesc)};
    if (!pass)
        return;

    wgpuComputePassEncoderSetPipeline(pass.Get(), computePipeline_.Get());
    wgpuComputePassEncoderSetBindGroup(pass.Get(), 0, bindGroup_.Get(), 0, nullptr);

    const uint32_t workgroups = static_cast<uint32_t>((primitiveCount_ + 63) / 64);
    wgpuComputePassEncoderDispatchWorkgroups(pass.Get(), workgroups, 1, 1);
    wgpuComputePassEncoderEnd(pass.Get());
}

} // namespace bgl::gpu
