/**
 * @file Camera.hpp
 * @brief Orbit, Pan, and Zoom 3D Camera system.
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "core/Types.hpp"

namespace bgl::core {

/**
 * @class Camera
 * @brief Free-orbit, pan, and zoom camera with smooth interpolation.
 */
class Camera {
public:
    /**
     * @brief Constructs an orbit camera focused on target at a specified distance.
     * @param target Point of interest in 3D world space.
     * @param distance Initial radial orbit radius.
     */
    Camera(glm::vec3 target = glm::vec3(0.0f), float distance = 5.0f);

    /**
     * @brief Orbits the camera around target by delta yaw and pitch angles.
     * @param deltaYaw Horizontal rotation in radians.
     * @param deltaPitch Vertical rotation in radians (clamped to prevent gimbal lock).
     */
    void Orbit(float deltaYaw, float deltaPitch) noexcept;

    /**
     * @brief Pans the camera laterally and vertically in view space.
     * @param deltaX Lateral displacement factor.
     * @param deltaY Vertical displacement factor.
     */
    void Pan(float deltaX, float deltaY) noexcept;

    /**
     * @brief Translates the camera and its orbit target forward along the horizontal view direction.
     * @param delta Forward displacement (positive moves forward, negative moves backward).
     */
    void MoveForward(float delta) noexcept;

    /**
     * @brief Translates the camera and its orbit target laterally along the right vector.
     * @param delta Lateral displacement (positive moves right, negative moves left).
     */
    void MoveRight(float delta) noexcept;

    /**
     * @brief Translates the camera and its orbit target vertically along the world up vector.
     * @param delta Vertical displacement (positive moves up, negative moves down).
     */
    void MoveUp(float delta) noexcept;

    /**
     * @brief Adjusts the orbit distance smoothly.
     * @param deltaZoom Distance change (positive zooms in, negative zooms out).
     */
    void Zoom(float deltaZoom) noexcept;

    /**
     * @brief Recalculates view and WebGPU 0..1 depth perspective projection matrices.
     * @param aspectRatio Viewport width divided by height.
     */
    void Update(float aspectRatio) noexcept;

    [[nodiscard]] glm::mat4 GetViewMatrix() const noexcept { return viewMatrix_; }
    [[nodiscard]] glm::mat4 GetProjectionMatrix() const noexcept { return projMatrix_; }
    [[nodiscard]] glm::mat4 GetViewProjectionMatrix() const noexcept { return projMatrix_ * viewMatrix_; }
    [[nodiscard]] glm::vec3 GetPosition() const noexcept { return position_; }
    [[nodiscard]] glm::vec3 GetTarget() const noexcept { return target_; }
    [[nodiscard]] float GetDistance() const noexcept { return distance_; }
    [[nodiscard]] float GetNearPlane() const noexcept { return nearPlane_; }
    [[nodiscard]] float GetFarPlane() const noexcept { return farPlane_; }
    [[nodiscard]] Frustum GetFrustum() const noexcept { return Frustum::FromViewProjection(GetViewProjectionMatrix()); }

    /**
     * @brief Unproject screen coordinates (normalized 0..1) into a 3D ray in world space.
     */
    [[nodiscard]] Ray ScreenPointToRay(float screenX, float screenY, float screenWidth, float screenHeight) const noexcept;

private:
    glm::vec3 target_{0.0f};
    float distance_{5.0f};
    float yaw_{0.0f};
    float pitch_{0.3f};
    float fovY_{glm::radians(45.0f)};
    float nearPlane_{0.1f};
    float farPlane_{1000.0f};

    glm::vec3 position_{0.0f};
    glm::mat4 viewMatrix_{1.0f};
    glm::mat4 projMatrix_{1.0f};
};

} // namespace bgl::core
