/**
 * @file Camera.cpp
 * @brief Implementation of Camera orbit, fly, pan, zoom, ray unprojection, bookmarks, and bounds framing.
 */
#include "core/Camera.hpp"
#include <algorithm>
#include <cmath>

namespace bgl::core {

Camera::Camera(glm::vec3 target, float distance)
    : target_{target}, distance_{distance} {
    Update(16.0f / 9.0f);
}

void Camera::Orbit(float deltaYaw, float deltaPitch) noexcept {
    yaw_ += deltaYaw;
    pitch_ += deltaPitch;
    pitch_ = std::clamp(pitch_, -1.55f, 1.55f);
}

void Camera::Pan(float deltaX, float deltaY) noexcept {
    const glm::vec3 forward{glm::normalize(target_ - position_)};
    const glm::vec3 right{glm::normalize(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}))};
    const glm::vec3 up{glm::normalize(glm::cross(right, forward))};
    const glm::vec3 delta = right * deltaX + up * deltaY;
    target_ += delta;
    if (mode_ == CameraMode::Fly) {
        position_ += delta;
    }
}

void Camera::MoveForward(float delta) noexcept {
    glm::vec3 forward{target_ - position_};
    forward.y = 0.0f; // Constrain to ground plane for smooth WASD navigation
    if (glm::length(forward) > 1e-4f) {
        forward = glm::normalize(forward);
    } else {
        forward = glm::vec3{0.0f, 0.0f, -1.0f};
    }
    target_ += forward * delta;
    if (mode_ == CameraMode::Fly) {
        position_ += forward * delta;
    }
}

void Camera::MoveRight(float delta) noexcept {
    glm::vec3 forward{target_ - position_};
    forward.y = 0.0f;
    if (glm::length(forward) > 1e-4f) {
        forward = glm::normalize(forward);
    } else {
        forward = glm::vec3{0.0f, 0.0f, -1.0f};
    }
    const glm::vec3 right{glm::normalize(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}))};
    target_ += right * delta;
    if (mode_ == CameraMode::Fly) {
        position_ += right * delta;
    }
}

void Camera::MoveUp(float delta) noexcept {
    target_.y += delta;
    if (mode_ == CameraMode::Fly) {
        position_.y += delta;
    }
}

void Camera::Zoom(float deltaZoom) noexcept {
    distance_ = std::clamp(distance_ - deltaZoom, 0.001f, 10000.0f);
}

void Camera::FocusOnBounds(const AABB& bounds, float margin) noexcept {
    if (bounds.minBound.x > bounds.maxBound.x) return;
    const glm::vec3 center = bounds.Center();
    const glm::vec3 extents = bounds.Extents();
    const float radius = std::max(glm::length(extents), 0.1f);
    const float halfFov = std::max(fovY_ * 0.5f, 0.01f);
    const float fitDistance = (radius / std::sin(halfFov)) * margin;

    target_ = center;
    distance_ = std::clamp(fitDistance, 0.05f, 50000.0f);
    mode_ = CameraMode::Orbit;
    Update(16.0f / 9.0f);
}

bool Camera::SaveBookmark(size_t slot) noexcept {
    if (slot >= bookmarks_.size()) return false;
    bookmarks_[slot] = CameraBookmark{
        .position = position_,
        .target = target_,
        .distance = distance_,
        .yaw = yaw_,
        .pitch = pitch_,
        .mode = mode_,
        .valid = true
    };
    return true;
}

bool Camera::RestoreBookmark(size_t slot) noexcept {
    if (slot >= bookmarks_.size() || !bookmarks_[slot].valid) return false;
    const auto& b = bookmarks_[slot];
    position_ = b.position;
    target_ = b.target;
    distance_ = b.distance;
    yaw_ = b.yaw;
    pitch_ = b.pitch;
    mode_ = b.mode;
    Update(16.0f / 9.0f);
    return true;
}

bool Camera::HasBookmark(size_t slot) const noexcept {
    return slot < bookmarks_.size() && bookmarks_[slot].valid;
}

const CameraBookmark* Camera::GetBookmark(size_t slot) const noexcept {
    if (slot < bookmarks_.size() && bookmarks_[slot].valid) {
        return &bookmarks_[slot];
    }
    return nullptr;
}

void Camera::SetPositionAndTarget(const glm::vec3& position, const glm::vec3& target) noexcept {
    position_ = position;
    target_ = target;
    const glm::vec3 diff = target - position;
    distance_ = std::max(glm::length(diff), 0.001f);
    const glm::vec3 dir = glm::normalize(diff);
    pitch_ = std::asin(std::clamp(dir.y, -0.999f, 0.999f));
    yaw_ = std::atan2(-dir.x, -dir.z);
    Update(16.0f / 9.0f);
}

void Camera::SetFromTransform(const glm::mat4& transform, float fovY, float nearPlane, float farPlane) noexcept {
    position_ = glm::vec3(transform[3]);
    // glTF camera looks toward -Z
    const glm::vec3 forward = -glm::normalize(glm::vec3(transform[2]));
    distance_ = 5.0f;
    target_ = position_ + forward * distance_;
    pitch_ = std::asin(std::clamp(forward.y, -0.999f, 0.999f));
    yaw_ = std::atan2(-forward.x, -forward.z);
    fovY_ = fovY > 0.0f ? fovY : glm::radians(45.0f);
    nearPlane_ = nearPlane > 0.0f ? nearPlane : 0.1f;
    farPlane_ = farPlane > nearPlane_ ? farPlane : 1000.0f;
    mode_ = CameraMode::Fly;
    Update(16.0f / 9.0f);
}

void Camera::Update(float aspectRatio) noexcept {
    if (mode_ == CameraMode::Orbit) {
        const float x{target_.x + distance_ * std::cos(pitch_) * std::sin(yaw_)};
        const float y{target_.y + distance_ * std::sin(pitch_)};
        const float z{target_.z + distance_ * std::cos(pitch_) * std::cos(yaw_)};
        position_ = glm::vec3{x, y, z};
    } else { // CameraMode::Fly
        const glm::vec3 forward{
            -std::cos(pitch_) * std::sin(yaw_),
            std::sin(pitch_),
            -std::cos(pitch_) * std::cos(yaw_)
        };
        target_ = position_ + forward * distance_;
    }

    viewMatrix_ = glm::lookAt(position_, target_, glm::vec3{0.0f, 1.0f, 0.0f});
    dynamicNear_ = std::max(0.0005f, std::min(nearPlane_, distance_ * 0.01f));
    dynamicFar_ = std::max(farPlane_, distance_ * 10.0f);
    // Use WebGPU 0..1 depth clip space
    projMatrix_ = glm::perspectiveRH_ZO(fovY_, aspectRatio, dynamicNear_, dynamicFar_);
}

Ray Camera::ScreenPointToRay(float screenX, float screenY, float screenWidth, float screenHeight) const noexcept {
    const float ndcX{(2.0f * screenX) / screenWidth - 1.0f};
    const float ndcY{1.0f - (2.0f * screenY) / screenHeight};

    const glm::mat4 invVP{glm::inverse(projMatrix_ * viewMatrix_)};
    glm::vec4 nearPoint{invVP * glm::vec4{ndcX, ndcY, 0.0f, 1.0f}};
    glm::vec4 farPoint{invVP * glm::vec4{ndcX, ndcY, 1.0f, 1.0f}};

    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    Ray ray{};
    ray.origin = glm::vec3{nearPoint};
    ray.direction = glm::normalize(glm::vec3{farPoint - nearPoint});
    return ray;
}

} // namespace bgl::core
