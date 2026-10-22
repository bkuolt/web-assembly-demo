/**
 * @file Camera.cpp
 * @brief Implementation of Camera orbit, pan, zoom and ray unprojection.
 */
#include "core/Camera.hpp"
#include <algorithm>

namespace bgl::core {

Camera::Camera(glm::vec3 target, float distance)
    : target_(target), distance_(distance) {
    Update(16.0f / 9.0f);
}

void Camera::Orbit(float deltaYaw, float deltaPitch) noexcept {
    yaw_ += deltaYaw;
    pitch_ += deltaPitch;
    pitch_ = std::clamp(pitch_, -1.55f, 1.55f);
}

void Camera::Pan(float deltaX, float deltaY) noexcept {
    glm::vec3 forward = glm::normalize(target_ - position_);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));
    target_ += right * deltaX + up * deltaY;
}

void Camera::Zoom(float deltaZoom) noexcept {
    distance_ = std::clamp(distance_ - deltaZoom, 0.001f, 10000.0f);
}

void Camera::Update(float aspectRatio) noexcept {
    float x = target_.x + distance_ * std::cos(pitch_) * std::sin(yaw_);
    float y = target_.y + distance_ * std::sin(pitch_);
    float z = target_.z + distance_ * std::cos(pitch_) * std::cos(yaw_);
    position_ = glm::vec3(x, y, z);

    viewMatrix_ = glm::lookAt(position_, target_, glm::vec3(0.0f, 1.0f, 0.0f));
    float dynamicNear = std::min(nearPlane_, distance_ * 0.01f);
    float dynamicFar = std::max(farPlane_, distance_ * 10.0f);
    // Use WebGPU 0..1 depth clip space
    projMatrix_ = glm::perspectiveRH_ZO(fovY_, aspectRatio, dynamicNear, dynamicFar);
}

Ray Camera::ScreenPointToRay(float screenX, float screenY, float screenWidth, float screenHeight) const noexcept {
    float ndcX = (2.0f * screenX) / screenWidth - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY) / screenHeight;

    glm::mat4 invVP = glm::inverse(projMatrix_ * viewMatrix_);
    glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPoint = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);

    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    Ray ray;
    ray.origin = glm::vec3(nearPoint);
    ray.direction = glm::normalize(glm::vec3(farPoint - nearPoint));
    return ray;
}

} // namespace bgl::core
