/**
 * @file Quadtree.hpp
 * @brief Spatial XZ-plane partitioning, view frustum culling, and LOD selection.
 */
#pragma once

#include <vector>
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
    std::shared_ptr<SceneNode> node;
    size_t primitiveIndex = 0;
    core::AABB aabb;
    LODLevel currentLOD = LODLevel::LOD0;
};

/**
 * @class QuadtreeNode
 * @brief 4-way spatial partition node on the XZ ground plane.
 */
class QuadtreeNode {
public:
    static constexpr size_t kMaxItems = 8;
    static constexpr size_t kMaxDepth = 6;

    QuadtreeNode(const core::AABB& bounds, size_t depth = 0);

    bool Insert(const QuadtreeItem& item);
    void ForceInsert(const QuadtreeItem& item) { items_.push_back(item); }
    void QueryFrustum(const core::Frustum& frustum, const glm::vec3& camPos, std::vector<QuadtreeItem>& visibleItemsOut) const;

    [[nodiscard]] const core::AABB& GetBounds() const noexcept { return bounds_; }
    [[nodiscard]] const core::AABB& GetLooseBounds() const noexcept { return looseBounds_; }
    [[nodiscard]] const std::vector<QuadtreeItem>& GetItems() const noexcept { return items_; }
    [[nodiscard]] bool IsLeaf() const noexcept { return isLeaf_; }
    [[nodiscard]] const std::array<std::unique_ptr<QuadtreeNode>, 4>& GetChildren() const noexcept { return children_; }

private:
    void Subdivide();

    core::AABB bounds_;
    core::AABB looseBounds_;
    size_t depth_;
    bool isLeaf_ = true;
    std::vector<QuadtreeItem> items_;
    std::array<std::unique_ptr<QuadtreeNode>, 4> children_;
};

/**
 * @class Quadtree
 * @brief Root spatial manager for scene frustum culling and LOD selection.
 */
class Quadtree {
public:
    explicit Quadtree(const core::AABB& worldBounds);

    void Insert(std::shared_ptr<SceneNode> node);
    void Clear();
    void Reset(const core::AABB& worldBounds);

    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const core::Frustum& frustum, const glm::vec3& cameraPos) const;
    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const glm::vec3& cameraPos) const;
    void GetNodeBounds(std::vector<core::AABB>& boundsOut) const;
    [[nodiscard]] size_t GetNodeCount() const noexcept;

private:
    core::AABB worldBounds_;
    std::unique_ptr<QuadtreeNode> root_;
};

} // namespace bgl::scene
