/**
 * @file RayTracingScene.hpp
 * @brief Desktop 2-level BVH (BLAS + TLAS), geometry/material storage pools, and bindless texture registry.
 */
#ifndef BGL_GPU_RAYTRACINGSCENE_HPP
#define BGL_GPU_RAYTRACINGSCENE_HPP

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::scene {
struct Scene;
struct MeshPrimitive;
} // namespace bgl::scene

namespace bgl::gpu {

/**
 * @struct RtVertex
 * @brief Cache-friendly 32-byte vertex payload stored in the global RT vertex storage buffer.
 */
struct alignas(16) RtVertex {
    glm::vec4 posU{0.0f};  ///< `xyz` = local position, `w` = texture coordinate U.
    glm::vec4 normV{0.0f}; ///< `xyz` = local unit normal, `w` = texture coordinate V.
};
static_assert(sizeof(RtVertex) == 32, "RtVertex must be 32 bytes");

/**
 * @struct RtBvhNode
 * @brief 32-byte SAH BVH2 node used for both primitive BLAS and scene-level TLAS.
 *        When `triCount == 0`, `leftFirst` is the right child index (`left = nodeIdx + 1`).
 *        When `triCount > 0`, `leftFirst` is the first primitive/instance index in the leaf.
 */
struct alignas(16) RtBvhNode {
    glm::vec3 aabbMin{1e30f};  ///< Minimum corner of the node bounding box.
    uint32_t leftFirst{0};     ///< Right child node index (internal) or first item offset (leaf).
    glm::vec3 aabbMax{-1e30f}; ///< Maximum corner of the node bounding box.
    uint32_t triCount{0};      ///< Item count (`0` marks an internal node, `>0` marks a leaf).
};
static_assert(sizeof(RtBvhNode) == 32, "RtBvhNode must be 32 bytes");

/**
 * @struct RtInstance
 * @brief Per-instance TLAS payload mapping world-space rays into a shared primitive BLAS.
 */
struct alignas(16) RtInstance {
    glm::mat4 model{1.0f};     ///< Local-to-world transform matrix.
    glm::mat4 invModel{1.0f};  ///< World-to-local inverse transform matrix.
    glm::vec3 aabbMin{1e30f};  ///< World-space AABB minimum corner.
    uint32_t blasRootNode{0};  ///< Starting node index inside the shared BLAS buffer.
    glm::vec3 aabbMax{-1e30f}; ///< World-space AABB maximum corner.
    uint32_t materialIdx{0};   ///< Index into the `RtMaterial` storage buffer.
    uint32_t vertexOffset{0};  ///< Base vertex index for this primitive in `RtVertex` SSBO.
    uint32_t triOffset{0};     ///< Base triangle-index triplet offset in index SSBO.
    uint32_t flags{0};         ///< Instance bitmask (`bit 0`: double-sided, `bit 1`: transparent/transmissive).
    uint32_t nodeIndex{0};     ///< Source `SceneNode` index for picking/diagnostics.
};
static_assert(sizeof(RtInstance) == 176, "RtInstance must be 176 bytes");

/**
 * @struct RtMaterial
 * @brief Packed material record read on secondary ray hits and during path tracing.
 */
struct alignas(16) RtMaterial {
    glm::vec4 baseColorFactor{1.0f};             ///< Linear RGBA base color multiplier.
    glm::vec4 emissiveFactor{0.0f};              ///< `xyz` = emissive * strength, `w` = emissive strength.
    glm::vec4 pbrParams{0.0f, 1.0f, 0.5f, 1.5f}; ///< `x` = metallic, `y` = roughness, `z` = alphaCutoff, `w` = ior.
    glm::ivec4 texIndices{0, 0, 0, 0};           ///< Bindless texture slots (`0` = fallback, `>0` = array slot).
    glm::uvec4 flagsAlpha{0u, 0u, 0u,
                          0u}; ///< `x` = MaterialFlags, `y` = alphaMode, `z` = doubleSided, `w` = transmissive.
};
static_assert(sizeof(RtMaterial) == 80, "RtMaterial must be 80 bytes");

/**
 * @struct RtCpuHit
 * @brief Result returned by `RayTracingScene::TraceRayCpu` for CPU verification and tests.
 */
struct RtCpuHit {
    bool hit{false};                         ///< True when the ray intersected a scene triangle within `maxDist`.
    float distance{1e30f};                   ///< World-space hit distance along the normalized ray.
    uint32_t instanceIndex{0};               ///< Hit index inside `instances_`.
    uint32_t triangleIndex{0};               ///< Hit triangle index inside the global index buffer.
    uint32_t nodeIndex{0};                   ///< Source `SceneNode` index associated with the hit instance.
    glm::vec3 worldPos{0.0f};                ///< World-space intersection coordinates.
    glm::vec3 worldNormal{0.0f, 1.0f, 0.0f}; ///< World-space outward geometric normal at the hit point.
};

/**
 * @struct RtSceneStats
 * @brief Diagnostic BVH and geometry counters for telemetry HUD and unit tests.
 */
struct RtSceneStats {
    uint32_t blasCount{0};            ///< Number of unique bottom-level acceleration structures built.
    uint32_t tlasNodeCount{0};        ///< Node count of the scene top-level BVH2.
    uint32_t blasNodeCount{0};        ///< Combined node count across all BLAS trees.
    uint32_t instanceCount{0};        ///< Active TLAS geometry instances.
    uint32_t triangleCount{0};        ///< Unique triangles indexed across built BLAS slices.
    uint32_t materialCount{0};        ///< Uploaded `RtMaterial` records.
    uint32_t bindlessTextureCount{0}; ///< Unique textures bound into the bindless descriptor array.
    bool hasGpuBuffers{false};        ///< True once GPU storage buffers have been created and bound.
};

/**
 * @class RayTracingScene
 * @brief Builds and maintains a 2-level SAH BVH (BLAS + TLAS) and GPU storage buffers for desktop ray tracing.
 */
class RayTracingScene {
public:
    static constexpr uint32_t kMaxBindlessTextures = 128u; ///< Maximum textures indexed in `@group(2)` bindless array.

    RayTracingScene() = default;
    ~RayTracingScene();

    RayTracingScene(const RayTracingScene&) = delete;
    RayTracingScene& operator=(const RayTracingScene&) = delete;
    RayTracingScene(RayTracingScene&&) noexcept = default;
    RayTracingScene& operator=(RayTracingScene&&) noexcept = default;

    /**
     * @brief Initializes WebGPU bind group layouts and fallback resources for desktop ray tracing.
     * @param device Active WebGPU device handle.
     * @param queue Active WebGPU command queue handle.
     * @param supportsBindlessTextures Enables `@group(2)` `binding_array` creation when supported by adapter.
     * @param fallbackWhiteView Default 1x1 white texture view used for unbound slots.
     * @param defaultSampler Default filtering sampler bound alongside the bindless texture array.
     * @return Success or descriptive error message.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, bool supportsBindlessTextures,
                                  WGPUTextureView fallbackWhiteView, WGPUSampler defaultSampler);

    /**
     * @brief Releases all GPU acceleration buffers, bind groups, and cached CPU BVH state.
     */
    void Shutdown() noexcept;

    /**
     * @brief Builds BLAS structures for all unique primitives in `scene`, uploads geometry & materials,
     *        and constructs the initial TLAS.
     * @param scene Source scene containing meshes, materials, textures, and world transforms.
     */
    void BuildFromScene(const scene::Scene& scene);

    /**
     * @brief Rebuilds and uploads the TLAS if scene transforms or animation state changed.
     * @param scene Source scene whose `transformVersion_` and node transforms are evaluated.
     */
    void UpdateTlas(const scene::Scene& scene);

    /**
     * @brief Evaluates closest-hit ray traversal on the CPU against the current 2-level BVH.
     * @param ray World-space origin and direction ray.
     * @param maxDist Maximum traversal distance clamp.
     * @return Intersection payload indicating hit status, distance, and surface normal.
     */
    [[nodiscard]] RtCpuHit TraceRayCpu(const core::Ray& ray, float maxDist = 1e30f) const noexcept;

    /** @brief Returns true if GPU bind group layouts have been initialized. */
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    /** @brief Returns true if a non-empty TLAS and instance list are ready for tracing. */
    [[nodiscard]] bool IsReady() const noexcept { return hasSceneData_ && !tlasNodes_.empty() && !instances_.empty(); }
    /** @brief Returns true if bindless texture arrays are active on this scene. */
    [[nodiscard]] bool HasBindlessTextures() const noexcept { return supportsBindlessTextures_; }
    /** @brief Returns the `@group(1)` bind group layout containing BVH, vertex, index, and material SSBOs. */
    [[nodiscard]] WGPUBindGroupLayout GetSceneBindGroupLayout() const noexcept { return sceneBgl_.Get(); }
    /** @brief Returns the active `@group(1)` bind group referencing uploaded BVH/geometry buffers. */
    [[nodiscard]] WGPUBindGroup GetSceneBindGroup() const noexcept { return sceneBindGroup_.Get(); }
    /** @brief Returns the optional `@group(2)` bind group layout for bindless texture arrays. */
    [[nodiscard]] WGPUBindGroupLayout GetBindlessTexturesBgl() const noexcept { return bindlessBgl_.Get(); }
    /** @brief Returns the active `@group(2)` bindless texture array bind group. */
    [[nodiscard]] WGPUBindGroup GetBindlessTexturesBindGroup() const noexcept { return bindlessBindGroup_.Get(); }
    /** @brief Returns immutable BVH, triangle, material, and bindless counters for telemetry. */
    [[nodiscard]] const RtSceneStats& GetStats() const noexcept { return stats_; }

    /**
     * @brief Builds a binned-SAH BVH2 for a single triangle list on the CPU.
     * @param vertices Global vertex pool backing the triangle indices.
     * @param sourceIndices Triangle index stream (multiple of 3).
     * @param outNodes Destination vector receiving appended BVH2 nodes for this BLAS.
     * @param outReorderedIndices Destination vector receiving SAH-reordered triangle indices.
     * @return Number of nodes appended to `outNodes`.
     */
    static uint32_t BuildBinnedSahBlas(std::span<const RtVertex> vertices, std::span<const uint32_t> sourceIndices,
                                       std::vector<RtBvhNode>& outNodes, std::vector<uint32_t>& outReorderedIndices);

private:
    struct PrimitiveBlasSlice {
        uint32_t blasRootNode{0};
        uint32_t vertexOffset{0};
        uint32_t triOffset{0};
        uint32_t triCount{0};
        core::AABB localBounds{};
    };

    void RebuildTlasInternal(const scene::Scene& scene, bool forceUpload);
    void UploadStaticGeometryBuffers();
    void UploadTlasBuffers();
    void RebuildBindGroups(const scene::Scene* scene);
    void ReleaseGpuBuffers() noexcept;

    bool initialized_{false};
    bool supportsBindlessTextures_{false};
    bool hasSceneData_{false};

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureView fallbackWhiteView_{nullptr};
    WGPUSampler defaultSampler_{nullptr};

    BindGroupLayoutHandle sceneBgl_{};
    BindGroupHandle sceneBindGroup_{};
    BindGroupLayoutHandle bindlessBgl_{};
    BindGroupHandle bindlessBindGroup_{};

    BufferHandle tlasNodesBuffer_{};
    BufferHandle instancesBuffer_{};
    BufferHandle blasNodesBuffer_{};
    BufferHandle indicesBuffer_{};
    BufferHandle verticesBuffer_{};
    BufferHandle materialsBuffer_{};
    uint64_t tlasBufferBytes_{0};
    uint64_t instancesBufferBytes_{0};
    uint64_t blasBufferBytes_{0};
    uint64_t indicesBufferBytes_{0};
    uint64_t verticesBufferBytes_{0};
    uint64_t materialsBufferBytes_{0};

    std::vector<RtVertex> vertices_{};
    std::vector<uint32_t> indices_{};
    std::vector<RtBvhNode> blasNodes_{};
    std::vector<RtMaterial> materials_{};
    std::vector<RtInstance> instances_{};
    std::vector<RtBvhNode> tlasNodes_{};
    std::unordered_map<const scene::MeshPrimitive*, PrimitiveBlasSlice> primSliceMap_{};

    const scene::Scene* lastScene_{nullptr};
    uint64_t lastTransformVersion_{~uint64_t{0}};
    RtSceneStats stats_{};
};

} // namespace bgl::gpu

#endif // BGL_GPU_RAYTRACINGSCENE_HPP
