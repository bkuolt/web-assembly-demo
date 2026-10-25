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

@compute @workgroup_size(64)
fn cs_pick(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let count = u32(uniforms.rayDirection.w);
    if (index >= count) {
        return;
    }

    let b = bounds[index];
    let ro = uniforms.rayOrigin.xyz;
    let rd = uniforms.rayDirection.xyz;

    let invD = 1.0 / rd;
    let t0 = (b.minBound.xyz - ro) * invD;
    let t1 = (b.maxBound.xyz - ro) * invD;

    let tmin_v = min(t0, t1);
    let tmax_v = max(t0, t1);

    let tmin = max(max(tmin_v.x, tmin_v.y), tmin_v.z);
    let tmax = min(min(tmax_v.x, tmax_v.y), tmax_v.z);

    if (tmax >= max(tmin, 0.0)) {
        let hitDist = select(tmin, tmax, tmin < 0.0);
        if (hitDist > 0.0 && hitDist < uniforms.rayOrigin.w) {
            let distBits = bitcast<u32>(hitDist);
            let prevMin = atomicMin(&result.hitDistanceBits, distBits);
            if (distBits < prevMin) {
                atomicStore(&result.hitNodeIndex, b.nodeIndex);
                atomicStore(&result.hitPrimitiveIndex, b.primitiveIndex);
                atomicStore(&result.hitFound, 1u);
            }
        }
    }
}
)";

} // namespace

GpuRayPicker::~GpuRayPicker() {
    Shutdown();
}

GpuRayPicker::GpuRayPicker(GpuRayPicker&& other) noexcept
    : device_{other.device_}, queue_{other.queue_}, computePipeline_{other.computePipeline_},
      bindGroupLayout_{other.bindGroupLayout_}, bindGroup_{other.bindGroup_}, uniformBuffer_{other.uniformBuffer_},
      resultBuffer_{other.resultBuffer_}, cachedBoundsBuffer_{other.cachedBoundsBuffer_} {
    other.device_ = nullptr;
    other.queue_ = nullptr;
    other.computePipeline_ = nullptr;
    other.bindGroupLayout_ = nullptr;
    other.bindGroup_ = nullptr;
    other.uniformBuffer_ = nullptr;
    other.resultBuffer_ = nullptr;
    other.cachedBoundsBuffer_ = nullptr;
}

GpuRayPicker& GpuRayPicker::operator=(GpuRayPicker&& other) noexcept {
    if (this != &other) {
        Shutdown();
        device_ = other.device_;
        queue_ = other.queue_;
        computePipeline_ = other.computePipeline_;
        bindGroupLayout_ = other.bindGroupLayout_;
        bindGroup_ = other.bindGroup_;
        uniformBuffer_ = other.uniformBuffer_;
        resultBuffer_ = other.resultBuffer_;
        cachedBoundsBuffer_ = other.cachedBoundsBuffer_;

        other.device_ = nullptr;
        other.queue_ = nullptr;
        other.computePipeline_ = nullptr;
        other.bindGroupLayout_ = nullptr;
        other.bindGroup_ = nullptr;
        other.uniformBuffer_ = nullptr;
        other.resultBuffer_ = nullptr;
        other.cachedBoundsBuffer_ = nullptr;
    }
    return *this;
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
    if (computePipeline_) {
        wgpuComputePipelineRelease(computePipeline_);
        computePipeline_ = nullptr;
    }
    if (bindGroupLayout_) {
        wgpuBindGroupLayoutRelease(bindGroupLayout_);
        bindGroupLayout_ = nullptr;
    }
    cachedBoundsBuffer_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> GpuRayPicker::Initialize(WGPUDevice device, WGPUQueue queue) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize GpuRayPicker with null WebGPU device or queue");
    }
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

    // 3. Compute Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout_;
    plDesc.label = ToWgpuStringView("GpuRayPick_PipelineLayout");
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(device_, &plDesc);

    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = pl;
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = ToWgpuStringView("cs_pick");
    cpDesc.label = ToWgpuStringView("GpuRayPick_ComputePipeline");

    computePipeline_ = wgpuDeviceCreateComputePipeline(device_, &cpDesc);
    wgpuPipelineLayoutRelease(pl);
    wgpuShaderModuleRelease(shaderModule);

    if (!computePipeline_) {
        return std::unexpected("Failed to create GPU RayPicker compute pipeline");
    }

    // 4. Uniform & Result Buffers
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(GpuRayPickUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("GpuRayPick_Uniforms");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(GpuRayPickUniforms));

    WGPUBufferDescriptor resDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    resDesc.size = sizeof(GpuRayPickResult);
    resDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    resDesc.label = ToWgpuStringView("GpuRayPick_Result");
    resultBuffer_ = wgpuDeviceCreateBuffer(device_, &resDesc);
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(GpuRayPickResult));

    BGL_LOG_INFO("GpuRayPicker initialized successfully (WebGPU Compute Parallel Ray Picker)");
    return {};
}

void GpuRayPicker::Dispatch(WGPUCommandEncoder encoder, WGPUBuffer boundsBuffer, uint32_t primitiveCount,
                            const core::Ray& ray) {
    if (!encoder || !computePipeline_ || !boundsBuffer || primitiveCount == 0)
        return;

    if (cachedBoundsBuffer_ != boundsBuffer || !bindGroup_) {
        cachedBoundsBuffer_ = boundsBuffer;
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
        bgEntries[1].size = primitiveCount * sizeof(glm::vec4) * 3;

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

    WGPUComputePassDescriptor cpDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    cpDesc.label = ToWgpuStringView("GpuRayPick_Pass");
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, &cpDesc);
    if (!pass)
        return;

    wgpuComputePassEncoderSetPipeline(pass, computePipeline_);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);

    const uint32_t workgroups = (primitiveCount + 63) / 64;
    wgpuComputePassEncoderDispatchWorkgroups(pass, workgroups, 1, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
}

} // namespace bgl::gpu
