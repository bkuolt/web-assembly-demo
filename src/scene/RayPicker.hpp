/**
 * @file RayPicker.hpp
 * @brief Broad-phase AABB test and narrow-phase Möller-Trumbore triangle intersection.
 */
#pragma once

#include "core/Types.hpp"
#include "scene/Quadtree.hpp"
#include "scene/Scene.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <span>

namespace bgl::scene {

/**
 * @struct RayHit
 * @brief Detailed intersection result for polygon picking.
 */
struct RayHit {
    float distance{1e30f};
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    uint32_t triangleIndex{0};
    std::shared_ptr<SceneNode> hitNode{nullptr};
};

/**
 * @class RayPicker
 * @brief High-precision 3D ray caster for polygonal interactivity.
 */
class RayPicker {
public:
    /**
     * @brief Möller-Trumbore ray-triangle intersection test.
     */
    [[nodiscard]] static bool IntersectTriangle(const core::Ray& ray, const glm::vec3& v0, const glm::vec3& v1,
                                                const glm::vec3& v2, float& outDistance, glm::vec3& outHitPoint,
                                                glm::vec3& outNormal) noexcept;

    /**
     * @brief Pick nearest scene node and triangle from a span of root nodes.
     */
    [[nodiscard]] static std::optional<RayHit> Pick(const core::Ray& ray,
                                                    std::span<const std::shared_ptr<SceneNode>> rootNodes) noexcept;

    /**
     * @brief Pick nearest scene node and triangle from a camera ray.
     */
    [[nodiscard]] static std::optional<RayHit> Pick(const core::Ray& ray, const Scene& scene) noexcept;
};

} // namespace bgl::scene
