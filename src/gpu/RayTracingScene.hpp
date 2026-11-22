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
    glm::vec4 posU{0.0f};  // xyz = local position, w = uv.x
    glm::vec4 normV{0.0f}; // xyz = local normal,   w = uv.y
};
static_assert(sizeof(RtVertex) == 32, "RtVertex must be 32 bytes");

/**
 * @struct RtBvhNode
 * @brief 32-byte SAH BVH2 node used for both primitive BLAS and scene-level TLAS.
 *        When `triCount == 0`, `leftFirst` is the right child index (`left = nodeIdx + 1`).
 *        When `triCount > 0`, `leftFirst` is the first primitive/instance index in the leaf.
 */
struct alignas(16) RtBvhNode {
    glm::vec3 aabbMin{1e30f};
    uint32_t leftFirst{0};
    glm::vec3 aabbMax{-1e30f};
    uint32_t triCount{0};
};
static_assert(sizeof(RtBvhNode) == 32, "RtBvhNode must be 32 bytes");

/**
 * @struct RtInstance
 * @brief Per-instance TLAS payload mapping world-space rays into a shared primitive BLAS.
 */
struct alignas(16) RtInstance {
    glm::mat4 model{1.0f};     //   0.. 63
    glm::mat4 invModel{1.0f};  //  64..127
    glm::vec3 aabbMin{1e30f};  // 128..139
    uint32_t blasRootNode{0};  // 140..143
    glm::vec3 aabbMax{-1e30f}; // 144..155
    uint32_t materialIdx{0};   // 156..159
    uint32_t vertexOffset{0};  // 160..163
    uint32_t triOffset{0};     // 164..167
    uint32_t flags{0};         // 168..171 (bit 0: doubleSided, bit 1: transparent/transmissive)
    uint32_t nodeIndex{0};     // 172..175
};
static_assert(sizeof(RtInstance) == 176, "RtInstance must be 176 bytes");

/**
 * @struct RtMaterial
 * @brief Packed material record read on secondary ray hits and during path tracing.
 */
struct alignas(16) RtMaterial {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveFactor{0.0f};              // xyz = emissive * strength, w = unused
    glm::vec4 pbrParams{0.0f, 1.0f, 0.5f, 1.5f}; // x = metallic, y = roughness, z = alphaCutoff, w = ior
    glm::ivec4 texIndices{0, 0, 0, 0};           // 0 = fallback white/none, >0 = bindless texture slot
    glm::uvec4 flagsAlpha{0u, 0u, 0u, 0u};       // x = flags, y = alphaMode, z = doubleSided, w = transmissive
};
static_assert(sizeof(RtMaterial) == 80, "RtMaterial must be 80 bytes");

/**
 * @struct RtCpuHit
 * @brief Result returned by `RayTracingScene::TraceRayCpu` for CPU verification and tests.
 */
struct RtCpuHit {
    bool hit{false};
    float distance{1e30f};
    uint32_t instanceIndex{0};
    uint32_t triangleIndex{0};
    uint32_t nodeIndex{0};
    glm::vec3 worldPos{0.0f};
    glm::vec3 worldNormal{0.0f, 1.0f, 0.0f};
};

/**
 * @struct RtSceneStats
 * @brief Diagnostic BVH and geometry counters for telemetry HUD and unit tests.
 */
struct RtSceneStats {
    uint32_t blasCount{0};
    uint32_t tlasNodeCount{0};
    uint32_t blasNodeCount{0};
    uint32_t instanceCount{0};
    uint32_t triangleCount{0};
    uint32_t materialCount{0};
    uint32_t bindlessTextureCount{0};
    bool hasGpuBuffers{false};
};

/**
 * @class RayTracingScene
 * @brief Builds and maintains a 2-level SAH BVH (BLAS + TLAS) and GPU storage buffers for desktop ray tracing.
 */
class RayTracingScene {
public:
    static constexpr uint32_t kMaxBindlessTextures = 128u;

    RayTracingScene() = default;
    ~RayTracingScene();

    RayTracingScene(const RayTracingScene&) = delete;
    RayTracingScene& operator=(const RayTracingScene&) = delete;
    RayTracingScene(RayTracingScene&&) noexcept = default;
    RayTracingScene& operator=(RayTracingScene&&) noexcept = default;

    /**
     * @brief Initializes WebGPU bind group layouts and fallback buffers for desktop ray tracing.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, bool supportsBindlessTextures,
                                  WGPUTextureView fallbackWhiteView, WGPUSampler defaultSampler);

    /**
     * @brief Releases all GPU acceleration buffers, bind groups, and cached CPU BVH state.
     */
    void Shutdown() noexcept;

    /**
     * @brief Builds BLAS structures for all unique primitives in `scene`, uploads geometry & materials,
     *        and builds the initial TLAS.
     */
    void BuildFromScene(const scene::Scene& scene);

    /**
     * @brief Rebuilds and uploads the TLAS if scene transforms or animation state changed.
     */
    void UpdateTlas(const scene::Scene& scene);

    /**
     * @brief Evaluates closest-hit ray traversal on the CPU against the current 2-level BVH.
     */
    [[nodiscard]] RtCpuHit TraceRayCpu(const core::Ray& ray, float maxDist = 1e30f) const noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] bool IsReady() const noexcept { return hasSceneData_ && !tlasNodes_.empty() && !instances_.empty(); }
    [[nodiscard]] bool HasBindlessTextures() const noexcept { return supportsBindlessTextures_; }
    [[nodiscard]] WGPUBindGroupLayout GetSceneBindGroupLayout() const noexcept { return sceneBgl_.Get(); }
    [[nodiscard]] WGPUBindGroup GetSceneBindGroup() const noexcept { return sceneBindGroup_.Get(); }
    [[nodiscard]] WGPUBindGroupLayout GetBindlessTexturesBgl() const noexcept { return bindlessBgl_.Get(); }
    [[nodiscard]] WGPUBindGroup GetBindlessTexturesBindGroup() const noexcept { return bindlessBindGroup_.Get(); }
    [[nodiscard]] const RtSceneStats& GetStats() const noexcept { return stats_; }

    /**
     * @brief Builds a binned-SAH BVH2 for a single triangle list on the CPU.
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
