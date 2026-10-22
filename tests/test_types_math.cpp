/**
 * @file test_types_math.cpp
 * @brief Comprehensive unit tests for core 3D types: AABB, Plane, Frustum, Ray, and Vertex layout.
 */
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include "core/Types.hpp"

using namespace bgl::core;

TEST(TypesMathTest, AABBInitializationAndExpansion) {
    AABB box;
    // Default box is inverted (minBound > maxBound)
    EXPECT_GT(box.minBound.x, box.maxBound.x);
    EXPECT_GT(box.minBound.y, box.maxBound.y);
    EXPECT_GT(box.minBound.z, box.maxBound.z);

    // Expand with single point
    box.Expand(glm::vec3(2.0f, -3.0f, 5.0f));
    EXPECT_EQ(box.minBound, glm::vec3(2.0f, -3.0f, 5.0f));
    EXPECT_EQ(box.maxBound, glm::vec3(2.0f, -3.0f, 5.0f));

    // Expand with second point
    box.Expand(glm::vec3(-4.0f, 6.0f, 1.0f));
    EXPECT_EQ(box.minBound, glm::vec3(-4.0f, -3.0f, 1.0f));
    EXPECT_EQ(box.maxBound, glm::vec3(2.0f, 6.0f, 5.0f));

    // Center and Extents
    glm::vec3 center = box.Center();
    EXPECT_FLOAT_EQ(center.x, -1.0f);
    EXPECT_FLOAT_EQ(center.y, 1.5f);
    EXPECT_FLOAT_EQ(center.z, 3.0f);

    glm::vec3 extents = box.Extents();
    EXPECT_FLOAT_EQ(extents.x, 3.0f);
    EXPECT_FLOAT_EQ(extents.y, 4.5f);
    EXPECT_FLOAT_EQ(extents.z, 2.0f);
}

TEST(TypesMathTest, AABBBoxExpansion) {
    AABB box1;
    box1.Expand(glm::vec3(0.0f, 0.0f, 0.0f));
    box1.Expand(glm::vec3(2.0f, 2.0f, 2.0f));

    AABB box2;
    box2.Expand(glm::vec3(-1.0f, 1.0f, 3.0f));
    box2.Expand(glm::vec3(4.0f, 5.0f, 6.0f));

    box1.Expand(box2);
    EXPECT_EQ(box1.minBound, glm::vec3(-1.0f, 0.0f, 0.0f));
    EXPECT_EQ(box1.maxBound, glm::vec3(4.0f, 5.0f, 6.0f));
}

TEST(TypesMathTest, AABBRayIntersection) {
    AABB box;
    box.Expand(glm::vec3(-1.0f, -1.0f, -1.0f));
    box.Expand(glm::vec3(1.0f, 1.0f, 1.0f));

    float tMin = 0.0f;
    float tMax = 0.0f;

    // Direct hit along Z
    Ray ray1{ glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
    EXPECT_TRUE(box.Intersects(ray1, tMin, tMax));
    EXPECT_NEAR(tMin, 4.0f, 1e-4f);
    EXPECT_NEAR(tMax, 6.0f, 1e-4f);

    // Direct hit from inside box
    Ray rayInside{ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
    EXPECT_TRUE(box.Intersects(rayInside, tMin, tMax));
    EXPECT_NEAR(tMin, 0.0f, 1e-4f);
    EXPECT_NEAR(tMax, 1.0f, 1e-4f);

    // Miss - ray pointing opposite direction
    Ray rayAway{ glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
    EXPECT_FALSE(box.Intersects(rayAway, tMin, tMax));

    // Miss - parallel to box
    Ray rayParallel{ glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
    EXPECT_FALSE(box.Intersects(rayParallel, tMin, tMax));
}

TEST(TypesMathTest, PlaneNormalizationAndDistance) {
    Plane plane;
    plane.normal = glm::vec3(0.0f, 3.0f, 4.0f);
    plane.distance = 10.0f;

    plane.Normalize();
    EXPECT_NEAR(glm::length(plane.normal), 1.0f, 1e-5f);
    EXPECT_NEAR(plane.normal.y, 3.0f / 5.0f, 1e-5f);
    EXPECT_NEAR(plane.normal.z, 4.0f / 5.0f, 1e-5f);
    EXPECT_NEAR(plane.distance, 2.0f, 1e-5f);

    // Point on positive side
    EXPECT_GT(plane.DistanceToPoint(glm::vec3(0.0f, 10.0f, 0.0f)), 0.0f);

    // Point on negative side
    EXPECT_LT(plane.DistanceToPoint(glm::vec3(0.0f, -10.0f, 0.0f)), 0.0f);
}

TEST(TypesMathTest, FrustumPerspectivePlaneExtraction) {
    glm::vec3 camPos(0.0f, 2.0f, 10.0f);
    glm::vec3 target(0.0f, 2.0f, 0.0f);
    glm::mat4 view = glm::lookAt(camPos, target, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    glm::mat4 vp = proj * view;

    Frustum frustum = Frustum::FromViewProjection(vp);

    // All plane normals must be unit vectors
    for (const auto& plane : frustum.planes) {
        EXPECT_NEAR(glm::length(plane.normal), 1.0f, 1e-4f);
    }

    // Target point (0, 2, 0) is well within frustum
    for (const auto& plane : frustum.planes) {
        EXPECT_GT(plane.DistanceToPoint(target), 0.0f);
    }

    // Point behind camera must be on negative side of near plane
    glm::vec3 behindPoint(0.0f, 2.0f, 15.0f);
    EXPECT_LT(frustum.planes[4].DistanceToPoint(behindPoint), 0.0f);
}

TEST(TypesMathTest, FrustumIntersectsAABBAllCases) {
    glm::vec3 camPos(0.0f, 0.0f, 10.0f);
    glm::vec3 target(0.0f, 0.0f, 0.0f);
    glm::mat4 view = glm::lookAt(camPos, target, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 1.0f, 20.0f);
    Frustum frustum = Frustum::FromViewProjection(proj * view);

    // 1. Box centered at target (fully inside)
    AABB boxCenter;
    boxCenter.Expand(glm::vec3(-1.0f));
    boxCenter.Expand(glm::vec3(1.0f));
    EXPECT_TRUE(frustum.IntersectsAABB(boxCenter));

    // 2. Box behind camera (Z > 10)
    AABB boxBehind;
    boxBehind.Expand(glm::vec3(-1.0f, -1.0f, 12.0f));
    boxBehind.Expand(glm::vec3(1.0f, 1.0f, 15.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxBehind));

    // 3. Box beyond far plane (Z < -15, dist from cam > 25)
    AABB boxBeyondFar;
    boxBeyondFar.Expand(glm::vec3(-1.0f, -1.0f, -25.0f));
    boxBeyondFar.Expand(glm::vec3(1.0f, 1.0f, -22.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxBeyondFar));

    // 4. Box far off to the right
    AABB boxFarRight;
    boxFarRight.Expand(glm::vec3(50.0f, -1.0f, 0.0f));
    boxFarRight.Expand(glm::vec3(55.0f, 1.0f, 2.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxFarRight));

    // 5. Box far above
    AABB boxFarAbove;
    boxFarAbove.Expand(glm::vec3(-1.0f, 40.0f, 0.0f));
    boxFarAbove.Expand(glm::vec3(1.0f, 45.0f, 2.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxFarAbove));
}

TEST(TypesMathTest, VertexLayoutAndCacheLineAlignment) {
    EXPECT_EQ(sizeof(Vertex), 64);
    EXPECT_EQ(alignof(Vertex), 16);

    Vertex v;
    v.position = glm::vec3(1.0f, 2.0f, 3.0f);
    v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    v.uv = glm::vec2(0.5f, 0.5f);

    EXPECT_FLOAT_EQ(v.position.x, 1.0f);
    EXPECT_FLOAT_EQ(v.normal.y, 1.0f);
    EXPECT_FLOAT_EQ(v.tangent.w, 1.0f);
    EXPECT_FLOAT_EQ(v.uv.x, 0.5f);
}

