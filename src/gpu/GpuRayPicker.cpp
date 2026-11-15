/**
 * @file GpuRayPicker.cpp
 * @brief Implementation of WebGPU Compute-shader-driven parallel Ray-Bounding & Geometry Picking.
 */
#include "gpu/GpuRayPicker.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "gpu/Handle.hpp"
#include <algorithm>
#include <cstring>

namespace bgl::gpu {

namespace {

constexpr std::string_view kGpuRayPickComputeWGSL = R"(
struct GpuBoundingBox {
    minBound: vec4<f32>,
    maxBound: vec4<f32>,
    nodeIndex: u32,
    primitiveIndex: u32,
    pad0: u32,
    pad1: u32,
};

struct GpuRayPickUniforms {
    rayOrigin: vec4<f32>,
    rayDirection: vec4<f32>,
};

struct GpuRayPickResult {
    hitNodeIndex: atomic<u32>,
    hitPrimitiveIndex: atomic<u32>,
    hitDistanceBits: atomic<u32>,
    hitFound: atomic<u32>,
};

@group(0) @binding(0) var<uniform> uniforms: GpuRayPickUniforms;
@group(0) @binding(1) var<storage, read> bounds: array<GpuBoundingBox>;
@group(0) @binding(2) var<storage, read_write> result: GpuRayPickResult;

fn intersectRayAabb(ro: vec3<f32>, rd: vec3<f32>, bmin: vec3<f32>, bmax: vec3<f32>, maxT: f32) -> f32 {
    var tmin : f32 = -1e30;
    var tmax : f32 = maxT;

    for (var axis : u32 = 0u; axis < 3u; axis = axis + 1u) {
        let d = rd[axis];
        if (abs(d) < 1e-8) {
            if (ro[axis] < bmin[axis] || ro[axis] > bmax[axis]) {
                return -1.0;
            }
        } else {
            let invD = 1.0 / d;
            let t0 = (bmin[axis] - ro[axis]) * invD;
            let t1 = (bmax[axis] - ro[axis]) * invD;
            tmin = max(tmin, min(t0, t1));
            tmax = min(tmax, max(t0, t1));
        }
    }

    if (tmax < max(tmin, 0.0)) {
        return -1.0;
    }
    let hitDist = select(tmin, tmax, tmin < 0.0);
    return select(-1.0, hitDist, hitDist > 0.0 && hitDist < maxT);
}

@compute @workgroup_size(64)
fn cs_pick(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let count = u32(uniforms.rayDirection.w);
    if (index >= count) {
        return;
    }

    let b = bounds[index];
    let hitDist = intersectRayAabb(uniforms.rayOrigin.xyz, uniforms.rayDirection.xyz, b.minBound.xyz, b.maxBound.xyz,
                                   uniforms.rayOrigin.w);
    if (hitDist > 0.0) {
        atomicMin(&result.hitDistanceBits, bitcast<u32>(hitDist));
    }
}

@compute @workgroup_size(64)
fn cs_resolve(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let count = u32(uniforms.rayDirection.w);
    if (index >= count) {
        return;
    }

    let minDistBits = atomicLoad(&result.hitDistanceBits);
    if (minDistBits == 0x7F7FFFFFu) {
        return;
    }

    let b = bounds[index];
    let hitDist = intersectRayAabb(uniforms.rayOrigin.xyz, uniforms.rayDirection.xyz, b.minBound.xyz, b.maxBound.xyz,
                                   uniforms.rayOrigin.w);
    if (hitDist > 0.0 && bitcast<u32>(hitDist) == minDistBits) {
        if (atomicExchange(&result.hitFound, 1u) == 0u) {
            atomicStore(&result.hitNodeIndex, b.nodeIndex);
            atomicStore(&result.hitPrimitiveIndex, b.primitiveIndex);
        }
    }
}
)";

} // namespace

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
    computePipeline_ = std::exchange(other.computePipeline_, nullptr);
    resolvePipeline_ = std::exchange(other.resolvePipeline_, nullptr);
    bindGroupLayout_ = std::exchange(other.bindGroupLayout_, nullptr);
    bindGroup_ = std::exchange(other.bindGroup_, nullptr);
    uniformBuffer_ = std::exchange(other.uniformBuffer_, nullptr);
    resultBuffer_ = std::exchange(other.resultBuffer_, nullptr);
    cachedBoundsBuffer_ = std::exchange(other.cachedBoundsBuffer_, nullptr);
    cachedPrimitiveCount_ = std::exchange(other.cachedPrimitiveCount_, 0);
}

void GpuRayPicker::Shutdown() noexcept {
    if (bindGroup_) {
        wgpuBindGroupRelease(bindGroup_);
        bindGroup_ = nullptr;
    }
    if (resultBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(GpuRayPickResult));
        wgpuBufferRelease(resultBuffer_);
        resultBuffer_ = nullptr;
    }
    if (uniformBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(GpuRayPickUniforms));
        wgpuBufferRelease(uniformBuffer_);
        uniformBuffer_ = nullptr;
    }
    if (resolvePipeline_) {
        wgpuComputePipelineRelease(resolvePipeline_);
        resolvePipeline_ = nullptr;
    }
    if (computePipeline_) {
        wgpuComputePipelineRelease(computePipeline_);
        computePipeline_ = nullptr;
    }
    if (bindGroupLayout_) {
        wgpuBindGroupLayoutRelease(bindGroupLayout_);
        bindGroupLayout_ = nullptr;
    }
    cachedBoundsBuffer_ = nullptr;
    cachedPrimitiveCount_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> GpuRayPicker::Initialize(WGPUDevice device, WGPUQueue queue) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize GpuRayPicker with null WebGPU device or queue");
    }
    Shutdown();
    device_ = device;
    queue_ = queue;

    // 1. Create WGSL Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWgpuStringView(kGpuRayPickComputeWGSL.data());

    WGPUShaderModuleDescriptor shaderDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDesc.nextInChain = &wgslDesc.chain;
    shaderDesc.label = ToWgpuStringView("GpuRayPick_ComputeModule");

    WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(device_, &shaderDesc);
    if (!shaderModule) {
        return std::unexpected("Failed to compile GPU RayPicker WGSL compute module");
    }

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
        wgpuShaderModuleRelease(shaderModule);
        Shutdown();
        return std::unexpected("Failed to create GPU RayPicker bind group layout");
    }

    // 3. Compute Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout_;
    plDesc.label = ToWgpuStringView("GpuRayPick_PipelineLayout");
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(device_, &plDesc);
    if (!pl) {
        wgpuShaderModuleRelease(shaderModule);
        Shutdown();
        return std::unexpected("Failed to create GPU RayPicker pipeline layout");
    }

    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = pl;
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = ToWgpuStringView("cs_pick");
    cpDesc.label = ToWgpuStringView("GpuRayPick_ComputePipeline");

    computePipeline_ = wgpuDeviceCreateComputePipeline(device_, &cpDesc);

    WGPUComputePipelineDescriptor resolveDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    resolveDesc.layout = pl;
    resolveDesc.compute.module = shaderModule;
    resolveDesc.compute.entryPoint = ToWgpuStringView("cs_resolve");
    resolveDesc.label = ToWgpuStringView("GpuRayPick_ResolvePipeline");
    resolvePipeline_ = wgpuDeviceCreateComputePipeline(device_, &resolveDesc);

    wgpuPipelineLayoutRelease(pl);
    wgpuShaderModuleRelease(shaderModule);

    if (!computePipeline_ || !resolvePipeline_) {
        Shutdown();
        return std::unexpected("Failed to create GPU RayPicker compute pipelines");
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
        if (bindGroup_)
            wgpuBindGroupRelease(bindGroup_);

        std::array<WGPUBindGroupEntry, 3> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = uniformBuffer_;
        bgEntries[0].offset = 0;
        bgEntries[0].size = sizeof(GpuRayPickUniforms);

        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].buffer = boundsBuffer;
        bgEntries[1].offset = 0;
        bgEntries[1].size = static_cast<uint64_t>(primitiveCount) * sizeof(glm::vec4) * 3;

        bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[2].binding = 2;
        bgEntries[2].buffer = resultBuffer_;
        bgEntries[2].offset = 0;
        bgEntries[2].size = sizeof(GpuRayPickResult);

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = bindGroupLayout_;
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
    wgpuQueueWriteBuffer(queue_, uniformBuffer_, 0, &uniforms, sizeof(uniforms));

    GpuRayPickResult initialResult{};
    initialResult.hitNodeIndex = 0xFFFFFFFF;
    initialResult.hitPrimitiveIndex = 0xFFFFFFFF;
    initialResult.hitDistanceBits = 0x7F7FFFFF; // Max float
    initialResult.hitFound = 0;
    wgpuQueueWriteBuffer(queue_, resultBuffer_, 0, &initialResult, sizeof(initialResult));

    const uint32_t workgroups = (primitiveCount + 63) / 64;

    auto dispatchStage = [&](WGPUComputePipeline pipeline, const char* label) {
        WGPUComputePassDescriptor cpDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        cpDesc.label = ToWgpuStringView(label);
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, &cpDesc);
        if (!pass)
            return;
        wgpuComputePassEncoderSetPipeline(pass, pipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, workgroups, 1, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
    };

    // Two-pass atomic resolution prevents data races on winning node/primitive indices across workgroups.
    dispatchStage(computePipeline_, "GpuRayPick_DistancePass");
    dispatchStage(resolvePipeline_, "GpuRayPick_ResolvePass");
}

} // namespace bgl::gpu
