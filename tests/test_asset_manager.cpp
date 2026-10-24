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

TEST(AssetManagerTest, KtxTextureDecoderValidation) {
    std::vector<uint8_t> emptyData;
    auto resEmpty = bgl::scene::TextureDecoder::DecodeKTX2(emptyData);
    EXPECT_FALSE(resEmpty.has_value());

    // Truncated / invalid KTX header
    std::vector<uint8_t> truncatedData = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    auto resTrunc = bgl::scene::TextureDecoder::DecodeKTX2(truncatedData);
    EXPECT_FALSE(resTrunc.has_value());
}

TEST(AssetManagerTest, ClearCacheAndScratchArena) {
    bgl::scene::AssetManager mgr;
    uint64_t hash = 0x1122334455667788ULL;

    auto img = std::make_shared<bgl::scene::DecodedImage>();
    img->width = 128;
    img->height = 128;
    mgr.InsertTexture(hash, img);
    EXPECT_TRUE(mgr.HasTexture(hash));

    // Clear must purge the cache
    mgr.Clear();
    EXPECT_FALSE(mgr.HasTexture(hash));

    // Scratch arena must be accessible and provide valid memory resource
    auto& arena = mgr.GetScratchArena();
    EXPECT_GT(arena.Capacity(), 0u);
    auto* mr = arena.GetResource();
    ASSERT_NE(mr, nullptr);
    void* mem = mr->allocate(256, alignof(std::max_align_t));
    ASSERT_NE(mem, nullptr);
    mr->deallocate(mem, 256, alignof(std::max_align_t));
    arena.Reset();
}

