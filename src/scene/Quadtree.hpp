/**
 * @file Quadtree.hpp
 * @brief Spatial XZ-plane partitioning, frustum culling, and LOD selection.
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
    void QueryFrustum(const glm::vec3& camPos, std::vector<QuadtreeItem>& visibleItemsOut) const;

    [[nodiscard]] const core::AABB& GetBounds() const noexcept { return bounds_; }

private:
    void Subdivide();

    core::AABB bounds_;
    size_t depth_;
    bool isLeaf_ = true;
    std::vector<QuadtreeItem> items_;
    std::array<std::unique_ptr<QuadtreeNode>, 4> children_;
};

/**
 * @class Quadtree
 * @brief Root spatial manager for scene LOD selection and culling.
 */
class Quadtree {
public:
    explicit Quadtree(const core::AABB& worldBounds);

    void Insert(std::shared_ptr<SceneNode> node);
    void UpdateLOD(const glm::vec3& cameraPos);
    [[nodiscard]] std::vector<QuadtreeItem> QueryVisible(const glm::vec3& cameraPos) const;

private:
    std::unique_ptr<QuadtreeNode> root_;
};

} // namespace bgl::scene

