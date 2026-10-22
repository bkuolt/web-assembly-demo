/**
 * @file RayPicker.cpp
 * @brief Implementation of Möller-Trumbore ray picking algorithm with hierarchical scene traversal.
 */
#include "scene/RayPicker.hpp"
#include <cmath>

namespace bgl::scene {

bool RayPicker::IntersectTriangle(const core::Ray& ray,
                                  const glm::vec3& v0,
                                  const glm::vec3& v1,
                                  const glm::vec3& v2,
                                  float& outDistance,
                                  glm::vec3& outHitPoint,
                                  glm::vec3& outNormal) noexcept {
    constexpr float kEpsilon = 1e-7f;
    const glm::vec3 edge1 = v1 - v0;
    const glm::vec3 edge2 = v2 - v0;
    const glm::vec3 pvec = glm::cross(ray.direction, edge2);
    const float det = glm::dot(edge1, pvec);

    if (std::abs(det) < kEpsilon) return false;
    const float invDet = 1.0f / det;

    const glm::vec3 tvec = ray.origin - v0;
    const float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    const glm::vec3 qvec = glm::cross(tvec, edge1);
    const float v = glm::dot(ray.direction, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = glm::dot(edge2, qvec) * invDet;
    if (t < kEpsilon) return false;

    outDistance = t;
    outHitPoint = ray.origin + t * ray.direction;
    outNormal = glm::normalize(glm::cross(edge1, edge2));
    return true;
}

std::optional<RayHit> RayPicker::Pick(const core::Ray& ray, const Scene& scene) noexcept {
    std::optional<RayHit> closestHit;
    float minDistance = 1e30f;

    auto traverseNode = [&](auto& self, const std::shared_ptr<SceneNode>& node, const glm::mat4& parentTransform) -> void {
        if (!node) return;
        const glm::mat4 worldMat = parentTransform * node->localTransform;

        for (const auto& prim : node->primitives) {
            float t0 = 0.0f, t1 = 0.0f;
            if (!prim.boundingBox.Intersects(ray, t0, t1)) {
                // Skip primitive if ray misses AABB
                continue;
            }

            if (prim.indices.empty()) {
                // Non-indexed triangle list
                for (size_t i = 0; i + 2 < prim.vertices.size(); i += 3) {
                    const glm::vec3 v0 = glm::vec3(worldMat * glm::vec4(prim.vertices[i].position, 1.0f));
                    const glm::vec3 v1 = glm::vec3(worldMat * glm::vec4(prim.vertices[i + 1].position, 1.0f));
                    const glm::vec3 v2 = glm::vec3(worldMat * glm::vec4(prim.vertices[i + 2].position, 1.0f));

                    float dist = 0.0f;
                    glm::vec3 pt{0.0f};
                    glm::vec3 norm{0.0f};
                    if (IntersectTriangle(ray, v0, v1, v2, dist, pt, norm) && dist < minDistance) {
                        minDistance = dist;
                        closestHit = RayHit{
                            .distance = dist,
                            .hitPoint = pt,
                            .normal = norm,
                            .triangleIndex = static_cast<uint32_t>(i / 3),
                            .hitNode = node
                        };
                    }
                }
            } else {
                // Indexed triangles
                for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
                    const uint32_t idx0 = prim.indices[i];
                    const uint32_t idx1 = prim.indices[i + 1];
                    const uint32_t idx2 = prim.indices[i + 2];

                    const glm::vec3 v0 = glm::vec3(worldMat * glm::vec4(prim.vertices[idx0].position, 1.0f));
                    const glm::vec3 v1 = glm::vec3(worldMat * glm::vec4(prim.vertices[idx1].position, 1.0f));
                    const glm::vec3 v2 = glm::vec3(worldMat * glm::vec4(prim.vertices[idx2].position, 1.0f));

                    float dist = 0.0f;
                    glm::vec3 pt{0.0f};
                    glm::vec3 norm{0.0f};
                    if (IntersectTriangle(ray, v0, v1, v2, dist, pt, norm) && dist < minDistance) {
                        minDistance = dist;
                        closestHit = RayHit{
                            .distance = dist,
                            .hitPoint = pt,
                            .normal = norm,
                            .triangleIndex = static_cast<uint32_t>(i / 3),
                            .hitNode = node
                        };
                    }
                }
            }
        }

        for (const auto& child : node->children) {
            self(self, child, worldMat);
        }
    };

    for (const auto& rootNode : scene.rootNodes) {
        traverseNode(traverseNode, rootNode, glm::mat4(1.0f));
    }

    return closestHit;
}

} // namespace bgl::scene
