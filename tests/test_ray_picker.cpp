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

TEST(RayPickerTest, MollerTrumboreParallelOrDegenerate) {
    bgl::core::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);

    // Ray is in the plane of the triangle (parallel / edge-on)
    glm::vec3 v0(-1.0f, 0.0f, 5.0f);
    glm::vec3 v1( 1.0f, 0.0f, 5.0f);
    glm::vec3 v2( 0.0f, 0.0f, 0.0f);

    float distance = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    bool hit = bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, distance, hitPoint, normal);
    EXPECT_FALSE(hit);

    // Degenerate triangle (points are collinear)
    glm::vec3 d0(0.0f, 0.0f, 0.0f);
    glm::vec3 d1(1.0f, 1.0f, 0.0f);
    glm::vec3 d2(2.0f, 2.0f, 0.0f);
    EXPECT_FALSE(bgl::scene::RayPicker::IntersectTriangle(ray, d0, d1, d2, distance, hitPoint, normal));
}

TEST(RayPickerTest, ScenePickingNearestNode) {
    bgl::scene::Scene scene;

    // Node 1: Near triangle at Z = 2
    auto nodeNear = std::make_shared<bgl::scene::SceneNode>();
    nodeNear->name = "NearObject";
    bgl::scene::MeshPrimitive primNear;
    bgl::core::Vertex v0, v1, v2;
    v0.position = glm::vec3(-2.0f, -2.0f, 2.0f);
    v1.position = glm::vec3( 2.0f, -2.0f, 2.0f);
    v2.position = glm::vec3( 0.0f,  2.0f, 2.0f);
    primNear.vertices = { v0, v1, v2 };
    primNear.indices = { 0, 1, 2 };
    primNear.boundingBox.Expand(v0.position);
    primNear.boundingBox.Expand(v1.position);
    primNear.boundingBox.Expand(v2.position);
    nodeNear->primitives.push_back(std::move(primNear));

    // Node 2: Far triangle at Z = -5
    auto nodeFar = std::make_shared<bgl::scene::SceneNode>();
    nodeFar->name = "FarObject";
    bgl::scene::MeshPrimitive primFar;
    v0.position = glm::vec3(-5.0f, -5.0f, -5.0f);
    v1.position = glm::vec3( 5.0f, -5.0f, -5.0f);
    v2.position = glm::vec3( 0.0f,  5.0f, -5.0f);
    primFar.vertices = { v0, v1, v2 };
    primFar.indices = { 0, 1, 2 };
    primFar.boundingBox.Expand(v0.position);
    primFar.boundingBox.Expand(v1.position);
    primFar.boundingBox.Expand(v2.position);
    nodeFar->primitives.push_back(std::move(primFar));

    scene.rootNodes.push_back(nodeNear);
    scene.rootNodes.push_back(nodeFar);
    scene.Update();

    // Ray starting at (0, 0, 10) looking down -Z
    bgl::core::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 10.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);

    auto hit = bgl::scene::RayPicker::Pick(ray, scene);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->hitNode->name, "NearObject");
    EXPECT_NEAR(hit->distance, 8.0f, 1e-3f); // from 10 to 2 = 8
    EXPECT_NEAR(hit->hitPoint.z, 2.0f, 1e-3f);
}


