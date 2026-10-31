/**
 * @file test_ray_picker.cpp
 * @brief Unit tests for Möller-Trumbore ray-triangle intersection.
 */
#include "scene/RayPicker.hpp"
#include <gtest/gtest.h>

TEST(RayPickerTest, MollerTrumboreDirectHit) {
    bgl::core::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);

    glm::vec3 v0(-1.0f, -1.0f, 0.0f);
    glm::vec3 v1(1.0f, -1.0f, 0.0f);
    glm::vec3 v2(0.0f, 1.0f, 0.0f);

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
    glm::vec3 v1(1.0f, -1.0f, 0.0f);
    glm::vec3 v2(0.0f, 1.0f, 0.0f);

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
    glm::vec3 v1(1.0f, 0.0f, 5.0f);
    glm::vec3 v2(0.0f, 0.0f, 0.0f);

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
    v1.position = glm::vec3(2.0f, -2.0f, 2.0f);
    v2.position = glm::vec3(0.0f, 2.0f, 2.0f);
    primNear.vertices = {v0, v1, v2};
    primNear.indices = {0, 1, 2};
    primNear.boundingBox.Expand(v0.position);
    primNear.boundingBox.Expand(v1.position);
    primNear.boundingBox.Expand(v2.position);
    nodeNear->primitives.push_back(std::move(primNear));

    // Node 2: Far triangle at Z = -5
    auto nodeFar = std::make_shared<bgl::scene::SceneNode>();
    nodeFar->name = "FarObject";
    bgl::scene::MeshPrimitive primFar;
    v0.position = glm::vec3(-5.0f, -5.0f, -5.0f);
    v1.position = glm::vec3(5.0f, -5.0f, -5.0f);
    v2.position = glm::vec3(0.0f, 5.0f, -5.0f);
    primFar.vertices = {v0, v1, v2};
    primFar.indices = {0, 1, 2};
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

TEST(RayPickerTest, RayOriginBehindTrianglePointingAway) {
    bgl::core::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 5.0f);
    // Pointing in +Z direction (away from triangle at Z = 0)
    ray.direction = glm::vec3(0.0f, 0.0f, 1.0f);

    glm::vec3 v0(-2.0f, -2.0f, 0.0f);
    glm::vec3 v1(2.0f, -2.0f, 0.0f);
    glm::vec3 v2(0.0f, 2.0f, 0.0f);

    float distance = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    // Must return false because intersection distance is negative (-5.0)
    bool hit = bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, distance, hitPoint, normal);
    EXPECT_FALSE(hit);
}

TEST(RayPickerTest, RayHitsTriangleVerticesAndEdges) {
    glm::vec3 v0(0.0f, 0.0f, 0.0f);
    glm::vec3 v1(4.0f, 0.0f, 0.0f);
    glm::vec3 v2(0.0f, 4.0f, 0.0f);

    float distance = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    // 1. Ray aimed at vertex v0 (0, 0)
    bgl::core::Ray rayV0{glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    EXPECT_TRUE(bgl::scene::RayPicker::IntersectTriangle(rayV0, v0, v1, v2, distance, hitPoint, normal));
    EXPECT_NEAR(distance, 5.0f, 1e-3f);

    // 2. Ray aimed at hypotenuse edge midpoint (2, 2)
    bgl::core::Ray rayEdge{glm::vec3(2.0f, 2.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    EXPECT_TRUE(bgl::scene::RayPicker::IntersectTriangle(rayEdge, v0, v1, v2, distance, hitPoint, normal));
    EXPECT_NEAR(distance, 5.0f, 1e-3f);

    // 3. Ray slightly outside hypotenuse (2.05, 2.05) must miss
    bgl::core::Ray rayOutside{glm::vec3(2.05f, 2.05f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    EXPECT_FALSE(bgl::scene::RayPicker::IntersectTriangle(rayOutside, v0, v1, v2, distance, hitPoint, normal));
}

TEST(RayPickerTest, TriangleHitNormalIsPerpendicularToTrianglePlane) {
    bgl::core::Ray ray{glm::vec3(1.0f, 1.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    // Triangle tilted 45 degrees around X axis
    glm::vec3 v0(0.0f, 0.0f, 0.0f);
    glm::vec3 v1(3.0f, 0.0f, 0.0f);
    glm::vec3 v2(0.0f, 2.0f, 2.0f);

    float distance = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    ASSERT_TRUE(bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, distance, hitPoint, normal));
    EXPECT_NEAR(glm::length(normal), 1.0f, 1e-4f);

    // Normal must be orthogonal to triangle edge vectors
    glm::vec3 e1 = v1 - v0;
    glm::vec3 e2 = v2 - v0;
    EXPECT_NEAR(glm::dot(normal, e1), 0.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(normal, e2), 0.0f, 1e-4f);
}

TEST(RayPickerTest, HierarchicalChildNodeTransformPicking) {
    bgl::scene::Scene scene;

    auto parentNode = std::make_shared<bgl::scene::SceneNode>();
    parentNode->name = "ParentTransformed";
    parentNode->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));

    auto childNode = std::make_shared<bgl::scene::SceneNode>();
    childNode->name = "ChildTarget";
    childNode->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f));

    bgl::scene::MeshPrimitive prim;
    bgl::core::Vertex v0, v1, v2;
    v0.position = glm::vec3(-1.0f, -1.0f, 0.0f);
    v1.position = glm::vec3(1.0f, -1.0f, 0.0f);
    v2.position = glm::vec3(0.0f, 1.0f, 0.0f);
    prim.vertices = {v0, v1, v2};
    prim.indices = {0, 1, 2};
    prim.boundingBox.Expand(v0.position);
    prim.boundingBox.Expand(v1.position);
    prim.boundingBox.Expand(v2.position);
    childNode->primitives.push_back(std::move(prim));

    parentNode->children.push_back(childNode);
    scene.rootNodes.push_back(parentNode);
    scene.Update();

    // World position of child triangle center should be (10, 5, 0)
    bgl::core::Ray ray;
    ray.origin = glm::vec3(10.0f, 5.0f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);

    auto hit = bgl::scene::RayPicker::Pick(ray, scene);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->hitNode->name, "ChildTarget");
    EXPECT_NEAR(hit->distance, 5.0f, 1e-3f);
    EXPECT_NEAR(hit->hitPoint.x, 10.0f, 1e-3f);
    EXPECT_NEAR(hit->hitPoint.y, 5.0f, 1e-3f);
    EXPECT_NEAR(hit->hitPoint.z, 0.0f, 1e-3f);
}

TEST(RayPickerTest, EmptyScenePickReturnsNoHit) {
    bgl::scene::Scene emptyScene;
    bgl::core::Ray ray{glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    auto hit = bgl::scene::RayPicker::Pick(ray, emptyScene);
    EXPECT_FALSE(hit.has_value());
}

TEST(RayPickerTest, TriangleBehindRayOriginRejected) {
    // Triangle is at Z = +10
    glm::vec3 v0(-1.0f, -1.0f, 10.0f);
    glm::vec3 v1(1.0f, -1.0f, 10.0f);
    glm::vec3 v2(0.0f, 1.0f, 10.0f);

    // Ray origin is at Z = 5 looking in negative Z direction (away from triangle)
    bgl::core::Ray ray{glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    float dist = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    bool hit = bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, dist, hitPoint, normal);
    EXPECT_FALSE(hit);
}

TEST(RayPickerTest, RayExactVertexHit) {
    glm::vec3 v0(-2.0f, 0.0f, 0.0f);
    glm::vec3 v1(2.0f, 0.0f, 0.0f);
    glm::vec3 v2(0.0f, 3.0f, 0.0f);

    // Ray directed at vertex v2 (0, 3, 0)
    bgl::core::Ray ray{glm::vec3(0.0f, 3.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    float dist = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    bool hit = bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, dist, hitPoint, normal);
    EXPECT_TRUE(hit);
    EXPECT_NEAR(dist, 10.0f, 1e-3f);
    EXPECT_NEAR(hitPoint.x, 0.0f, 1e-3f);
    EXPECT_NEAR(hitPoint.y, 3.0f, 1e-3f);
}

TEST(RayPickerTest, RayExactEdgeHit) {
    glm::vec3 v0(-2.0f, 0.0f, 0.0f);
    glm::vec3 v1(2.0f, 0.0f, 0.0f);
    glm::vec3 v2(0.0f, 4.0f, 0.0f);

    // Ray directed at midpoint of edge v0-v1: (0, 0, 0)
    bgl::core::Ray ray{glm::vec3(0.0f, 0.0f, 8.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    float dist = 0.0f;
    glm::vec3 hitPoint{0.0f};
    glm::vec3 normal{0.0f};

    bool hit = bgl::scene::RayPicker::IntersectTriangle(ray, v0, v1, v2, dist, hitPoint, normal);
    EXPECT_TRUE(hit);
    EXPECT_NEAR(dist, 8.0f, 1e-3f);
    EXPECT_NEAR(hitPoint.z, 0.0f, 1e-3f);
}

TEST(RayPickerTest, ClosestTriangleDepthSorting) {
    bgl::scene::Scene scene;

    auto makeNode = [](std::string_view name, float zPos) {
        auto node = std::make_shared<bgl::scene::SceneNode>();
        node->name = name;
        node->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, zPos));

        bgl::scene::MeshPrimitive prim;
        bgl::core::Vertex v0, v1, v2;
        v0.position = glm::vec3(-1.0f, -1.0f, 0.0f);
        v1.position = glm::vec3(1.0f, -1.0f, 0.0f);
        v2.position = glm::vec3(0.0f, 1.0f, 0.0f);
        prim.vertices = {v0, v1, v2};
        prim.indices = {0, 1, 2};
        prim.boundingBox.Expand(v0.position);
        prim.boundingBox.Expand(v1.position);
        prim.boundingBox.Expand(v2.position);
        node->primitives.push_back(std::move(prim));
        return node;
    };

    // Three layers along Z: Z=0 (closest to origin Z=10), Z=-5 (middle), Z=-10 (furthest)
    scene.rootNodes.push_back(makeNode("Furthest", -10.0f));
    scene.rootNodes.push_back(makeNode("Closest", 0.0f));
    scene.rootNodes.push_back(makeNode("Middle", -5.0f));
    scene.Update();

    bgl::core::Ray ray{glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    auto hit = bgl::scene::RayPicker::Pick(ray, scene);

    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->hitNode->name, "Closest");
    EXPECT_NEAR(hit->distance, 10.0f, 1e-3f);
}

TEST(RayPickerTest, MultipleRootNodesPicking) {
    bgl::scene::Scene scene;

    auto makeQuadNode = [](std::string_view name, glm::vec3 center) {
        auto node = std::make_shared<bgl::scene::SceneNode>();
        node->name = name;
        node->localTransform = glm::translate(glm::mat4(1.0f), center);

        bgl::scene::MeshPrimitive prim;
        bgl::core::Vertex v0, v1, v2;
        v0.position = glm::vec3(-0.5f, -0.5f, 0.0f);
        v1.position = glm::vec3(0.5f, -0.5f, 0.0f);
        v2.position = glm::vec3(0.0f, 0.5f, 0.0f);
        prim.vertices = {v0, v1, v2};
        prim.indices = {0, 1, 2};
        prim.boundingBox.Expand(v0.position);
        prim.boundingBox.Expand(v1.position);
        prim.boundingBox.Expand(v2.position);
        node->primitives.push_back(std::move(prim));
        return node;
    };

    scene.rootNodes.push_back(makeQuadNode("NodeLeft", glm::vec3(-5.0f, 0.0f, 0.0f)));
    scene.rootNodes.push_back(makeQuadNode("NodeCenter", glm::vec3(0.0f, 0.0f, 0.0f)));
    scene.rootNodes.push_back(makeQuadNode("NodeRight", glm::vec3(5.0f, 0.0f, 0.0f)));
    scene.Update();

    // Pick right node specifically
    bgl::core::Ray rayRight{glm::vec3(5.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    auto hitRight = bgl::scene::RayPicker::Pick(rayRight, scene);
    ASSERT_TRUE(hitRight.has_value());
    EXPECT_EQ(hitRight->hitNode->name, "NodeRight");

    // Pick empty space in between
    bgl::core::Ray rayVoid{glm::vec3(2.5f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    auto hitVoid = bgl::scene::RayPicker::Pick(rayVoid, scene);
    EXPECT_FALSE(hitVoid.has_value());
}
