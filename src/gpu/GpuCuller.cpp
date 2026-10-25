/**
 * @file GpuCuller.cpp
 * @brief Implementation of WebGPU Compute-shader-driven Frustum & Spatial Bounding Culling.
 */
#include "gpu/GpuCuller.hpp"
#include "gpu/Handle.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include <algorithm>
#include <cstring>

namespace bgl::gpu {

namespace {

constexpr std::string_view kGpuCullComputeWGSL = R"(
struct GpuBoundingBox {
    minBound: vec4<f32>,
    maxBound: vec4<f32>,
    nodeIndex: u32,
    primitiveIndex: u32,
    pad0: u32,
    pad1: u32,
};

struct GpuCullUniforms {
    frustumPlanes: array<vec4<f32>, 6>,
    primitiveCount: u32,
    pad0: u32,
    pad1: u32,
    pad2: u32,
};

@group(0) @binding(0) var<uniform> uniforms: GpuCullUniforms;
@group(0) @binding(1) var<storage, read> bounds: array<GpuBoundingBox>;
@group(0) @binding(2) var<storage, read_write> visibility: array<u32>;
@group(0) @binding(3) var<storage, read_write> visibleCount: atomic<u32>;

@compute @workgroup_size(64)
fn cs_cull(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    if (index >= uniforms.primitiveCount) {
        return;
    }

    let b = bounds[index];
    let minB = b.minBound.xyz;
    let maxB = b.maxBound.xyz;

    var isVisible: u32 = 1u;
    for (var p: i32 = 0; p < 6; p++) {
        let plane = uniforms.frustumPlanes[p];
        let normal = plane.xyz;
        let d = plane.w;

        let pVertex = vec3<f32>(
            select(minB.x, maxB.x, normal.x >= 0.0),
            select(minB.y, maxB.y, normal.y >= 0.0),
            select(minB.z, maxB.z, normal.z >= 0.0)
        );

        if (dot(normal, pVertex) + d < 0.0) {
            isVisible = 0u;
            break;
        }
    }

    visibility[index] = isVisible;
    if (isVisible == 1u) {
        atomicAdd(&visibleCount, 1u);
    }
}
)";

} // namespace

GpuCuller::~GpuCuller() {
    Shutdown();
}

GpuCuller::GpuCuller(GpuCuller&& other) noexcept
    : device_{other.device_}
    , queue_{other.queue_}
    , computePipeline_{other.computePipeline_}
    , bindGroupLayout_{other.bindGroupLayout_}
    , bindGroup_{other.bindGroup_}
    , uniformBuffer_{other.uniformBuffer_}
    , boundsStorageBuffer_{other.boundsStorageBuffer_}
    , visibilityStorageBuffer_{other.visibilityStorageBuffer_}
    , atomicCounterBuffer_{other.atomicCounterBuffer_}
    , primitiveCount_{other.primitiveCount_}
    , boundsBufferSize_{other.boundsBufferSize_}
    , cpuBounds_{std::move(other.cpuBounds_)}
{
    other.device_ = nullptr;
    other.queue_ = nullptr;
    other.computePipeline_ = nullptr;
    other.bindGroupLayout_ = nullptr;
    other.bindGroup_ = nullptr;
    other.uniformBuffer_ = nullptr;
    other.boundsStorageBuffer_ = nullptr;
    other.visibilityStorageBuffer_ = nullptr;
    other.atomicCounterBuffer_ = nullptr;
    other.primitiveCount_ = 0;
    other.boundsBufferSize_ = 0;
}

GpuCuller& GpuCuller::operator=(GpuCuller&& other) noexcept {
    if (this != &other) {
        Shutdown();
        device_ = other.device_;
        queue_ = other.queue_;
        computePipeline_ = other.computePipeline_;
        bindGroupLayout_ = other.bindGroupLayout_;
        bindGroup_ = other.bindGroup_;
        uniformBuffer_ = other.uniformBuffer_;
        boundsStorageBuffer_ = other.boundsStorageBuffer_;
        visibilityStorageBuffer_ = other.visibilityStorageBuffer_;
        atomicCounterBuffer_ = other.atomicCounterBuffer_;
        primitiveCount_ = other.primitiveCount_;
        boundsBufferSize_ = other.boundsBufferSize_;
        cpuBounds_ = std::move(other.cpuBounds_);

        other.device_ = nullptr;
        other.queue_ = nullptr;
        other.computePipeline_ = nullptr;
        other.bindGroupLayout_ = nullptr;
        other.bindGroup_ = nullptr;
        other.uniformBuffer_ = nullptr;
        other.boundsStorageBuffer_ = nullptr;
        other.visibilityStorageBuffer_ = nullptr;
        other.atomicCounterBuffer_ = nullptr;
        other.primitiveCount_ = 0;
        other.boundsBufferSize_ = 0;
    }
    return *this;
}

void GpuCuller::Shutdown() noexcept {
    if (bindGroup_) {
        wgpuBindGroupRelease(bindGroup_);
        bindGroup_ = nullptr;
    }
    if (atomicCounterBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(sizeof(uint32_t));
        wgpuBufferRelease(atomicCounterBuffer_);
        atomicCounterBuffer_ = nullptr;
    }
    if (visibilityStorageBuffer_) {
        core::MemoryTracker::Instance().TrackBufferFree(boundsBufferSize_);
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
    if (bindGroupLayout_) {
        wgpuBindGroupLayoutRelease(bindGroupLayout_);
        bindGroupLayout_ = nullptr;
    }
    cpuBounds_.clear();
    primitiveCount_ = 0;
    boundsBufferSize_ = 0;
    device_ = nullptr;
    queue_ = nullptr;
}

core::Result<void> GpuCuller::Initialize(WGPUDevice device, WGPUQueue queue) {
    if (!device || !queue) {
        return std::unexpected("Cannot initialize GpuCuller with null WebGPU device or queue");
    }
    device_ = device;
    queue_ = queue;

    // 1. Create WGSL Compute Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.chain.next = nullptr;
    wgslDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc.code = ToWgpuStringView(kGpuCullComputeWGSL.data());

    WGPUShaderModuleDescriptor shaderDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDesc.nextInChain = &wgslDesc.chain;
    shaderDesc.label = ToWgpuStringView("GpuCull_ComputeModule");

    WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(device_, &shaderDesc);
    if (!shaderModule) {
        return std::unexpected("Failed to compile GPU culler WGSL compute module");
    }

    // 2. Bind Group Layout
    std::array<WGPUBindGroupLayoutEntry, 4> entries{};
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

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = entries.size();
    bglDesc.entries = entries.data();
    bglDesc.label = ToWgpuStringView("GpuCull_BindGroupLayout");
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    // 3. Compute Pipeline Layout
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &bindGroupLayout_;
    plDesc.label = ToWgpuStringView("GpuCull_PipelineLayout");
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(device_, &plDesc);

    WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cpDesc.layout = pl;
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = ToWgpuStringView("cs_cull");
    cpDesc.label = ToWgpuStringView("GpuCull_ComputePipeline");

    computePipeline_ = wgpuDeviceCreateComputePipeline(device_, &cpDesc);
    wgpuPipelineLayoutRelease(pl);
    wgpuShaderModuleRelease(shaderModule);

    if (!computePipeline_) {
        return std::unexpected("Failed to create GPU culler compute pipeline");
    }

    // 4. Uniform & Counter Buffers
    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(GpuCullUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("GpuCull_Uniforms");
    uniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(GpuCullUniforms));

    WGPUBufferDescriptor countDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    countDesc.size = sizeof(uint32_t);
    countDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    countDesc.label = ToWgpuStringView("GpuCull_AtomicCounter");
    atomicCounterBuffer_ = wgpuDeviceCreateBuffer(device_, &countDesc);
    core::MemoryTracker::Instance().TrackBufferAlloc(sizeof(uint32_t));

    BGL_LOG_INFO("GpuCuller initialized successfully (WebGPU Compute Shader Frustum Culling)");
    return {};
}

void GpuCuller::UploadSceneBounds(const scene::Scene& scene) {
    cpuBounds_.clear();
    auto collectBounds = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentMat) -> void {
        const glm::mat4 worldMat = parentMat * node.localTransform;
        auto prims = node.GetPrimitives();
        for (size_t pIdx = 0; pIdx < prims.size(); ++pIdx) {
            const auto& prim = prims[pIdx];
            if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                const auto worldBox = prim.boundingBox.Transformed(worldMat);
                GpuBoundingBox box{};
                box.minBound = glm::vec4{worldBox.minBound, 1.0f};
                box.maxBound = glm::vec4{worldBox.maxBound, 1.0f};
                box.nodeIndex = static_cast<uint32_t>(node.nodeIndex >= 0 ? node.nodeIndex : 0);
                box.primitiveIndex = static_cast<uint32_t>(pIdx);
                cpuBounds_.push_back(box);
            }
        }
        for (const auto& child : node.children) {
            if (child) self(self, *child, worldMat);
        }
    };

    for (const auto& root : scene.rootNodes) {
        if (root) collectBounds(collectBounds, *root, glm::mat4{1.0f});
    }

    primitiveCount_ = cpuBounds_.size();
    if (primitiveCount_ == 0) return;

    const size_t neededBytes = primitiveCount_ * sizeof(GpuBoundingBox);
    const size_t visNeededBytes = primitiveCount_ * sizeof(uint32_t);

    if (neededBytes > boundsBufferSize_ || !boundsStorageBuffer_) {
        if (boundsStorageBuffer_) {
            core::MemoryTracker::Instance().TrackBufferFree(boundsBufferSize_);
            wgpuBufferRelease(boundsStorageBuffer_);
        }
        if (visibilityStorageBuffer_) {
            core::MemoryTracker::Instance().TrackBufferFree(boundsBufferSize_);
            wgpuBufferRelease(visibilityStorageBuffer_);
        }

        boundsBufferSize_ = std::max(neededBytes * 2, size_t{64 * 1024});

        WGPUBufferDescriptor bDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        bDesc.size = boundsBufferSize_;
        bDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        bDesc.label = ToWgpuStringView("GpuCull_BoundsStorage");
        boundsStorageBuffer_ = wgpuDeviceCreateBuffer(device_, &bDesc);
        core::MemoryTracker::Instance().TrackBufferAlloc(boundsBufferSize_);

        WGPUBufferDescriptor vDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        vDesc.size = std::max(visNeededBytes * 2, size_t{16 * 1024});
        vDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
        vDesc.label = ToWgpuStringView("GpuCull_VisibilityStorage");
        visibilityStorageBuffer_ = wgpuDeviceCreateBuffer(device_, &vDesc);
        core::MemoryTracker::Instance().TrackBufferAlloc(vDesc.size);

        if (bindGroup_) wgpuBindGroupRelease(bindGroup_);

        std::array<WGPUBindGroupEntry, 4> bgEntries{};
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
        bgEntries[2].size = vDesc.size;

        bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[3].binding = 3;
        bgEntries[3].buffer = atomicCounterBuffer_;
        bgEntries[3].offset = 0;
        bgEntries[3].size = sizeof(uint32_t);

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = bindGroupLayout_;
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        bgDesc.label = ToWgpuStringView("GpuCull_BindGroup");
        bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    }

    wgpuQueueWriteBuffer(queue_, boundsStorageBuffer_, 0, cpuBounds_.data(), neededBytes);
}

void GpuCuller::Dispatch(WGPUCommandEncoder encoder, const core::Frustum& frustum) {
    if (!encoder || !computePipeline_ || !bindGroup_ || primitiveCount_ == 0) return;

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
    if (!pass) return;

    wgpuComputePassEncoderSetPipeline(pass, computePipeline_);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup_, 0, nullptr);

    const uint32_t workgroups = static_cast<uint32_t>((primitiveCount_ + 63) / 64);
    wgpuComputePassEncoderDispatchWorkgroups(pass, workgroups, 1, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
}

} // namespace bgl::gpu
