/**
 * @file test_ray_picker.cpp
 * @brief Unit tests for Möller-Trumbore ray-triangle intersection.
 */
#include <gtest/gtest.h>
#include "scene/RayPicker.hpp"

TEST(RayPickerTest, MollerTrumboreDirectHit) {
    bgl::core::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);

    glm::vec3 v0(-1.0f, -1.0f, 0.0f);
    glm::vec3 v1( 1.0f, -1.0f, 0.0f);
    glm::vec3 v2( 0.0f,  1.0f, 0.0f);

    float distance = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    bool hit = bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, distance, hitPoint, normal);
    EXPECT_TRUE(hit);
    EXPECT_NEAR(distance, 5.0f, 1e-4f);
    EXPECT_NEAR(hitPoint.x, 0.0f, 1e-4f);
    EXPECT_NEAR(hitPoint.y, 0.0f, 1e-4f);
    EXPECT_NEAR(hitPoint.z, 0.0f, 1e-4f);
}

TEST(RayPickerTest, MollerTrumboreMiss) {
    bgl::core::Ray ray;
    ray.origin = glm::vec3(10.0f, 10.0f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);

    glm::vec3 v0(-1.0f, -1.0f, 0.0f);
    glm::vec3 v1( 1.0f, -1.0f, 0.0f);
    glm::vec3 v2( 0.0f,  1.0f, 0.0f);

    float distance = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    bool hit = bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, distance, hitPoint, normal);
    EXPECT_FALSE(hit);
}

