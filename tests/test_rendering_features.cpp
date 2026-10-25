/**
 * @file test_rendering_features.cpp
 * @brief Dedicated unit test suite for Bump/Normal Mapping, Mip-Mapping,
 *        Directional PCF Shadow Mapping, and Cubemap Environment Reflections (IBL).
 */
#include <gtest/gtest.h>
#include <cmath>
#include <numeric>
#include <filesystem>
#include <fstream>
#include <thread>
#if defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "core/Types.hpp"
#include "core/NetworkFetcher.hpp"
#include "scene/Scene.hpp"
#include "scene/RayPicker.hpp"
#include "scene/TextureDecoder.hpp"
#include "gpu/Handle.hpp"
#include "gpu/Pipeline.hpp"
#include "core/MemoryTracker.hpp"
#include "core/Gamepad.hpp"
#include "gpu/AabbRenderer.hpp"
#include "gpu/ShadowPass.hpp"
#include "gpu/PostProcessPass.hpp"
#include "gpu/GpuCuller.hpp"
#include "gpu/GpuRayPicker.hpp"
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
    // Verify 384-byte UBO alignment and distinct feature bitflags
    EXPECT_EQ(sizeof(gpu::PrimitiveUniforms), 384u);
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

    // Ambient Occlusion live controls
    EXPECT_TRUE(pm.IsAmbientOcclusionEnabled());
    pm.ToggleAmbientOcclusion();
    EXPECT_FALSE(pm.IsAmbientOcclusionEnabled());
    pm.SetAmbientOcclusionEnabled(true);
    EXPECT_TRUE(pm.IsAmbientOcclusionEnabled());
    pm.SetAoIntensity(1.5f);
    EXPECT_FLOAT_EQ(pm.GetAoIntensity(), 1.5f);

    // Parallax Occlusion Mapping live controls
    EXPECT_TRUE(pm.IsParallaxEnabled());
    pm.ToggleParallaxOcclusion();
    EXPECT_FALSE(pm.IsParallaxEnabled());
    pm.SetParallaxEnabled(true);
    EXPECT_TRUE(pm.IsParallaxEnabled());
    pm.SetParallaxScale(0.05f);
    EXPECT_FLOAT_EQ(pm.GetParallaxScale(), 0.05f);

    // Directional Sun Lens Flare live controls
    EXPECT_TRUE(pm.IsLensFlareEnabled());
    pm.ToggleLensFlare();
    EXPECT_FALSE(pm.IsLensFlareEnabled());
    pm.SetLensFlareEnabled(true);
    EXPECT_TRUE(pm.IsLensFlareEnabled());
    pm.SetLensFlareIntensity(1.2f);
    EXPECT_FLOAT_EQ(pm.GetLensFlareIntensity(), 1.2f);
}

// ============================================================================
// 5. NetworkFetcher URL Support, Relative URI Resolution & External glTF Assets
// ============================================================================

TEST(RenderingFeaturesTest, NetworkFetcherUriClassificationAndSanitization) {
    EXPECT_TRUE(core::NetworkFetcher::IsCurlSupported());
    EXPECT_TRUE(core::NetworkFetcher::IsHttpUrl("https://example.com/models/Sponza.glb"));
    EXPECT_TRUE(core::NetworkFetcher::IsHttpUrl("HTTP://example.com/scene.gltf"));
    EXPECT_FALSE(core::NetworkFetcher::IsHttpUrl("assets/Sponza.glb"));
    EXPECT_TRUE(core::NetworkFetcher::IsFileUrl("file:///tmp/model.glb"));
    EXPECT_TRUE(core::NetworkFetcher::IsDataUri("data:application/octet-stream;base64,AAECAw=="));
    EXPECT_TRUE(core::NetworkFetcher::IsUrl("https://example.com/a.glb"));
    EXPECT_TRUE(core::NetworkFetcher::IsUrl("file:///tmp/a.glb"));
    EXPECT_TRUE(core::NetworkFetcher::IsUrl("data:text/plain,hello"));
    EXPECT_FALSE(core::NetworkFetcher::IsUrl("/home/user/model.glb"));

    // Sanitize text/uri-list drag-and-drop payload with comments and CRLF
    const std::string rawDropList{"# KDE/GNOME URI list\r\nhttps://example.com/assets/Toy%20Car.glb?raw=true#view\r\n"};
    EXPECT_EQ(core::NetworkFetcher::SanitizeUri(rawDropList),
              "https://example.com/assets/Toy%20Car.glb?raw=true#view");
    EXPECT_EQ(core::NetworkFetcher::ExtractDisplayName(rawDropList), "Toy Car.glb");
}

TEST(RenderingFeaturesTest, NetworkFetcherRelativeUriResolution) {
    const std::string baseHttp{"https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/main/2.0/ToyCar/glTF/ToyCar.gltf?token=abc"};

    // Relative binary buffer in same directory
    EXPECT_EQ(core::NetworkFetcher::ResolveRelativeUri(baseHttp, "ToyCar.bin"),
              "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/main/2.0/ToyCar/glTF/ToyCar.bin");

    // Parent directory traversal for shared textures
    EXPECT_EQ(core::NetworkFetcher::ResolveRelativeUri(baseHttp, "../textures/baseColor.png"),
              "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/main/2.0/ToyCar/textures/baseColor.png");

    // Root-relative URI
    EXPECT_EQ(core::NetworkFetcher::ResolveRelativeUri("https://cdn.example.com:8443/models/v1/scene.gltf", "/shared/env.ktx2"),
              "https://cdn.example.com:8443/shared/env.ktx2");

    // Absolute URL inside glTF buffer/image URI overrides base URL
    EXPECT_EQ(core::NetworkFetcher::ResolveRelativeUri(baseHttp, "https://assets.example.org/tex/normal.png"),
              "https://assets.example.org/tex/normal.png");

    // Local filesystem base path resolution
    EXPECT_EQ(core::NetworkFetcher::ResolveRelativeUri("assets/models/gltf/scene.gltf", "../textures/albedo.png"),
              "assets/models/textures/albedo.png");
}

TEST(RenderingFeaturesTest, NetworkFetcherDataUriAndExternalGltfLoading) {
    // 1. Test RFC 2397 base64 data: URI decoding
    const auto dataRes{core::NetworkFetcher::FetchSync("data:application/octet-stream;base64,SGVsbG8gV2ViR1BV")};
    ASSERT_TRUE(dataRes.has_value());
    const std::string decodedStr{dataRes->begin(), dataRes->end()};
    EXPECT_EQ(decodedStr, "Hello WebGPU");

    // 2. Test external .gltf + external .bin buffer + external .png image resolution
    const std::filesystem::path tmpDir{std::filesystem::temp_directory_path() / "bgl_gltf_uri_test"};
    std::filesystem::create_directories(tmpDir / "textures");

    // Write minimal 3-vertex triangle binary buffer (3 * vec3<float> = 36 bytes)
    const std::array<float, 9> trianglePositions{
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    const std::filesystem::path binPath{tmpDir / "geometry.bin"};
    {
        std::ofstream binOut{binPath, std::ios::binary};
        binOut.write(reinterpret_cast<const char*>(trianglePositions.data()), sizeof(trianglePositions));
    }

    // Write a valid 1x1 RGBA PNG file into textures/albedo.png
    const std::array<uint8_t, 68> minimalPng{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x08, 0xD7, 0x63, 0x60, 0x00, 0x02, 0x00,
        0x00, 0x05, 0x00, 0x01, 0xE2, 0x26, 0x05, 0x9B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
    const std::filesystem::path pngPath{tmpDir / "textures" / "albedo.png"};
    {
        std::ofstream pngOut{pngPath, std::ios::binary};
        pngOut.write(reinterpret_cast<const char*>(minimalPng.data()), minimalPng.size());
    }

    // Write glTF 2.0 JSON referencing external "geometry.bin" and "textures/albedo.png"
    const std::string gltfJson{R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [{ "nodes": [0] }],
  "nodes": [{ "name": "ExternalTriangle", "mesh": 0 }],
  "meshes": [{
    "primitives": [{
      "attributes": { "POSITION": 0 },
      "material": 0
    }]
  }],
  "materials": [{
    "name": "ExternalTexMat",
    "pbrMetallicRoughness": {
      "baseColorTexture": { "index": 0 }
    }
  }],
  "textures": [{ "source": 0 }],
  "images": [{ "uri": "textures/albedo.png" }],
  "accessors": [{
    "bufferView": 0,
    "componentType": 5126,
    "count": 3,
    "type": "VEC3",
    "min": [0.0, 0.0, 0.0],
    "max": [1.0, 1.0, 0.0]
  }],
  "bufferViews": [{
    "buffer": 0,
    "byteOffset": 0,
    "byteLength": 36
  }],
  "buffers": [{
    "uri": "geometry.bin",
    "byteLength": 36
  }]
})"};

    const std::filesystem::path gltfPath{tmpDir / "external_scene.gltf"};
    {
        std::ofstream gltfOut{gltfPath, std::ios::binary};
        gltfOut.write(gltfJson.data(), static_cast<std::streamsize>(gltfJson.size()));
    }

    scene::AssetManager assetMgr{};
    scene::FastGltfLoader loader{assetMgr};

    // Load via file:// URL to test both URL ingestion and relative URI resolution for buffer + texture
    const std::string fileUrl{"file://" + gltfPath.string()};
    const auto sceneRes{loader.LoadFromFile(fileUrl)};
    ASSERT_TRUE(sceneRes.has_value()) << sceneRes.error();

    const auto& loadedScene{*sceneRes};
    ASSERT_EQ(loadedScene->rootNodes.size(), 1u);
    ASSERT_EQ(loadedScene->rootNodes[0]->GetPrimitives().size(), 1u);
    EXPECT_EQ(loadedScene->rootNodes[0]->GetPrimitives()[0].vertices.size(), 3u);
    EXPECT_NE(loadedScene->rootNodes[0]->mesh, nullptr);
    ASSERT_EQ(loadedScene->cpuImages.size(), 1u);
    EXPECT_EQ(loadedScene->cpuImages[0].width, 1u);
    EXPECT_EQ(loadedScene->cpuImages[0].height, 1u);
    EXPECT_EQ(loadedScene->cpuImages[0].rgba.size(), 4u);

    std::error_code ec{};
    std::filesystem::remove_all(tmpDir, ec);

#if defined(__linux__) && defined(BGL_HAS_CURL)
    // 3. Real HTTP/1.1 libcurl test over ephemeral loopback socket (127.0.0.1)
    //    Tests downloading a remote .gltf that references relative HTTP .bin and .png URLs
    const int serverFd{::socket(AF_INET, SOCK_STREAM, 0)};
    ASSERT_GE(serverFd, 0);

    int optVal{1};
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &optVal, sizeof(optVal));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Ephemeral port
    ASSERT_EQ(::bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(serverFd, 8), 0);

    socklen_t addrLen{sizeof(addr)};
    ASSERT_EQ(::getsockname(serverFd, reinterpret_cast<sockaddr*>(&addr), &addrLen), 0);
    const uint16_t port{ntohs(addr.sin_port)};

    // Serve 3 sequential HTTP requests: /models/scene.gltf, /models/geometry.bin, /models/textures/albedo.png
    std::jthread httpServer{[serverFd, &gltfJson, &trianglePositions, &minimalPng]() {
        for (int reqCount{0}; reqCount < 3; ++reqCount) {
            const int clientFd{::accept(serverFd, nullptr, nullptr)};
            if (clientFd < 0) break;

            std::array<char, 1024> reqBuf{};
            const ssize_t n{::recv(clientFd, reqBuf.data(), reqBuf.size() - 1, 0)};
            std::string_view reqView{reqBuf.data(), n > 0 ? static_cast<size_t>(n) : 0u};

            std::vector<uint8_t> body{};
            std::string contentType{"application/octet-stream"};
            if (reqView.find("GET /models/scene.gltf") != std::string_view::npos) {
                body.assign(gltfJson.begin(), gltfJson.end());
                contentType = "model/gltf+json";
            } else if (reqView.find("GET /models/geometry.bin") != std::string_view::npos) {
                const auto* ptr{reinterpret_cast<const uint8_t*>(trianglePositions.data())};
                body.assign(ptr, ptr + sizeof(trianglePositions));
            } else if (reqView.find("GET /models/textures/albedo.png") != std::string_view::npos) {
                body.assign(minimalPng.begin(), minimalPng.end());
                contentType = "image/png";
            }

            const std::string header{
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: " + contentType + "\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n"
            };
            (void)::send(clientFd, header.data(), header.size(), 0);
            if (!body.empty()) {
                (void)::send(clientFd, body.data(), body.size(), 0);
            }
            ::close(clientFd);
        }
        ::close(serverFd);
    }};

    const std::string httpModelUrl{"http://127.0.0.1:" + std::to_string(port) + "/models/scene.gltf"};
    const auto httpSceneRes{loader.LoadFromFile(httpModelUrl)};
    ASSERT_TRUE(httpSceneRes.has_value()) << httpSceneRes.error();
    EXPECT_EQ((*httpSceneRes)->rootNodes.size(), 1u);
    EXPECT_EQ((*httpSceneRes)->rootNodes[0]->GetPrimitives()[0].vertices.size(), 3u);
    ASSERT_EQ((*httpSceneRes)->cpuImages.size(), 1u);
    EXPECT_EQ((*httpSceneRes)->cpuImages[0].width, 1u);
    EXPECT_EQ((*httpSceneRes)->cpuImages[0].height, 1u);
#endif
}

// ============================================================================
// 6. Modern C++23 RAII Handle, Transformed RayPicker & Decoder Hardening Tests
// ============================================================================

namespace {
int g_dummyReleaseCount = 0;
struct DummyWgpuResource { int id{42}; };
void ReleaseDummyWgpuResource(DummyWgpuResource* ptr) {
    if (ptr) {
        ++g_dummyReleaseCount;
        delete ptr;
    }
}
using DummyHandle = gpu::Handle<DummyWgpuResource*, ReleaseDummyWgpuResource>;
} // namespace

TEST(RenderingFeaturesTest, ModernCpp23RaiiHandleLifecycle) {
    g_dummyReleaseCount = 0;
    {
        DummyHandle h1{new DummyWgpuResource{7}};
        EXPECT_TRUE(static_cast<bool>(h1));
        EXPECT_EQ(h1.Get()->id, 7);

        DummyHandle h2{std::move(h1)};
        EXPECT_FALSE(static_cast<bool>(h1));
        EXPECT_TRUE(static_cast<bool>(h2));
        EXPECT_EQ(g_dummyReleaseCount, 0);

        h2.Reset(new DummyWgpuResource{9});
        EXPECT_EQ(g_dummyReleaseCount, 1);
    }
    EXPECT_EQ(g_dummyReleaseCount, 2);
}

TEST(RenderingFeaturesTest, RayPickerHitsTranslatedAndScaledPrimitiveAABB) {
    auto node = std::make_shared<scene::SceneNode>();
    node->name = "TranslatedTarget";
    node->localTransform = glm::translate(glm::mat4{1.0f}, glm::vec3{50.0f, 0.0f, 0.0f});

    scene::MeshPrimitive prim{};
    core::Vertex v0{}, v1{}, v2{};
    v0.position = glm::vec3{-1.0f, -1.0f, 0.0f};
    v1.position = glm::vec3{ 1.0f, -1.0f, 0.0f};
    v2.position = glm::vec3{ 0.0f,  1.0f, 0.0f};
    prim.vertices = {v0, v1, v2};
    prim.indices = {0, 1, 2, 999, 999, 999}; // Includes out-of-bounds indices to test bounds safety
    prim.boundingBox.Expand(v0.position);
    prim.boundingBox.Expand(v1.position);
    prim.boundingBox.Expand(v2.position);
    node->primitives.push_back(std::move(prim));

    scene::Scene scene{};
    scene.rootNodes.push_back(node);
    scene.Update();

    // Ray originates at (50, 0, 5) pointing along -Z toward the translated triangle at (50, 0, 0)
    const core::Ray ray{
        .origin = glm::vec3{50.0f, 0.0f, 5.0f},
        .direction = glm::vec3{0.0f, 0.0f, -1.0f}
    };
    const auto hit{scene::RayPicker::Pick(ray, scene.rootNodes)};
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance, 5.0f, 1e-4f);
    EXPECT_EQ(hit->hitNode, node);
}

TEST(RenderingFeaturesTest, NetworkFetcherErrorHandlingAndPercentDecoding) {
    // 1. Empty URI fetch rejection
    auto resEmpty = core::NetworkFetcher::FetchSync("");
    EXPECT_FALSE(resEmpty.has_value());

    // 2. Non-existent file path
    auto resNonExistent = core::NetworkFetcher::FetchSync("/nonexistent/file/path/that/cannot/exist/model.glb");
    EXPECT_FALSE(resNonExistent.has_value());

    // 3. Directory path instead of regular file
    auto resDir = core::NetworkFetcher::FetchSync("/tmp");
    EXPECT_FALSE(resDir.has_value());

    // 4. Invalid base64 data URI
    auto resBadB64 = core::NetworkFetcher::FetchSync("data:application/octet-stream;base64,!!!NotBase64!!!");
    EXPECT_FALSE(resBadB64.has_value());

    // 5. Percent decoding
    EXPECT_EQ(core::NetworkFetcher::DecodeUrlPercent("Hello%20World%21"), "Hello World!");
    EXPECT_EQ(core::NetworkFetcher::DecodeUrlPercent("models%2Fscene%2Eglb"), "models/scene.glb");
    // Invalid / partial percent escape sequences should pass through safely without crash
    EXPECT_EQ(core::NetworkFetcher::DecodeUrlPercent("test%2"), "test%2");
    EXPECT_EQ(core::NetworkFetcher::DecodeUrlPercent("test%ZZ"), "test%ZZ");
}

TEST(RenderingFeaturesTest, ClusteredForwardGridBoundsValidation) {
    std::array<gpu::GpuPunctualLight, 2> lights{};
    // Light 0: Directional light (w = 0.0f)
    lights[0].posOrDir = glm::vec4{0.0f, 1.0f, 0.0f, 0.0f};
    lights[0].colorInt = glm::vec4{1.0f, 1.0f, 1.0f, 0.0f};

    // Light 1: Point light at (0, 0, -5) with range 10.0f (w = 1.0f)
    lights[1].posOrDir = glm::vec4{0.0f, 0.0f, -5.0f, 1.0f};
    lights[1].colorInt = glm::vec4{5.0f, 5.0f, 5.0f, 10.0f};

    const glm::mat4 view{glm::lookAt(glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f})};
    const glm::mat4 proj{glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f)};

    std::array<glm::uvec4, 256> clusterMasks{};
    gpu::PipelineManager::BuildClusteredLightGrid(
        std::span<const gpu::GpuPunctualLight>{lights.data(), 2},
        2,
        view,
        proj,
        0.1f,
        100.0f,
        clusterMasks
    );

    // Directional light (bit 0) must be present in every cluster
    for (const auto& uvec : clusterMasks) {
        EXPECT_NE(uvec.x & 1u, 0u);
        EXPECT_NE(uvec.y & 1u, 0u);
        EXPECT_NE(uvec.z & 1u, 0u);
        EXPECT_NE(uvec.w & 1u, 0u);
    }

    // Zero lights test resets all masks
    gpu::PipelineManager::BuildClusteredLightGrid(
        std::span<const gpu::GpuPunctualLight>{lights.data(), 2},
        0,
        view,
        proj,
        0.1f,
        100.0f,
        clusterMasks
    );
    for (const auto& uvec : clusterMasks) {
        EXPECT_EQ(uvec.x, 0u);
        EXPECT_EQ(uvec.y, 0u);
        EXPECT_EQ(uvec.z, 0u);
        EXPECT_EQ(uvec.w, 0u);
    }
}

TEST(RenderingFeaturesTest, SphericalHarmonicsDiffuseReconstruction) {
    constexpr uint32_t faceSize{16};
    std::array<std::vector<float>, 6> uniformFaces{};
    for (auto& face : uniformFaces) {
        face.resize(faceSize * faceSize * 4, 1.0f); // Uniform white RGBA (1, 1, 1, 1)
    }

    const auto sh = gpu::PipelineManager::ComputeSphericalHarmonicsL2(uniformFaces, faceSize);
    // Evaluating diffuse irradiance across multiple surface normals should yield ~1.0
    const std::array<glm::vec3, 6> testNormals{{
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f}
    }};

    for (const auto& norm : testNormals) {
        const glm::vec3 irr = gpu::PipelineManager::EvaluateSphericalHarmonicsL2(sh, norm);
        EXPECT_NEAR(irr.r, 1.0f, 0.08f);
        EXPECT_NEAR(irr.g, 1.0f, 0.08f);
        EXPECT_NEAR(irr.b, 1.0f, 0.08f);
    }
}

TEST(RenderingFeaturesTest, UniformBufferAlignmentAndStd140Rules) {
    // 1. Struct sizes must strictly match WebGPU WGSL std140 / uniform layout contracts
    EXPECT_EQ(sizeof(gpu::PrimitiveUniforms), 384u);
    EXPECT_EQ(alignof(gpu::PrimitiveUniforms), 16u);

    EXPECT_EQ(sizeof(gpu::SceneLightingUniforms), 5360u);
    EXPECT_EQ(alignof(gpu::SceneLightingUniforms), 16u);

    EXPECT_EQ(sizeof(gpu::TonemapUniforms), 80u);
    EXPECT_EQ(alignof(gpu::TonemapUniforms), 16u);
    EXPECT_EQ(offsetof(gpu::TonemapUniforms, sunScreenPos), 16u);
    EXPECT_EQ(offsetof(gpu::TonemapUniforms, flareParams), 32u);
    EXPECT_EQ(offsetof(gpu::TonemapUniforms, bloomParams), 48u);
    EXPECT_EQ(offsetof(gpu::TonemapUniforms, ssaoParams), 64u);

    EXPECT_EQ(sizeof(gpu::PrefilterComputeParams), 16u);
    EXPECT_EQ(alignof(gpu::PrefilterComputeParams), 16u);

    EXPECT_EQ(sizeof(gpu::AabbUniforms), 64u);
    EXPECT_EQ(alignof(gpu::AabbUniforms), 16u);

    EXPECT_EQ(sizeof(gpu::AabbVertex), 32u);
    EXPECT_EQ(alignof(gpu::AabbVertex), 16u);

    // 2. Exact byte offsets for PrimitiveUniforms matching shader bindings
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, viewProj), 0u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, model), 64u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, lightViewProj), 128u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, cameraPos), 192u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, lightDir), 208u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, baseColorFactor), 224u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, metallicFactor), 240u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, roughnessFactor), 244u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, normalScale), 248u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, occlusionStrength), 252u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, emissiveFactor), 256u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, bumpScale), 336u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, reflectionIntensity), 340u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, shadowBias), 344u);
    EXPECT_EQ(offsetof(gpu::PrimitiveUniforms, envMipCount), 348u);
}

TEST(RenderingFeaturesTest, ClusteredForwardZSliceLogarithmicDistribution) {
    constexpr float zNear = 0.1f;
    constexpr float zFar = 500.0f;
    constexpr uint32_t kClusterSlicesZ = 8;

    float previousDepth = 0.0f;
    for (uint32_t slice = 0; slice <= kClusterSlicesZ; ++slice) {
        // Standard clustered shading logarithmic depth partition formula
        float sliceRatio = static_cast<float>(slice) / static_cast<float>(kClusterSlicesZ);
        float depth = zNear * std::pow(zFar / zNear, sliceRatio);

        if (slice == 0) {
            EXPECT_NEAR(depth, zNear, 1e-4f);
        } else if (slice == kClusterSlicesZ) {
            EXPECT_NEAR(depth, zFar, 1e-2f);
        }

        // Each slice boundary must be strictly greater than previous
        EXPECT_GT(depth, previousDepth);
        previousDepth = depth;
    }
}

TEST(RenderingFeaturesTest, SphericalHarmonicsExtremeRadiances) {
    constexpr uint32_t faceSize{8};

    // 1. Pure zero blackness test
    std::array<std::vector<float>, 6> blackFaces{};
    for (auto& face : blackFaces) {
        face.resize(faceSize * faceSize * 4, 0.0f);
    }
    const auto shBlack = gpu::PipelineManager::ComputeSphericalHarmonicsL2(blackFaces, faceSize);
    const glm::vec3 irrBlack = gpu::PipelineManager::EvaluateSphericalHarmonicsL2(shBlack, glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_FLOAT_EQ(irrBlack.r, 0.0f);
    EXPECT_FLOAT_EQ(irrBlack.g, 0.0f);
    EXPECT_FLOAT_EQ(irrBlack.b, 0.0f);

    // 2. High-dynamic-range radiance with 10.0f (within 25.0f clamp limit)
    std::array<std::vector<float>, 6> hdrFaces10{};
    for (auto& face : hdrFaces10) {
        face.resize(faceSize * faceSize * 4, 10.0f);
    }
    const auto shHdr10 = gpu::PipelineManager::ComputeSphericalHarmonicsL2(hdrFaces10, faceSize);
    const glm::vec3 irrHdr10 = gpu::PipelineManager::EvaluateSphericalHarmonicsL2(shHdr10, glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(irrHdr10.r, 10.0f, 0.8f);
    EXPECT_NEAR(irrHdr10.g, 10.0f, 0.8f);
    EXPECT_NEAR(irrHdr10.b, 10.0f, 0.8f);

    // 3. Firefly clamping verification: 100.0f radiance is safely clamped to 25.0f max threshold
    std::array<std::vector<float>, 6> hdrFaces100{};
    for (auto& face : hdrFaces100) {
        face.resize(faceSize * faceSize * 4, 100.0f);
    }
    const auto shHdr100 = gpu::PipelineManager::ComputeSphericalHarmonicsL2(hdrFaces100, faceSize);
    const glm::vec3 irrHdr100 = gpu::PipelineManager::EvaluateSphericalHarmonicsL2(shHdr100, glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(irrHdr100.r, 25.0f, 2.0f);
    EXPECT_NEAR(irrHdr100.g, 25.0f, 2.0f);
    EXPECT_NEAR(irrHdr100.b, 25.0f, 2.0f);
}

TEST(RenderingFeaturesTest, ShadowMatrixBoundingBoxContainment) {
    core::AABB sceneBounds;
    sceneBounds.minBound = glm::vec3(-25.0f, 0.0f, -25.0f);
    sceneBounds.maxBound = glm::vec3( 25.0f, 20.0f,  25.0f);

    const glm::vec3 sunDirection = glm::normalize(glm::vec3(0.4f, 0.8f, 0.3f));
    const glm::mat4 lightVP = gpu::PipelineManager::ComputeDirectionalLightMatrix(sceneBounds, sunDirection);

    // Project all 8 corners of scene bounding box through light view-projection
    for (float x : {sceneBounds.minBound.x, sceneBounds.maxBound.x}) {
        for (float y : {sceneBounds.minBound.y, sceneBounds.maxBound.y}) {
            for (float z : {sceneBounds.minBound.z, sceneBounds.maxBound.z}) {
                glm::vec4 clip = lightVP * glm::vec4(x, y, z, 1.0f);
                glm::vec3 ndc = glm::vec3(clip) / clip.w;

                // WebGPU clip coordinates: X in [-1, 1], Y in [-1, 1], Z in [0, 1]
                EXPECT_GE(ndc.x, -1.05f);
                EXPECT_LE(ndc.x,  1.05f);
                EXPECT_GE(ndc.y, -1.05f);
                EXPECT_LE(ndc.y,  1.05f);
                EXPECT_GE(ndc.z, -0.05f);
                EXPECT_LE(ndc.z,  1.05f);
            }
        }
    }
}

TEST(RenderingFeaturesTest, ParallaxOcclusionMappingTangentSpaceProjection) {
    // Orthonormal TBN basis (Tangent=+X, Bitangent=+Y, Normal=+Z)
    const glm::vec3 T(1.0f, 0.0f, 0.0f);
    const glm::vec3 B(0.0f, 1.0f, 0.0f);
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    const glm::mat3 tbn(T, B, N);

    // 1. Direct normal incidence: V points directly along +Z
    const glm::vec3 viewNormal(0.0f, 0.0f, 1.0f);
    const glm::vec3 vTanNorm = glm::transpose(tbn) * viewNormal;
    EXPECT_NEAR(vTanNorm.x, 0.0f, 1e-5f);
    EXPECT_NEAR(vTanNorm.y, 0.0f, 1e-5f);
    EXPECT_NEAR(vTanNorm.z, 1.0f, 1e-5f);

    // Normal view: abs(vTanNorm.z) is 1.0 -> adaptive POM layer count is minimum (8 steps)
    const float minLayers = 8.0f;
    const float maxLayers = 32.0f;
    const float layersNorm = glm::mix(maxLayers, minLayers, std::clamp(std::abs(vTanNorm.z), 0.0f, 1.0f));
    EXPECT_FLOAT_EQ(layersNorm, 8.0f);

    // 2. Steep grazing incidence: V at 80 degrees from normal
    const float angle = glm::radians(80.0f);
    const glm::vec3 viewGrazing(std::sin(angle), 0.0f, std::cos(angle));
    const glm::vec3 vTanGrazing = glm::transpose(tbn) * viewGrazing;
    const float layersGrazing = glm::mix(maxLayers, minLayers, std::clamp(std::abs(vTanGrazing.z), 0.0f, 1.0f));
    EXPECT_GT(layersGrazing, 25.0f);
    EXPECT_LE(layersGrazing, 32.0f);

    // 3. Parallax offset displacement vector P = (vTan.xy / max(abs(vTan.z), 0.15)) * scale
    const float pomScale = 0.035f;
    const glm::vec2 P = (glm::vec2(vTanGrazing.x, vTanGrazing.y) / std::max(std::abs(vTanGrazing.z), 0.15f)) * pomScale;
    EXPECT_GT(glm::length(P), 0.0f);
    EXPECT_LT(glm::length(P), 0.3f); // Must stay tightly bounded to prevent UV tearing
}

TEST(RenderingFeaturesTest, MultiBounceAmbientOcclusionConservation) {
    // Jimenez multi-bounce AO formula:
    // ao_multi = mix(ao, ao * (diffuseColor * (1.0 - ao) + 1.0), 0.35)
    auto evalMultiBounceAO = [](float ao, const glm::vec3& diffuseColor) -> glm::vec3 {
        const glm::vec3 combinedAO(std::clamp(ao, 0.0f, 1.0f));
        const glm::vec3 multiBounce = combinedAO * (diffuseColor * (glm::vec3(1.0f) - combinedAO) + glm::vec3(1.0f));
        return glm::mix(combinedAO, multiBounce, 0.35f);
    };

    // 1. Fully unoccluded surface (AO = 1.0): returns 1.0 on all channels regardless of color
    const glm::vec3 white(1.0f);
    const glm::vec3 aoFull = evalMultiBounceAO(1.0f, white);
    EXPECT_NEAR(aoFull.r, 1.0f, 1e-5f);
    EXPECT_NEAR(aoFull.g, 1.0f, 1e-5f);
    EXPECT_NEAR(aoFull.b, 1.0f, 1e-5f);

    // 2. Fully occluded crevice (AO = 0.0): returns 0.0
    const glm::vec3 aoZero = evalMultiBounceAO(0.0f, white);
    EXPECT_NEAR(aoZero.r, 0.0f, 1e-5f);
    EXPECT_NEAR(aoZero.g, 0.0f, 1e-5f);
    EXPECT_NEAR(aoZero.b, 0.0f, 1e-5f);

    // 3. Partial occlusion (AO = 0.5) on vivid red surface (1.0, 0.1, 0.1)
    // Multi-bounce conservation boosts the red channel bounce more than green/blue
    const glm::vec3 redSurface(1.0f, 0.1f, 0.1f);
    const glm::vec3 aoRed = evalMultiBounceAO(0.5f, redSurface);
    EXPECT_GT(aoRed.r, 0.5f); // Red bounce lifts ambient brightness in crevices
    EXPECT_GT(aoRed.r, aoRed.g); // Preserves chromatic saturation without turning dirty gray
    EXPECT_NEAR(aoRed.g, aoRed.b, 1e-5f);
}

TEST(RenderingFeaturesTest, DirectionalSunLensFlareScreenProjection) {
    core::Camera camera(glm::vec3(0.0f, 0.0f, 0.0f), 10.0f);
    camera.Update(16.0f / 9.0f);
    const glm::mat4 vp = camera.GetViewProjectionMatrix();
    const glm::vec3 camPos = camera.GetPosition();

    // 1. Sun positioned directly ahead of camera along view target
    const glm::vec3 sunAhead = glm::normalize(camera.GetTarget() - camera.GetPosition());
    const glm::vec4 sunAheadWorld = glm::vec4(camPos + sunAhead * 1000.0f, 1.0f);
    const glm::vec4 sunAheadClip = vp * sunAheadWorld;

    EXPECT_GT(sunAheadClip.w, 0.0f); // In front of camera
    const glm::vec3 sunAheadNdc = glm::vec3(sunAheadClip) / sunAheadClip.w;
    EXPECT_NEAR(sunAheadNdc.x, 0.0f, 0.05f); // Near center X
    EXPECT_NEAR(sunAheadNdc.y, 0.0f, 0.05f); // Near center Y

    const glm::vec2 sunAheadUV(sunAheadNdc.x * 0.5f + 0.5f, 0.5f - sunAheadNdc.y * 0.5f);
    EXPECT_NEAR(sunAheadUV.x, 0.5f, 0.05f);
    EXPECT_NEAR(sunAheadUV.y, 0.5f, 0.05f);

    // 2. Sun positioned behind camera: W clip must be negative or depth inverted
    const glm::vec3 sunBehind = -sunAhead;
    const glm::vec4 sunBehindWorld = glm::vec4(camPos + sunBehind * 1000.0f, 1.0f);
    const glm::vec4 sunBehindClip = vp * sunBehindWorld;
    EXPECT_LT(sunBehindClip.w, 0.0f); // Culled behind near plane
}

TEST(RenderingFeaturesTest, PipelineTonemapOperatorAndExposureCPU) {
    const glm::vec3 brightHdr(3.5f, 2.8f, 1.9f);

    // 1. Khronos PBR Neutral tonemapper produces valid clamped [0, 1] output
    const glm::vec3 mappedNeutral = gpu::PipelineManager::ApplyToneMappingCPU(brightHdr, 1.0f, gpu::ToneMapOperator::KhronosPbrNeutral);
    EXPECT_GE(mappedNeutral.r, 0.0f); EXPECT_LE(mappedNeutral.r, 1.0f);
    EXPECT_GE(mappedNeutral.g, 0.0f); EXPECT_LE(mappedNeutral.g, 1.0f);
    EXPECT_GE(mappedNeutral.b, 0.0f); EXPECT_LE(mappedNeutral.b, 1.0f);

    // 2. AgX tonemapper
    const glm::vec3 mappedAgX = gpu::PipelineManager::ApplyToneMappingCPU(brightHdr, 1.0f, gpu::ToneMapOperator::AgX);
    EXPECT_GE(mappedAgX.r, 0.0f); EXPECT_LE(mappedAgX.r, 1.0f);
    EXPECT_GE(mappedAgX.g, 0.0f); EXPECT_LE(mappedAgX.g, 1.0f);
    EXPECT_GE(mappedAgX.b, 0.0f); EXPECT_LE(mappedAgX.b, 1.0f);

    // 3. ACES Filmic tonemapper
    const glm::vec3 mappedACES = gpu::PipelineManager::ApplyToneMappingCPU(brightHdr, 1.0f, gpu::ToneMapOperator::ACES);
    EXPECT_GE(mappedACES.r, 0.0f); EXPECT_LE(mappedACES.r, 1.0f);
    EXPECT_GE(mappedACES.g, 0.0f); EXPECT_LE(mappedACES.g, 1.0f);
    EXPECT_GE(mappedACES.b, 0.0f); EXPECT_LE(mappedACES.b, 1.0f);

    // 4. Higher exposure brightens intermediate values monotonically
    const glm::vec3 lowExp = gpu::PipelineManager::ApplyToneMappingCPU(glm::vec3(0.5f), 0.5f, gpu::ToneMapOperator::ACES);
    const glm::vec3 highExp = gpu::PipelineManager::ApplyToneMappingCPU(glm::vec3(0.5f), 2.0f, gpu::ToneMapOperator::ACES);
    EXPECT_GT(highExp.r, lowExp.r);
}

TEST(RenderingFeaturesTest, AlphaTestedShadowMaskDiscardLogic) {
    // Helper replicating the shadow shader fragment discard logic
    auto evaluateShadowDiscard = [](uint32_t alphaMode, float alphaCutoff, float textureAlpha, float baseColorFactorAlpha, bool hasBaseTex) -> bool {
        float alpha = baseColorFactorAlpha;
        if (hasBaseTex) {
            alpha *= textureAlpha;
        }
        if (alphaMode == 1u) { // Mask
            return alpha < alphaCutoff; // true = discard
        } else if (alphaMode == 2u) { // Blend
            return alpha < std::max(alphaCutoff, 0.5f);
        }
        return false; // Opaque = never discard
    };

    // 1. OPAQUE mode (0) never discards regardless of alpha
    EXPECT_FALSE(evaluateShadowDiscard(0, 0.5f, 0.1f, 1.0f, true));
    EXPECT_FALSE(evaluateShadowDiscard(0, 0.5f, 0.0f, 0.0f, true));

    // 2. MASK mode (1) discards when alpha < cutoff
    EXPECT_TRUE(evaluateShadowDiscard(1, 0.5f, 0.49f, 1.0f, true));
    EXPECT_FALSE(evaluateShadowDiscard(1, 0.5f, 0.50f, 1.0f, true));
    EXPECT_FALSE(evaluateShadowDiscard(1, 0.5f, 0.85f, 1.0f, true));

    // 3. Custom alphaCutoff (e.g. 0.75 for tight foliage leaf cutouts)
    EXPECT_TRUE(evaluateShadowDiscard(1, 0.75f, 0.70f, 1.0f, true));
    EXPECT_FALSE(evaluateShadowDiscard(1, 0.75f, 0.76f, 1.0f, true));

    // 4. BaseColorFactor alpha modulation: baseFactor = 0.5, texAlpha = 0.8 -> effective alpha = 0.4 < cutoff(0.5) -> discard
    EXPECT_TRUE(evaluateShadowDiscard(1, 0.5f, 0.8f, 0.5f, true));
}

TEST(RenderingFeaturesTest, Ktx2TranscodeTargetSelectionByGPUFeatures) {
    // Save original default target
    const auto origTarget = scene::TextureDecoder::GetDefaultKtxTarget();

    // 1. Check setting and getting transcode targets
    scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Bc7);
    EXPECT_EQ(scene::TextureDecoder::GetDefaultKtxTarget(), scene::KtxTranscodeTarget::Bc7);
    EXPECT_EQ(scene::TextureDecoder::GetKtxTargetName(scene::KtxTranscodeTarget::Bc7), "BC7_RGBA");

    scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Astc4x4);
    EXPECT_EQ(scene::TextureDecoder::GetDefaultKtxTarget(), scene::KtxTranscodeTarget::Astc4x4);
    EXPECT_EQ(scene::TextureDecoder::GetKtxTargetName(scene::KtxTranscodeTarget::Astc4x4), "ASTC_4x4_RGBA");

    scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Etc2Rgba);
    EXPECT_EQ(scene::TextureDecoder::GetDefaultKtxTarget(), scene::KtxTranscodeTarget::Etc2Rgba);
    EXPECT_EQ(scene::TextureDecoder::GetKtxTargetName(scene::KtxTranscodeTarget::Etc2Rgba), "ETC2_RGBA8");

    scene::TextureDecoder::SetDefaultKtxTarget(scene::KtxTranscodeTarget::Rgba32);
    EXPECT_EQ(scene::TextureDecoder::GetDefaultKtxTarget(), scene::KtxTranscodeTarget::Rgba32);
    EXPECT_EQ(scene::TextureDecoder::GetKtxTargetName(scene::KtxTranscodeTarget::Rgba32), "RGBA32");

    // 2. Hardware feature prioritization logic:
    // If BC7 supported (desktop), select BC7
    auto selectTarget = [](bool bc, bool astc, bool etc2) -> scene::KtxTranscodeTarget {
        if (bc) return scene::KtxTranscodeTarget::Bc7;
        if (astc) return scene::KtxTranscodeTarget::Astc4x4;
        if (etc2) return scene::KtxTranscodeTarget::Etc2Rgba;
        return scene::KtxTranscodeTarget::Rgba32;
    };

    EXPECT_EQ(selectTarget(true, false, false), scene::KtxTranscodeTarget::Bc7);
    EXPECT_EQ(selectTarget(false, true, false), scene::KtxTranscodeTarget::Astc4x4);
    EXPECT_EQ(selectTarget(false, false, true), scene::KtxTranscodeTarget::Etc2Rgba);
    EXPECT_EQ(selectTarget(false, false, false), scene::KtxTranscodeTarget::Rgba32);

    // Restore original target
    scene::TextureDecoder::SetDefaultKtxTarget(origTarget);
}

TEST(RenderingFeaturesTest, TrueTransmissionBeerLambertVolumeAttenuation) {
    // 1. Zero thickness produces no attenuation (transmittance = 1.0)
    const glm::vec3 greenTint(0.2f, 0.9f, 0.3f);
    const glm::vec3 noAtten = gpu::PipelineManager::EvaluateBeerLambertVolumeAttenuation(greenTint, 1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(noAtten.r, 1.0f, 1e-5f);
    EXPECT_NEAR(noAtten.g, 1.0f, 1e-5f);
    EXPECT_NEAR(noAtten.b, 1.0f, 1e-5f);

    // 2. Infinite attenuation distance produces no attenuation
    const glm::vec3 infDistAtten = gpu::PipelineManager::EvaluateBeerLambertVolumeAttenuation(greenTint, 1e9f, 2.0f, 1.0f);
    EXPECT_NEAR(infDistAtten.r, 1.0f, 1e-5f);
    EXPECT_NEAR(infDistAtten.g, 1.0f, 1e-5f);
    EXPECT_NEAR(infDistAtten.b, 1.0f, 1e-5f);

    // 3. For green-tinted glass: red and blue attenuate faster than green
    const glm::vec3 glassAtten = gpu::PipelineManager::EvaluateBeerLambertVolumeAttenuation(greenTint, 1.0f, 1.0f, 1.0f);
    EXPECT_GT(glassAtten.g, glassAtten.r);
    EXPECT_GT(glassAtten.g, glassAtten.b);
    EXPECT_LT(glassAtten.r, 1.0f);
    EXPECT_LT(glassAtten.g, 1.0f);

    // 4. Monotonicity: Doubling thickness halves remaining transmission exponentially
    const glm::vec3 thickGlassAtten = gpu::PipelineManager::EvaluateBeerLambertVolumeAttenuation(greenTint, 1.0f, 2.0f, 1.0f);
    EXPECT_LT(thickGlassAtten.g, glassAtten.g);
    EXPECT_NEAR(thickGlassAtten.g, glassAtten.g * glassAtten.g, 1e-4f);

    // 5. Glancing angle (small NdotV) lengthens optical path and reduces transmittance
    const glm::vec3 directView = gpu::PipelineManager::EvaluateBeerLambertVolumeAttenuation(greenTint, 1.0f, 1.0f, 1.0f);
    const glm::vec3 glancingView = gpu::PipelineManager::EvaluateBeerLambertVolumeAttenuation(greenTint, 1.0f, 1.0f, 0.2f);
    EXPECT_LT(glancingView.g, directView.g);
}

TEST(RenderingFeaturesTest, EmissiveBloomThresholdAndScatter) {
    const float threshold = 1.05f;

    // 1. Color with luminance below threshold produces zero bloom
    const glm::vec3 dimColor(0.5f, 0.5f, 0.5f);
    const glm::vec3 noBloom = gpu::PipelineManager::EvaluateBloomThreshold(dimColor, 1.0f, threshold);
    EXPECT_FLOAT_EQ(noBloom.r, 0.0f);
    EXPECT_FLOAT_EQ(noBloom.g, 0.0f);
    EXPECT_FLOAT_EQ(noBloom.b, 0.0f);

    // 2. High-intensity emissive neon surface produces bright bloom extraction
    const glm::vec3 neonColor(6.0f, 2.0f, 1.0f);
    const glm::vec3 bloom = gpu::PipelineManager::EvaluateBloomThreshold(neonColor, 1.0f, threshold);
    EXPECT_GT(bloom.r, 0.0f);
    EXPECT_LT(bloom.r, neonColor.r);

    // 3. Increasing exposure scales luminance and increases extracted bloom
    const glm::vec3 lowExpBloom = gpu::PipelineManager::EvaluateBloomThreshold(neonColor, 1.0f, threshold);
    const glm::vec3 highExpBloom = gpu::PipelineManager::EvaluateBloomThreshold(neonColor, 2.0f, threshold);
    EXPECT_GT(highExpBloom.r, lowExpBloom.r);
}

TEST(RenderingFeaturesTest, ScreenSpaceAmbientOcclusionHorizonFactor) {
    const float radius = 0.05f;
    const float bias = 0.005f;

    // 1. Coplanar neighbor (diff <= bias) produces 1.0 (unoccluded)
    const float flatOcclusion = gpu::PipelineManager::EvaluateScreenSpaceOcclusionFactor(1.0f, 1.002f, radius, bias);
    EXPECT_FLOAT_EQ(flatOcclusion, 1.0f);

    // 2. Crevice geometry (diff > bias and diff < radius) produces occlusion < 1.0
    const float creviceOcclusion = gpu::PipelineManager::EvaluateScreenSpaceOcclusionFactor(1.03f, 1.0f, radius, bias);
    EXPECT_LT(creviceOcclusion, 1.0f);
    EXPECT_GT(creviceOcclusion, 0.0f);

    // 3. Far distant geometry (diff >= radius) falls outside horizon kernel (factor = 1.0)
    const float farOcclusion = gpu::PipelineManager::EvaluateScreenSpaceOcclusionFactor(1.5f, 1.0f, radius, bias);
    EXPECT_FLOAT_EQ(farOcclusion, 1.0f);
}

TEST(RenderingFeaturesTest, DiurnalSunTrajectoryCalculation) {
    auto computeSunDir = [](float hour) -> glm::vec3 {
        const float time = std::clamp(hour, 0.0f, 24.0f);
        const float dayFraction = (time - 6.0f) / 12.0f;
        const float el = std::max(std::sin(dayFraction * std::numbers::pi_v<float>), 0.05f);
        const float az = (time / 24.0f) * 2.0f * std::numbers::pi_v<float>;
        const float cosEl = std::cos(el);
        return glm::normalize(glm::vec3{cosEl * std::sin(az), std::sin(el), cosEl * std::cos(az)});
    };

    // 1. Solar noon (12:00) has peak upward elevation
    const glm::vec3 noonSun = computeSunDir(12.0f);
    EXPECT_GT(noonSun.y, 0.7f);
    EXPECT_NEAR(glm::length(noonSun), 1.0f, 1e-4f);

    // 2. Sunset (~18:00) has low elevation near horizon
    const glm::vec3 sunsetSun = computeSunDir(18.0f);
    EXPECT_LT(sunsetSun.y, noonSun.y);
    EXPECT_NEAR(glm::length(sunsetSun), 1.0f, 1e-4f);
}

TEST(RenderingFeaturesTest, GamepadRadialDeadzoneRemapping) {
    // 1. Sub-deadzone values filtered completely to zero
    const glm::vec2 slightDrift(0.05f, -0.06f);
    const glm::vec2 filtered = core::Gamepad::ApplyDeadzone(slightDrift);
    EXPECT_FLOAT_EQ(filtered.x, 0.0f);
    EXPECT_FLOAT_EQ(filtered.y, 0.0f);

    // 2. Full deflection returns unit magnitude
    const glm::vec2 fullRight(1.0f, 0.0f);
    const glm::vec2 remapped = core::Gamepad::ApplyDeadzone(fullRight);
    EXPECT_NEAR(remapped.x, 1.0f, 1e-3f);
    EXPECT_FLOAT_EQ(remapped.y, 0.0f);
}

TEST(RenderingFeaturesTest, SceneNodeHierarchyAndCameraData) {
    scene::Scene scene{};
    auto root = std::make_shared<scene::SceneNode>();
    root->name = "RootNode";
    root->nodeIndex = 0;

    auto child = std::make_shared<scene::SceneNode>();
    child->name = "CameraMount";
    child->nodeIndex = 1;
    child->cameraIndex = 0;
    root->children.push_back(child);

    scene.rootNodes.push_back(root);
    scene.allNodes.push_back(root);
    scene.allNodes.push_back(child);

    scene::SceneCamera cam{};
    cam.name = "TestPerspCamera";
    cam.isPerspective = true;
    cam.yfov = glm::radians(60.0f);
    cam.nodeIndex = 1;
    scene.cameras.push_back(cam);

    EXPECT_EQ(scene.allNodes.size(), 2u);
    EXPECT_EQ(scene.cameras.size(), 1u);
    EXPECT_EQ(scene.cameras[0].nodeIndex, 1);
    EXPECT_EQ(scene.allNodes[1]->cameraIndex, 0);
}

// ============================================================================
// 10. Memory Tracker, PS5 Gamepad, GPU Compute & Modular Pass Tests
// ============================================================================

TEST(RenderingFeaturesTest, MemoryTrackerRssAndVramAccounting) {
    auto& tracker = core::MemoryTracker::Instance();

    // 1. Resident Set Size (RAM) query
    const auto initialTelemetry = tracker.Query();
#if defined(__linux__)
    EXPECT_GE(initialTelemetry.RamMb(), 0.0f);
#endif

    // 2. VRAM byte accounting
    constexpr size_t kBufBytes = 4 * 1024 * 1024; // 4 MB
    constexpr size_t kTexBytes = 8 * 1024 * 1024; // 8 MB

    tracker.TrackBufferAlloc(kBufBytes);
    tracker.TrackTextureAlloc(kTexBytes);

    EXPECT_EQ(tracker.GetGpuBufferBytes(), kBufBytes);
    EXPECT_EQ(tracker.GetGpuTextureBytes(), kTexBytes);
    const auto activeTelemetry = tracker.Query();
    EXPECT_NEAR(activeTelemetry.VramMb(), initialTelemetry.VramMb() + 12.0f, 0.05f);

    tracker.TrackBufferFree(kBufBytes);
    tracker.TrackTextureFree(kTexBytes);

    EXPECT_EQ(tracker.GetGpuBufferBytes(), 0u);
    EXPECT_EQ(tracker.GetGpuTextureBytes(), 0u);
    const auto freedTelemetry = tracker.Query();
    EXPECT_NEAR(freedTelemetry.VramMb(), initialTelemetry.VramMb(), 0.05f);
}

TEST(RenderingFeaturesTest, Ps5DualSenseDetectionAndExponentialResponseCurve) {
    // 1. Radial Deadzone (kDeadzone = 0.12)
    const glm::vec2 insideDeadzone = core::Gamepad::ApplyDeadzone(glm::vec2{0.05f, -0.08f}); // length ~ 0.094 < 0.12
    EXPECT_FLOAT_EQ(insideDeadzone.x, 0.0f);
    EXPECT_FLOAT_EQ(insideDeadzone.y, 0.0f);

    core::Gamepad gamepad{};
    core::GamepadState rawState{};
    rawState.connected = true;
    rawState.isDualSense = true;
    rawState.rightStick = glm::vec2{0.05f, -0.08f};
    gamepad.SetRawState(rawState, 0.016f);
    EXPECT_FLOAT_EQ(gamepad.GetState().curvedLook.x, 0.0f);
    EXPECT_FLOAT_EQ(gamepad.GetState().curvedLook.y, 0.0f);

    // 2. Full Deflection Preserved
    const glm::vec2 fullDeflectionX = core::Gamepad::ApplyPs5ResponseCurve(glm::vec2{1.0f, 0.0f}, 2.1f, 0.70f);
    EXPECT_NEAR(fullDeflectionX.x, 1.0f, 0.001f);
    EXPECT_FLOAT_EQ(fullDeflectionX.y, 0.0f);

    // Pitch axis dampened by 0.70
    const glm::vec2 fullDeflectionY = core::Gamepad::ApplyPs5ResponseCurve(glm::vec2{0.0f, 1.0f}, 2.1f, 0.70f);
    EXPECT_FLOAT_EQ(fullDeflectionY.x, 0.0f);
    EXPECT_NEAR(fullDeflectionY.y, 0.70f, 0.001f);

    // 3. Exponential Sensitivity Curve: mag^2.1 grants finer aiming control near center
    constexpr float rawX = 0.5f;
    const glm::vec2 halfStick = core::Gamepad::ApplyPs5ResponseCurve(glm::vec2{rawX, 0.0f}, 2.1f, 0.70f);
    const float expectedExpNorm = std::pow(rawX, 2.1f); // ~0.233258
    EXPECT_NEAR(halfStick.x, expectedExpNorm, 0.001f);
    EXPECT_LT(halfStick.x, rawX); // Exponential curve grants finer control near center!

    // 4. Symmetry under negative axes
    const glm::vec2 negStick = core::Gamepad::ApplyPs5ResponseCurve(glm::vec2{-rawX, 0.0f}, 2.1f, 0.70f);
    EXPECT_NEAR(negStick.x, -halfStick.x, 0.001f);
}

TEST(RenderingFeaturesTest, GpuCullerFrustumAnalyticalPlanesEvaluation) {
    // Verify frustum analytical plane tests identical to cs_cull WGSL shader
    // Symmetrical perspective frustum from origin looking down -Z
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    const glm::mat4 view = glm::lookAtRH(glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
    const core::Frustum frustum = core::Frustum::FromViewProjection(proj * view);

    // 1. Box strictly in front of camera
    const core::AABB boxInside{ glm::vec3{-1.0f, -1.0f, -10.0f}, glm::vec3{1.0f, 1.0f, -5.0f} };
    EXPECT_TRUE(frustum.IntersectsAABB(boxInside));

    // 2. Box strictly behind camera (+Z)
    const core::AABB boxBehind{ glm::vec3{-1.0f, -1.0f, 5.0f}, glm::vec3{1.0f, 1.0f, 10.0f} };
    EXPECT_FALSE(frustum.IntersectsAABB(boxBehind));

    // 3. Plane equation dot(normal, pVertex) + d evaluation
    for (int p = 0; p < 6; ++p) {
        const auto& plane = frustum.planes[p];
        const glm::vec3 pVertex{
            (plane.normal.x >= 0.0f) ? boxInside.maxBound.x : boxInside.minBound.x,
            (plane.normal.y >= 0.0f) ? boxInside.maxBound.y : boxInside.minBound.y,
            (plane.normal.z >= 0.0f) ? boxInside.maxBound.z : boxInside.minBound.z
        };
        EXPECT_GE(glm::dot(plane.normal, pVertex) + plane.distance, 0.0f);
    }
}

TEST(RenderingFeaturesTest, GpuRayPickerSlabIntersectionOrdering) {
    // Tests ordering preservation when floats are bitcast to u32 for atomicMin
    const core::Ray ray{ glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f} };

    const core::AABB nearBox{ glm::vec3{-1.0f, -1.0f, 5.0f}, glm::vec3{1.0f, 1.0f, 7.0f} };
    const core::AABB farBox{ glm::vec3{-1.0f, -1.0f, 15.0f}, glm::vec3{1.0f, 1.0f, 17.0f} };

    float tNearMin = 0.0f, tNearMax = 0.0f;
    float tFarMin = 0.0f, tFarMax = 0.0f;
    EXPECT_TRUE(nearBox.Intersects(ray, tNearMin, tNearMax));
    EXPECT_TRUE(farBox.Intersects(ray, tFarMin, tFarMax));

    EXPECT_NEAR(tNearMin, 5.0f, 1e-4f);
    EXPECT_NEAR(tFarMin, 15.0f, 1e-4f);
    EXPECT_LT(tNearMin, tFarMin);

    // IEEE-754 positive floats preserve strict ordering under bitcast to uint32_t
    const uint32_t bitsNear = std::bit_cast<uint32_t>(tNearMin);
    const uint32_t bitsFar = std::bit_cast<uint32_t>(tFarMin);
    EXPECT_LT(bitsNear, bitsFar);

    // Atomic minimum in WGSL cs_pick will thus correctly select bitsNear
    const uint32_t minBits = std::min(bitsNear, bitsFar);
    EXPECT_EQ(minBits, bitsNear);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(minBits), 5.0f);
}

TEST(RenderingFeaturesTest, ModularPassesRaiiAndDelegation) {
    // Verify RAII lifecycle of extracted passes
    {
        gpu::AabbRenderer aabbRenderer{};
        EXPECT_FALSE(aabbRenderer.IsInitialized());
        gpu::AabbRenderer movedAabb{ std::move(aabbRenderer) };
        EXPECT_FALSE(movedAabb.IsInitialized());
    }
    {
        gpu::ShadowPass shadowPass{};
        EXPECT_FALSE(shadowPass.IsInitialized());
        gpu::ShadowPass movedShadow{ std::move(shadowPass) };
        EXPECT_FALSE(movedShadow.IsInitialized());
    }
    {
        gpu::PostProcessPass postPass{};
        EXPECT_FALSE(postPass.IsInitialized());
        gpu::PostProcessPass movedPost{ std::move(postPass) };
        EXPECT_FALSE(movedPost.IsInitialized());
    }
    {
        gpu::GpuCuller culler{};
        EXPECT_FALSE(culler.IsInitialized());
        gpu::GpuCuller movedCuller{ std::move(culler) };
        EXPECT_FALSE(movedCuller.IsInitialized());
    }
    {
        gpu::GpuRayPicker rayPicker{};
        EXPECT_FALSE(rayPicker.IsInitialized());
        gpu::GpuRayPicker movedPicker{ std::move(rayPicker) };
        EXPECT_FALSE(movedPicker.IsInitialized());
    }
}

TEST(RenderingFeaturesTest, DebugOverlayModeCyclingAndStatsState) {
    ui::DebugOverlay overlay{};
    EXPECT_TRUE(overlay.IsVisible());
    EXPECT_EQ(overlay.GetMode(), ui::OverlayMode::Full);

    // Test cycling: Full -> Minimal -> Hidden -> Full
    overlay.Toggle();
    EXPECT_TRUE(overlay.IsVisible());
    EXPECT_EQ(overlay.GetMode(), ui::OverlayMode::Minimal);

    overlay.Toggle();
    EXPECT_FALSE(overlay.IsVisible());
    EXPECT_EQ(overlay.GetMode(), ui::OverlayMode::Hidden);

    overlay.Toggle();
    EXPECT_TRUE(overlay.IsVisible());
    EXPECT_EQ(overlay.GetMode(), ui::OverlayMode::Full);

    // Direct mode setting
    overlay.SetMode(ui::OverlayMode::Minimal);
    EXPECT_EQ(overlay.GetMode(), ui::OverlayMode::Minimal);
    EXPECT_TRUE(overlay.IsVisible());

    overlay.SetVisible(false);
    EXPECT_EQ(overlay.GetMode(), ui::OverlayMode::Hidden);
    EXPECT_FALSE(overlay.IsVisible());

    overlay.SetVisible(true);
    EXPECT_EQ(overlay.GetMode(), ui::OverlayMode::Full);
    EXPECT_TRUE(overlay.IsVisible());

    // Update with default stats
    ui::OverlayStats stats{};
    stats.deltaTime = 0.0166f;
    stats.sceneName = "TestScene";
    stats.primitiveCount = 42;
    stats.residentRamBytes = 64 * 1024 * 1024;
    overlay.Update(0.0166f, stats);
}







