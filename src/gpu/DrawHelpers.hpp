/**
 * @file DrawHelpers.hpp
 * @brief Shared helpers for populating per-draw uniforms, GPU skinning SSBOs, and morph weights across passes.
 */
#pragma once

#include "core/MemoryTracker.hpp"
#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "gpu/Uniforms.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <string_view>

namespace bgl::gpu {

struct PreparedDrawBindings {
    PerDrawUniforms uniforms{};
    WGPUBindGroup skinBindGroup{nullptr};
};

/**
 * @brief Populates per-draw transform/skinning/morph uniforms and uploads cached skin matrices when needed.
 */
inline void PopulatePerDrawMorphAndSkin(WGPUQueue queue, scene::MeshPrimitive& prim, const scene::SceneNode* node,
                                        const glm::mat4& worldMat, const glm::mat4& viewProj,
                                        const glm::mat4& lightViewProj, uint32_t baseDrawFlags, float lodTransition,
                                        std::span<const scene::Skin> skins,
                                        std::span<const std::shared_ptr<scene::SceneNode>> allNodes,
                                        uint64_t skinFrameId, WGPUBindGroup dummySkinBg,
                                        PreparedDrawBindings& out) noexcept {
    out.uniforms = PerDrawUniforms{};
    out.uniforms.viewProj = viewProj;
    out.uniforms.model = worldMat;
    out.uniforms.lightViewProj = lightViewProj;
    out.uniforms.drawParams = glm::vec4{lodTransition, 0.0f, 0.0f, 0.0f};
    uint32_t flags{baseDrawFlags};

    if (prim.morphDirty && !prim.gpuMorphDeltaBuffer && prim.gpuVertexBuffer && !prim.vertices.empty()) {
        wgpuQueueWriteBuffer(queue, prim.gpuVertexBuffer, 0, prim.vertices.data(),
                             prim.vertices.size() * sizeof(core::Vertex));
        prim.morphDirty = false;
    }

    if (node && prim.gpuMorphDeltaBuffer && prim.gpuMorphTargetCount > 0 && !node->morphWeights.empty()) {
        const uint32_t count{
            std::min<uint32_t>({static_cast<uint32_t>(node->morphWeights.size()), prim.gpuMorphTargetCount, 8u})};
        bool hasActiveMorph{false};
        for (uint32_t m{0}; m < count; ++m) {
            const float w{node->morphWeights[m]};
            if (m < 4u) {
                out.uniforms.morphWeights0[static_cast<glm::length_t>(m)] = w;
            } else {
                out.uniforms.morphWeights1[static_cast<glm::length_t>(m - 4u)] = w;
            }
            if (std::abs(w) > 1e-6f) {
                hasActiveMorph = true;
            }
        }
        if (hasActiveMorph) {
            out.uniforms.morphTargetCount = count;
            out.uniforms.vertexCount = prim.gpuMorphVertexCount;
            flags |= MaterialFlags::HasMorphTargets;
        }
    }

    out.skinBindGroup = dummySkinBg;
    if (node && node->skinIndex >= 0 && static_cast<size_t>(node->skinIndex) < skins.size()) {
        const auto& skin = skins[static_cast<size_t>(node->skinIndex)];
        if (!skin.joints.empty() && skin.gpuJointBuffer && skin.gpuBindGroup) {
            const auto jointsSpan = skin.GetOrComputeJointMatrices(skinFrameId, node, worldMat, allNodes);
            if (!jointsSpan.empty()) {
                if (skin.NeedsGpuUpload(skinFrameId, node)) {
                    wgpuQueueWriteBuffer(queue, skin.gpuJointBuffer, 0, jointsSpan.data(),
                                         jointsSpan.size() * sizeof(glm::mat4));
                    skin.MarkGpuUploaded(skinFrameId, node);
                }
                out.uniforms.jointCount = static_cast<uint32_t>(jointsSpan.size());
                out.skinBindGroup = skin.gpuBindGroup;
                flags |= MaterialFlags::HasSkinning;
            }
        }
    }

    out.uniforms.drawFlags = flags;
}

/**
 * @struct VertexAndInstanceLayoutStorage
 * @brief Non-movable storage backing the 2-slot vertex + instance buffer layout (locations 0..9).
 */
struct VertexAndInstanceLayoutStorage {
    std::array<WGPUVertexAttribute, 6> vertexAttributes{};
    std::array<WGPUVertexAttribute, 4> instanceAttributes{};
    std::array<WGPUVertexBufferLayout, 2> layouts{};

    VertexAndInstanceLayoutStorage() = default;
    ~VertexAndInstanceLayoutStorage() = default;
    VertexAndInstanceLayoutStorage(const VertexAndInstanceLayoutStorage&) = delete;
    VertexAndInstanceLayoutStorage& operator=(const VertexAndInstanceLayoutStorage&) = delete;
    VertexAndInstanceLayoutStorage(VertexAndInstanceLayoutStorage&&) = delete;
    VertexAndInstanceLayoutStorage& operator=(VertexAndInstanceLayoutStorage&&) = delete;
};

/**
 * @brief Populates Slot 0 (per-vertex core::Vertex, @location 0..5) and Slot 1 (per-instance mat4, @location 6..9).
 */
inline void FillVertexAndInstanceLayouts(VertexAndInstanceLayoutStorage& storage) noexcept {
    const auto makeAttr = [](WGPUVertexFormat fmt, uint64_t offset, uint32_t location) noexcept {
        WGPUVertexAttribute attr{};
        attr.format = fmt;
        attr.offset = offset;
        attr.shaderLocation = location;
        return attr;
    };

    storage.vertexAttributes[0] = makeAttr(WGPUVertexFormat_Float32x3, offsetof(core::Vertex, position), 0);
    storage.vertexAttributes[1] = makeAttr(WGPUVertexFormat_Float32x3, offsetof(core::Vertex, normal), 1);
    storage.vertexAttributes[2] = makeAttr(WGPUVertexFormat_Float32x4, offsetof(core::Vertex, tangent), 2);
    storage.vertexAttributes[3] = makeAttr(WGPUVertexFormat_Float32x2, offsetof(core::Vertex, uv), 3);
    storage.vertexAttributes[4] = makeAttr(WGPUVertexFormat_Uint32x4, offsetof(core::Vertex, joints), 4);
    storage.vertexAttributes[5] = makeAttr(WGPUVertexFormat_Float32x4, offsetof(core::Vertex, weights), 5);

    storage.layouts[0] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    storage.layouts[0].arrayStride = sizeof(core::Vertex);
    storage.layouts[0].stepMode = WGPUVertexStepMode_Vertex;
    storage.layouts[0].attributeCount = static_cast<uint32_t>(storage.vertexAttributes.size());
    storage.layouts[0].attributes = storage.vertexAttributes.data();

    for (uint32_t col = 0; col < 4; ++col) {
        storage.instanceAttributes[col] =
            makeAttr(WGPUVertexFormat_Float32x4, static_cast<uint64_t>(col) * sizeof(glm::vec4), 6u + col);
    }

    storage.layouts[1] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    storage.layouts[1].arrayStride = sizeof(glm::mat4);
    storage.layouts[1].stepMode = WGPUVertexStepMode_Instance;
    storage.layouts[1].attributeCount = static_cast<uint32_t>(storage.instanceAttributes.size());
    storage.layouts[1].attributes = storage.instanceAttributes.data();
}

/**
 * @class DynamicInstanceBuffer
 * @brief Dynamic WebGPU slot-1 instance matrix vertex buffer keeping an identity matrix permanently at index 0.
 *
 * Keeping `glm::mat4(1.0f)` at byte offset 0 allows non-instanced draws (`startIdx = 0`) and batched instanced
 * draws (`startIdx >= 1`) to share the same 2-slot pipeline without `firstInstance` indirect draw extensions.
 */
class DynamicInstanceBuffer {
public:
    DynamicInstanceBuffer() = default;
    ~DynamicInstanceBuffer() { Shutdown(); }

    DynamicInstanceBuffer(const DynamicInstanceBuffer&) = delete;
    DynamicInstanceBuffer& operator=(const DynamicInstanceBuffer&) = delete;

    DynamicInstanceBuffer(DynamicInstanceBuffer&& other) noexcept { MoveFrom(other); }

    DynamicInstanceBuffer& operator=(DynamicInstanceBuffer&& other) noexcept {
        if (this != &other) {
            Shutdown();
            MoveFrom(other);
        }
        return *this;
    }

    [[nodiscard]] core::Result<void> Initialize(WGPUDevice device, size_t initialInstanceCapacity = 256,
                                                std::string_view label = "DynamicInstanceBuffer") {
        if (!device) {
            return std::unexpected("Cannot initialize DynamicInstanceBuffer with null device");
        }
        device_ = device;
        label_ = std::string(label);
        const size_t minCapacity = std::max<size_t>(initialInstanceCapacity, 1);
        return EnsureCapacityBytes(nullptr, minCapacity * sizeof(glm::mat4), true);
    }

    void Shutdown() noexcept {
        ReleaseBuffer();
        device_ = nullptr;
        capacityBytes_ = 0;
        identityWritten_ = false;
    }

    void Upload(WGPUQueue queue, std::span<const glm::mat4> instances) {
        if (!device_ || !queue) {
            return;
        }
        const size_t requiredBytes = (1u + instances.size()) * sizeof(glm::mat4);
        const bool resized = !buffer_ || requiredBytes > capacityBytes_;
        if (resized) {
            size_t nextBytes = std::max<size_t>(sizeof(glm::mat4) * 64u, capacityBytes_ == 0 ? 16384u : capacityBytes_);
            while (nextBytes < requiredBytes) {
                nextBytes *= 2u;
            }
            if (!EnsureCapacityBytes(queue, nextBytes, true)) {
                return;
            }
        } else if (!identityWritten_) {
            const glm::mat4 identity{1.0f};
            wgpuQueueWriteBuffer(queue, buffer_.Get(), 0, &identity, sizeof(glm::mat4));
            identityWritten_ = true;
        }

        if (!instances.empty() && buffer_) {
            wgpuQueueWriteBuffer(queue, buffer_.Get(), sizeof(glm::mat4), instances.data(),
                                 instances.size() * sizeof(glm::mat4));
        }
    }

    [[nodiscard]] WGPUBuffer GetBuffer() const noexcept { return buffer_.Get(); }
    [[nodiscard]] size_t GetCapacityBytes() const noexcept { return capacityBytes_; }

    [[nodiscard]] static constexpr uint64_t GetByteOffset(uint32_t instanceStartIndex) noexcept {
        return static_cast<uint64_t>(instanceStartIndex) * sizeof(glm::mat4);
    }

    [[nodiscard]] static constexpr uint64_t GetSliceSize(uint32_t instanceCount) noexcept {
        return static_cast<uint64_t>(std::max(1u, instanceCount)) * sizeof(glm::mat4);
    }

private:
    void MoveFrom(DynamicInstanceBuffer& other) noexcept {
        device_ = other.device_;
        buffer_ = std::move(other.buffer_);
        capacityBytes_ = other.capacityBytes_;
        identityWritten_ = other.identityWritten_;
        label_ = std::move(other.label_);
        other.device_ = nullptr;
        other.capacityBytes_ = 0;
        other.identityWritten_ = false;
    }

    void ReleaseBuffer() noexcept {
        if (buffer_) {
            if (capacityBytes_ > 0) {
                core::MemoryTracker::Instance().TrackBufferFree(capacityBytes_);
            }
            buffer_.Reset();
        }
        capacityBytes_ = 0;
        identityWritten_ = false;
    }

    [[nodiscard]] core::Result<void> EnsureCapacityBytes(WGPUQueue queue, size_t newCapacityBytes,
                                                         bool mappedIdentity) {
        ReleaseBuffer();
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        desc.size = newCapacityBytes;
        desc.label = ToWgpuStringView(label_);
        desc.mappedAtCreation = (mappedIdentity && queue == nullptr) ? 1u : 0u;

        BufferHandle nextBuf{wgpuDeviceCreateBuffer(device_, &desc)};
        if (!nextBuf) {
            return std::unexpected("Failed to create DynamicInstanceBuffer");
        }
        if (desc.mappedAtCreation) {
            void* mapped = wgpuBufferGetMappedRange(nextBuf.Get(), 0, sizeof(glm::mat4));
            if (mapped) {
                const glm::mat4 identity{1.0f};
                std::memcpy(mapped, &identity, sizeof(glm::mat4));
            }
            wgpuBufferUnmap(nextBuf.Get());
            identityWritten_ = true;
        } else if (queue != nullptr) {
            const glm::mat4 identity{1.0f};
            wgpuQueueWriteBuffer(queue, nextBuf.Get(), 0, &identity, sizeof(glm::mat4));
            identityWritten_ = true;
        }

        core::MemoryTracker::Instance().TrackBufferAlloc(newCapacityBytes);
        buffer_ = std::move(nextBuf);
        capacityBytes_ = newCapacityBytes;
        return {};
    }

    WGPUDevice device_{nullptr};
    BufferHandle buffer_{};
    size_t capacityBytes_{0};
    bool identityWritten_{false};
    std::string label_{"DynamicInstanceBuffer"};
};

} // namespace bgl::gpu
