/**
 * @file test_gltf_loader.cpp
 * @brief Unit tests for FastGltfLoader, scene graph hierarchy, and PBR/KHR material extensions.
 */
#include <gtest/gtest.h>
#include <filesystem>
#include "scene/FastGltfLoader.hpp"
#include "scene/AssetManager.hpp"

using namespace bgl::scene;

TEST(GltfLoaderTest, LoadToyCarAsset) {
    if (!std::filesystem::exists("assets/ToyCar.glb")) {
        GTEST_SKIP() << "assets/ToyCar.glb not found, skipping";
    }

    AssetManager assetMgr;
    FastGltfLoader loader(assetMgr);

    auto res = loader.LoadFromFile("assets/ToyCar.glb");
    ASSERT_TRUE(res.has_value()) << "Failed to load ToyCar.glb: " << res.error();

    auto scene = *res;
    ASSERT_NE(scene, nullptr);
    EXPECT_FALSE(scene->rootNodes.empty());

    // Check scene bounding box validity
    EXPECT_LE(scene->sceneAABB.minBound.x, scene->sceneAABB.maxBound.x);
    EXPECT_LE(scene->sceneAABB.minBound.y, scene->sceneAABB.maxBound.y);
    EXPECT_LE(scene->sceneAABB.minBound.z, scene->sceneAABB.maxBound.z);

    // Count meshes and primitives
    size_t totalPrimitives = 0;
    size_t totalVertices = 0;
    auto traverse = [&](auto& self, const std::shared_ptr<SceneNode>& node) -> void {
        for (const auto& prim : node->primitives) {
            totalPrimitives++;
            totalVertices += prim.vertices.size();
            EXPECT_FALSE(prim.vertices.empty());
            EXPECT_FALSE(prim.indices.empty());
        }
        for (const auto& child : node->children) {
            self(self, child);
        }
    };
    for (const auto& root : scene->rootNodes) {
        traverse(traverse, root);
    }

    EXPECT_GT(totalPrimitives, 0);
    EXPECT_GT(totalVertices, 100);

    // Materials and KHR extensions check
    EXPECT_FALSE(scene->materials.empty());
    bool foundClearcoatOrSheen = false;
    for (const auto& mat : scene->materials) {
        if (mat.clearcoatFactor > 0.0f || mat.transmissionFactor > 0.0f || mat.sheenRoughnessFactor > 0.0f) {
            foundClearcoatOrSheen = true;
            break;
        }
    }
    EXPECT_TRUE(foundClearcoatOrSheen);
}

TEST(GltfLoaderTest, CorruptedGlbHandling) {
    AssetManager assetMgr;
    FastGltfLoader loader(assetMgr);

    // Pass invalid truncated buffer
    std::vector<uint8_t> corruptedData = { 'g', 'l', 'T', 'F', 0x02, 0x00, 0x00, 0x00, 0x10, 0x00 };
    auto res = loader.LoadFromMemory(corruptedData, "corrupted.glb");
    EXPECT_FALSE(res.has_value());
    EXPECT_FALSE(res.error().empty());
}

TEST(GltfLoaderTest, SceneTransformEvaluation) {
    auto scene = std::make_shared<Scene>();
    auto root = std::make_shared<SceneNode>();
    root->name = "Parent";
    root->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));

    auto child = std::make_shared<SceneNode>();
    child->name = "Child";
    child->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f));

    MeshPrimitive prim;
    prim.boundingBox.minBound = glm::vec3(-1.0f);
    prim.boundingBox.maxBound = glm::vec3(1.0f);
    child->primitives.push_back(std::move(prim));

    root->children.push_back(child);
    scene->rootNodes.push_back(root);

    // Update scene graph transforms
    scene->Update();

    // Parent world position should be (10, 0, 0)
    EXPECT_FLOAT_EQ(root->worldTransform[3][0], 10.0f);
    EXPECT_FLOAT_EQ(root->worldTransform[3][1], 0.0f);

    // Child world position should be (10, 5, 0)
    EXPECT_FLOAT_EQ(child->worldTransform[3][0], 10.0f);
    EXPECT_FLOAT_EQ(child->worldTransform[3][1], 5.0f);

    // Scene bounding box must enclose child in world space (10 +/- 1, 5 +/- 1, 0 +/- 1)
    EXPECT_FLOAT_EQ(scene->sceneAABB.minBound.x, 9.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.maxBound.x, 11.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.minBound.y, 4.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.maxBound.y, 6.0f);
}
