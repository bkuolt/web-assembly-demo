/**
 * @file Types.hpp
 * @brief Fundamental mathematical types, AABB, Ray, Vertex, Uniforms, and Result<T>.
 */
#pragma once

#include <expected>
#include <string>
#include <algorithm>
#include <cmath>
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

    [[nodiscard]] bool Intersects(const Ray& ray, float& tMinOut, float& tMaxOut) const noexcept {
        float tmin = 0.0f;
        float tmax = 1e30f;
        for (int i = 0; i < 3; ++i) {
            const float invD = 1.0f / (std::abs(ray.direction[i]) > 1e-8f ? ray.direction[i] : 1e-8f);
            float t0 = (minBound[i] - ray.origin[i]) * invD;
            float t1 = (maxBound[i] - ray.origin[i]) * invD;
            if (invD < 0.0f) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmax < tmin) return false;
        }
        tMinOut = tmin;
        tMaxOut = tmax;
        return true;
    }
};

/**
 * @struct alignas(16) Vertex
 * @brief 3D PBR standard vertex layout aligned to 64 bytes (cache-line friendly).
 */
struct alignas(16) Vertex {
    glm::vec3 position{0.0f};
    float _pad0 = 0.0f;
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float _pad1 = 0.0f;
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
