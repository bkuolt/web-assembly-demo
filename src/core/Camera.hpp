/**
 * @file Camera.hpp
 * @brief Orbit, Pan, and Zoom 3D Camera system.
 */
#pragma once

#include "core/Types.hpp"
#include <array>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace bgl::core {

/**
 * @enum CameraMode
 * @brief Navigation mode of the 3D camera.
 */
enum class CameraMode : uint32_t {
    Orbit = 0, ///< Orbits around a 3D target point at a radial distance.
    Fly = 1    ///< First-person free-look / WASD flight through scene interior.
};

/**
 * @struct CameraBookmark
 * @brief Stored viewpoint bookmark state for rapid scene recall.
 */
struct CameraBookmark {
    glm::vec3 position{0.0f};
    glm::vec3 target{0.0f};
    float distance{5.0f};
    float yaw{0.0f};
    float pitch{0.3f};
    CameraMode mode{CameraMode::Orbit};
    bool valid{false};
};

/**
 * @class Camera
 * @brief Free-orbit, pan, zoom, and first-person fly camera with bookmarks and bounds framing.
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
     * @brief Sets camera navigation mode (Orbit or Fly).
     */
    void SetMode(CameraMode mode) noexcept { mode_ = mode; }

    /**
     * @brief Toggles between Orbit and Fly navigation modes.
     */
    void ToggleMode() noexcept { SetMode(mode_ == CameraMode::Orbit ? CameraMode::Fly : CameraMode::Orbit); }

    [[nodiscard]] CameraMode GetMode() const noexcept { return mode_; }

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
     * @brief Translates the camera forward along the horizontal view direction.
     * @param delta Forward displacement (positive moves forward, negative moves backward).
     */
    void MoveForward(float delta) noexcept;

    /**
     * @brief Translates the camera laterally along the right vector.
     * @param delta Lateral displacement (positive moves right, negative moves left).
     */
    void MoveRight(float delta) noexcept;

    /**
     * @brief Translates the camera vertically along the world up vector.
     * @param delta Vertical displacement (positive moves up, negative moves down).
     */
    void MoveUp(float delta) noexcept;

    /**
     * @brief Adjusts the orbit distance smoothly.
     * @param deltaZoom Distance change (positive zooms in, negative zooms out).
     */
    void Zoom(float deltaZoom) noexcept;

    /**
     * @brief Frames the camera target and distance smoothly to encompass an AABB bounding box.
     * @param bounds 3D axis-aligned bounding box to focus on.
     * @param margin Distance multiplier ensuring comfortable padding in the viewport.
     */
    void FocusOnBounds(const AABB& bounds, float margin = 1.25f) noexcept;

    /**
     * @brief Saves the current camera viewpoint into a bookmark slot (0..8).
     * @param slot Bookmark index from 0 to 8 (corresponding to keys 1..9).
     * @return True if slot is within range and successfully saved.
     */
    bool SaveBookmark(size_t slot) noexcept;

    /**
     * @brief Restores a previously saved camera viewpoint bookmark.
     * @param slot Bookmark index from 0 to 8.
     * @return True if slot is valid and has stored data.
     */
    bool RestoreBookmark(size_t slot) noexcept;

    [[nodiscard]] bool HasBookmark(size_t slot) const noexcept;
    [[nodiscard]] const CameraBookmark* GetBookmark(size_t slot) const noexcept;

    /**
     * @brief Sets camera viewpoint directly from a 4x4 world transform matrix (glTF camera).
     */
    void SetFromTransform(const glm::mat4& transform, float fovY = glm::radians(45.0f), float nearPlane = 0.1f,
                          float farPlane = 1000.0f) noexcept;

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
    [[nodiscard]] float GetNearPlane() const noexcept { return dynamicNear_; }
    [[nodiscard]] float GetFarPlane() const noexcept { return dynamicFar_; }
    [[nodiscard]] float GetYaw() const noexcept { return yaw_; }
    [[nodiscard]] float GetPitch() const noexcept { return pitch_; }
    [[nodiscard]] float GetFieldOfView() const noexcept { return fovY_; }
    void SetFieldOfView(float fovY) noexcept { fovY_ = std::clamp(fovY, 0.05f, 3.10f); }
    void SetYawPitch(float yaw, float pitch) noexcept {
        yaw_ = yaw;
        pitch_ = std::clamp(pitch, -1.55f, 1.55f);
    }
    void SetPositionAndTarget(const glm::vec3& position, const glm::vec3& target) noexcept;

    [[nodiscard]] Frustum GetFrustum() const noexcept { return Frustum::FromViewProjection(GetViewProjectionMatrix()); }

    /**
     * @brief Unproject screen coordinates (normalized 0..1) into a 3D ray in world space.
     */
    [[nodiscard]] Ray ScreenPointToRay(float screenX, float screenY, float screenWidth,
                                       float screenHeight) const noexcept;

private:
    CameraMode mode_{CameraMode::Orbit};
    glm::vec3 target_{0.0f};
    float distance_{5.0f};
    float yaw_{0.0f};
    float pitch_{0.3f};
    float fovY_{glm::radians(45.0f)};
    float nearPlane_{0.1f};
    float farPlane_{1000.0f};
    float dynamicNear_{0.1f};
    float dynamicFar_{1000.0f};

    glm::vec3 position_{0.0f};
    glm::mat4 viewMatrix_{1.0f};
    glm::mat4 projMatrix_{1.0f};

    std::array<CameraBookmark, 9> bookmarks_{};
};

} // namespace bgl::core
