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

TEST(AssetManagerTest, Fnv1aSpanAndStringEquivalence) {
    const std::string text = "https://example.com/models/Sponza/glTF/Sponza.gltf?token=xyz123";
    std::string_view sv = text;
    std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(text.data()), text.size());

    uint64_t hashString = bgl::scene::Fnv1a64(sv);
    uint64_t hashSpan = bgl::scene::Fnv1a64(span);

    EXPECT_NE(hashString, 0ULL);
    EXPECT_EQ(hashString, hashSpan);
}

TEST(AssetManagerTest, Fnv1aEmptyStringAndAvalanche) {
    // Empty input must equal FNV offset basis
    constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    EXPECT_EQ(bgl::scene::Fnv1a64(""), kFnvOffsetBasis);
    EXPECT_EQ(bgl::scene::Fnv1a64(std::span<const uint8_t>{}), kFnvOffsetBasis);

    // Avalanche effect: altering one character should produce distinct hashes
    uint64_t h1 = bgl::scene::Fnv1a64("texture_diffuse_01.png");
    uint64_t h2 = bgl::scene::Fnv1a64("texture_diffuse_02.png");
    EXPECT_NE(h1, h2);

    // Count differing bits
    uint64_t diff = h1 ^ h2;
    int differingBits = 0;
    while (diff > 0) {
        differingBits += (diff & 1ULL);
        diff >>= 1ULL;
    }
    // High-quality hash should flip significant number of bits across 64-bit word
    EXPECT_GE(differingBits, 15);
}

TEST(AssetManagerTest, NonExistentPathReturnsError) {
    bgl::scene::AssetManager mgr;
    auto result = mgr.LoadTexture("non_existent_directory/missing_image_12345.png");
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}

TEST(AssetManagerTest, TextureCacheInsertionReplacement) {
    bgl::scene::AssetManager mgr;
    const uint64_t hash = 0xDEADBEEFCAFEBABEULL;

    auto img1 = std::make_shared<bgl::scene::DecodedImage>();
    img1->width = 32;
    img1->height = 32;
    mgr.InsertTexture(hash, img1);
    EXPECT_TRUE(mgr.HasTexture(hash));

    // Inserting a new instance with same hash overwrites cleanly
    auto img2 = std::make_shared<bgl::scene::DecodedImage>();
    img2->width = 64;
    img2->height = 64;
    mgr.InsertTexture(hash, img2);
    EXPECT_TRUE(mgr.HasTexture(hash));
}


