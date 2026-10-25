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
    std::string sponzaPath = "assets/Sponza.glb";
    for (const auto& path : {"assets/Sponza.glb", "../assets/Sponza.glb", "../../assets/Sponza.glb"}) {
        if (std::filesystem::exists(path)) {
            sponzaPath = path;
            break;
        }
    }
    auto res = loader.LoadFromFile(sponzaPath);
    if (!res) {
        GTEST_SKIP() << "Sponza.glb not available, skipping Sponza test";
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
        totalPrimitives += n->GetPrimitives().size();
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

TEST(QuadtreeTest, GridSubdivisionAndContainment) {
    bgl::core::AABB worldBounds;
    worldBounds.minBound = glm::vec3(-50.0f, -10.0f, -50.0f);
    worldBounds.maxBound = glm::vec3( 50.0f,  10.0f,  50.0f);

    bgl::scene::Quadtree quadtree(worldBounds);

    // Insert 25 small objects in quadrant 0 (X in [-40, -10], Z in [-40, -10])
    // kMaxItems is 8, so this should trigger subdivision
    for (int x = 0; x < 5; ++x) {
        for (int z = 0; z < 5; ++z) {
            auto node = std::make_shared<bgl::scene::SceneNode>();
            float px = -35.0f + x * 5.0f;
            float pz = -35.0f + z * 5.0f;
            node->worldAABB.minBound = glm::vec3(px - 1.0f, -1.0f, pz - 1.0f);
            node->worldAABB.maxBound = glm::vec3(px + 1.0f,  1.0f, pz + 1.0f);
            quadtree.Insert(node);
        }
    }

    // Node count should have increased due to subdivision
    EXPECT_GT(quadtree.GetNodeCount(), 1);

    // Also insert a large spanning object across the center line X = 0
    auto spanningNode = std::make_shared<bgl::scene::SceneNode>();
    spanningNode->worldAABB.minBound = glm::vec3(-20.0f, -1.0f, -2.0f);
    spanningNode->worldAABB.maxBound = glm::vec3( 20.0f,  1.0f,  2.0f);
    quadtree.Insert(spanningNode);

    // Query looking exclusively at quadrant 0
    glm::vec3 camPos(-25.0f, 0.0f, 0.0f);
    glm::mat4 view = glm::lookAt(camPos, glm::vec3(-25.0f, 0.0f, -25.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    bgl::core::Frustum frustum = bgl::core::Frustum::FromViewProjection(proj * view);

    auto visible = quadtree.QueryVisible(frustum, camPos);
    EXPECT_GT(visible.size(), 0);
    EXPECT_LE(visible.size(), 26);
}

TEST(QuadtreeTest, ResetAndClear) {
    bgl::core::AABB bounds;
    bounds.minBound = glm::vec3(-10.0f);
    bounds.maxBound = glm::vec3(10.0f);

    bgl::scene::Quadtree quadtree(bounds);

    for (int i = 0; i < 10; ++i) {
        auto node = std::make_shared<bgl::scene::SceneNode>();
        node->worldAABB.minBound = glm::vec3(i * 0.5f);
        node->worldAABB.maxBound = glm::vec3(i * 0.5f + 0.5f);
        quadtree.Insert(node);
    }
    EXPECT_GT(quadtree.GetNodeCount(), 1);

    quadtree.Clear();
    EXPECT_EQ(quadtree.GetNodeCount(), 1);

    auto visible = quadtree.QueryVisible(glm::vec3(0.0f));
    EXPECT_EQ(visible.size(), 0);
}

TEST(QuadtreeTest, HierarchicalOcclusionCullingAndFrontToBackOrdering) {
    bgl::core::AABB worldBounds{};
    worldBounds.minBound = glm::vec3{-100.0f, -20.0f, -100.0f};
    worldBounds.maxBound = glm::vec3{ 100.0f,  20.0f,  100.0f};

    bgl::scene::Quadtree quadtree{worldBounds};

    // 1. Place a large near occluder wall right in front of the camera at Z = -5
    auto nearWall = std::make_shared<bgl::scene::SceneNode>();
    nearWall->name = "NearWallOccluder";
    nearWall->worldAABB.minBound = glm::vec3{-12.0f, -8.0f, -6.0f};
    nearWall->worldAABB.maxBound = glm::vec3{ 12.0f,  8.0f, -4.0f};
    quadtree.Insert(nearWall);

    // 2. Place a smaller object directly behind the wall at Z = -25
    auto hiddenBehindWall = std::make_shared<bgl::scene::SceneNode>();
    hiddenBehindWall->name = "HiddenBehindWall";
    hiddenBehindWall->worldAABB.minBound = glm::vec3{-1.5f, -1.5f, -26.0f};
    hiddenBehindWall->worldAABB.maxBound = glm::vec3{ 1.5f,  1.5f, -24.0f};
    quadtree.Insert(hiddenBehindWall);

    // 3. Place a visible object to the side (not behind the wall)
    auto sideObject = std::make_shared<bgl::scene::SceneNode>();
    sideObject->name = "VisibleSideObject";
    sideObject->worldAABB.minBound = glm::vec3{14.0f, -1.0f, -22.0f};
    sideObject->worldAABB.maxBound = glm::vec3{16.0f,  1.0f, -20.0f};
    quadtree.Insert(sideObject);

    const glm::vec3 camPos{0.0f, 0.0f, 0.0f};
    const glm::mat4 view{glm::lookAt(camPos, glm::vec3{0.0f, 0.0f, -10.0f}, glm::vec3{0.0f, 1.0f, 0.0f})};
    const glm::mat4 proj{glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 200.0f)};
    const glm::mat4 viewProj{proj * view};
    const bgl::core::Frustum frustum{bgl::core::Frustum::FromViewProjection(viewProj)};

    // Without occlusion culling: all 3 objects are inside the frustum
    auto frustumOnly = quadtree.QueryVisible(frustum, camPos, viewProj, false);
    EXPECT_EQ(frustumOnly.size(), 3u);
    EXPECT_EQ(quadtree.GetLastOccludedCount(), 0u);
    // Verify Front-to-Back sorting (NearWallOccluder must be first)
    ASSERT_FALSE(frustumOnly.empty());
    EXPECT_EQ(frustumOnly.front().node, nearWall);

    // With occlusion culling: HiddenBehindWall is occluded by NearWallOccluder!
    auto occlusionCulled = quadtree.QueryVisible(frustum, camPos, viewProj, true);
    EXPECT_EQ(occlusionCulled.size(), 2u);
    EXPECT_EQ(quadtree.GetLastOccludedCount(), 1u);
    for (const auto& item : occlusionCulled) {
        EXPECT_NE(item.node, hiddenBehindWall);
    }
}

TEST(QuadtreeTest, OcclusionBufferDirectTileOperationsAndEdgeCases) {
    bgl::scene::OcclusionBuffer occBuffer{};

    // 1. Fresh buffer: no screen tiles occluded for standard depth values in [0, 1]
    EXPECT_FALSE(occBuffer.IsOccluded(glm::vec2{-0.5f, -0.5f}, glm::vec2{0.5f, 0.5f}, 0.5f));
    EXPECT_FALSE(occBuffer.IsOccluded(glm::vec2{-1.0f, -1.0f}, glm::vec2{1.0f, 1.0f}, 1.0f));

    // 2. ProjectAABB on uninitialized / inverted bounding box returns false
    bgl::core::AABB invertedBox{};
    glm::vec2 ndcMin{0.0f}, ndcMax{0.0f};
    float minZ{0.0f}, maxZ{0.0f};
    const glm::mat4 identity{1.0f};
    EXPECT_FALSE(bgl::scene::OcclusionBuffer::ProjectAABB(invertedBox, identity, ndcMin, ndcMax, minZ, maxZ));

    // 3. ProjectAABB on a box behind camera (z > 0 in standard view-proj where camera looks down -Z)
    bgl::core::AABB behindBox{};
    behindBox.minBound = glm::vec3{-1.0f, -1.0f, 5.0f};
    behindBox.maxBound = glm::vec3{ 1.0f,  1.0f, 10.0f};
    const glm::mat4 view = glm::lookAt(glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    EXPECT_FALSE(bgl::scene::OcclusionBuffer::ProjectAABB(behindBox, proj * view, ndcMin, ndcMax, minZ, maxZ));

    // 4. ProjectAABB on a box in front of camera succeeds and produces valid NDC coordinates
    bgl::core::AABB frontBox{};
    frontBox.minBound = glm::vec3{-2.0f, -2.0f, -10.0f};
    frontBox.maxBound = glm::vec3{ 2.0f,  2.0f, -5.0f};
    EXPECT_TRUE(bgl::scene::OcclusionBuffer::ProjectAABB(frontBox, proj * view, ndcMin, ndcMax, minZ, maxZ));
    EXPECT_LE(ndcMin.x, ndcMax.x);
    EXPECT_LE(ndcMin.y, ndcMax.y);
    EXPECT_GE(minZ, 0.0f);
    EXPECT_LE(maxZ, 1.0f);

    // 5. Conservative rasterization and occlusion testing
    // Rasterize an occluder covering the center of the screen at depth 0.2
    occBuffer.RasterizeOccluder(glm::vec2{-0.8f, -0.8f}, glm::vec2{0.8f, 0.8f}, 0.2f);

    // A small target behind the occluder (depth 0.6 > 0.2 + bias) must be occluded
    EXPECT_TRUE(occBuffer.IsOccluded(glm::vec2{-0.2f, -0.2f}, glm::vec2{0.2f, 0.2f}, 0.6f));

    // A target in front of the occluder (depth 0.1 < 0.2) must NOT be occluded
    EXPECT_FALSE(occBuffer.IsOccluded(glm::vec2{-0.2f, -0.2f}, glm::vec2{0.2f, 0.2f}, 0.1f));

    // 6. Resetting the buffer clears all occluders
    occBuffer.Clear();
    EXPECT_FALSE(occBuffer.IsOccluded(glm::vec2{-0.2f, -0.2f}, glm::vec2{0.2f, 0.2f}, 0.6f));
}

TEST(QuadtreeTest, DeepSubdivisionAndMaxDepthSafety) {
    bgl::core::AABB worldBounds;
    worldBounds.minBound = glm::vec3(-100.0f, -10.0f, -100.0f);
    worldBounds.maxBound = glm::vec3( 100.0f,  10.0f,  100.0f);

    bgl::scene::Quadtree quadtree(worldBounds);

    // Insert 60 objects tightly clustered within [1.0, 1.05]
    for (int i = 0; i < 60; ++i) {
        auto node = std::make_shared<bgl::scene::SceneNode>();
        node->worldAABB.minBound = glm::vec3(1.0f + i * 0.0005f, 0.0f, 1.0f + i * 0.0005f);
        node->worldAABB.maxBound = glm::vec3(1.0005f + i * 0.0005f, 1.0f, 1.0005f + i * 0.0005f);
        quadtree.Insert(node);
    }

    // Must subdivide down to max depth without infinite recursion
    EXPECT_GT(quadtree.GetNodeCount(), 5);

    // All objects must be retrieved when querying visible
    auto visible = quadtree.QueryVisible(glm::vec3(1.0f, 0.0f, 1.0f));
    EXPECT_EQ(visible.size(), 60u);
}

TEST(QuadtreeTest, OutOfBoundsObjectPlacement) {
    bgl::core::AABB bounds;
    bounds.minBound = glm::vec3(-10.0f);
    bounds.maxBound = glm::vec3( 10.0f);

    bgl::scene::Quadtree quadtree(bounds);

    // Object far outside the tree bounds
    auto outNode = std::make_shared<bgl::scene::SceneNode>();
    outNode->worldAABB.minBound = glm::vec3(50.0f, 0.0f, 50.0f);
    outNode->worldAABB.maxBound = glm::vec3(52.0f, 2.0f, 52.0f);
    quadtree.Insert(outNode);

    // Must still be queryable
    auto visible = quadtree.QueryVisible(glm::vec3(50.0f, 0.0f, 50.0f));
    EXPECT_EQ(visible.size(), 1u);
}

TEST(QuadtreeTest, OcclusionBufferScreenEdgeClipping) {
    bgl::scene::OcclusionBuffer occBuffer;

    // Rasterize an occluder exceeding NDC boundaries (e.g. [-3.0, 3.0])
    occBuffer.RasterizeOccluder(glm::vec2(-3.0f, -3.0f), glm::vec2(3.0f, 3.0f), 0.3f);

    // Any point inside screen at depth > 0.3 must be occluded
    EXPECT_TRUE(occBuffer.IsOccluded(glm::vec2(-0.5f, -0.5f), glm::vec2(0.5f, 0.5f), 0.8f));

    // Point in front must not be occluded
    EXPECT_FALSE(occBuffer.IsOccluded(glm::vec2(-0.5f, -0.5f), glm::vec2(0.5f, 0.5f), 0.1f));
}






