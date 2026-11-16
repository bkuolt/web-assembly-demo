/**
 * @file GpuRayPicker.cpp
 * @brief Implementation of WebGPU Compute-shader-driven parallel Ray-Bounding & Geometry Picking.
 */
#include "gpu/GpuRayPicker.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/Handle.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <cstring>

namespace bgl::gpu {

GpuRayPicker::~GpuRayPicker() {
    Shutdown();
}

GpuRayPicker::GpuRayPicker(GpuRayPicker&& other) noexcept {
    MoveFrom(other);
}

GpuRayPicker& GpuRayPicker::operator=(GpuRayPicker&& other) noexcept {
    if (this != &other) {
        Shutdown();
        MoveFrom(other);
    }
    return *this;
}

void GpuRayPicker::MoveFrom(GpuRayPicker& other) noexcept {
    device_ = std::exchange(other.device_, nullptr);
    queue_ = std::exchange(other.queue_, nullptr);
    computePipeline_ = std::move(other.computePipeline_);
    resolvePipeline_ = std::move(other.resolvePipeline_);
    pipelineLayout_ = std::move(other.pipelineLayout_);
    bindGroupLayout_ = std::move(other.bindGroupLayout_);
    bindGroup_ = std::move(other.bindGroup_);
    uniformBuffer_ = std::move(other.uniformBuffer_);
    resultBuffer_ = std::move(other.resultBuffer_);
    cachedBoundsBuffer_ = std::exchange(other.cachedBoundsBuffer_, nullptr);
    cachedPrimitiveCount_ = std::exchange(other.cachedPrimitiveCount_, 0);
}

void GpuRayPicker::Shutdown() noexcept {
    bindGroup_.Reset();
    if (resultBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(GpuRayPickResult));
        resultBuffer_.Reset();
    }
    if (uniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(GpuRayPickUniforms));
        uniformBuffer_.Reset();
    }
    resolvePipeline_.Reset();
    computePipeline_.Reset();
    pipelineLayout_.Reset();
    bindGroupLayout_.Reset();
    cachedBoundsBuffer_ = nullptr;
    cachedPrimitiveCount_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> GpuRayPicker::CreateComputePipelines(WGPUShaderModule shaderModule) {
    if (!device_ || !shaderModule || !pipelineLayout_) {
        return std::unexpected("Cannot create GPU RayPicker compute pipelines with invalid state");
    }

    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = pipelineLayout_.Get();
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = ToWgpuStringView("cs_pick");
    cpDesc.label = ToWgpuStringView("GpuRayPick_ComputePipeline");

    ComputePipelineHandle nextCompute{wgpuDeviceCreateComputePipeline(device_, &cpDesc)};

    WGPUComputePipelineDescriptor resolveDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    resolveDesc.layout = pipelineLayout_.Get();
    resolveDesc.compute.module = shaderModule;
    resolveDesc.compute.entryPoint = ToWgpuStringView("cs_resolve");
    resolveDesc.label = ToWgpuStringView("GpuRayPick_ResolvePipeline");
    ComputePipelineHandle nextResolve{wgpuDeviceCreateComputePipeline(device_, &resolveDesc)};

    if (!nextCompute || !nextResolve) {
        return std::unexpected("Failed to create GPU RayPicker compute pipelines");
    }

    computePipeline_ = std::move(nextCompute);
    resolvePipeline_ = std::move(nextResolve);
    return {};
}

core::Result<void> GpuRayPicker::ReloadShader() {
    if (!device_ || !pipelineLayout_) {
        return std::unexpected("GpuRayPicker not initialized");
    }
    auto modRes =
        ShaderManager::Instance().LoadAndCompile(device_, "gpu_ray_pick.wgsl", "GpuRayPick_ComputeModule", true);
    if (!modRes) {
        return std::unexpected(modRes.error());
    }
    return CreateComputePipelines(modRes->Get());
}

core::Result<void> GpuRayPicker::Initialize(WGPUDevice device, WGPUQueue queue) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize GpuRayPicker with null WebGPU device or queue");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;

    // 1. Load & Compile WGSL Shader Module
    auto modRes = ShaderManager::Instance().LoadAndCompile(device_, "gpu_ray_pick.wgsl", "GpuRayPick_ComputeModule");
    if (!modRes) {
        Shutdown();
        return std::unexpected(modRes.error());
    }
    ShaderModuleHandle shaderModule{std::move(*modRes)};

    // 2. Bind Group Layout
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    // Uniforms
    entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Compute;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(GpuRayPickUniforms);

    // Bounds Storage (read: GpuBoundingBox array, each element is 48 bytes)
    entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    entries[1].buffer.minBindingSize = 48;

    // Output Result (read_write atomic)
    entries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Compute;
    entries[2].buffer.type = WGPUBufferBindingType_Storage;
    entries[2].buffer.minBindingSize = sizeof(GpuRayPickResult);

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = entries.size();
    bglDesc.entries = entries.data();
    bglDesc.label = ToWgpuStringView("GpuRayPick_BindGroupLayout");
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!bindGroupLayout_) {
        Shutdown();
        return std::unexpected("Failed to create GPU RayPicker bind group layout");
    }

    // 3. Compute Pipeline Layout & Pipelines
    const WGPUBindGroupLayout bglRaw{bindGroupLayout_.Get()};
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bglRaw;
    plDesc.label = ToWgpuStringView("GpuRayPick_PipelineLayout");
    pipelineLayout_ = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!pipelineLayout_) {
        Shutdown();
        return std::unexpected("Failed to create GPU RayPicker pipeline layout");
    }

    if (auto pipeRes = CreateComputePipelines(shaderModule.Get()); !pipeRes) {
        Shutdown();
        return std::unexpected(pipeRes.error());
    }

    // 4. Uniform & Result Buffers
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(GpuRayPickUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("GpuRayPick_Uniforms");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    if (!uniformBuffer_) {
        Shutdown();
        return std::unexpected("Failed to create GPU RayPicker uniform buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(GpuRayPickUniforms));

    WGPUBufferDescriptor resDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    resDesc.size = sizeof(GpuRayPickResult);
    resDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    resDesc.label = ToWgpuStringView("GpuRayPick_Result");
    resultBuffer_ = wgpuDeviceCreateBuffer(device_, &resDesc);
    if (!resultBuffer_) {
        Shutdown();
        return std::unexpected("Failed to create GPU RayPicker result buffer");
    }
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(GpuRayPickResult));

    BGL_LOG_INFO("GpuRayPicker initialized successfully (WebGPU Compute Parallel Ray Picker)");
    return {};
}

void GpuRayPicker::Dispatch(WGPUCommandEncoder encoder, WGPUBuffer boundsBuffer, uint32_t primitiveCount,
                            const core::Ray& ray) {
    if (!encoder || !computePipeline_ || !resolvePipeline_ || !boundsBuffer || !uniformBuffer_ || !resultBuffer_ ||
        primitiveCount == 0)
        return;

    if (cachedBoundsBuffer_ != boundsBuffer || cachedPrimitiveCount_ != primitiveCount || !bindGroup_) {
        cachedBoundsBuffer_ = boundsBuffer;
        cachedPrimitiveCount_ = primitiveCount;
        bindGroup_.Reset();

        std::array<WGPUBindGroupEntry, 3> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = uniformBuffer_.Get();
        bgEntries[0].offset = 0;
        bgEntries[0].size = sizeof(GpuRayPickUniforms);

        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].buffer = boundsBuffer;
        bgEntries[1].offset = 0;
        bgEntries[1].size = static_cast<uint64_t>(primitiveCount) * sizeof(glm::vec4) * 3;

        bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[2].binding = 2;
        bgEntries[2].buffer = resultBuffer_.Get();
        bgEntries[2].offset = 0;
        bgEntries[2].size = sizeof(GpuRayPickResult);

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = bindGroupLayout_.Get();
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        bgDesc.label = ToWgpuStringView("GpuRayPick_BindGroup");
        bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
        if (!bindGroup_) {
            cachedBoundsBuffer_ = nullptr;
            return;
        }
    }

    GpuRayPickUniforms uniforms{};
    uniforms.rayOrigin = glm::vec4{ray.origin, 10000.0f};
    uniforms.rayDirection = glm::vec4{ray.direction, static_cast<float>(primitiveCount)};
    wgpuQueueWriteBuffer(queue_, uniformBuffer_.Get(), 0, &uniforms, sizeof(uniforms));

    GpuRayPickResult initialResult{};
    initialResult.hitNodeIndex = 0xFFFFFFFF;
    initialResult.hitPrimitiveIndex = 0xFFFFFFFF;
    initialResult.hitDistanceBits = 0x7F7FFFFF; // Max float
    initialResult.hitFound = 0;
    wgpuQueueWriteBuffer(queue_, resultBuffer_.Get(), 0, &initialResult, sizeof(initialResult));

    const uint32_t workgroups = (primitiveCount + 63) / 64;

    auto dispatchStage = [&](WGPUComputePipeline pipeline, const char* label) {
        WGPUComputePassDescriptor cpDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        cpDesc.label = ToWgpuStringView(label);
        ComputePassEncoderHandle pass{wgpuCommandEncoderBeginComputePass(encoder, &cpDesc)};
        if (!pass)
            return;
        wgpuComputePassEncoderSetPipeline(pass.Get(), pipeline);
        wgpuComputePassEncoderSetBindGroup(pass.Get(), 0, bindGroup_.Get(), 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass.Get(), workgroups, 1, 1);
        wgpuComputePassEncoderEnd(pass.Get());
    };

    // Two-pass atomic resolution prevents data races on winning node/primitive indices across workgroups.
    dispatchStage(computePipeline_.Get(), "GpuRayPick_DistancePass");
    dispatchStage(resolvePipeline_.Get(), "GpuRayPick_ResolvePass");
}

} // namespace bgl::gpu
