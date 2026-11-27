/**
 * @file RayPicker.cpp
 * @brief Implementation of Möller-Trumbore ray picking algorithm with hierarchical scene traversal.
 */
#include "scene/RayPicker.hpp"
#include <cmath>
#include <unordered_set>

namespace bgl::scene {

bool RayPicker::IntersectTriangle(const core::Ray& ray, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                  float& outDistance, glm::vec3& outHitPoint, glm::vec3& outNormal) noexcept {
    constexpr float kEpsilon = 1e-7f;
    const glm::vec3 edge1 = v1 - v0;
    const glm::vec3 edge2 = v2 - v0;
    const glm::vec3 pvec = glm::cross(ray.direction, edge2);
    const float det = glm::dot(edge1, pvec);

    if (std::abs(det) < kEpsilon)
        return false;
    const float invDet = 1.0f / det;

    const glm::vec3 tvec = ray.origin - v0;
    const float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;

    const glm::vec3 qvec = glm::cross(tvec, edge1);
    const float v = glm::dot(ray.direction, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    const float t = glm::dot(edge2, qvec) * invDet;
    if (t < kEpsilon)
        return false;

    outDistance = t;
    outHitPoint = ray.origin + t * ray.direction;
    const glm::vec3 faceCross{glm::cross(edge1, edge2)};
    const float crossLenSq{glm::dot(faceCross, faceCross)};
    outNormal = (crossLenSq > 1e-12f) ? (faceCross * (1.0f / std::sqrt(crossLenSq))) : glm::vec3{0.0f, 1.0f, 0.0f};
    return true;
}

std::optional<RayHit> RayPicker::Pick(const core::Ray& ray,
                                      std::span<const std::shared_ptr<SceneNode>> rootNodes) noexcept {
    if (glm::dot(ray.direction, ray.direction) <= 1e-12f) {
        return std::nullopt;
    }
    std::optional<RayHit> closestHit{};
    float minDistance{1e30f};
    std::unordered_set<const SceneNode*> visited{};
    constexpr size_t kMaxPickDepth{256u};

    auto traverseNode = [&](this auto&& self, const std::shared_ptr<SceneNode>& node, const glm::mat4& parentTransform,
                            size_t depth) -> void {
        if (!node || depth > kMaxPickDepth || !visited.insert(node.get()).second)
            return;
        const glm::mat4 worldMat{parentTransform * node->localTransform};

        // Hierarchical broad-phase AABB test for entire scene node
        float nt0{0.0f}, nt1{0.0f};
        if (node->worldAABB.IsValid() && !node->worldAABB.Intersects(ray, nt0, nt1)) {
            return;
        }

        for (const auto& prim : node->GetPrimitives()) {
            float t0{0.0f}, t1{0.0f};
            const core::AABB worldPrimAABB{prim.boundingBox.Transformed(worldMat)};
            if (worldPrimAABB.IsValid() && !worldPrimAABB.Intersects(ray, t0, t1)) {
                // Skip primitive if ray misses world-space AABB
                continue;
            }

            std::span<const core::Vertex> vertSpan{prim.vertices.data(), prim.vertices.size()};
            std::span<const uint32_t> idxSpan{prim.indices.data(), prim.indices.size()};

            if (idxSpan.empty()) {
                // Non-indexed triangle list
                for (size_t i{0}; i + 2 < vertSpan.size(); i += 3) {
                    const glm::vec3 v0{worldMat * glm::vec4{vertSpan[i].position, 1.0f}};
                    const glm::vec3 v1{worldMat * glm::vec4{vertSpan[i + 1].position, 1.0f}};
                    const glm::vec3 v2{worldMat * glm::vec4{vertSpan[i + 2].position, 1.0f}};

                    float dist{0.0f};
                    glm::vec3 pt{0.0f};
                    glm::vec3 norm{0.0f};
                    if (IntersectTriangle(ray, v0, v1, v2, dist, pt, norm) && dist < minDistance) {
                        minDistance = dist;
                        closestHit = RayHit{.distance = dist,
                                            .hitPoint = pt,
                                            .normal = norm,
                                            .triangleIndex = static_cast<uint32_t>(i / 3),
                                            .hitNode = node};
                    }
                }
            } else {
                // Indexed triangles
                for (size_t i{0}; i + 2 < idxSpan.size(); i += 3) {
                    const uint32_t idx0{idxSpan[i]};
                    const uint32_t idx1{idxSpan[i + 1]};
                    const uint32_t idx2{idxSpan[i + 2]};
                    if (idx0 >= vertSpan.size() || idx1 >= vertSpan.size() || idx2 >= vertSpan.size()) {
                        continue;
                    }

                    const glm::vec3 v0{worldMat * glm::vec4{vertSpan[idx0].position, 1.0f}};
                    const glm::vec3 v1{worldMat * glm::vec4{vertSpan[idx1].position, 1.0f}};
                    const glm::vec3 v2{worldMat * glm::vec4{vertSpan[idx2].position, 1.0f}};

                    float dist{0.0f};
                    glm::vec3 pt{0.0f};
                    glm::vec3 norm{0.0f};
                    if (IntersectTriangle(ray, v0, v1, v2, dist, pt, norm) && dist < minDistance) {
                        minDistance = dist;
                        closestHit = RayHit{.distance = dist,
                                            .hitPoint = pt,
                                            .normal = norm,
                                            .triangleIndex = static_cast<uint32_t>(i / 3),
                                            .hitNode = node};
                    }
                }
            }
        }

        for (const auto& child : node->children) {
            self(child, worldMat, depth + 1u);
        }
    };

    for (const auto& rootNode : rootNodes) {
        traverseNode(rootNode, glm::mat4{1.0f}, 0u);
    }

    return closestHit;
}

std::optional<RayHit> RayPicker::Pick(const core::Ray& ray, const Scene& scene) noexcept {
    return Pick(ray, std::span<const std::shared_ptr<SceneNode>>{scene.rootNodes});
}

} // namespace bgl::scene
