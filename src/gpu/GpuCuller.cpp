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
#include <utility>

namespace bgl::gpu {

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
    computePipeline_ = std::exchange(other.computePipeline_, nullptr);
    pipelineLayout_ = std::exchange(other.pipelineLayout_, nullptr);
    bindGroupLayout_ = std::exchange(other.bindGroupLayout_, nullptr);
    bindGroup_ = std::exchange(other.bindGroup_, nullptr);
    uniformBuffer_ = std::exchange(other.uniformBuffer_, nullptr);
    boundsStorageBuffer_ = std::exchange(other.boundsStorageBuffer_, nullptr);
    visibilityStorageBuffer_ = std::exchange(other.visibilityStorageBuffer_, nullptr);
    atomicCounterBuffer_ = std::exchange(other.atomicCounterBuffer_, nullptr);
    indirectArgsBuffer_ = std::exchange(other.indirectArgsBuffer_, nullptr);
    primitiveCount_ = std::exchange(other.primitiveCount_, 0);
    boundsBufferSize_ = std::exchange(other.boundsBufferSize_, 0);
    visibilityBufferSize_ = std::exchange(other.visibilityBufferSize_, 0);
    indirectArgsBufferSize_ = std::exchange(other.indirectArgsBufferSize_, 0);
    cpuBounds_ = std::move(other.cpuBounds_);
    cpuIndirectArgs_ = std::move(other.cpuIndirectArgs_);
}

void GpuCuller::Shutdown() noexcept {
    if (bindGroup_) {
        wgpuBindGroupRelease(bindGroup_);
        bindGroup_ = nullptr;
    }
    if (indirectArgsBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(indirectArgsBufferSize_);
        wgpuBufferRelease(indirectArgsBuffer_);
        indirectArgsBuffer_ = nullptr;
    }
    if (atomicCounterBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(uint32_t));
        wgpuBufferRelease(atomicCounterBuffer_);
        atomicCounterBuffer_ = nullptr;
    }
    if (visibilityStorageBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(visibilityBufferSize_);
        wgpuBufferRelease(visibilityStorageBuffer_);
        visibilityStorageBuffer_ = nullptr;
    }
    if (boundsStorageBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(boundsBufferSize_);
        wgpuBufferRelease(boundsStorageBuffer_);
        boundsStorageBuffer_ = nullptr;
    }
    if (uniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(GpuCullUniforms));
        wgpuBufferRelease(uniformBuffer_);
        uniformBuffer_ = nullptr;
    }
    if (computePipeline_) {
        wgpuComputePipelineRelease(computePipeline_);
        computePipeline_ = nullptr;
    }
    if (pipelineLayout_) {
        wgpuPipelineLayoutRelease(pipelineLayout_);
        pipelineLayout_ = nullptr;
    }
    if (bindGroupLayout_) {
        wgpuBindGroupLayoutRelease(bindGroupLayout_);
        bindGroupLayout_ = nullptr;
    }
    cpuBounds_.clear();
    cpuIndirectArgs_.clear();
    primitiveCount_ = 0;
    boundsBufferSize_ = 0;
    visibilityBufferSize_ = 0;
    indirectArgsBufferSize_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> GpuCuller::CreateComputePipeline(WGPUShaderModule shaderModule) {
    if (!device_ || !shaderModule || !pipelineLayout_) {
        return std::unexpected("Cannot create GPU culler compute pipeline with invalid state");
    }

    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = pipelineLayout_;
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = ToWgpuStringView("cs_cull");
    cpDesc.label = ToWgpuStringView("GpuCull_ComputePipeline");

    WGPUComputePipeline nextPipe = wgpuDeviceCreateComputePipeline(device_, &cpDesc);
    if (!nextPipe) {
        return std::unexpected("Failed to create GPU culler compute pipeline");
    }
    if (computePipeline_) {
        wgpuComputePipelineRelease(computePipeline_);
    }
    computePipeline_ = nextPipe;
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
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout_;
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
    auto collectBounds = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentMat) -> void {
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
                self(self, *child, worldMat);
        }
    };

    for (const auto& root : scene.rootNodes) {
        if (root)
            collectBounds(collectBounds, *root, glm::mat4{1.0f});
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
        if (boundsStorageBuffer_) {
            core::MemoryTracker::Instance().TrackBufferFree(boundsBufferSize_);
            wgpuBufferRelease(boundsStorageBuffer_);
            boundsStorageBuffer_ = nullptr;
            boundsBufferSize_ = 0;
        }
        if (visibilityStorageBuffer_) {
            core::MemoryTracker::Instance().TrackBufferFree(visibilityBufferSize_);
            wgpuBufferRelease(visibilityStorageBuffer_);
            visibilityStorageBuffer_ = nullptr;
            visibilityBufferSize_ = 0;
        }
        if (indirectArgsBuffer_) {
            core::MemoryTracker::Instance().TrackBufferFree(indirectArgsBufferSize_);
            wgpuBufferRelease(indirectArgsBuffer_);
            indirectArgsBuffer_ = nullptr;
            indirectArgsBufferSize_ = 0;
        }

        const size_t newBoundsSize = std::max(neededBytes * 2, size_t{64 * 1024});
        WGPUBufferDescriptor bDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        bDesc.size = newBoundsSize;
        bDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        bDesc.label = ToWgpuStringView("GpuCull_BoundsStorage");
        boundsStorageBuffer_ = wgpuDeviceCreateBuffer(device_, &bDesc);
        if (!boundsStorageBuffer_) {
            primitiveCount_ = 0;
            return;
        }
        boundsBufferSize_ = newBoundsSize;
        core::MemoryTracker::Instance().TrackBufferAlloc(boundsBufferSize_);

        const size_t newVisSize = std::max(visNeededBytes * 2, size_t{16 * 1024});
        WGPUBufferDescriptor vDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        vDesc.size = newVisSize;
        vDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
        vDesc.label = ToWgpuStringView("GpuCull_VisibilityStorage");
        visibilityStorageBuffer_ = wgpuDeviceCreateBuffer(device_, &vDesc);
        if (!visibilityStorageBuffer_) {
            primitiveCount_ = 0;
            return;
        }
        visibilityBufferSize_ = newVisSize;
        core::MemoryTracker::Instance().TrackBufferAlloc(visibilityBufferSize_);

        const size_t newIndirectSize = std::max(indirectNeededBytes * 2, size_t{32 * 1024});
        WGPUBufferDescriptor iDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        iDesc.size = newIndirectSize;
        iDesc.usage =
            WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
        iDesc.label = ToWgpuStringView("GpuCull_IndirectArgs");
        indirectArgsBuffer_ = wgpuDeviceCreateBuffer(device_, &iDesc);
        if (!indirectArgsBuffer_) {
            primitiveCount_ = 0;
            return;
        }
        indirectArgsBufferSize_ = newIndirectSize;
        core::MemoryTracker::Instance().TrackBufferAlloc(indirectArgsBufferSize_);

        if (bindGroup_) {
            wgpuBindGroupRelease(bindGroup_);
            bindGroup_ = nullptr;
        }

        std::array<WGPUBindGroupEntry, 5> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = uniformBuffer_;
        bgEntries[0].offset = 0;
        bgEntries[0].size = sizeof(GpuCullUniforms);

        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].buffer = boundsStorageBuffer_;
        bgEntries[1].offset = 0;
        bgEntries[1].size = boundsBufferSize_;

        bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[2].binding = 2;
        bgEntries[2].buffer = visibilityStorageBuffer_;
        bgEntries[2].offset = 0;
        bgEntries[2].size = visibilityBufferSize_;

        bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[3].binding = 3;
        bgEntries[3].buffer = atomicCounterBuffer_;
        bgEntries[3].offset = 0;
        bgEntries[3].size = sizeof(uint32_t);

        bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[4].binding = 4;
        bgEntries[4].buffer = indirectArgsBuffer_;
        bgEntries[4].offset = 0;
        bgEntries[4].size = indirectArgsBufferSize_;

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = bindGroupLayout_;
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        bgDesc.label = ToWgpuStringView("GpuCull_BindGroup");
        bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    }

    wgpuQueueWriteBuffer(queue_, boundsStorageBuffer_, 0, cpuBounds_.data(), neededBytes);
    wgpuQueueWriteBuffer(queue_, indirectArgsBuffer_, 0, cpuIndirectArgs_.data(), indirectNeededBytes);
}

void GpuCuller::Dispatch(WGPUCommandEncoder encoder, const core::Frustum& frustum) {
    if (!encoder || !computePipeline_ || !bindGroup_ || primitiveCount_ == 0)
        return;

    GpuCullUniforms uniforms{};
    for (size_t i = 0; i < 6; ++i) {
        uniforms.frustumPlanes[i] = glm::vec4{frustum.planes[i].normal, frustum.planes[i].distance};
    }
    uniforms.primitiveCount = static_cast<uint32_t>(primitiveCount_);

    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &uniforms, sizeof(uniforms));
    const uint32_t zeroCount = 0;
    wgpuQueueWriteBuffer(queue_, atomicCounterBuffer_, 0, &zeroCount, sizeof(uint32_t));

    WGPUComputePassDescriptor cpDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    cpDesc.label = ToWgpuStringView("GpuCull_Pass");
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, &cpDesc);
    if (!pass)
        return;

    wgpuComputePassEncoderSetPipeline(pass, computePipeline_);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);

    const uint32_t workgroups = static_cast<uint32_t>((primitiveCount_ + 63) / 64);
    wgpuComputePassEncoderDispatchWorkgroups(pass, workgroups, 1, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
}

} // namespace bgl::gpu
