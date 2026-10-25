/**
 * @file test_types_math.cpp
 * @brief Comprehensive unit tests for core 3D types: AABB, Plane, Frustum, Ray, and Vertex layout.
 */
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include "core/Types.hpp"
#include "scene/Scene.hpp"
#include "gpu/Pipeline.hpp"

using namespace bgl::core;

TEST(TypesMathTest, AABBInitializationAndExpansion) {
    AABB box;
    // Default box is inverted (minBound > maxBound)
    EXPECT_GT(box.minBound.x, box.maxBound.x);
    EXPECT_GT(box.minBound.y, box.maxBound.y);
    EXPECT_GT(box.minBound.z, box.maxBound.z);

    // Expand with single point
    box.Expand(glm::vec3(2.0f, -3.0f, 5.0f));
    EXPECT_EQ(box.minBound, glm::vec3(2.0f, -3.0f, 5.0f));
    EXPECT_EQ(box.maxBound, glm::vec3(2.0f, -3.0f, 5.0f));

    // Expand with second point
    box.Expand(glm::vec3(-4.0f, 6.0f, 1.0f));
    EXPECT_EQ(box.minBound, glm::vec3(-4.0f, -3.0f, 1.0f));
    EXPECT_EQ(box.maxBound, glm::vec3(2.0f, 6.0f, 5.0f));

    // Center and Extents
    glm::vec3 center = box.Center();
    EXPECT_FLOAT_EQ(center.x, -1.0f);
    EXPECT_FLOAT_EQ(center.y, 1.5f);
    EXPECT_FLOAT_EQ(center.z, 3.0f);

    glm::vec3 extents = box.Extents();
    EXPECT_FLOAT_EQ(extents.x, 3.0f);
    EXPECT_FLOAT_EQ(extents.y, 4.5f);
    EXPECT_FLOAT_EQ(extents.z, 2.0f);
}

TEST(TypesMathTest, AABBBoxExpansion) {
    AABB box1;
    box1.Expand(glm::vec3(0.0f, 0.0f, 0.0f));
    box1.Expand(glm::vec3(2.0f, 2.0f, 2.0f));

    AABB box2;
    box2.Expand(glm::vec3(-1.0f, 1.0f, 3.0f));
    box2.Expand(glm::vec3(4.0f, 5.0f, 6.0f));

    box1.Expand(box2);
    EXPECT_EQ(box1.minBound, glm::vec3(-1.0f, 0.0f, 0.0f));
    EXPECT_EQ(box1.maxBound, glm::vec3(4.0f, 5.0f, 6.0f));
}

TEST(TypesMathTest, AABBRayIntersection) {
    AABB box;
    box.Expand(glm::vec3(-1.0f, -1.0f, -1.0f));
    box.Expand(glm::vec3(1.0f, 1.0f, 1.0f));

    float tMin = 0.0f;
    float tMax = 0.0f;

    // Direct hit along Z
    Ray ray1{ glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
    EXPECT_TRUE(box.Intersects(ray1, tMin, tMax));
    EXPECT_NEAR(tMin, 4.0f, 1e-4f);
    EXPECT_NEAR(tMax, 6.0f, 1e-4f);

    // Direct hit from inside box
    Ray rayInside{ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
    EXPECT_TRUE(box.Intersects(rayInside, tMin, tMax));
    EXPECT_NEAR(tMin, 0.0f, 1e-4f);
    EXPECT_NEAR(tMax, 1.0f, 1e-4f);

    // Miss - ray pointing opposite direction
    Ray rayAway{ glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
    EXPECT_FALSE(box.Intersects(rayAway, tMin, tMax));

    // Miss - parallel to box
    Ray rayParallel{ glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
    EXPECT_FALSE(box.Intersects(rayParallel, tMin, tMax));
}

TEST(TypesMathTest, PlaneNormalizationAndDistance) {
    Plane plane;
    plane.normal = glm::vec3(0.0f, 3.0f, 4.0f);
    plane.distance = 10.0f;

    plane.Normalize();
    EXPECT_NEAR(glm::length(plane.normal), 1.0f, 1e-5f);
    EXPECT_NEAR(plane.normal.y, 3.0f / 5.0f, 1e-5f);
    EXPECT_NEAR(plane.normal.z, 4.0f / 5.0f, 1e-5f);
    EXPECT_NEAR(plane.distance, 2.0f, 1e-5f);

    // Point on positive side
    EXPECT_GT(plane.DistanceToPoint(glm::vec3(0.0f, 10.0f, 0.0f)), 0.0f);

    // Point on negative side
    EXPECT_LT(plane.DistanceToPoint(glm::vec3(0.0f, -10.0f, 0.0f)), 0.0f);
}

TEST(TypesMathTest, FrustumPerspectivePlaneExtraction) {
    glm::vec3 camPos(0.0f, 2.0f, 10.0f);
    glm::vec3 target(0.0f, 2.0f, 0.0f);
    glm::mat4 view = glm::lookAt(camPos, target, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    glm::mat4 vp = proj * view;

    Frustum frustum = Frustum::FromViewProjection(vp);

    // All plane normals must be unit vectors
    for (const auto& plane : frustum.planes) {
        EXPECT_NEAR(glm::length(plane.normal), 1.0f, 1e-4f);
    }

    // Target point (0, 2, 0) is well within frustum
    for (const auto& plane : frustum.planes) {
        EXPECT_GT(plane.DistanceToPoint(target), 0.0f);
    }

    // Point behind camera must be on negative side of near plane
    glm::vec3 behindPoint(0.0f, 2.0f, 15.0f);
    EXPECT_LT(frustum.planes[4].DistanceToPoint(behindPoint), 0.0f);
}

TEST(TypesMathTest, FrustumIntersectsAABBAllCases) {
    glm::vec3 camPos(0.0f, 0.0f, 10.0f);
    glm::vec3 target(0.0f, 0.0f, 0.0f);
    glm::mat4 view = glm::lookAt(camPos, target, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 1.0f, 20.0f);
    Frustum frustum = Frustum::FromViewProjection(proj * view);

    // 1. Box centered at target (fully inside)
    AABB boxCenter;
    boxCenter.Expand(glm::vec3(-1.0f));
    boxCenter.Expand(glm::vec3(1.0f));
    EXPECT_TRUE(frustum.IntersectsAABB(boxCenter));

    // 2. Box behind camera (Z > 10)
    AABB boxBehind;
    boxBehind.Expand(glm::vec3(-1.0f, -1.0f, 12.0f));
    boxBehind.Expand(glm::vec3(1.0f, 1.0f, 15.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxBehind));

    // 3. Box beyond far plane (Z < -15, dist from cam > 25)
    AABB boxBeyondFar;
    boxBeyondFar.Expand(glm::vec3(-1.0f, -1.0f, -25.0f));
    boxBeyondFar.Expand(glm::vec3(1.0f, 1.0f, -22.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxBeyondFar));

    // 4. Box far off to the right
    AABB boxFarRight;
    boxFarRight.Expand(glm::vec3(50.0f, -1.0f, 0.0f));
    boxFarRight.Expand(glm::vec3(55.0f, 1.0f, 2.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxFarRight));

    // 5. Box far above
    AABB boxFarAbove;
    boxFarAbove.Expand(glm::vec3(-1.0f, 40.0f, 0.0f));
    boxFarAbove.Expand(glm::vec3(1.0f, 45.0f, 2.0f));
    EXPECT_FALSE(frustum.IntersectsAABB(boxFarAbove));
}

TEST(TypesMathTest, VertexLayoutAndCacheLineAlignment) {
    EXPECT_EQ(sizeof(Vertex), 96);
    EXPECT_EQ(alignof(Vertex), 16);
    EXPECT_EQ(offsetof(Vertex, position), 0);
    EXPECT_EQ(offsetof(Vertex, normal), 16);
    EXPECT_EQ(offsetof(Vertex, tangent), 32);
    EXPECT_EQ(offsetof(Vertex, uv), 48);
    EXPECT_EQ(offsetof(Vertex, joints), 64);
    EXPECT_EQ(offsetof(Vertex, weights), 80);

    Vertex v;
    v.position = glm::vec3(1.0f, 2.0f, 3.0f);
    v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    v.uv = glm::vec2(0.5f, 0.5f);
    v.joints = glm::uvec4(1, 2, 3, 4);
    v.weights = glm::vec4(0.7f, 0.2f, 0.1f, 0.0f);

    EXPECT_FLOAT_EQ(v.position.x, 1.0f);
    EXPECT_FLOAT_EQ(v.normal.y, 1.0f);
    EXPECT_FLOAT_EQ(v.tangent.w, 1.0f);
    EXPECT_FLOAT_EQ(v.uv.x, 0.5f);
    EXPECT_EQ(v.joints.y, 2u);
    EXPECT_FLOAT_EQ(v.weights.x, 0.7f);
}

TEST(TypesMathTest, MeshPrimitiveComputeTangentsOrthogonalityAndHandedness) {
    bgl::scene::MeshPrimitive prim;
    // Create a quad on an X-facing wall (Normal = +X = (1, 0, 0)), where default tangent (1,0,0) would be degenerate!
    Vertex v0, v1, v2, v3;
    v0.position = glm::vec3(0.0f, 0.0f, 0.0f); v0.normal = glm::vec3(1.0f, 0.0f, 0.0f); v0.uv = glm::vec2(0.0f, 0.0f);
    v1.position = glm::vec3(0.0f, 0.0f, 2.0f); v1.normal = glm::vec3(1.0f, 0.0f, 0.0f); v1.uv = glm::vec2(1.0f, 0.0f);
    v2.position = glm::vec3(0.0f, 2.0f, 2.0f); v2.normal = glm::vec3(1.0f, 0.0f, 0.0f); v2.uv = glm::vec2(1.0f, 1.0f);
    v3.position = glm::vec3(0.0f, 2.0f, 0.0f); v3.normal = glm::vec3(1.0f, 0.0f, 0.0f); v3.uv = glm::vec2(0.0f, 1.0f);

    prim.vertices = { v0, v1, v2, v3 };
    prim.indices = { 0, 1, 2, 0, 2, 3 };

    prim.ComputeTangents();

    for (const auto& v : prim.vertices) {
        glm::vec3 T(v.tangent);
        // Tangent must be unit length
        EXPECT_NEAR(glm::length(T), 1.0f, 1e-4f);
        // Tangent must be strictly orthogonal to normal (+X)
        EXPECT_NEAR(glm::dot(T, v.normal), 0.0f, 1e-4f);
        // UV.u increases along +Z, so tangent must point along +Z (0, 0, 1)
        EXPECT_NEAR(T.z, 1.0f, 1e-4f);
        // Handedness w must be +/- 1.0
        EXPECT_NEAR(std::abs(v.tangent.w), 1.0f, 1e-4f);
    }
}

TEST(TypesMathTest, MipmapCalculationAndDownsamplingWithNormalMapRenormalization) {
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(1, 1), 1u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(1024, 1024), 11u);
    EXPECT_EQ(bgl::gpu::PipelineManager::CalculateMipLevels(512, 256), 10u);

    // 1. Box-filtered color mipmap test (2x2 -> 1x1)
    std::vector<uint8_t> colorSrc = {
        200, 100, 50, 255,   100, 200, 50, 255,
        100, 100, 150, 255,  200, 200, 150, 255
    };
    auto colorMip1 = bgl::gpu::PipelineManager::GenerateNextMipLevel(colorSrc.data(), 2, 2, 1, 1, false);
    ASSERT_EQ(colorMip1.size(), 4u);
    EXPECT_EQ(colorMip1[0], 150u);
    EXPECT_EQ(colorMip1[1], 150u);
    EXPECT_EQ(colorMip1[2], 100u);
    EXPECT_EQ(colorMip1[3], 255u);

    // 2. Normal-map vector renormalization test (2x2 opposing tilted normals -> 1x1 straight +Z normal (0, 0, 1))
    // Texel 0 & 1 tilted +X: (0.7071, 0, 0.7071) -> RGB(218, 128, 218)
    // Texel 2 & 3 tilted -X: (-0.7071, 0, 0.7071) -> RGB(37, 128, 218)
    // Simple box average would yield unnormalized (0, 0, 0.7071) -> B=218.
    // Proper 3D vector renormalization must yield unit normal (0, 0, 1.0) -> RGB(128, 128, 255)!
    std::vector<uint8_t> normalSrc = {
        218, 128, 218, 255,  218, 128, 218, 255,
         37, 128, 218, 255,   37, 128, 218, 255
    };
    auto normalMip1 = bgl::gpu::PipelineManager::GenerateNextMipLevel(normalSrc.data(), 2, 2, 1, 1, true);
    ASSERT_EQ(normalMip1.size(), 4u);
    EXPECT_NEAR(normalMip1[0], 128, 2);
    EXPECT_NEAR(normalMip1[1], 128, 2);
    EXPECT_EQ(normalMip1[2], 255u); // Z component renormalized to +1.0 (255)
    EXPECT_EQ(normalMip1[3], 255u);
}

TEST(TypesMathTest, DirectionalShadowLightMatrixAndEnvCubemapGeneration) {
    AABB sceneBounds;
    sceneBounds.Expand(glm::vec3(-10.0f, 0.0f, -10.0f));
    sceneBounds.Expand(glm::vec3(10.0f, 8.0f, 10.0f));

    glm::vec3 sunDir = glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));
    glm::mat4 lightVP = bgl::gpu::PipelineManager::ComputeDirectionalLightMatrix(sceneBounds, sunDir);

    // Project scene center and corners through lightVP: all must map into valid WebGPU clip space [-1, 1]^2 x [0, 1]
    glm::vec4 centerClip = lightVP * glm::vec4(sceneBounds.Center(), 1.0f);
    glm::vec3 centerNdc = glm::vec3(centerClip) / centerClip.w;
    EXPECT_GE(centerNdc.x, -1.0f);
    EXPECT_LE(centerNdc.x, 1.0f);
    EXPECT_GE(centerNdc.y, -1.0f);
    EXPECT_LE(centerNdc.y, 1.0f);
    EXPECT_GE(centerNdc.z, 0.0f);
    EXPECT_LE(centerNdc.z, 1.0f);

    // Environment cubemap face generation for all 6 faces
    for (uint32_t face = 0; face < 6; ++face) {
        auto faceSharp = bgl::gpu::PipelineManager::GenerateEnvCubemapFaceMip(face, 8, 0.0f, sunDir);
        auto faceRough = bgl::gpu::PipelineManager::GenerateEnvCubemapFaceMip(face, 8, 1.0f, sunDir);
        EXPECT_EQ(faceSharp.size(), 8u * 8u * 4u);
        EXPECT_EQ(faceRough.size(), 8u * 8u * 4u);
    }
}

TEST(TypesMathTest, ArvoBoundingBoxTransformAlgorithm) {
    AABB box;
    box.Expand(glm::vec3(1.0f, 2.0f, 3.0f));
    box.Expand(glm::vec3(3.0f, 5.0f, 7.0f));

    // Test 1: Identity transform must return exact same bounds
    AABB identBox = box.Transformed(glm::mat4(1.0f));
    EXPECT_FLOAT_EQ(identBox.minBound.x, box.minBound.x);
    EXPECT_FLOAT_EQ(identBox.minBound.y, box.minBound.y);
    EXPECT_FLOAT_EQ(identBox.minBound.z, box.minBound.z);
    EXPECT_FLOAT_EQ(identBox.maxBound.x, box.maxBound.x);
    EXPECT_FLOAT_EQ(identBox.maxBound.y, box.maxBound.y);
    EXPECT_FLOAT_EQ(identBox.maxBound.z, box.maxBound.z);

    // Test 2: Translation
    glm::mat4 transMat = glm::translate(glm::mat4(1.0f), glm::vec3(-10.0f, 20.0f, 5.0f));
    AABB transBox = box.Transformed(transMat);
    EXPECT_FLOAT_EQ(transBox.minBound.x, box.minBound.x - 10.0f);
    EXPECT_FLOAT_EQ(transBox.minBound.y, box.minBound.y + 20.0f);
    EXPECT_FLOAT_EQ(transBox.minBound.z, box.minBound.z + 5.0f);
    EXPECT_FLOAT_EQ(transBox.maxBound.x, box.maxBound.x - 10.0f);
    EXPECT_FLOAT_EQ(transBox.maxBound.y, box.maxBound.y + 20.0f);
    EXPECT_FLOAT_EQ(transBox.maxBound.z, box.maxBound.z + 5.0f);

    // Test 3: Complex rotation & scaling (ensure all 8 transformed corners are contained)
    glm::mat4 complexTransform = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, -3.0f, 4.0f)) *
                                 glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                                 glm::scale(glm::mat4(1.0f), glm::vec3(1.5f, -2.0f, 0.8f));

    AABB transformedBox = box.Transformed(complexTransform);
    EXPECT_LE(transformedBox.minBound.x, transformedBox.maxBound.x);
    EXPECT_LE(transformedBox.minBound.y, transformedBox.maxBound.y);
    EXPECT_LE(transformedBox.minBound.z, transformedBox.maxBound.z);

    // Generate all 8 original corners and check containment in transformed AABB
    for (float x : {box.minBound.x, box.maxBound.x}) {
        for (float y : {box.minBound.y, box.maxBound.y}) {
            for (float z : {box.minBound.z, box.maxBound.z}) {
                glm::vec4 pt = complexTransform * glm::vec4(x, y, z, 1.0f);
                glm::vec3 worldPt = glm::vec3(pt) / pt.w;
                EXPECT_GE(worldPt.x, transformedBox.minBound.x - 1e-4f);
                EXPECT_LE(worldPt.x, transformedBox.maxBound.x + 1e-4f);
                EXPECT_GE(worldPt.y, transformedBox.minBound.y - 1e-4f);
                EXPECT_LE(worldPt.y, transformedBox.maxBound.y + 1e-4f);
                EXPECT_GE(worldPt.z, transformedBox.minBound.z - 1e-4f);
                EXPECT_LE(worldPt.z, transformedBox.maxBound.z + 1e-4f);
            }
        }
    }
}

TEST(TypesMathTest, MeshPrimitiveCopySemanticsAndGPUHandleSafety) {
    bgl::scene::MeshPrimitive orig;
    orig.materialIndex = 42;
    orig.boundingBox.minBound = glm::vec3(-1.0f, -2.0f, -3.0f);
    orig.boundingBox.maxBound = glm::vec3(1.0f, 2.0f, 3.0f);

    Vertex v1{}; v1.position = glm::vec3(0.0f, 1.0f, 0.0f);
    Vertex v2{}; v2.position = glm::vec3(-1.0f, 0.0f, 0.0f);
    Vertex v3{}; v3.position = glm::vec3(1.0f, 0.0f, 0.0f);
    orig.vertices = {v1, v2, v3};
    orig.indices = {0, 1, 2};

    // 1. Copy Constructor: CPU data copied, GPU handles set to nullptr
    bgl::scene::MeshPrimitive copied(orig);
    EXPECT_EQ(copied.materialIndex, 42);
    EXPECT_EQ(copied.vertices.size(), 3u);
    EXPECT_EQ(copied.indices.size(), 3u);
    EXPECT_EQ(copied.boundingBox.minBound, orig.boundingBox.minBound);
    EXPECT_EQ(copied.boundingBox.maxBound, orig.boundingBox.maxBound);
    EXPECT_EQ(copied.gpuVertexBuffer, nullptr);
    EXPECT_EQ(copied.gpuIndexBuffer, nullptr);
    EXPECT_EQ(copied.gpuWireframeIndexBuffer, nullptr);
    EXPECT_EQ(copied.gpuUniformBuffer, nullptr);
    EXPECT_EQ(copied.gpuBindGroup, nullptr);
    EXPECT_EQ(copied.gpuShadowBindGroup, nullptr);

    // 2. Copy Assignment
    bgl::scene::MeshPrimitive assigned;
    assigned = orig;
    EXPECT_EQ(assigned.materialIndex, 42);
    EXPECT_EQ(assigned.vertices.size(), 3u);
    EXPECT_EQ(assigned.indices.size(), 3u);
    EXPECT_EQ(assigned.gpuVertexBuffer, nullptr);

    // 3. Move Constructor: transfers ownership, nulls source
    bgl::scene::MeshPrimitive moved(std::move(copied));
    EXPECT_EQ(moved.materialIndex, 42);
    EXPECT_EQ(moved.vertices.size(), 3u);
    EXPECT_TRUE(copied.vertices.empty());
    EXPECT_TRUE(copied.indices.empty());
}

TEST(TypesMathTest, RaySegmentParametricEvaluator) {
    const Ray ray{
        .origin = glm::vec3{1.0f, 2.0f, 3.0f},
        .direction = glm::vec3{0.0f, 1.0f, 0.0f}
    };
    EXPECT_EQ(ray.At(0.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(ray.At(5.0f), glm::vec3(1.0f, 7.0f, 3.0f));
    EXPECT_EQ(ray.At(-2.5f), glm::vec3(1.0f, -0.5f, 3.0f));
}

TEST(TypesMathTest, SceneDestructorTextureSamplerNulling) {
    // Verify Scene cleanup resets textures and sets non-owning sampler references to nullptr
    {
        bgl::scene::Scene scene{};
        bgl::scene::TextureGpuResource texRes{};
        texRes.width = 64;
        texRes.height = 64;
        texRes.texture = nullptr;
        texRes.view = nullptr;
        // Non-null mock sampler borrowed from pipeline
        texRes.sampler = reinterpret_cast<WGPUSampler>(0xCAFE);

        scene.gpuTextures.push_back(texRes);
        EXPECT_EQ(scene.gpuTextures.size(), 1u);
        EXPECT_NE(scene.gpuTextures[0].sampler, nullptr);
        // Exiting scope triggers Scene::~Scene()
    }
    SUCCEED();
}

TEST(TypesMathTest, AABBOverlapAndContainmentCases) {
    AABB base;
    base.Expand(glm::vec3(-2.0f, -2.0f, -2.0f));
    base.Expand(glm::vec3( 2.0f,  2.0f,  2.0f));

    // 1. Fully contained box
    AABB inner;
    inner.Expand(glm::vec3(-1.0f, -1.0f, -1.0f));
    inner.Expand(glm::vec3( 1.0f,  1.0f,  1.0f));
    EXPECT_TRUE(base.Intersects(inner));
    EXPECT_TRUE(inner.Intersects(base));

    // 2. Disjoint box far away
    AABB disjoint;
    disjoint.Expand(glm::vec3(10.0f, 10.0f, 10.0f));
    disjoint.Expand(glm::vec3(12.0f, 12.0f, 12.0f));
    EXPECT_FALSE(base.Intersects(disjoint));
    EXPECT_FALSE(disjoint.Intersects(base));

    // 3. Partially overlapping box
    AABB overlapping;
    overlapping.Expand(glm::vec3(1.0f, 1.0f, 1.0f));
    overlapping.Expand(glm::vec3(5.0f, 5.0f, 5.0f));
    EXPECT_TRUE(base.Intersects(overlapping));
    EXPECT_TRUE(overlapping.Intersects(base));

    // 4. Touching on face (X = 2.0)
    AABB touchingFace;
    touchingFace.Expand(glm::vec3(2.0f, -1.0f, -1.0f));
    touchingFace.Expand(glm::vec3(4.0f,  1.0f,  1.0f));
    EXPECT_TRUE(base.Intersects(touchingFace));
}

TEST(TypesMathTest, AABBRayGrazingEdgesAndCorners) {
    AABB box;
    box.Expand(glm::vec3(-1.0f, -1.0f, -1.0f));
    box.Expand(glm::vec3( 1.0f,  1.0f,  1.0f));

    float tMin = 0.0f;
    float tMax = 0.0f;

    // Ray passing slightly inside the top face boundary (Y = 0.999f, X = 0.0f)
    Ray grazingRay{ glm::vec3(0.0f, 0.999f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
    EXPECT_TRUE(box.Intersects(grazingRay, tMin, tMax));
    EXPECT_NEAR(tMin, 4.0f, 1e-3f);
    EXPECT_NEAR(tMax, 6.0f, 1e-3f);

    // Ray passing strictly outside by epsilon
    Ray missedRay{ glm::vec3(0.0f, 1.01f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
    EXPECT_FALSE(box.Intersects(missedRay, tMin, tMax));
}

TEST(TypesMathTest, FrustumAnalyticalPlanesSymmetry) {
    // Symmetrical perspective frustum centered at origin looking down -Z
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    Frustum frustum = Frustum::FromViewProjection(proj * view);

    // Left and Right planes must have opposite X normals and identical Y, Z
    const auto& left = frustum.planes[0];
    const auto& right = frustum.planes[1];
    EXPECT_NEAR(left.normal.x, -right.normal.x, 1e-4f);
    EXPECT_NEAR(left.normal.y, right.normal.y, 1e-4f);
    EXPECT_NEAR(left.normal.z, right.normal.z, 1e-4f);

    // Bottom and Top planes must have opposite Y normals
    const auto& bottom = frustum.planes[2];
    const auto& top = frustum.planes[3];
    EXPECT_NEAR(bottom.normal.y, -top.normal.y, 1e-4f);
    EXPECT_NEAR(bottom.normal.x, top.normal.x, 1e-4f);
    EXPECT_NEAR(bottom.normal.z, top.normal.z, 1e-4f);
}

TEST(TypesMathTest, TransformMatrixPropertiesAndInversion) {
    glm::mat4 T = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, -7.0f, 12.0f));
    glm::mat4 R = glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
    glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(2.5f, 0.8f, 1.2f));

    glm::mat4 M = T * R * S;
    glm::mat4 invM = glm::inverse(M);
    glm::mat4 ident = M * invM;

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float expected = (col == row) ? 1.0f : 0.0f;
            EXPECT_NEAR(ident[col][row], expected, 1e-4f);
        }
    }
}




