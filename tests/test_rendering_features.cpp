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
    ASSERT_EQ(loadedScene->rootNodes[0]->primitives.size(), 1u);
    EXPECT_EQ(loadedScene->rootNodes[0]->primitives[0].vertices.size(), 3u);
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
    EXPECT_EQ((*httpSceneRes)->rootNodes[0]->primitives[0].vertices.size(), 3u);
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


