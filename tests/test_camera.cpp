/**
 * @file test_camera.cpp
 * @brief Unit tests for Camera orbit, pan, zoom, and screen-to-world ray unprojection.
 */
#include "core/Camera.hpp"
#include <gtest/gtest.h>

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

TEST(CameraTest, ExtremePitchAngleClamping) {
    Camera camera(glm::vec3(0.0f), 5.0f);
    camera.Update(1.0f);

    // Apply extreme positive pitch (+50 radians)
    camera.Orbit(0.0f, 50.0f);
    camera.Update(1.0f);
    glm::vec3 posHigh = camera.GetPosition();
    EXPECT_FALSE(std::isnan(posHigh.x) || std::isnan(posHigh.y) || std::isnan(posHigh.z));
    EXPECT_GT(posHigh.y, 0.0f); // Looking down from above
    EXPECT_NEAR(glm::distance(posHigh, camera.GetTarget()), 5.0f, 1e-3f);

    // Apply extreme negative pitch (-100 radians)
    camera.Orbit(0.0f, -100.0f);
    camera.Update(1.0f);
    glm::vec3 posLow = camera.GetPosition();
    EXPECT_FALSE(std::isnan(posLow.x) || std::isnan(posLow.y) || std::isnan(posLow.z));
    EXPECT_LT(posLow.y, 0.0f); // Looking up from below
    EXPECT_NEAR(glm::distance(posLow, camera.GetTarget()), 5.0f, 1e-3f);
}

TEST(CameraTest, UltrawideAndPortraitAspectRatios) {
    Camera camera(glm::vec3(0.0f, 1.0f, 0.0f), 6.0f);

    // 1. Ultrawide 32:9 (aspect ratio ~3.555)
    camera.Update(32.0f / 9.0f);
    glm::mat4 ultrawideProj = camera.GetProjectionMatrix();
    EXPECT_FALSE(std::isnan(ultrawideProj[0][0]) || ultrawideProj[0][0] == 0.0f);
    EXPECT_FALSE(std::isnan(ultrawideProj[1][1]) || ultrawideProj[1][1] == 0.0f);

    Frustum ultrawideFrustum = camera.GetFrustum();
    for (const auto& plane : ultrawideFrustum.planes) {
        EXPECT_NEAR(glm::length(plane.normal), 1.0f, 1e-4f);
    }

    // 2. Portrait 9:16 (mobile screen aspect ratio ~0.5625)
    camera.Update(9.0f / 16.0f);
    glm::mat4 portraitProj = camera.GetProjectionMatrix();
    EXPECT_FALSE(std::isnan(portraitProj[0][0]) || portraitProj[0][0] == 0.0f);
    // In portrait, horizontal FOV is narrower so proj[0][0] should be larger than ultrawide
    EXPECT_GT(portraitProj[0][0], ultrawideProj[0][0]);
}

TEST(CameraTest, FrustumCornersUnprojectionDivergence) {
    Camera camera(glm::vec3(0.0f, 0.0f, 0.0f), 8.0f);
    camera.Update(16.0f / 9.0f);

    const float width = 1920.0f;
    const float height = 1080.0f;

    // Four viewport corners
    Ray topLeft = camera.ScreenPointToRay(0.0f, 0.0f, width, height);
    Ray topRight = camera.ScreenPointToRay(width, 0.0f, width, height);
    Ray bottomLeft = camera.ScreenPointToRay(0.0f, height, width, height);
    Ray bottomRight = camera.ScreenPointToRay(width, height, width, height);

    // All ray directions must be unit vectors
    EXPECT_NEAR(glm::length(topLeft.direction), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(topRight.direction), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(bottomLeft.direction), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(bottomRight.direction), 1.0f, 1e-4f);

    // Left rays should have smaller X than right rays
    EXPECT_LT(topLeft.direction.x, topRight.direction.x);
    EXPECT_LT(bottomLeft.direction.x, bottomRight.direction.x);

    // Top rays should have larger Y than bottom rays in standard view
    EXPECT_GT(topLeft.direction.y, bottomLeft.direction.y);
    EXPECT_GT(topRight.direction.y, bottomRight.direction.y);
}

TEST(CameraTest, CameraTargetAndFrustumContainment) {
    Camera camera(glm::vec3(2.0f, 3.0f, -4.0f), 12.0f);
    camera.Update(1.0f);

    Frustum frustum = camera.GetFrustum();
    // The look-at target must have positive distance to all 6 frustum planes (inside view)
    for (const auto& plane : frustum.planes) {
        EXPECT_GT(plane.DistanceToPoint(camera.GetTarget()), 0.0f);
    }
}

TEST(CameraTest, NearAndFarClippingPlaneProperties) {
    Camera camera(glm::vec3(0.0f), 5.0f);
    camera.Update(16.0f / 9.0f);

    EXPECT_GT(camera.GetNearPlane(), 0.0f);
    EXPECT_GT(camera.GetFarPlane(), camera.GetNearPlane());
    EXPECT_LE(camera.GetNearPlane(), 1.0f);
    EXPECT_GE(camera.GetFarPlane(), 100.0f);
}

TEST(CameraTest, WASDAndArrowKeyNavigation) {
    Camera camera(glm::vec3(0.0f, 0.0f, 0.0f), 5.0f);
    camera.Update(16.0f / 9.0f);

    const glm::vec3 initialTarget = camera.GetTarget();
    const glm::vec3 initialPos = camera.GetPosition();

    // 1. MoveForward (W / Up Arrow)
    camera.MoveForward(2.0f);
    camera.Update(16.0f / 9.0f);
    EXPECT_NE(camera.GetTarget(), initialTarget);
    EXPECT_NE(camera.GetPosition(), initialPos);
    // Pure forward/backward moves both target and position along view direction without changing distance
    EXPECT_NEAR(glm::distance(camera.GetPosition(), camera.GetTarget()), 5.0f, 1e-3f);

    // 2. MoveBackward (S / Down Arrow) cancels forward
    camera.MoveForward(-2.0f);
    camera.Update(16.0f / 9.0f);
    EXPECT_NEAR(glm::distance(camera.GetTarget(), initialTarget), 0.0f, 1e-3f);

    // 3. MoveRight (D / Right Arrow)
    camera.MoveRight(1.5f);
    camera.Update(16.0f / 9.0f);
    EXPECT_NE(camera.GetTarget(), initialTarget);
    EXPECT_NEAR(glm::distance(camera.GetPosition(), camera.GetTarget()), 5.0f, 1e-3f);

    // 4. MoveLeft (A / Left Arrow) cancels right
    camera.MoveRight(-1.5f);
    camera.Update(16.0f / 9.0f);
    EXPECT_NEAR(glm::distance(camera.GetTarget(), initialTarget), 0.0f, 1e-3f);

    // 5. MoveUp / MoveDown
    camera.MoveUp(3.0f);
    camera.Update(16.0f / 9.0f);
    EXPECT_NEAR(camera.GetTarget().y, 3.0f, 1e-3f);
}

TEST(CameraTest, KeyboardZoomInOut) {
    Camera camera(glm::vec3(0.0f), 8.0f);
    camera.Update(1.0f);

    // '+' zooms in (distance decreases)
    camera.Zoom(2.5f);
    EXPECT_FLOAT_EQ(camera.GetDistance(), 5.5f);

    // '-' zooms out (distance increases)
    camera.Zoom(-4.0f);
    EXPECT_FLOAT_EQ(camera.GetDistance(), 9.5f);
}

TEST(CameraTest, DiagonalWASDNavigationInvariance) {
    Camera camera(glm::vec3(0.0f, 0.0f, 0.0f), 7.5f);
    camera.Update(16.0f / 9.0f);

    const glm::vec3 startTarget = camera.GetTarget();
    const glm::vec3 startPos = camera.GetPosition();

    // Simultaneous W + D (Forward + Right)
    camera.MoveForward(2.0f);
    camera.MoveRight(2.0f);
    camera.Update(16.0f / 9.0f);

    // Camera target and position both translate on horizontal plane
    EXPECT_NEAR(glm::distance(camera.GetPosition(), camera.GetTarget()), 7.5f, 1e-3f);
    EXPECT_NEAR(camera.GetTarget().y, startTarget.y, 1e-4f); // Horizontal plane invariance

    // Reverse with S + A (Backward + Left)
    camera.MoveForward(-2.0f);
    camera.MoveRight(-2.0f);
    camera.Update(16.0f / 9.0f);

    EXPECT_NEAR(glm::distance(camera.GetTarget(), startTarget), 0.0f, 1e-3f);
    EXPECT_NEAR(glm::distance(camera.GetPosition(), startPos), 0.0f, 1e-3f);
}

TEST(CameraTest, ExtremeDistanceDynamicClippingPlanes) {
    // 1. Very close distance (macro / close-up inspection: 0.05m)
    Camera camClose(glm::vec3(0.0f), 0.05f);
    camClose.Update(1.0f);
    EXPECT_GT(camClose.GetNearPlane(), 0.0f);
    EXPECT_LT(camClose.GetNearPlane(), 0.01f); // Dynamic near plane scales with small distance

    // 2. Huge distance (landscape / orbital: 500m)
    Camera camFar(glm::vec3(0.0f), 500.0f);
    camFar.Update(1.0f);
    EXPECT_GT(camFar.GetFarPlane(), 1000.0f); // Dynamic far plane expands to contain scene
    EXPECT_GT(camFar.GetFarPlane(), camFar.GetNearPlane());
}

TEST(CameraTest, FlyModeFreeNavigation) {
    Camera camera(glm::vec3(0.0f, 1.5f, 0.0f), 5.0f);
    camera.Update(16.0f / 9.0f);
    EXPECT_EQ(camera.GetMode(), CameraMode::Orbit);

    camera.SetMode(CameraMode::Fly);
    EXPECT_EQ(camera.GetMode(), CameraMode::Fly);

    const glm::vec3 initialEye = camera.GetPosition();
    camera.MoveForward(3.0f);
    camera.Update(16.0f / 9.0f);

    // In fly mode, MoveForward directly translates the camera eye position
    EXPECT_NE(camera.GetPosition(), initialEye);
    camera.MoveForward(-3.0f);
    camera.Update(16.0f / 9.0f);
    EXPECT_NEAR(glm::distance(camera.GetPosition(), initialEye), 0.0f, 1e-3f);
}

TEST(CameraTest, FocusOnBoundsFraming) {
    Camera camera(glm::vec3(100.0f), 2.0f);
    camera.Update(16.0f / 9.0f);

    AABB box{};
    box.minBound = glm::vec3{-2.0f, -1.0f, -3.0f};
    box.maxBound = glm::vec3{2.0f, 3.0f, 1.0f};

    camera.FocusOnBounds(box, 1.25f);
    EXPECT_NEAR(camera.GetTarget().x, 0.0f, 1e-3f);
    EXPECT_NEAR(camera.GetTarget().y, 1.0f, 1e-3f);
    EXPECT_NEAR(camera.GetTarget().z, -1.0f, 1e-3f);

    // Distance must comfortably encompass bounding radius (> extents length)
    const float radius = glm::length(box.Extents());
    EXPECT_GT(camera.GetDistance(), radius);
}

TEST(CameraTest, CameraBookmarksSaveAndRecall) {
    Camera camera(glm::vec3(0.0f, 0.0f, 0.0f), 10.0f);
    camera.Orbit(0.5f, 0.2f);
    camera.Update(16.0f / 9.0f);

    // Save slot 0 (Key '1')
    EXPECT_TRUE(camera.SaveBookmark(0));
    EXPECT_TRUE(camera.HasBookmark(0));
    EXPECT_FALSE(camera.HasBookmark(1));

    const glm::vec3 savedPos = camera.GetPosition();
    const glm::vec3 savedTarget = camera.GetTarget();

    // Mutate camera position drastically
    camera.Pan(15.0f, 20.0f);
    camera.Zoom(6.0f);
    camera.Orbit(1.0f, -0.4f);
    camera.Update(16.0f / 9.0f);
    EXPECT_NE(camera.GetPosition(), savedPos);

    // Restore slot 0
    EXPECT_TRUE(camera.RestoreBookmark(0));
    EXPECT_NEAR(glm::distance(camera.GetPosition(), savedPos), 0.0f, 1e-3f);
    EXPECT_NEAR(glm::distance(camera.GetTarget(), savedTarget), 0.0f, 1e-3f);

    // Out of range slot fails safely
    EXPECT_FALSE(camera.SaveBookmark(9));
    EXPECT_FALSE(camera.RestoreBookmark(9));
}

TEST(CameraTest, glTFCameraTransformIngestion) {
    Camera camera{};
    // Construct a view matrix positioned at (5, 2, 8) looking along -Z
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 2.0f, 8.0f));

    camera.SetFromTransform(transform, glm::radians(60.0f), 0.2f, 500.0f);
    EXPECT_NEAR(camera.GetPosition().x, 5.0f, 1e-3f);
    EXPECT_NEAR(camera.GetPosition().y, 2.0f, 1e-3f);
    EXPECT_NEAR(camera.GetPosition().z, 8.0f, 1e-3f);
    EXPECT_FLOAT_EQ(camera.GetFieldOfView(), glm::radians(60.0f));
    EXPECT_EQ(camera.GetMode(), CameraMode::Fly);
}
