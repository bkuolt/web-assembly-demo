/**
 * @file test_quadtree_lod.cpp
 * @brief Unit tests for Quadtree spatial partitioning and distance-based LOD calculation.
 */
#include <gtest/gtest.h>
#include "scene/Quadtree.hpp"
#include "scene/AssetManager.hpp"
#include "scene/FastGltfLoader.hpp"

TEST(QuadtreeTest, InsertionAndLODQuery) {
    bgl::core::AABB worldBounds;
    worldBounds.minBound = glm::vec3(-100.0f);
    worldBounds.maxBound = glm::vec3(100.0f);

    bgl::scene::Quadtree quadtree(worldBounds);

    auto nearNode = std::make_shared<bgl::scene::SceneNode>();
    nearNode->worldAABB.minBound = glm::vec3(1.0f);
    nearNode->worldAABB.maxBound = glm::vec3(2.0f);
    quadtree.Insert(nearNode);

    auto farNode = std::make_shared<bgl::scene::SceneNode>();
    farNode->worldAABB.minBound = glm::vec3(80.0f);
    farNode->worldAABB.maxBound = glm::vec3(82.0f);
    quadtree.Insert(farNode);

    glm::vec3 cameraPos(0.0f, 0.0f, 0.0f);
    auto visible = quadtree.QueryVisible(cameraPos);

    EXPECT_EQ(visible.size(), 2);
    for (const auto& item : visible) {
        if (item.node == nearNode) {
            EXPECT_EQ(item.currentLOD, bgl::scene::LODLevel::LOD0);
        } else if (item.node == farNode) {
            EXPECT_EQ(item.currentLOD, bgl::scene::LODLevel::LOD2);
        }
    }
}

TEST(QuadtreeTest, SponzaFrustumCullingDiagnostic) {
    bgl::scene::AssetManager assetManager;
    bgl::scene::FastGltfLoader loader(assetManager);
    auto res = loader.LoadFromFile("assets/Sponza.glb");
    if (!res) {
        GTEST_SKIP() << "assets/Sponza.glb not available, skipping Sponza test";
    }
    auto scene = *res;
    std::cout << "[Sponza Diagnostics]\n";
    std::cout << "  Scene minBound: (" << scene->sceneAABB.minBound.x << ", "
              << scene->sceneAABB.minBound.y << ", " << scene->sceneAABB.minBound.z << ")\n";
    std::cout << "  Scene maxBound: (" << scene->sceneAABB.maxBound.x << ", "
              << scene->sceneAABB.maxBound.y << ", " << scene->sceneAABB.maxBound.z << ")\n";

    bgl::core::AABB worldBounds = scene->sceneAABB;
    glm::vec3 margin = glm::max(glm::vec3(10.0f), worldBounds.Extents() * 0.1f);
    worldBounds.minBound -= margin;
    worldBounds.maxBound += margin;

    bgl::scene::Quadtree quadtree(worldBounds);
    size_t totalPrimitives = 0;
    auto countPrims = [&](auto& self, const std::shared_ptr<bgl::scene::SceneNode>& n) -> void {
        totalPrimitives += n->primitives.size();
        for (const auto& c : n->children) self(self, c);
    };
    for (const auto& r : scene->rootNodes) {
        countPrims(countPrims, r);
        quadtree.Insert(r);
    }

    std::cout << "  Total primitives in scene: " << totalPrimitives << "\n";
    std::cout << "  Quadtree node count: " << quadtree.GetNodeCount() << "\n";

    // Inspect some primitive bounds vs scene
    for (const auto& r : scene->rootNodes) {
        auto inspectNode = [&](auto& self, const std::shared_ptr<bgl::scene::SceneNode>& n) -> void {
            std::cout << "Node: " << n->name << "\n";
            std::cout << "  worldTransform[0]: " << n->worldTransform[0][0] << ", " << n->worldTransform[0][1] << ", " << n->worldTransform[0][2] << ", " << n->worldTransform[0][3] << "\n";
            std::cout << "  worldTransform[1]: " << n->worldTransform[1][0] << ", " << n->worldTransform[1][1] << ", " << n->worldTransform[1][2] << ", " << n->worldTransform[1][3] << "\n";
            std::cout << "  worldTransform[2]: " << n->worldTransform[2][0] << ", " << n->worldTransform[2][1] << ", " << n->worldTransform[2][2] << ", " << n->worldTransform[2][3] << "\n";
            std::cout << "  worldTransform[3]: " << n->worldTransform[3][0] << ", " << n->worldTransform[3][1] << ", " << n->worldTransform[3][2] << ", " << n->worldTransform[3][3] << "\n";
            std::cout << "  worldAABB min: (" << n->worldAABB.minBound.x << ", " << n->worldAABB.minBound.y << ", " << n->worldAABB.minBound.z << ")\n";
            std::cout << "  worldAABB max: (" << n->worldAABB.maxBound.x << ", " << n->worldAABB.maxBound.y << ", " << n->worldAABB.maxBound.z << ")\n";
            for (const auto& c : n->children) self(self, c);
        };
        inspectNode(inspectNode, r);
    }


    // Set up camera inside courtyard
    glm::vec3 camPos(-0.5f, 2.0f, -0.3f);
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 1000.0f);

    // Looking along +X
    glm::mat4 view = glm::lookAt(camPos, camPos + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    bgl::core::Frustum frustum = bgl::core::Frustum::FromViewProjection(proj * view);
    auto visiblePosX = quadtree.QueryVisible(frustum, camPos);
    EXPECT_GT(visiblePosX.size(), 0);
    EXPECT_LT(visiblePosX.size(), totalPrimitives);

    // Looking along -X
    view = glm::lookAt(camPos, camPos + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    frustum = bgl::core::Frustum::FromViewProjection(proj * view);
    auto visibleNegX = quadtree.QueryVisible(frustum, camPos);
    EXPECT_GT(visibleNegX.size(), 0);
    EXPECT_LT(visibleNegX.size(), totalPrimitives);

    // Looking along +Z
    view = glm::lookAt(camPos, camPos + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    frustum = bgl::core::Frustum::FromViewProjection(proj * view);
    auto visiblePosZ = quadtree.QueryVisible(frustum, camPos);
    EXPECT_GT(visiblePosZ.size(), 0);
    EXPECT_LT(visiblePosZ.size(), totalPrimitives);

    // Looking along -Z
    view = glm::lookAt(camPos, camPos + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    frustum = bgl::core::Frustum::FromViewProjection(proj * view);
    auto visibleNegZ = quadtree.QueryVisible(frustum, camPos);
    EXPECT_GT(visibleNegZ.size(), 0);
    EXPECT_LT(visibleNegZ.size(), totalPrimitives);
}

TEST(QuadtreeTest, FrustumPlaneAABBIntersection) {
    glm::vec3 camPos(0.0f, 0.0f, 5.0f);
    glm::vec3 camTarget(0.0f, 0.0f, 0.0f);
    glm::mat4 view = glm::lookAt(camPos, camTarget, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    bgl::core::Frustum frustum = bgl::core::Frustum::FromViewProjection(proj * view);

    // Box right in front of camera
    bgl::core::AABB boxInside;
    boxInside.Expand(glm::vec3(-1.0f, -1.0f, -1.0f));
    boxInside.Expand(glm::vec3(1.0f, 1.0f, 1.0f));
    EXPECT_TRUE(frustum.IntersectsAABB(boxInside));

    // Box behind camera
    bgl::core::AABB boxBehind;
    boxBehind.Expand(glm::vec3(-1.0f, -1.0f, 10.0f));
    boxBehind.Expand(glm::vec3(1.0f, 1.0f, 12.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxBehind));

    // Box far off to the left outside FOV
    bgl::core::AABB boxFarLeft;
    boxFarLeft.Expand(glm::vec3(-50.0f, -1.0f, 0.0f));
    boxFarLeft.Expand(glm::vec3(-40.0f, 1.0f, 2.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxFarLeft));
}



