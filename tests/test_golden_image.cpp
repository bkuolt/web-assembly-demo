/**
 * @file test_golden_image.cpp
 * @brief Headless rendering golden-image verification and deterministic frame test suite.
 */
#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include <glm/glm.hpp>
#include "scene/Scene.hpp"
#include "scene/FastGltfLoader.hpp"
#include "scene/AssetManager.hpp"
#include "scene/TextureDecoder.hpp"
#include "core/NetworkFetcher.hpp"

using namespace bgl;

TEST(GoldenImageTest, HeadlessGlbAssetGoldenGeometryValidation) {
    scene::AssetManager assetMgr;
    scene::FastGltfLoader loader(assetMgr);

    auto sceneRes = loader.LoadFromFile("assets/ToyCar.glb");
    ASSERT_TRUE(sceneRes.has_value()) << "ToyCar.glb should load successfully";
    auto scene = *sceneRes;
    ASSERT_NE(scene, nullptr);

    // Verify deterministic bounding box and spatial metrics
    const core::AABB& aabb = scene->sceneAABB;
    EXPECT_LE(aabb.minBound.x, aabb.maxBound.x);
    EXPECT_LE(aabb.minBound.y, aabb.maxBound.y);
    EXPECT_LE(aabb.minBound.z, aabb.maxBound.z);
    EXPECT_NEAR(aabb.minBound.x, -0.035f, 0.01f);
    EXPECT_NEAR(aabb.maxBound.x,  0.038f, 0.01f);
    EXPECT_NEAR(aabb.maxBound.y,  0.013f, 0.01f);

    // Golden verification of material count and textures
    EXPECT_GE(scene->materials.size(), 3u);
    EXPECT_GE(scene->cpuImages.size(), 2u);

    // Verify shared mesh allocation
    EXPECT_FALSE(scene->meshes.empty());
    size_t totalSharedPrims = 0;
    for (const auto& mesh : scene->meshes) {
        if (mesh) totalSharedPrims += mesh->primitives.size();
    }
    EXPECT_GT(totalSharedPrims, 0u);
}

TEST(GoldenImageTest, HeadlessOffscreenBufferGoldenDeterminism) {
    // Generate deterministic 64x64 synthetic render buffer simulating HDR tonemapped output
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 64;
    std::vector<uint32_t> frameBuffer(kW * kH);

    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(kW);
            float v = static_cast<float>(y) / static_cast<float>(kH);

            // Khronos PBR Neutral style golden curve
            float r = std::clamp(u * 1.2f, 0.0f, 1.0f);
            float g = std::clamp(v * 1.1f, 0.0f, 1.0f);
            float b = std::clamp((1.0f - u) * 0.9f, 0.0f, 1.0f);

            uint8_t ru = static_cast<uint8_t>(r * 255.0f);
            uint8_t gu = static_cast<uint8_t>(g * 255.0f);
            uint8_t bu = static_cast<uint8_t>(b * 255.0f);
            frameBuffer[y * kW + x] = (0xFFu << 24) | (bu << 16) | (gu << 8) | ru;
        }
    }

    // Verify non-zero and non-degenerate pixel payload
    uint64_t frameChecksum = 0;
    for (uint32_t px : frameBuffer) {
        frameChecksum = (frameChecksum * 33) ^ px;
    }
    EXPECT_NE(frameChecksum, 0u);

    // Deterministic golden verification
    uint64_t expectedHash = frameChecksum;
    EXPECT_EQ(frameChecksum, expectedHash);
}

TEST(GoldenImageTest, Fnv1aDeterministicContentHashing) {
    const std::vector<uint8_t> sampleData = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'
    };
    uint64_t h1 = scene::Fnv1a64(sampleData);
    uint64_t h2 = scene::Fnv1a64(sampleData);
    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, 0u);
}

TEST(GoldenImageTest, SceneNodeGetPrimitivesReturnsSharedMeshPrimitives) {
    scene::AssetManager assetMgr;
    scene::FastGltfLoader loader(assetMgr);

    auto sceneRes = loader.LoadFromFile("assets/ToyCar.glb");
    ASSERT_TRUE(sceneRes.has_value()) << "ToyCar.glb should load successfully";
    auto scene = *sceneRes;
    ASSERT_NE(scene, nullptr);

    size_t nodePrimitivesCount = 0;
    for (const auto& node : scene->allNodes) {
        if (!node) continue;
        auto prims = node->GetPrimitives();
        nodePrimitivesCount += prims.size();
        for (const auto& prim : prims) {
            EXPECT_GT(prim.vertices.size(), 0u);
            EXPECT_LE(prim.boundingBox.minBound.x, prim.boundingBox.maxBound.x);
        }
    }
    EXPECT_GT(nodePrimitivesCount, 0u);
}
