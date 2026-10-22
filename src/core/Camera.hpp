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
    Camera(glm::vec3 target = glm::vec3(0.0f), float distance = 5.0f);

    void Orbit(float deltaYaw, float deltaPitch) noexcept;
    void Pan(float deltaX, float deltaY) noexcept;
    void Zoom(float deltaZoom) noexcept;
    void Update(float aspectRatio) noexcept;

    [[nodiscard]] glm::mat4 GetViewMatrix() const noexcept { return viewMatrix_; }
    [[nodiscard]] glm::mat4 GetProjectionMatrix() const noexcept { return projMatrix_; }
    [[nodiscard]] glm::mat4 GetViewProjectionMatrix() const noexcept { return projMatrix_ * viewMatrix_; }
    [[nodiscard]] glm::vec3 GetPosition() const noexcept { return position_; }
    [[nodiscard]] glm::vec3 GetTarget() const noexcept { return target_; }

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

