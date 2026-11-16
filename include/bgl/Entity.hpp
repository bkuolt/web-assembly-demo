/**
 * @file Entity.hpp
 * @brief Lightweight handle to a node in the active scene (transform, hierarchy, lights).
 */
#ifndef BGL_PUBLIC_ENTITY_HPP
#define BGL_PUBLIC_ENTITY_HPP

#include "bgl/Types.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bgl {

enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 }; ///< matches scene::LightType

struct LightDesc {
    LightType type{LightType::Point};
    glm::vec3 color{1.0f, 0.98f, 0.95f};
    float intensity{1.0f};
    float range{0.0f};
    float innerConeAngle{0.0f};
    float outerConeAngle{0.785398f};
};

/**
 * @class Entity
 * @brief Copyable handle. Becomes invalid (IsValid() == false) when the scene is replaced by Engine::LoadModel().
 */
class Entity {
public:
    Entity() = default;
    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::string Name() const;
    void SetPosition(const glm::vec3& position);
    [[nodiscard]] glm::vec3 Position() const;
    /** @brief Euler angles in degrees (XYZ order). */
    void SetRotationEuler(const glm::vec3& degrees);
    void SetRotation(const glm::quat& rotation);
    [[nodiscard]] glm::quat Rotation() const;
    void SetScale(const glm::vec3& scale);
    [[nodiscard]] glm::vec3 Scale() const;
    /** @brief Reparents this entity under `parent` (or to the scene root if `parent` is invalid). */
    void SetParent(const Entity& parent);
    /** @brief Attaches a punctual light to this entity (KHR_lights_punctual semantics). */
    void AddLight(const LightDesc& desc);

    struct Impl;

private:
    friend class Engine;
    std::shared_ptr<Impl> impl_;
};

} // namespace bgl

#endif // BGL_PUBLIC_ENTITY_HPP
