/**
 * @file test_asset_manager.cpp
 * @brief Unit tests for FNV-1a 64-bit hashing and AssetManager caching.
 */
#include <gtest/gtest.h>
#include "scene/AssetManager.hpp"

TEST(AssetManagerTest, Fnv1a64Hashing) {
    std::string_view uri1 = "assets/ToyCar.glb";
    std::string_view uri2 = "assets/env/skybox.ktx2";

    uint64_t hash1 = bgl::scene::Fnv1a64(uri1);
    uint64_t hash2 = bgl::scene::Fnv1a64(uri2);

    EXPECT_NE(hash1, 0ULL);
    EXPECT_NE(hash2, 0ULL);
    EXPECT_NE(hash1, hash2);

    // Deterministic check
    EXPECT_EQ(hash1, bgl::scene::Fnv1a64(uri1));
}

TEST(AssetManagerTest, CacheInsertionAndDeduplication) {
    bgl::scene::AssetManager mgr;
    uint64_t testHash = 0xABCD1234EF567890ULL;

    EXPECT_FALSE(mgr.HasTexture(testHash));

    auto dummy = std::make_shared<bgl::scene::DecodedImage>();
    dummy->width = 64;
    dummy->height = 64;
    mgr.InsertTexture(testHash, dummy);

    EXPECT_TRUE(mgr.HasTexture(testHash));
}

