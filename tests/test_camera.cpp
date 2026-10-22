/**
 * @file test_camera.cpp
 * @brief Unit tests for Camera orbit, pan, zoom, and screen-to-world ray unprojection.
 */
#include <gtest/gtest.h>
#include "core/Camera.hpp"

using namespace bgl::core;

TEST(CameraTest, DefaultInitialization) {
    glm::vec3 target(0.0f, 1.0f, 0.0f);
    Camera camera(target, 10.0f);
    camera.Update(16.0f / 9.0f);

    EXPECT_EQ(camera.GetTarget(), target);
    EXPECT_FLOAT_EQ(camera.GetDistance(), 10.0f);

    glm::vec3 pos = camera.GetPosition();
    EXPECT_NEAR(glm::distance(pos, target), 10.0f, 1e-3f);

    glm::mat4 view = camera.GetViewMatrix();
    glm::mat4 proj = camera.GetProjectionMatrix();
    glm::mat4 vp = camera.GetViewProjectionMatrix();

    EXPECT_EQ(vp, proj * view);
}

TEST(CameraTest, OrbitRotation) {
    glm::vec3 target(0.0f, 0.0f, 0.0f);
    Camera camera(target, 5.0f);
    camera.Update(1.0f);
    glm::vec3 initialPos = camera.GetPosition();

    // Orbit 90 degrees yaw
    camera.Orbit(glm::half_pi<float>(), 0.0f);
    camera.Update(1.0f);
    glm::vec3 rotatedPos = camera.GetPosition();

    EXPECT_NE(initialPos.x, rotatedPos.x);
    EXPECT_NEAR(glm::distance(rotatedPos, target), 5.0f, 1e-3f);
}

TEST(CameraTest, PanMovement) {
    glm::vec3 target(0.0f, 0.0f, 0.0f);
    Camera camera(target, 5.0f);
    camera.Update(1.0f);
    glm::vec3 initialPos = camera.GetPosition();

    camera.Pan(2.0f, 3.0f);
    camera.Update(1.0f);

    EXPECT_NE(camera.GetTarget(), target);
    EXPECT_NE(camera.GetPosition(), initialPos);
    // Distance from position to target remains invariant under pure pan
    EXPECT_NEAR(glm::distance(camera.GetPosition(), camera.GetTarget()), 5.0f, 1e-3f);
}

TEST(CameraTest, ZoomClamping) {
    Camera camera(glm::vec3(0.0f), 5.0f);
    camera.Update(1.0f);

    // Zoom in
    camera.Zoom(2.0f);
    EXPECT_FLOAT_EQ(camera.GetDistance(), 3.0f);

    // Zoom past target (should clamp to >= 0.001f)
    camera.Zoom(100.0f);
    EXPECT_GE(camera.GetDistance(), 0.001f);
}

TEST(CameraTest, ScreenPointToRayUnprojection) {
    Camera camera(glm::vec3(0.0f, 0.0f, 0.0f), 5.0f);
    camera.Update(16.0f / 9.0f);

    // Center of screen (640, 360 in 1280x720)
    Ray centerRay = camera.ScreenPointToRay(640.0f, 360.0f, 1280.0f, 720.0f);

    // Center ray direction must point towards target
    glm::vec3 expectedDir = glm::normalize(camera.GetTarget() - camera.GetPosition());
    EXPECT_NEAR(centerRay.direction.x, expectedDir.x, 1e-2f);
    EXPECT_NEAR(centerRay.direction.y, expectedDir.y, 1e-2f);
    EXPECT_NEAR(centerRay.direction.z, expectedDir.z, 1e-2f);

    // Ray origin must be near camera position
    EXPECT_NEAR(centerRay.origin.x, camera.GetPosition().x, 0.5f);
    EXPECT_NEAR(centerRay.origin.y, camera.GetPosition().y, 0.5f);
    EXPECT_NEAR(centerRay.origin.z, camera.GetPosition().z, 0.5f);
}
