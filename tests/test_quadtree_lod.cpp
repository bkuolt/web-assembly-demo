/**
 * @file test_quadtree_lod.cpp
 * @brief Unit tests for Quadtree spatial partitioning and distance-based LOD calculation.
 */
#include <gtest/gtest.h>
#include "scene/Quadtree.hpp"

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

