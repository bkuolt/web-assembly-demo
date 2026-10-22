/**
 * @file RayPicker.cpp
 * @brief Implementation of Möller-Trumbore ray picking algorithm.
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
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 pvec = glm::cross(ray.direction, edge2);
    float det = glm::dot(edge1, pvec);

    if (std::abs(det) < kEpsilon) return false;
    float invDet = 1.0f / det;

    glm::vec3 tvec = ray.origin - v0;
    float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    glm::vec3 qvec = glm::cross(tvec, edge1);
    float v = glm::dot(ray.direction, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    float t = glm::dot(edge2, qvec) * invDet;
    if (t < kEpsilon) return false;

    outDistance = t;
    outHitPoint = ray.origin + t * ray.direction;
    outNormal = glm::normalize(glm::cross(edge1, edge2));
    return true;
}

std::optional<RayHit> RayPicker::Pick(const core::Ray& ray, const Scene& scene) noexcept {
    std::optional<RayHit> closestHit;
    float minDistance = 1e30f;

    for (const auto& node : scene.rootNodes) {
        float t0, t1;
        if (!node->worldAABB.Intersects(ray, t0, t1)) {
            continue;
        }

        for (const auto& prim : node->primitives) {
            for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
                uint32_t idx0 = prim.indices[i];
                uint32_t idx1 = prim.indices[i + 1];
                uint32_t idx2 = prim.indices[i + 2];

                glm::vec3 v0 = glm::vec3(node->worldTransform * glm::vec4(prim.vertices[idx0].position, 1.0f));
                glm::vec3 v1 = glm::vec3(node->worldTransform * glm::vec4(prim.vertices[idx1].position, 1.0f));
                glm::vec3 v2 = glm::vec3(node->worldTransform * glm::vec4(prim.vertices[idx2].position, 1.0f));

                float dist;
                glm::vec3 pt;
                glm::vec3 norm;
                if (IntersectTriangle(ray, v0, v1, v2, dist, pt, norm)) {
                    if (dist < minDistance) {
                        minDistance = dist;
                        RayHit hit;
                        hit.distance = dist;
                        hit.hitPoint = pt;
                        hit.normal = norm;
                        hit.triangleIndex = static_cast<uint32_t>(i / 3);
                        hit.hitNode = node;
                        closestHit = hit;
                    }
                }
            }
        }
    }

    return closestHit;
}

} // namespace bgl::scene

