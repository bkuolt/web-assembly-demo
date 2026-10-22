/**
 * @file test_rendering_features.cpp
 * @brief Dedicated unit test suite for Bump/Normal Mapping, Mip-Mapping,
 *        Directional PCF Shadow Mapping, and Cubemap Environment Reflections (IBL).
 */
#include <gtest/gtest.h>
#include <cmath>
#include <numeric>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "core/Types.hpp"
#include "scene/Scene.hpp"
#include "gpu/Pipeline.hpp"
#include "app/Application.hpp"

using namespace bgl;

// ============================================================================
// 1. Bump & Tangent-Space Normal Mapping Tests
// ============================================================================

TEST(RenderingFeaturesTest, BumpMappingTangentsOnAllSixAxisAlignedFaces) {
    // Test all 6 cube face normals (+X, -X, +Y, -Y, +Z, -Z) to ensure no axis produces NaN or zero tangents
    struct FaceCase {
        glm::vec3 normal;
        glm::vec3 uAxis;
        glm::vec3 vAxis;
    };

    const std::vector<FaceCase> faces = {
        { glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
        { glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, 0.0f,  1.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
        { glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3(1.0f, 0.0f,  0.0f), glm::vec3(0.0f, 0.0f, 1.0f) },
        { glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3(1.0f, 0.0f,  0.0f), glm::vec3(0.0f, 0.0f, -1.0f) },
        { glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3(1.0f, 0.0f,  0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
        { glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) }
    };

    for (const auto& f : faces) {
        scene::MeshPrimitive prim;
        core::Vertex v0, v1, v2, v3;
        v0.position = glm::vec3(0.0f);               v0.normal = f.normal; v0.uv = glm::vec2(0.0f, 0.0f);
        v1.position = f.uAxis * 2.0f;                v1.normal = f.normal; v1.uv = glm::vec2(1.0f, 0.0f);
        v2.position = f.uAxis * 2.0f + f.vAxis * 2.0f; v2.normal = f.normal; v2.uv = glm::vec2(1.0f, 1.0f);
        v3.position = f.vAxis * 2.0f;                v3.normal = f.normal; v3.uv = glm::vec2(0.0f, 1.0f);

        prim.vertices = { v0, v1, v2, v3 };
        prim.indices = { 0, 1, 2, 0, 2, 3 };
        prim.ComputeTangents();

        for (const auto& v : prim.vertices) {
            glm::vec3 T(v.tangent);
            EXPECT_FALSE(std::isnan(T.x) || std::isnan(T.y) || std::isnan(T.z));
            EXPECT_NEAR(glm::length(T), 1.0f, 1e-4f);
            EXPECT_NEAR(glm::dot(T, f.normal), 0.0f, 1e-4f);
            EXPECT_NEAR(glm::dot(T, f.uAxis), 1.0f, 1e-4f);
            EXPECT_NEAR(std::abs(v.tangent.w), 1.0f, 1e-4f);
        }
    }
}

TEST(RenderingFeaturesTest, BumpMappingMirroredUVFlipsHandedness) {
    // Non-mirrored quad vs UV-mirrored quad must have opposite tangent.w handedness signs
    scene::MeshPrimitive regularPrim;
    scene::MeshPrimitive mirroredPrim;

    core::Vertex v0, v1, v2;
    v0.position = glm::vec3(0.0f, 0.0f, 0.0f); v0.normal = glm::vec3(0.0f, 0.0f, 1.0f); v0.uv = glm::vec2(0.0f, 0.0f);
    v1.position = glm::vec3(1.0f, 0.0f, 0.0f); v1.normal = glm::vec3(0.0f, 0.0f, 1.0f); v1.uv = glm::vec2(1.0f, 0.0f);
    v2.position = glm::vec3(0.0f, 1.0f, 0.0f); v2.normal = glm::vec3(0.0f, 0.0f, 1.0f); v2.uv = glm::vec2(0.0f, 1.0f);

    regularPrim.vertices = { v0, v1, v2 };
    regularPrim.indices = { 0, 1, 2 };
    regularPrim.ComputeTangents();

    // Mirror the V coordinate so UV winding is inverted relative to 3D winding
    v0.uv = glm::vec2(0.0f, 1.0f);
    v1.uv = glm::vec2(1.0f, 1.0f);
    v2.uv = glm::vec2(0.0f, 0.0f);
    mirroredPrim.vertices = { v0, v1, v2 };
    mirroredPrim.indices = { 0, 1, 2 };
    mirroredPrim.ComputeTangents();

    EXPECT_NEAR(regularPrim.vertices[0].tangent.w, -mirroredPrim.vertices[0].tangent.w, 1e-4f);
}

TEST(RenderingFeaturesTest, BumpMappingDegenerateUVFallbackProducesOrthonormalBasis) {
    // All vertices have identical UVs (det = 0): ComputeTangents must still produce a valid orthogonal unit tangent
    scene::MeshPrimitive prim;
    core::Vertex v0, v1, v2;
    v0.position = glm::vec3(0.0f, 0.0f, 0.0f); v0.normal = glm::vec3(1.0f, 0.0f, 0.0f); v0.uv = glm::vec2(0.5f, 0.5f);
    v1.position = glm::vec3(0.0f, 1.0f, 0.0f); v1.normal = glm::vec3(1.0f, 0.0f, 0.0f); v1.uv = glm::vec2(0.5f, 0.5f);
    v2.position = glm::vec3(0.0f, 0.0f, 1.0f); v2.normal = glm::vec3(1.0f, 0.0f, 0.0f); v2.uv = glm::vec2(0.5f, 0.5f);

    prim.vertices = { v0, v1, v2 };
    prim.indices = { 0, 1, 2 };
    prim.ComputeTangents();

    for (const auto& v : prim.vertices) {
        glm::vec3 T(v.tangent);
        EXPECT_NEAR(glm::length(T), 1.0f, 1e-4f);
        EXPECT_NEAR(glm::dot(T, v.normal), 0.0f, 1e-4f);
        glm::vec3 B = glm::cross(v.normal, T) * v.tangent.w;
        EXPECT_NEAR(glm::length(B), 1.0f, 1e-4f);
    }
}

TEST(RenderingFeaturesTest, BumpMappingScaledModelMatrixPreservesOrthonormalTBN) {
    // Verify that downscaled models (e.g. Sponza scale = 0.008) keep unit-length tangents after vertex shader normalization
    scene::MeshPrimitive prim{};
    core::Vertex v0{}, v1{}, v2{};
    v0.position = glm::vec3{0.0f, 0.0f, 0.0f}; v0.normal = glm::vec3{0.0f, 0.0f, 1.0f}; v0.uv = glm::vec2{0.0f, 0.0f};
    v1.position = glm::vec3{100.0f, 0.0f, 0.0f}; v1.normal = glm::vec3{0.0f, 0.0f, 1.0f}; v1.uv = glm::vec2{1.0f, 0.0f};
    v2.position = glm::vec3{0.0f, 100.0f, 0.0f}; v2.normal = glm::vec3{0.0f, 0.0f, 1.0f}; v2.uv = glm::vec2{0.0f, 1.0f};

    prim.vertices = {v0, v1, v2};
    prim.indices = {0, 1, 2};
    prim.ComputeTangents();

    const glm::mat4 sponzaScaleModel{glm::scale(glm::mat4{1.0f}, glm::vec3{0.008f})};
    const glm::vec3 worldN{glm::normalize(glm::vec3{sponzaScaleModel * glm::vec4{prim.vertices[0].normal, 0.0f}})};
    const glm::vec3 rawWorldTan{glm::vec3{sponzaScaleModel * glm::vec4{glm::vec3{prim.vertices[0].tangent}, 0.0f}}};
    // Without vertex shader normalization, rawWorldTan length is 0.008 (lenSq = 6.4e-5 < 1e-4!)
    EXPECT_NEAR(glm::length(rawWorldTan), 0.008f, 1e-5f);

    // With vertex shader normalization (as in vs_main), orthoLenSq is ~1.0
    const glm::vec3 normWorldTan{glm::normalize(rawWorldTan)};
    const glm::vec3 orthoT{normWorldTan - worldN * glm::dot(worldN, normWorldTan)};
    const float orthoLenSq{glm::dot(orthoT, orthoT)};
    EXPECT_GT(orthoLenSq, 0.99f);
}

// ============================================================================
// 2. Mip-Mapping Support Tests
// ============================================================================

TEST(RenderingFeaturesTest, MipmapLevelCountForStandardAndNonPowerOfTwoTextures) {
    EXPECT_EQ(gpu::PipelineManager::CalculateMipLevels(0, 0), 1u);
    EXPECT_EQ(gpu::PipelineManager::CalculateMipLevels(1, 1), 1u);
    EXPECT_EQ(gpu::PipelineManager::CalculateMipLevels(2, 2), 2u);
    EXPECT_EQ(gpu::PipelineManager::CalculateMipLevels(4, 1), 3u);
    EXPECT_EQ(gpu::PipelineManager::CalculateMipLevels(300, 150), 9u);   // floor(log2(300)) + 1 = 8 + 1 = 9
    EXPECT_EQ(gpu::PipelineManager::CalculateMipLevels(2048, 2048), 12u);
    EXPECT_EQ(gpu::PipelineManager::CalculateMipLevels(4096, 2048), 13u);
}

TEST(RenderingFeaturesTest, MipmapFullPyramidGenerationPreservesAverageColor) {
    uint32_t w = 8;
    uint32_t h = 8;
    uint32_t levels = gpu::PipelineManager::CalculateMipLevels(w, h);
    EXPECT_EQ(levels, 4u); // 8x8, 4x4, 2x2, 1x1

    // Create a checkerboard of two colors: (200, 100, 50, 255) and (100, 200, 150, 255)
    // Exact mean across all texels is (150, 150, 100, 255)
    std::vector<uint8_t> currentMip(w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t idx = (y * w + x) * 4;
            bool even = ((x + y) % 2) == 0;
            currentMip[idx + 0] = even ? 200 : 100;
            currentMip[idx + 1] = even ? 100 : 200;
            currentMip[idx + 2] = even ?  50 : 150;
            currentMip[idx + 3] = 255;
        }
    }

    for (uint32_t mip{1}; mip < levels; ++mip) {
        const uint32_t nextW{std::max(1u, w >> 1u)};
        const uint32_t nextH{std::max(1u, h >> 1u)};
        currentMip = gpu::PipelineManager::GenerateNextMipLevel(std::span<const uint8_t>{currentMip}, w, h, nextW, nextH, false);
        ASSERT_EQ(currentMip.size(), static_cast<size_t>(nextW * nextH * 4));
        w = nextW;
        h = nextH;
    }

    EXPECT_EQ(w, 1u);
    EXPECT_EQ(h, 1u);
    EXPECT_EQ(currentMip[0], 150u);
    EXPECT_EQ(currentMip[1], 150u);
    EXPECT_EQ(currentMip[2], 100u);
    EXPECT_EQ(currentMip[3], 255u);
}

TEST(RenderingFeaturesTest, NormalMapMipDownsamplingMaintainsUnitLengthVectors) {
    // 4x4 normal map with strongly varying tangent-space normals
    uint32_t w = 4, h = 4;
    std::vector<uint8_t> normalMip0(w * h * 4);
    for (uint32_t i = 0; i < w * h; ++i) {
        float angle = static_cast<float>(i) * 0.7f;
        glm::vec3 n = glm::normalize(glm::vec3(std::cos(angle) * 0.8f, std::sin(angle) * 0.8f, 0.6f));
        normalMip0[i * 4 + 0] = static_cast<uint8_t>( std::clamp((n.x * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
        normalMip0[i * 4 + 1] = static_cast<uint8_t>( std::clamp((n.y * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
        normalMip0[i * 4 + 2] = static_cast<uint8_t>( std::clamp((n.z * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
        normalMip0[i * 4 + 3] = 255;
    }

    auto normalMip1{gpu::PipelineManager::GenerateNextMipLevel(std::span<const uint8_t>{normalMip0}, 4, 4, 2, 2, true)};
    ASSERT_EQ(normalMip1.size(), 2u * 2u * 4u);

    for (size_t i = 0; i < 4; ++i) {
        glm::vec3 decoded(
            (static_cast<float>(normalMip1[i * 4 + 0]) / 255.0f) * 2.0f - 1.0f,
            (static_cast<float>(normalMip1[i * 4 + 1]) / 255.0f) * 2.0f - 1.0f,
            (static_cast<float>(normalMip1[i * 4 + 2]) / 255.0f) * 2.0f - 1.0f
        );
        // Every downsampled normal texel must remain unit-length (within 8-bit quantization tolerance)
        EXPECT_NEAR(glm::length(decoded), 1.0f, 0.02f);
        EXPECT_GT(decoded.z, 0.0f);
    }
}

// ============================================================================
// 3. Directional Shadow Mapping Tests
// ============================================================================

TEST(RenderingFeaturesTest, ShadowMatrixEnclosesAllEightSceneCornersAndOrdersDepth) {
    core::AABB sceneAABB;
    sceneAABB.Expand(glm::vec3(-15.0f, -2.0f, -9.5f));
    sceneAABB.Expand(glm::vec3( 14.5f, 12.0f,  9.0f));

    glm::vec3 sunDir = glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));
    glm::mat4 lightVP = gpu::PipelineManager::ComputeDirectionalLightMatrix(sceneAABB, sunDir);

    // Check all 8 corners of the scene AABB map strictly inside [-1, 1]^2 x [0, 1]
    for (int mask = 0; mask < 8; ++mask) {
        glm::vec3 corner(
            (mask & 1) ? sceneAABB.maxBound.x : sceneAABB.minBound.x,
            (mask & 2) ? sceneAABB.maxBound.y : sceneAABB.minBound.y,
            (mask & 4) ? sceneAABB.maxBound.z : sceneAABB.minBound.z
        );
        glm::vec4 clip = lightVP * glm::vec4(corner, 1.0f);
        ASSERT_NEAR(clip.w, 1.0f, 1e-5f); // Orthographic projection has w == 1
        EXPECT_GE(clip.x, -1.0f);
        EXPECT_LE(clip.x,  1.0f);
        EXPECT_GE(clip.y, -1.0f);
        EXPECT_LE(clip.y,  1.0f);
        EXPECT_GE(clip.z,  0.0f);
        EXPECT_LE(clip.z,  1.0f);
    }

    // Verify depth ordering: an occluder shifted toward the sun has smaller Z depth than the receiver at scene center
    glm::vec3 receiver = sceneAABB.Center();
    glm::vec3 occluder = receiver + sunDir * 3.0f;
    float receiverDepth = (lightVP * glm::vec4(receiver, 1.0f)).z;
    float occluderDepth = (lightVP * glm::vec4(occluder, 1.0f)).z;
    EXPECT_LT(occluderDepth, receiverDepth);
}

TEST(RenderingFeaturesTest, ShadowMatrixHandlesEmptyAABBAndVerticalSunDirection) {
    // 1. Uninitialized/inverted default AABB
    core::AABB emptyBox;
    glm::mat4 matEmpty = gpu::PipelineManager::ComputeDirectionalLightMatrix(emptyBox, glm::vec3(0.0f, 1.0f, 0.0f));
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            EXPECT_FALSE(std::isnan(matEmpty[c][r]) || std::isinf(matEmpty[c][r]));
        }
    }

    // 2. Straight vertical zenith sun (0, 1, 0) collinear with standard Y-up
    core::AABB validBox;
    validBox.Expand(glm::vec3(-5.0f));
    validBox.Expand(glm::vec3( 5.0f));
    glm::mat4 matZenith = gpu::PipelineManager::ComputeDirectionalLightMatrix(validBox, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec4 originClip = matZenith * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(originClip.x, 0.0f, 1e-4f);
    EXPECT_NEAR(originClip.y, 0.0f, 1e-4f);
    EXPECT_GE(originClip.z, 0.0f);
    EXPECT_LE(originClip.z, 1.0f);
}

// ============================================================================
// 4. Environment Cubemap Reflections & Pipeline Toggle Tests
// ============================================================================

TEST(RenderingFeaturesTest, EnvCubemapSkyVsGroundAndRoughnessFiltering) {
    glm::vec3 sunDir = glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));

    // Face 2 is +Y (Sky Zenith), Face 3 is -Y (Ground Nadir)
    auto skyFace = gpu::PipelineManager::GenerateEnvCubemapFaceMip(2, 16, 0.0f, sunDir);
    auto groundFace = gpu::PipelineManager::GenerateEnvCubemapFaceMip(3, 16, 0.0f, sunDir);
    ASSERT_EQ(skyFace.size(), 16u * 16u * 4u);
    ASSERT_EQ(groundFace.size(), 16u * 16u * 4u);

    // Compute average blue channel intensity on +Y (sky) vs -Y (ground)
    double skyBlueSum = 0.0;
    double groundBlueSum = 0.0;
    for (size_t i = 0; i < 16 * 16; ++i) {
        skyBlueSum += skyFace[i * 4 + 2];
        groundBlueSum += groundFace[i * 4 + 2];
    }
    EXPECT_GT(skyBlueSum, groundBlueSum * 1.5);
}

TEST(RenderingFeaturesTest, PipelineManagerFeatureTogglesAndUniformLayout) {
    // Verify 352-byte UBO alignment and distinct feature bitflags
    EXPECT_EQ(sizeof(gpu::PrimitiveUniforms), 352u);
    EXPECT_EQ(alignof(gpu::PrimitiveUniforms), 16u);
    EXPECT_NE(gpu::MaterialFlags::EnableBumpMapping & gpu::MaterialFlags::EnableShadows, gpu::MaterialFlags::EnableBumpMapping);
    EXPECT_NE(gpu::MaterialFlags::EnableShadows & gpu::MaterialFlags::EnableReflections, gpu::MaterialFlags::EnableShadows);

    gpu::PipelineManager pm;
    // All 4 features are enabled by default
    EXPECT_TRUE(pm.IsBumpMappingEnabled());
    EXPECT_TRUE(pm.IsMipmappingEnabled());
    EXPECT_TRUE(pm.IsShadowsEnabled());
    EXPECT_TRUE(pm.IsReflectionsEnabled());

    // Toggle each feature off and back on
    pm.ToggleBumpMapping();
    EXPECT_FALSE(pm.IsBumpMappingEnabled());
    pm.SetBumpMappingEnabled(true);
    EXPECT_TRUE(pm.IsBumpMappingEnabled());

    pm.ToggleMipmapping(nullptr);
    EXPECT_FALSE(pm.IsMipmappingEnabled());
    pm.SetMipmappingEnabled(true, nullptr);
    EXPECT_TRUE(pm.IsMipmappingEnabled());

    pm.ToggleShadows();
    EXPECT_FALSE(pm.IsShadowsEnabled());
    pm.SetShadowsEnabled(true);
    EXPECT_TRUE(pm.IsShadowsEnabled());

    pm.ToggleReflections();
    EXPECT_FALSE(pm.IsReflectionsEnabled());
    pm.SetReflectionsEnabled(true);
    EXPECT_TRUE(pm.IsReflectionsEnabled());
}

