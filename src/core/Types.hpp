/**
 * @file Types.hpp
 * @brief Fundamental mathematical types, AABB, Ray, Plane, Frustum, Vertex, Uniforms, and Result<T>.
 */
#pragma once

#include <expected>
#include <string>
#include <algorithm>
#include <cmath>
#include <array>
#include <span>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace bgl::core {

/**
 * @brief Standardized result type without throwing exceptions across module boundaries.
 */
template <typename T>
using Result = std::expected<T, std::string>;

/**
 * @struct Ray
 * @brief 3D geometric ray representation with origin and normalized direction.
 */
struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};

    [[nodiscard]] constexpr glm::vec3 At(float t) const noexcept {
        return origin + t * direction;
    }
};

/**
 * @struct AABB
 * @brief Axis-Aligned Bounding Box with min and max bounds.
 */
struct AABB {
    glm::vec3 minBound{1e30f};
    glm::vec3 maxBound{-1e30f};

    constexpr void Expand(const glm::vec3& p) noexcept {
        minBound = glm::min(minBound, p);
        maxBound = glm::max(maxBound, p);
    }

    constexpr void Expand(const AABB& other) noexcept {
        minBound = glm::min(minBound, other.minBound);
        maxBound = glm::max(maxBound, other.maxBound);
    }

    [[nodiscard]] constexpr glm::vec3 Center() const noexcept {
        return (minBound + maxBound) * 0.5f;
    }

    [[nodiscard]] constexpr glm::vec3 Extents() const noexcept {
        return (maxBound - minBound) * 0.5f;
    }

    /**
     * @brief Transforms the Axis-Aligned Bounding Box by an affine 4x4 matrix using Arvo's algorithm.
     * Computes the new center and transformed extents via scalar absolute dot products:
     *   c' = M * c
     *   e'_k = sum_j (|M_jk| * e_j)
     * This eliminates the need to compute, transform, and bound 8 separate corners,
     * maintaining high performance during scene graph traversal and hierarchical culling.
     * @param transform 4x4 affine transformation matrix.
     * @return Newly computed enclosing Axis-Aligned Bounding Box in target space.
     */
    [[nodiscard]] AABB Transformed(const glm::mat4& transform) const noexcept {
        if (minBound.x > maxBound.x) return AABB{};
        const glm::vec3 c{Center()};
        const glm::vec3 e{Extents()};
        const glm::vec3 newCenter{transform * glm::vec4{c, 1.0f}};
        const glm::vec3 newExtents{
            std::abs(transform[0][0]) * e.x + std::abs(transform[1][0]) * e.y + std::abs(transform[2][0]) * e.z,
            std::abs(transform[0][1]) * e.x + std::abs(transform[1][1]) * e.y + std::abs(transform[2][1]) * e.z,
            std::abs(transform[0][2]) * e.x + std::abs(transform[1][2]) * e.y + std::abs(transform[2][2]) * e.z
        };
        return AABB{
            .minBound = newCenter - newExtents,
            .maxBound = newCenter + newExtents
        };
    }

    /**
     * @brief Computes ray-AABB intersection using Kay-Kajiya slab testing with copysign preservation.
     * @param ray 3D ray with origin and normalized direction.
     * @param[out] tMinOut Parametric distance to the entry point along the ray.
     * @param[out] tMaxOut Parametric distance to the exit point along the ray.
     * @return true if an intersection occurs along the ray; false otherwise.
     */
    [[nodiscard]] bool Intersects(const Ray& ray, float& tMinOut, float& tMaxOut) const noexcept {
        float tmin{0.0f};
        float tmax{1e30f};
        for (int i{0}; i < 3; ++i) {
            const float d = (std::abs(ray.direction[i]) > 1e-8f) ? ray.direction[i] : std::copysign(1e-8f, ray.direction[i]);
            const float invD{1.0f / d};
            float t0{(minBound[i] - ray.origin[i]) * invD};
            float t1{(maxBound[i] - ray.origin[i]) * invD};
            if (invD < 0.0f) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmax < tmin) return false;
        }
        tMinOut = tmin;
        tMaxOut = tmax;
        return true;
    }

    /**
     * @brief Tests intersection (overlap) with another Axis-Aligned Bounding Box.
     * @param other Bounding box to test against.
     * @return true if boxes overlap or touch; false otherwise.
     */
    [[nodiscard]] constexpr bool Intersects(const AABB& other) const noexcept {
        return (minBound.x <= other.maxBound.x && maxBound.x >= other.minBound.x) &&
               (minBound.y <= other.maxBound.y && maxBound.y >= other.minBound.y) &&
               (minBound.z <= other.maxBound.z && maxBound.z >= other.minBound.z);
    }
};

/**
 * @struct Plane
 * @brief 3D plane in Hesse normal form (dot(normal, p) + distance = 0).
 */
struct Plane {
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance{0.0f};

    void Normalize() noexcept {
        const float len{glm::length(normal)};
        if (len > 1e-6f) {
            normal /= len;
            distance /= len;
        }
    }

    [[nodiscard]] float DistanceToPoint(const glm::vec3& p) const noexcept {
        return glm::dot(normal, p) + distance;
    }
};

/**
 * @struct Frustum
 * @brief 6 clipping planes extracted from a View-Projection matrix for frustum culling.
 */
struct Frustum {
    std::array<Plane, 6> planes{};

    /**
     * @brief Extract 6 frustum planes from a WebGPU RH_ZO (0..1 depth) view-projection matrix.
     */
    static Frustum FromViewProjection(const glm::mat4& vp) noexcept {
        Frustum f{};
        // Left plane
        f.planes[0].normal = glm::vec3{vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0]};
        f.planes[0].distance = vp[3][3] + vp[3][0];
        // Right plane
        f.planes[1].normal = glm::vec3{vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0]};
        f.planes[1].distance = vp[3][3] - vp[3][0];
        // Bottom plane
        f.planes[2].normal = glm::vec3{vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1]};
        f.planes[2].distance = vp[3][3] + vp[3][1];
        // Top plane
        f.planes[3].normal = glm::vec3{vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1]};
        f.planes[3].distance = vp[3][3] - vp[3][1];
        // Near plane (WebGPU 0..1 depth)
        f.planes[4].normal = glm::vec3{vp[0][2], vp[1][2], vp[2][2]};
        f.planes[4].distance = vp[3][2];
        // Far plane
        f.planes[5].normal = glm::vec3{vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2]};
        f.planes[5].distance = vp[3][3] - vp[3][2];

        for (auto& plane : f.planes) {
            plane.Normalize();
        }
        return f;
    }

    /**
     * @brief Test whether an AABB is inside or intersects the view frustum.
     */
    [[nodiscard]] bool IntersectsAABB(const AABB& box) const noexcept {
        for (const auto& plane : planes) {
            glm::vec3 p{box.minBound};
            if (plane.normal.x >= 0.0f) p.x = box.maxBound.x;
            if (plane.normal.y >= 0.0f) p.y = box.maxBound.y;
            if (plane.normal.z >= 0.0f) p.z = box.maxBound.z;

            if (plane.DistanceToPoint(p) < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

/**
 * @struct alignas(16) Vertex
 * @brief 3D PBR standard vertex layout aligned to 64 bytes (cache-line friendly).
 */
struct alignas(16) Vertex {
    glm::vec3 position{0.0f};
    float _pad0{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float _pad1{0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};
    glm::vec2 _pad2{0.0f};
};
static_assert(sizeof(Vertex) == 64, "Vertex struct must be exactly 64 bytes (cache-line aligned)");

/**
 * @struct alignas(16) GlobalUniforms
 * @brief WebGPU uniform buffer matching 16-byte alignment requirements.
 */
struct alignas(16) GlobalUniforms {
    glm::mat4 viewProj{1.0f};
    glm::mat4 model{1.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 lightDir{0.577f, 0.577f, 0.577f, 0.0f};
};

} // namespace bgl::core
