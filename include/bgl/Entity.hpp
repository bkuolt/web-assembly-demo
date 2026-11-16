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

/**
 * @enum LightType
 * @brief Punctual light source category matching glTF 2.0 `KHR_lights_punctual` (`scene::LightType`).
 */
enum class LightType : uint32_t {
    Directional = 0, ///< Infinite distant parallel light oriented along the node's local `-Z` axis.
    Point = 1,       ///< Omnidirectional inverse-square light radiating from the node's world position.
    Spot = 2         ///< Conical inverse-square light radiating along `-Z` within inner/outer cone angles.
};

/**
 * @struct LightDesc
 * @brief Configuration descriptor for attaching a punctual light to an Entity.
 */
struct LightDesc {
    LightType type{LightType::Point};    ///< Light emitter category (Directional, Point, or Spot).
    glm::vec3 color{1.0f, 0.98f, 0.95f}; ///< Linear RGB light emission tint in `[0, 1]`.
    float intensity{1.0f}; ///< Illuminance in lux (Directional) or luminous intensity in candela (Point/Spot).
    float range{0.0f}; ///< Attenuation cutoff radius in meters (`0` for physically unbounded inverse-square falloff).
    float innerConeAngle{0.0f};      ///< Spot inner cone half-angle in radians (`[0, pi/2)`).
    float outerConeAngle{0.785398f}; ///< Spot outer cone half-angle in radians (default `pi/4`).
};

/**
 * @class Entity
 * @brief Copyable handle referencing a node in the active scene.
 * @note Becomes invalid (`IsValid() == false`) whenever the active scene is replaced by `Engine::LoadModel()`.
 */
class Entity {
public:
    Entity() = default;

    /**
     * @brief Checks whether the referenced scene and node are still alive.
     * @return `true` if the underlying node and scene are valid; `false` otherwise.
     */
    [[nodiscard]] bool IsValid() const noexcept;

    /** @brief Explicit boolean conversion equivalent to `IsValid()`. */
    [[nodiscard]] explicit operator bool() const noexcept { return IsValid(); }

    /**
     * @brief Compares whether two handles refer to the same live scene node (expired/invalid handles compare equal).
     * @param other Entity handle to compare against.
     * @return `true` if both refer to the same live node or both are invalid.
     */
    [[nodiscard]] bool operator==(const Entity& other) const noexcept;

    /**
     * @brief Returns the scene node's debug name.
     * @return Node name string, or an empty string if the handle is invalid.
     */
    [[nodiscard]] std::string Name() const;

    /**
     * @brief Sets the entity's local translation relative to its parent (or world origin if root).
     * @param position Local translation in meters.
     */
    void SetPosition(const glm::vec3& position);

    /**
     * @brief Returns the entity's local translation in meters.
     * @return Local XYZ translation (`{0, 0, 0}` if invalid).
     */
    [[nodiscard]] glm::vec3 Position() const;

    /**
     * @brief Sets the entity's local orientation using Euler angles in XYZ order.
     * @param degrees Pitch, yaw, and roll angles in degrees.
     */
    void SetRotationEuler(const glm::vec3& degrees);

    /**
     * @brief Sets the entity's local orientation quaternion (normalized internally).
     * @param rotation Local orientation quaternion.
     */
    void SetRotation(const glm::quat& rotation);

    /**
     * @brief Returns the entity's local orientation quaternion.
     * @return Local orientation quaternion (identity if invalid).
     */
    [[nodiscard]] glm::quat Rotation() const;

    /**
     * @brief Sets the entity's non-uniform local scale factor.
     * @param scale Scale multiplier along local X, Y, Z axes.
     */
    void SetScale(const glm::vec3& scale);

    /**
     * @brief Returns the entity's local scale factor.
     * @return Local XYZ scale (`{1, 1, 1}` if invalid).
     */
    [[nodiscard]] glm::vec3 Scale() const;

    /**
     * @brief Reparents this entity under `parent`, preventing cycle creation.
     * @param parent Target parent entity in the same scene, or an invalid Entity to detach to the scene root.
     */
    void SetParent(const Entity& parent);

    /**
     * @brief Attaches a punctual light to this entity (`KHR_lights_punctual` semantics).
     * @param desc Light emission, cone, and attenuation properties.
     */
    void AddLight(const LightDesc& desc);

    struct Impl;

private:
    friend class Engine;
    std::shared_ptr<Impl> impl_{}; ///< Internal weak handle pair (`SceneNode` and owning `Scene`).
};

} // namespace bgl

#endif // BGL_PUBLIC_ENTITY_HPP
