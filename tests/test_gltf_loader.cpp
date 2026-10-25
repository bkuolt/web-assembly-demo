/**
 * @file test_gltf_loader.cpp
 * @brief Unit tests for FastGltfLoader, scene graph hierarchy, and PBR/KHR material extensions.
 */
#include "scene/AssetManager.hpp"
#include "scene/FastGltfLoader.hpp"
#include "scene/Quadtree.hpp"
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>

using namespace bgl::scene;

static std::string FindToyCarAsset() {
    for (const auto& path : {"assets/ToyCar.glb", "../assets/ToyCar.glb", "../../assets/ToyCar.glb"}) {
        if (std::filesystem::exists(path))
            return path;
    }
    return "assets/ToyCar.glb";
}

TEST(GltfLoaderTest, LoadToyCarAsset) {
    const std::string assetPath = FindToyCarAsset();
    if (!std::filesystem::exists(assetPath)) {
        GTEST_SKIP() << "ToyCar.glb not found, skipping";
    }

    AssetManager assetMgr;
    FastGltfLoader loader(assetMgr);

    auto res = loader.LoadFromFile(assetPath);
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
        for (const auto& prim : node->GetPrimitives()) {
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
    std::vector<uint8_t> corruptedData = {'g', 'l', 'T', 'F', 0x02, 0x00, 0x00, 0x00, 0x10, 0x00};
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

TEST(GltfLoaderTest, LoadStressTestMegacityAsset) {
    const std::string path = "assets/stress_test/sponza_megacity_1gb.large.glb";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << path << " not found, skipping 1GB stress test";
    }

    auto fileSize = std::filesystem::file_size(path);
    EXPECT_GT(fileSize, 500ULL * 1024ULL * 1024ULL); // Verify it is a true massive model (> 500 MB)

    AssetManager assetMgr;
    FastGltfLoader loader(assetMgr);

    auto start = std::chrono::high_resolution_clock::now();
    auto res = loader.LoadFromFile(path);
    auto end = std::chrono::high_resolution_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    ASSERT_TRUE(res.has_value()) << "Failed to load 1GB asset: " << res.error();
    auto scene = *res;
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->rootNodes.size(), 20u); // 20 palace districts

    // Validate bounds
    EXPECT_LE(scene->sceneAABB.minBound.x, scene->sceneAABB.maxBound.x);
    EXPECT_GT(scene->sceneAABB.Extents().x, 100.0f); // Spans over 100m wide

    // Verify materials and textures
    EXPECT_GE(scene->materials.size(), 500u);
    EXPECT_GE(scene->cpuImages.size(), 1380u);

    // Count total primitives and vertices across the megacity
    size_t totalPrimitives = 0;
    size_t totalVertices = 0;
    size_t totalIndices = 0;
    auto traverse = [&](auto& self, const std::shared_ptr<SceneNode>& node) -> void {
        for (const auto& prim : node->GetPrimitives()) {
            totalPrimitives++;
            totalVertices += prim.vertices.size();
            totalIndices += prim.indices.size();
        }
        for (const auto& child : node->children) {
            self(self, child);
        }
    };
    for (const auto& root : scene->rootNodes) {
        traverse(traverse, root);
    }

    EXPECT_GT(totalVertices, 1000000u); // Millions of vertices
    EXPECT_GT(totalIndices, 2000000u);  // Millions of indices

    // Update scene graph transforms
    scene->Update();

    // Test Quadtree construction on the massive scene
    bgl::scene::Quadtree quadtree(scene->sceneAABB);
    for (const auto& root : scene->rootNodes) {
        quadtree.Insert(root);
    }
    EXPECT_GT(quadtree.GetNodeCount(), 1u);

    std::cout << "[STRESS TEST METRICS] Ingestion Duration: " << durationMs << " ms ("
              << (static_cast<double>(fileSize) / (1024.0 * 1024.0)) / (durationMs / 1000.0) << " MB/s)\n"
              << "  * File Size:      " << fileSize / (1024 * 1024) << " MB\n"
              << "  * Root Nodes:     " << scene->rootNodes.size() << "\n"
              << "  * Total Meshes:   " << totalPrimitives << " primitives\n"
              << "  * Total Vertices: " << totalVertices << "\n"
              << "  * Total Triangles:" << (totalIndices / 3) << "\n"
              << "  * Total Materials:" << scene->materials.size() << "\n"
              << "  * Total Textures: " << scene->cpuImages.size() << " (RGBA8 decoded in parallel)\n"
              << "  * Quadtree Nodes: " << quadtree.GetNodeCount() << "\n";
}

TEST(GltfLoaderTest, NonExistentFileGracefulFailure) {
    AssetManager assetMgr;
    FastGltfLoader loader(assetMgr);

    auto result = loader.LoadFromFile("missing_file_random_path_12345.glb");
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}

TEST(GltfLoaderTest, DeepHierarchyTransformComposition) {
    auto scene = std::make_shared<Scene>();

    // 4 levels of nested nodes: Root (X+10) -> Child1 (Y+5) -> Child2 (Z+2) -> Leaf (Scale 2x)
    auto root = std::make_shared<SceneNode>();
    root->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));

    auto child1 = std::make_shared<SceneNode>();
    child1->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f));

    auto child2 = std::make_shared<SceneNode>();
    child2->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 2.0f));

    auto leaf = std::make_shared<SceneNode>();
    leaf->localTransform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 2.0f));

    MeshPrimitive prim;
    prim.boundingBox.minBound = glm::vec3(-1.0f);
    prim.boundingBox.maxBound = glm::vec3(1.0f);
    leaf->primitives.push_back(std::move(prim));

    child2->children.push_back(leaf);
    child1->children.push_back(child2);
    root->children.push_back(child1);
    scene->rootNodes.push_back(root);

    scene->Update();

    // Leaf world transform must have accumulated all translations: (10, 5, 2)
    glm::vec4 origin(0.0f, 0.0f, 0.0f, 1.0f);
    glm::vec4 worldPos = leaf->worldTransform * origin;
    EXPECT_FLOAT_EQ(worldPos.x, 10.0f);
    EXPECT_FLOAT_EQ(worldPos.y, 5.0f);
    EXPECT_FLOAT_EQ(worldPos.z, 2.0f);

    // Leaf bounding box must be translated to (10, 5, 2) and scaled by 2 (extents 2 * 1 = 2)
    EXPECT_FLOAT_EQ(leaf->worldAABB.minBound.x, 8.0f);
    EXPECT_FLOAT_EQ(leaf->worldAABB.maxBound.x, 12.0f);
}

TEST(GltfLoaderTest, SceneAABBEnclosesAllPrimitives) {
    auto scene = std::make_shared<Scene>();

    auto nodeA = std::make_shared<SceneNode>();
    MeshPrimitive primA;
    primA.boundingBox.minBound = glm::vec3(-50.0f, -10.0f, -20.0f);
    primA.boundingBox.maxBound = glm::vec3(-40.0f, 0.0f, -10.0f);
    nodeA->primitives.push_back(std::move(primA));

    auto nodeB = std::make_shared<SceneNode>();
    MeshPrimitive primB;
    primB.boundingBox.minBound = glm::vec3(30.0f, 5.0f, 40.0f);
    primB.boundingBox.maxBound = glm::vec3(60.0f, 15.0f, 80.0f);
    nodeB->primitives.push_back(std::move(primB));

    scene->rootNodes.push_back(nodeA);
    scene->rootNodes.push_back(nodeB);
    scene->Update();

    EXPECT_FLOAT_EQ(scene->sceneAABB.minBound.x, -50.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.minBound.y, -10.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.minBound.z, -20.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.maxBound.x, 60.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.maxBound.y, 15.0f);
    EXPECT_FLOAT_EQ(scene->sceneAABB.maxBound.z, 80.0f);
}
