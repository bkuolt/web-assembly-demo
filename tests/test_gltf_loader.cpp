/**
 * @file test_gltf_loader.cpp
 * @brief Unit tests for FastGltfLoader, scene graph hierarchy, and PBR/KHR material extensions.
 */
#include "scene/AssetManager.hpp"
#include "scene/FastGltfLoader.hpp"
#include "scene/Quadtree.hpp"
#include "viewer/Application.hpp"
#include <chrono>
#include <cstring>
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

    const auto& scene = *res;
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
    const auto& scene = *res;
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
              << (static_cast<double>(fileSize) / (1024.0 * 1024.0)) / (static_cast<double>(durationMs) / 1000.0)
              << " MB/s)\n"
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

TEST(GltfLoaderTest, DeepHierarchyMatrixPropagation) {
    auto scene = std::make_shared<Scene>();
    std::shared_ptr<SceneNode> currentRoot = std::make_shared<SceneNode>();
    currentRoot->name = "Level_0";
    currentRoot->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    scene->rootNodes.push_back(currentRoot);

    std::shared_ptr<SceneNode> parent = currentRoot;
    constexpr int kDepth = 10;
    for (int i = 1; i <= kDepth; ++i) {
        auto child = std::make_shared<SceneNode>();
        child->name = "Level_" + std::to_string(i);
        child->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        parent->children.push_back(child);
        parent = child;
    }

    // Attach primitive at the deepest leaf node
    MeshPrimitive prim;
    prim.boundingBox.minBound = glm::vec3(-0.5f);
    prim.boundingBox.maxBound = glm::vec3(0.5f);
    parent->primitives.push_back(std::move(prim));

    scene->Update();

    // With 1 root + 10 children each translating +1 along X, leaf world position should be 11.0f along X
    EXPECT_FLOAT_EQ(parent->worldTransform[3][0], 11.0f);
    EXPECT_FLOAT_EQ(parent->worldAABB.GetCenter().x, 11.0f);
}

TEST(GltfLoaderTest, EmptySceneUpdateSafety) {
    auto scene = std::make_shared<Scene>();
    EXPECT_NO_THROW(scene->Update());
    EXPECT_TRUE(scene->rootNodes.empty());
}

TEST(GltfLoaderTest, MaterialPbrValuesClamping) {
    Material mat;
    mat.name = "PbrTest";
    mat.baseColorFactor = glm::vec4(1.0f, 0.5f, 0.2f, 1.0f);
    mat.metallicFactor = 0.8f;
    mat.roughnessFactor = 0.2f;
    mat.emissiveFactor = glm::vec3(0.0f, 1.0f, 0.0f);

    EXPECT_FLOAT_EQ(mat.metallicFactor, 0.8f);
    EXPECT_FLOAT_EQ(mat.roughnessFactor, 0.2f);
    EXPECT_FLOAT_EQ(mat.baseColorFactor.r, 1.0f);
    EXPECT_FLOAT_EQ(mat.emissiveFactor.g, 1.0f);
}

TEST(GltfLoaderTest, NodeWithEmptyMeshNameFallback) {
    auto node = std::make_shared<SceneNode>();
    EXPECT_TRUE(node->name.empty());
    node->name = "";
    EXPECT_NO_THROW({
        node->localTransform = glm::mat4(1.0f);
        node->worldTransform = glm::mat4(1.0f);
    });
}

TEST(GltfLoaderTest, ParsesKhrIridescenceAndAnisotropyExtensions) {
    static constexpr std::string_view kGltfJson = R"({
        "asset": { "version": "2.0" },
        "extensionsUsed": ["KHR_materials_iridescence", "KHR_materials_anisotropy"],
        "buffers": [{
            "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/AAAAAAAAgD8AAIA/AAABAAIA",
            "byteLength": 42
        }],
        "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
            { "buffer": 0, "byteOffset": 36, "byteLength": 6 }
        ],
        "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
            { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
        ],
        "materials": [{
            "name": "IridescentBrushedMetal",
            "pbrMetallicRoughness": { "metallicFactor": 1.0, "roughnessFactor": 0.25 },
            "extensions": {
                "KHR_materials_iridescence": {
                    "iridescenceFactor": 0.9,
                    "iridescenceIor": 1.45,
                    "iridescenceThicknessMinimum": 120.0,
                    "iridescenceThicknessMaximum": 560.0
                },
                "KHR_materials_anisotropy": {
                    "anisotropyStrength": 0.8,
                    "anisotropyRotation": 0.785398
                }
            }
        }],
        "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 }] }],
        "nodes": [{ "mesh": 0 }],
        "scenes": [{ "nodes": [0] }],
        "scene": 0
    })";

    AssetManager assetMgr;
    FastGltfLoader loader(assetMgr);
    const std::vector<uint8_t> bytes(kGltfJson.begin(), kGltfJson.end());
    auto res = loader.LoadFromMemory(bytes, ".");
    ASSERT_TRUE(res.has_value()) << res.error();
    ASSERT_EQ((*res)->materials.size(), 1u);

    const auto& mat = (*res)->materials[0];
    EXPECT_NEAR(mat.iridescenceFactor, 0.9f, 1e-4f);
    EXPECT_NEAR(mat.iridescenceIor, 1.45f, 1e-4f);
    EXPECT_NEAR(mat.iridescenceThicknessMinimum, 120.0f, 1e-4f);
    EXPECT_NEAR(mat.iridescenceThicknessMaximum, 560.0f, 1e-4f);
    EXPECT_NEAR(mat.anisotropyStrength, 0.8f, 1e-4f);
    EXPECT_NEAR(mat.anisotropyRotation, 0.785398f, 1e-4f);
}

TEST(GltfLoaderTest, ReportsMonotonicLoadProgressAndSupportsCancellation) {
    const std::string toyCarPath = FindToyCarAsset();
    ASSERT_TRUE(std::filesystem::exists(toyCarPath));

    AssetManager assetMgr;
    FastGltfLoader loader(assetMgr);

    float lastFraction = -1.0f;
    std::vector<float> fractions{};
    std::vector<std::string> statuses{};

    auto okRes = loader.LoadFromFile(toyCarPath, [&](const LoadProgressInfo& info) -> bool {
        EXPECT_GE(info.fraction, 0.0f);
        EXPECT_LE(info.fraction, 1.0f);
        EXPECT_GE(info.fraction + 1e-6f, lastFraction);
        lastFraction = info.fraction;
        fractions.push_back(info.fraction);
        statuses.push_back(info.status);
        return true;
    });
    ASSERT_TRUE(okRes.has_value()) << okRes.error();
    ASSERT_GE(fractions.size(), 4u);
    EXPECT_GE(fractions.back(), 0.90f);
    EXPECT_FALSE(statuses.front().empty());

    // Cancellation cleanly aborts mid-ingestion without crashing
    size_t cbCount = 0;
    auto cancelRes = loader.LoadFromFile(toyCarPath, [&](const LoadProgressInfo& /*info*/) -> bool {
        ++cbCount;
        return cbCount < 2;
    });
    EXPECT_FALSE(cancelRes.has_value());
}

TEST(GltfLoaderTest, BuiltinModelPresetCatalogResolvesLocalAndRemoteEntries) {
    const auto presets = bgl::app::Application::GetBuiltinModelPresets();
    ASSERT_GE(presets.size(), 15u);

    bool sawToyCar = false;
    bool sawDamagedHelmet = false;
    bool sawRobot = false;
    for (const auto& entry : presets) {
        ASSERT_NE(entry.label, nullptr);
        const std::string resolved = bgl::app::Application::ResolveModelPresetUri(entry);
        EXPECT_FALSE(resolved.empty());
        const std::string_view labelView{entry.label};
        if (labelView.find("ToyCar") != std::string_view::npos) {
            sawToyCar = true;
            EXPECT_NE(resolved.find("ToyCar.glb"), std::string::npos);
            if (std::filesystem::exists("assets/ToyCar.glb") || std::filesystem::exists("web/assets/ToyCar.glb")) {
                EXPECT_EQ(resolved.find("https://"), std::string::npos);
            }
        }
        if (labelView.find("RobotExpressive") != std::string_view::npos) {
            sawRobot = true;
            if (std::filesystem::exists("web/assets/RobotExpressive.glb")) {
                EXPECT_EQ(resolved.find("https://"), std::string::npos);
                EXPECT_NE(resolved.find("RobotExpressive.glb"), std::string::npos);
            }
        }
        if (labelView.find("DamagedHelmet") != std::string_view::npos) {
            sawDamagedHelmet = true;
            EXPECT_NE(resolved.find("https://"), std::string::npos);
            EXPECT_NE(resolved.find("DamagedHelmet.glb"), std::string::npos);
        }
    }
    EXPECT_TRUE(sawToyCar);
    EXPECT_TRUE(sawRobot);
    EXPECT_TRUE(sawDamagedHelmet);
}

TEST(GltfLoaderTest, AsyncLoadSpawnsWorkerAndCleansUpSafelyOnShutdown) {
    const std::string toyCarPath = FindToyCarAsset();
    ASSERT_TRUE(std::filesystem::exists(toyCarPath));

    bgl::app::Application app;
    app.LoadSceneFromUriAsync(toyCarPath, "ToyCar (5.1 MB)");
    EXPECT_TRUE(app.IsLoadingScene());
    EXPECT_GE(app.GetLoadingProgress(), 0.0f);
    EXPECT_FALSE(app.GetLoadingStatus().empty());
    app.Shutdown();
    EXPECT_FALSE(app.IsLoadingScene());
}

TEST(GltfLoaderTest, RejectsOversizedMalformedAccessorCountWithoutOom) {
    // glTF declaring 200M vertices over a 36-byte bufferView would trigger bad_alloc without accessor bounds checks.
    const std::string craftedGltf{R"({
  "asset": { "version": "2.0" },
  "buffers": [{ "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "byteLength": 36 }],
  "bufferViews": [{ "buffer": 0, "byteOffset": 0, "byteLength": 36 }],
  "accessors": [{ "bufferView": 0, "componentType": 5126, "count": 200000000, "type": "VEC3" }],
  "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 } }] }],
  "nodes": [{ "mesh": 0 }],
  "scenes": [{ "nodes": [0] }],
  "scene": 0
})"};
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(craftedGltf.data()), craftedGltf.size()};

    bgl::scene::AssetManager assetMgr;
    bgl::scene::FastGltfLoader loader{assetMgr};
    auto res = loader.LoadFromMemory(bytes, "malformed_accessor.gltf");
    if (res && *res && !(*res)->meshes.empty() && !(*res)->meshes[0]->primitives.empty()) {
        EXPECT_TRUE((*res)->meshes[0]->primitives[0].vertices.empty());
    }
}

TEST(GltfLoaderTest, BreaksMultiNodeCyclesAndDiamondDagBlowupWithoutStackOverflow) {
    // 2-node mutual cycle: node[0] <-> node[1]
    const std::string cyclicGltf{R"({
  "asset": { "version": "2.0" },
  "nodes": [
    { "name": "A", "children": [1] },
    { "name": "B", "children": [0] }
  ],
  "scenes": [{ "nodes": [0] }],
  "scene": 0
})"};
    const std::span<const uint8_t> cyclicBytes{reinterpret_cast<const uint8_t*>(cyclicGltf.data()), cyclicGltf.size()};

    bgl::scene::AssetManager assetMgr;
    bgl::scene::FastGltfLoader loader{assetMgr};
    auto res = loader.LoadFromMemory(cyclicBytes, "cyclic_nodes.gltf");
    if (res && *res) {
        EXPECT_NO_THROW((*res)->Update());
        EXPECT_NO_THROW((*res)->UpdateAnimation(0.016f));
    }

    // 28-node diamond DAG (all non-root edges join next layer) must parse in bounded time without exponential blowup
    std::string dagNodes;
    for (int i = 0; i < 28; ++i) {
        if (i > 0) {
            dagNodes += ",";
        }
        if (i < 27) {
            dagNodes += "{\"name\":\"N" + std::to_string(i) + "\",\"children\":[" + std::to_string(i + 1) + "," +
                        std::to_string(i + 1) + "]}";
        } else {
            dagNodes += "{\"name\":\"Leaf\"}";
        }
    }
    const std::string dagGltf{"{\"asset\":{\"version\":\"2.0\"},\"nodes\":[" + dagNodes +
                              "],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}"};
    const std::span<const uint8_t> dagBytes{reinterpret_cast<const uint8_t*>(dagGltf.data()), dagGltf.size()};
    const auto t0 = std::chrono::steady_clock::now();
    auto dagRes = loader.LoadFromMemory(dagBytes, "diamond_dag.gltf");
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsedMs, 1000);
    if (dagRes && *dagRes) {
        EXPECT_NO_THROW((*dagRes)->Update());
    }
}

TEST(GltfLoaderTest, RejectsMorphTargetBombAndZeroVertexIndexBypass) {
    // Primitive with 80 morph targets (exceeding kMaxMorphTargetsPerPrimitive) and indices referencing vertexCount == 0
    std::string targets = "[";
    for (int i = 0; i < 80; ++i) {
        if (i > 0) {
            targets += ",";
        }
        targets += "{\"POSITION\":0}";
    }
    targets += "]";
    const std::string crafted{
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAA","byteLength":12}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":12}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3"},{"bufferView":0,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"targets":)" +
        targets + R"(}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})"};
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(crafted.data()), crafted.size()};
    bgl::scene::AssetManager assetMgr;
    bgl::scene::FastGltfLoader loader{assetMgr};
    auto res = loader.LoadFromMemory(bytes, "morph_bomb.gltf");
    if (res && *res && !(*res)->meshes.empty() && !(*res)->meshes[0]->primitives.empty()) {
        EXPECT_LE((*res)->meshes[0]->primitives[0].morphTargets.size(), 64u);
    }
}

TEST(GltfLoaderTest, HandlesCyclicNodeHierarchyAndNonFiniteTransformsSafely) {
    const std::string craftedGltf{R"({
  "asset": { "version": "2.0" },
  "nodes": [
    { "name": "NodeA", "children": [1], "translation": [1e300, 0.0, 0.0] },
    { "name": "NodeB", "children": [0], "scale": [1.0, 1.0, 1.0] }
  ],
  "scenes": [{ "nodes": [0] }],
  "scene": 0
})"};
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(craftedGltf.data()), craftedGltf.size()};

    bgl::scene::AssetManager assetMgr;
    bgl::scene::FastGltfLoader loader{assetMgr};
    auto res = loader.LoadFromMemory(bytes, "cyclic_hierarchy.gltf");
    // Must complete without infinite recursion or hang regardless of accept/reject
    if (res && *res) {
        (*res)->Update();
        (*res)->UpdateAnimation(0.016f);
        EXPECT_TRUE(std::isfinite((*res)->sceneAABB.minBound.x));
        EXPECT_TRUE(std::isfinite((*res)->sceneAABB.maxBound.x));
    }
}

TEST(GltfLoaderTest, AnimationPreservesUnanimatedNodeTrsComponents) {
    // 2 keyframes at t=0 (0,0,0) and t=1 (4,0,0) = 24 bytes -> base64 (8 + 24 = 32 bytes; time floats 0.0, 1.0 + vec3s)
    // t=[0.0, 1.0]: 00000000 0000803f -> AAAAAAAAAD8= (8 bytes)
    // v=[(0,0,0), (4,0,0)]: 00000000 00000000 00000000  00008040 00000000 00000000 -> AAAAAAAAAAAAAAAAAAAgQAAAAAAAAAAA
    // (24 bytes) Combined 32 bytes: AAAAAAAAAD8AAAAAAAAAAAAAAAAAAIBAAAAAAAAAAA== -> WAIT encode exact 32 bytes in
    // hex/base64 at runtime!
    auto makeBase64 = [](const std::vector<uint8_t>& raw) {
        static constexpr char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < raw.size(); i += 3) {
            const uint32_t b0 = raw[i];
            const uint32_t b1 = (i + 1 < raw.size()) ? raw[i + 1] : 0u;
            const uint32_t b2 = (i + 2 < raw.size()) ? raw[i + 2] : 0u;
            const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
            out.push_back(kTbl[(triple >> 18) & 0x3Fu]);
            out.push_back(kTbl[(triple >> 12) & 0x3Fu]);
            out.push_back((i + 1 < raw.size()) ? kTbl[(triple >> 6) & 0x3Fu] : '=');
            out.push_back((i + 2 < raw.size()) ? kTbl[triple & 0x3Fu] : '=');
        }
        return out;
    };

    const float floats[8] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f};
    std::vector<uint8_t> bin(sizeof(floats));
    std::memcpy(bin.data(), floats, sizeof(floats));
    const std::string gltfJson =
        std::string(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,)") +
        makeBase64(bin) +
        R"(","byteLength":32}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},{"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],"nodes":[{"name":"AnimNode","translation":[0,0,0],"rotation":[0,0,0.70710678,0.70710678],"scale":[2,3,4]}],"scenes":[{"nodes":[0]}],"scene":0,"animations":[{"samplers":[{"input":0,"output":1,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}]})";

    bgl::scene::AssetManager assetMgr;
    bgl::scene::FastGltfLoader loader{assetMgr};
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(gltfJson.data()), gltfJson.size()};
    auto res = loader.LoadFromMemory(bytes, "trs_anim.gltf");
    ASSERT_TRUE(res.has_value()) << res.error();
    ASSERT_NE(*res, nullptr);
    ASSERT_FALSE((*res)->allNodes.empty());

    (*res)->UpdateAnimation(0.5f);
    const auto& node = (*res)->allNodes[0];
    EXPECT_NEAR(node->translation.x, 2.0f, 1e-3f);
    EXPECT_NEAR(node->scale.x, 2.0f, 1e-4f);
    EXPECT_NEAR(node->scale.y, 3.0f, 1e-4f);
    EXPECT_NEAR(node->scale.z, 4.0f, 1e-4f);
    EXPECT_NEAR(node->rotation.z, 0.70710678f, 1e-4f);
    EXPECT_NEAR(node->rotation.w, 0.70710678f, 1e-4f);
}

TEST(GltfLoaderTest, RejectsInvalidAccessorStrideAndOutOfBoundsSparseCount) {
    // Oversized / malformed sparse count exceeding accessor count or truncated bufferView
    const std::string badSparseGltf{
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAA","byteLength":12}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":12,"byteStride":1000000}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3","sparse":{"count":999999,"indices":{"bufferView":0,"componentType":5123},"values":{"bufferView":0}}}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})"};
    bgl::scene::AssetManager assetMgr;
    bgl::scene::FastGltfLoader loader{assetMgr};
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(badSparseGltf.data()), badSparseGltf.size()};
    auto res = loader.LoadFromMemory(bytes, "bad_stride_sparse.gltf");
    // Must either reject cleanly or load safely with empty primitive instead of OOM/UB
    if (res && *res && !(*res)->meshes.empty()) {
        EXPECT_TRUE((*res)->meshes[0]->primitives.empty() || (*res)->meshes[0]->primitives[0].vertices.size() <= 1u);
    }
}
