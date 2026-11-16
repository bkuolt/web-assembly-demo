/**
 * @file Quadtree.hpp
 * @brief Spatial XZ-plane partitioning, view frustum culling, and LOD selection.
 */
#ifndef BGL_SCENE_QUADTREE_HPP
#define BGL_SCENE_QUADTREE_HPP

#include "core/Types.hpp"
#include "scene/Scene.hpp"
#include <array>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>

namespace bgl::scene {

enum class LODLevel : uint8_t {
    LOD0 = 0, // High detail (Near: 100% geometry)
    LOD1 = 1, // Medium detail (Mid: ~50% geometry)
    LOD2 = 2, // Low detail (Far: ~25% geometry)
    LOD3 = 3  // Ultra-low detail (Distant: ~10% geometry)
};

inline constexpr size_t kMaxLODLevels{4};

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
    float lodTransition{0.0f};   ///< Smooth transition factor [0.0..1.0] near LOD boundary
    float screenSpaceArea{1.0f}; ///< Projected NDC area [0.0..4.0]
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

    void Clear() noexcept { tiles_.fill(1.0f); }

    /**
     * @brief Projects a world-space AABB into NDC [-1, 1]^2 x [0, 1].
     * @return false if the AABB clips behind the near plane (w <= 0) or is invalid.
     */
    [[nodiscard]] static bool ProjectAABB(const core::AABB& box, const glm::mat4& viewProj, glm::vec2& ndcMinOut,
                                          glm::vec2& ndcMaxOut, float& minZOut, float& maxZOut) noexcept;

    /**
     * @brief Returns true if a projected AABB [ndcMin..ndcMax] with nearest depth minZ
     *        is completely hidden behind existing occluders in all covered screen tiles.
     */
    [[nodiscard]] bool IsOccluded(const glm::vec2& ndcMin, const glm::vec2& ndcMax, float minZ) const noexcept;

    /**
     * @brief Conservatively rasterizes an opaque AABB's inner core into the depth tiles.
     */
    void RasterizeOccluder(const glm::vec2& ndcMin, const glm::vec2& ndcMax, float occluderMaxZ) noexcept;

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
    void ForceInsert(const QuadtreeItem& item) {
        looseBounds_.Expand(item.aabb);
        items_.push_back(item);
    }
    void ExtractRelocatedItems(std::vector<QuadtreeItem>& relocatedOut);
    bool RecomputeLooseBounds() noexcept;
    void QueryFrustum(const core::Frustum& frustum, const glm::vec3& camPos, std::vector<QuadtreeItem>& visibleItemsOut,
                      const glm::mat4* viewProj = nullptr, OcclusionBuffer* occlusionBuf = nullptr,
                      size_t* occludedCountOut = nullptr, float lodBias = 1.0f) const;

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
    /**
     * @brief Constructs root spatial Quadtree bounded by the enclosing world bounds.
     * @param worldBounds Enclosing world-space bounding box.
     */
    explicit Quadtree(const core::AABB& worldBounds);

    /**
     * @brief Inserts a scene node hierarchy and all its mesh primitives into the spatial index.
     * @param node Root or branch scene node to index.
     */
    void Insert(const std::shared_ptr<SceneNode>& node);

    /**
     * @brief Inserts a span of root scene nodes into the spatial index.
     * @param nodes Span of scene node pointers.
     */
    void InsertNodes(std::span<const std::shared_ptr<SceneNode>> nodes);

    /**
     * @brief Clears all spatial items and resets the root partition.
     */
    void Clear();

    /**
     * @brief Reinitializes the spatial tree with new world boundaries.
     * @param worldBounds New enclosing world-space bounding box.
     */
    void Reset(const core::AABB& worldBounds);

    /**
     * @brief Updates item AABBs from live SceneNode transforms, refits loose bounds, and relocates moved items.
     * @return Number of items relocated across spatial quadrants.
     */
    size_t Refit();

    /**
     * @brief Traverses the spatial tree, performing frustum culling and front-to-back Hi-Z occlusion culling.
     * @param frustum 6-plane view frustum extracted from the camera view-projection matrix.
     * @param cameraPos World-space camera eye position for front-to-back distance sorting.
     * @param viewProj Camera view-projection matrix used to project AABB bounds into NDC screen tiles.
     * @param enableOcclusionCulling Whether conservative Hi-Z tile occlusion testing is active.
     * @param lodBias Screen-space error multiplier scaling LOD selection distances.
     * @return Vector of visible QuadtreeItems sorted front-to-back.
     */
    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos,
                                                         const glm::mat4& viewProj, bool enableOcclusionCulling = true,
                                                         float lodBias = 1.0f) const;

    /**
     * @brief Zero-allocation variant writing visible items into caller-provided resultsOut buffer.
     */
    void QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos, const glm::mat4& viewProj,
                      std::vector<QuadtreeItem>& resultsOut, bool enableOcclusionCulling = true,
                      float lodBias = 1.0f) const;

    /**
     * @brief Queries visible items using view frustum culling only.
     * @param frustum 6-plane view frustum.
     * @param cameraPos Camera eye position for LOD and distance calculation.
     * @param lodBias Multiplier for LOD distance thresholds (default 1.0).
     * @return Vector of visible QuadtreeItems.
     */
    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos,
                                                         float lodBias = 1.0f) const;

    /**
     * @brief Zero-allocation frustum-only query writing visible items into caller-provided resultsOut buffer.
     */
    void QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos, std::vector<QuadtreeItem>& resultsOut,
                      float lodBias = 1.0f) const;

    /**
     * @brief Queries all items in the spatial tree with distance-based LOD calculation.
     * @param cameraPos Camera eye position for LOD and distance calculation.
     * @param lodBias Multiplier for LOD distance thresholds (default 1.0).
     * @return Vector of all QuadtreeItems.
     */
    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const glm::vec3& cameraPos, float lodBias = 1.0f) const;

    /**
     * @brief Collects all spatial cell bounding boxes for 3D debug wireframe visualization.
     * @param[out] boundsOut Output vector receiving cell AABB bounds.
     */
    void GetNodeBounds(std::vector<core::AABB>& boundsOut) const;

    /**
     * @brief Returns the total number of allocated quadtree partition nodes.
     */
    [[nodiscard]] size_t GetNodeCount() const noexcept;

    /**
     * @brief Result of evaluating distance/SSE metric against LOD thresholds.
     */
    struct LodEvaluation {
        LODLevel level{LODLevel::LOD0};
        float transition{0.0f};
    };

    /**
     * @brief Evaluates discrete LOD level and [0..1] boundary transition factor for an effective distance metric.
     */
    [[nodiscard]] static LodEvaluation EvaluateLodForMetric(float effectiveMetric) noexcept;

    /**
     * @brief Returns the number of primitives discarded by occlusion culling in the previous query.
     */
    [[nodiscard]] size_t GetLastOccludedCount() const noexcept { return lastOccludedCount_; }

private:
    core::AABB worldBounds_{};
    std::unique_ptr<QuadtreeNode> root_{nullptr};
    mutable OcclusionBuffer occlusionBuffer_{};
    mutable size_t lastOccludedCount_{0};
};

} // namespace bgl::scene

#endif // BGL_SCENE_QUADTREE_HPP
