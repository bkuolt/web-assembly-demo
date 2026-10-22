/**
 * @file Quadtree.hpp
 * @brief Spatial XZ-plane partitioning, view frustum culling, and LOD selection.
 */
#pragma once

#include <vector>
#include <span>
#include <memory>
#include <array>
#include <glm/glm.hpp>
#include "core/Types.hpp"
#include "scene/Scene.hpp"

namespace bgl::scene {

enum class LODLevel : uint8_t {
    LOD0 = 0, // High detail (Near)
    LOD1 = 1, // Medium detail (Mid)
    LOD2 = 2  // Low detail (Far)
};

/**
 * @struct QuadtreeItem
 * @brief Spatial item registered within the Quadtree hierarchy.
 */
struct QuadtreeItem {
    std::shared_ptr<SceneNode> node{nullptr};
    size_t primitiveIndex{0};
    core::AABB aabb{};
    LODLevel currentLOD{LODLevel::LOD0};
    float distanceToCamera{0.0f};
};

/**
 * @class OcclusionBuffer
 * @brief L1-cache-friendly 32x16 hierarchical screen-space depth buffer (Hi-Z tiles)
 *        for conservative CPU Quadtree & AABB occlusion culling.
 */
class OcclusionBuffer {
public:
    static constexpr uint32_t kWidth{32};
    static constexpr uint32_t kHeight{16};

    OcclusionBuffer() noexcept { Clear(); }

    void Clear() noexcept {
        tiles_.fill(1.0f);
    }

    /**
     * @brief Projects a world-space AABB into NDC [-1, 1]^2 x [0, 1].
     * @return false if the AABB clips behind the near plane (w <= 0) or is invalid.
     */
    [[nodiscard]] static bool ProjectAABB(const core::AABB& box,
                                          const glm::mat4& viewProj,
                                          glm::vec2& ndcMinOut,
                                          glm::vec2& ndcMaxOut,
                                          float& minZOut,
                                          float& maxZOut) noexcept;

    /**
     * @brief Returns true if a projected AABB [ndcMin..ndcMax] with nearest depth minZ
     *        is completely hidden behind existing occluders in all covered screen tiles.
     */
    [[nodiscard]] bool IsOccluded(const glm::vec2& ndcMin,
                                  const glm::vec2& ndcMax,
                                  float minZ) const noexcept;

    /**
     * @brief Conservatively rasterizes an opaque AABB's inner core into the depth tiles.
     */
    void RasterizeOccluder(const glm::vec2& ndcMin,
                           const glm::vec2& ndcMax,
                           float occluderMaxZ) noexcept;

private:
    std::array<float, kWidth * kHeight> tiles_{};
};

/**
 * @class QuadtreeNode
 * @brief 4-way spatial partition node on the XZ ground plane.
 */
class QuadtreeNode {
public:
    static constexpr size_t kMaxItems{8};
    static constexpr size_t kMaxDepth{6};

    QuadtreeNode(const core::AABB& bounds, size_t depth = 0);

    bool Insert(const QuadtreeItem& item);
    void ForceInsert(const QuadtreeItem& item) { items_.push_back(item); }
    void QueryFrustum(const core::Frustum& frustum,
                      const glm::vec3& camPos,
                      std::vector<QuadtreeItem>& visibleItemsOut,
                      const glm::mat4* viewProj = nullptr,
                      OcclusionBuffer* occlusionBuf = nullptr,
                      size_t* occludedCountOut = nullptr) const;

    [[nodiscard]] const core::AABB& GetBounds() const noexcept { return bounds_; }
    [[nodiscard]] const core::AABB& GetLooseBounds() const noexcept { return looseBounds_; }
    [[nodiscard]] std::span<const QuadtreeItem> GetItems() const noexcept { return items_; }
    [[nodiscard]] bool IsLeaf() const noexcept { return isLeaf_; }
    [[nodiscard]] const std::array<std::unique_ptr<QuadtreeNode>, 4>& GetChildren() const noexcept { return children_; }

private:
    void Subdivide();

    core::AABB bounds_{};
    core::AABB looseBounds_{};
    size_t depth_{0};
    bool isLeaf_{true};
    std::vector<QuadtreeItem> items_{};
    std::array<std::unique_ptr<QuadtreeNode>, 4> children_{};
};

/**
 * @class Quadtree
 * @brief Root spatial manager for scene frustum culling and LOD selection.
 */
class Quadtree {
public:
    explicit Quadtree(const core::AABB& worldBounds);

    void Insert(std::shared_ptr<SceneNode> node);
    void InsertNodes(std::span<const std::shared_ptr<SceneNode>> nodes);
    void Clear();
    void Reset(const core::AABB& worldBounds);

    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const core::Frustum& frustum,
                                                         const glm::vec3& cameraPos,
                                                         const glm::mat4& viewProj,
                                                         bool enableOcclusionCulling = true) const;
    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos) const;
    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const glm::vec3& cameraPos) const;
    void GetNodeBounds(std::vector<core::AABB>& boundsOut) const;
    [[nodiscard]] size_t GetNodeCount() const noexcept;
    [[nodiscard]] size_t GetLastOccludedCount() const noexcept { return lastOccludedCount_; }

private:
    core::AABB worldBounds_{};
    std::unique_ptr<QuadtreeNode> root_{nullptr};
    mutable size_t lastOccludedCount_{0};
};

} // namespace bgl::scene
