#include "scene/Scene.hpp"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vector>

using bgl::core::Vertex;
using bgl::scene::Mesh;
using bgl::scene::MeshPrimitive;
using bgl::scene::MorphTarget;
using bgl::scene::Scene;
using bgl::scene::SceneNode;

namespace {

MeshPrimitive MakeSubdividedGrid(int cellsPerAxis) {
    MeshPrimitive prim;
    const int vertsPerRow = cellsPerAxis + 1;
    prim.vertices.resize(static_cast<size_t>(vertsPerRow * vertsPerRow));
    for (int z = 0; z < vertsPerRow; ++z) {
        for (int x = 0; x < vertsPerRow; ++x) {
            const size_t idx = static_cast<size_t>(z * vertsPerRow + x);
            const float fx = static_cast<float>(x) / static_cast<float>(cellsPerAxis);
            const float fz = static_cast<float>(z) / static_cast<float>(cellsPerAxis);
            prim.vertices[idx].position = glm::vec3{fx * 10.0f - 5.0f, std::sin(fx * 3.14f) * 0.25f, fz * 10.0f - 5.0f};
            prim.vertices[idx].normal = glm::vec3{0.0f, 1.0f, 0.0f};
            prim.vertices[idx].uv = glm::vec2{fx, fz};
            prim.boundingBox.Expand(prim.vertices[idx].position);
        }
    }
    prim.indices.reserve(static_cast<size_t>(cellsPerAxis * cellsPerAxis * 6));
    for (int z = 0; z < cellsPerAxis; ++z) {
        for (int x = 0; x < cellsPerAxis; ++x) {
            const uint32_t i0 = static_cast<uint32_t>(z * vertsPerRow + x);
            const uint32_t i1 = i0 + 1u;
            const uint32_t i2 = i0 + static_cast<uint32_t>(vertsPerRow);
            const uint32_t i3 = i2 + 1u;
            prim.indices.insert(prim.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return prim;
}

TEST(SceneMeshTest, SmallPrimitiveRetainsFullGeometryAcrossLodTiers) {
    MeshPrimitive prim;
    prim.vertices.resize(3);
    prim.vertices[0].position = glm::vec3{0.0f, 0.0f, 0.0f};
    prim.vertices[1].position = glm::vec3{1.0f, 0.0f, 0.0f};
    prim.vertices[2].position = glm::vec3{0.0f, 1.0f, 0.0f};
    prim.indices = {0u, 1u, 2u};
    prim.GenerateLODs();

    for (uint32_t lvl = 0; lvl < 4u; ++lvl) {
        EXPECT_EQ(prim.lodIndices[lvl].size(), 3u);
        EXPECT_EQ(prim.GetLodSlice(lvl).indexCount, 3u);
    }
    // Out-of-range level clamps safely to LOD3
    EXPECT_EQ(prim.GetLodSlice(99u).indexCount, 3u);
}

TEST(SceneMeshTest, GenerateLodsProducesStrictlyCoarserValidSlicesOnDenseMesh) {
    MeshPrimitive prim = MakeSubdividedGrid(24); // 24*24*2 = 1152 tris, 3456 indices
    prim.GenerateLODs();

    ASSERT_EQ(prim.lodIndices[0].size(), prim.indices.size());
    EXPECT_FLOAT_EQ(prim.lodLevels[0].errorMetric, 0.0f);

    for (size_t lvl = 1; lvl < 4; ++lvl) {
        EXPECT_GT(prim.lodIndices[lvl].size(), 0u);
        EXPECT_EQ(prim.lodIndices[lvl].size() % 3u, 0u);
        EXPECT_LT(prim.lodIndices[lvl].size(), prim.lodIndices[lvl - 1].size());
        EXPECT_EQ(prim.lodLevels[lvl].indexCount, prim.lodIndices[lvl].size());
        EXPECT_GT(prim.lodLevels[lvl].errorMetric, prim.lodLevels[lvl - 1].errorMetric);

        for (uint32_t idx : prim.lodIndices[lvl]) {
            EXPECT_LT(idx, prim.vertices.size());
        }
    }
}

TEST(SceneMeshTest, GenerateLodsAreaDecimationFallbackHandlesClusterCollapse) {
    // 16 distinct indices pointing into closely grouped points within identical cell quantized coords
    MeshPrimitive prim;
    prim.vertices.resize(40);
    for (size_t i = 0; i < prim.vertices.size(); ++i) {
        const float eps = static_cast<float>(i) * 1e-4f;
        prim.vertices[i].position = glm::vec3{eps, (i % 3 == 0) ? 1.0f : 0.0f, (i % 2 == 0) ? 0.5f : 0.0f};
        prim.vertices[i].normal = glm::vec3{0.0f, 1.0f, 0.0f};
    }
    for (uint32_t i = 0; i + 2 < 39u; i += 3u) {
        prim.indices.insert(prim.indices.end(), {i, i + 1u, i + 2u});
    }
    prim.GenerateLODs();
    EXPECT_GE(prim.lodIndices[1].size(), 12u);
    EXPECT_LT(prim.lodIndices[3].size(), prim.lodIndices[0].size());
}

TEST(SceneMeshTest, ComputeTangentsOrthogonalizesIndexedUnindexedAndDegenerateUvs) {
    // 1. Indexed quad with valid UVs
    {
        MeshPrimitive prim = MakeSubdividedGrid(2);
        prim.ComputeTangents();
        for (const auto& v : prim.vertices) {
            const glm::vec3 t{v.tangent};
            EXPECT_NEAR(glm::length(t), 1.0f, 1e-4f);
            EXPECT_NEAR(glm::dot(t, v.normal), 0.0f, 1e-4f);
            EXPECT_FLOAT_EQ(std::abs(v.tangent.w), 1.0f);
        }
    }

    // 2. Unindexed mesh with degenerate zero UVs falls back safely without NaNs
    {
        MeshPrimitive prim;
        prim.vertices.resize(3);
        prim.vertices[0].position = glm::vec3{0.0f, 0.0f, 0.0f};
        prim.vertices[1].position = glm::vec3{1.0f, 0.0f, 0.0f};
        prim.vertices[2].position = glm::vec3{0.0f, 1.0f, 0.0f};
        for (auto& v : prim.vertices) {
            v.normal = glm::vec3{0.0f, 0.0f, 1.0f};
            v.uv = glm::vec2{0.5f, 0.5f};
        }
        prim.ComputeTangents();
        for (const auto& v : prim.vertices) {
            EXPECT_TRUE(std::isfinite(v.tangent.x) && std::isfinite(v.tangent.y) && std::isfinite(v.tangent.z) &&
                        std::isfinite(v.tangent.w));
            EXPECT_NEAR(glm::length(glm::vec3{v.tangent}), 1.0f, 1e-4f);
        }
    }
}

TEST(SceneMeshTest, ApplyMorphTargetsBlendsDeltasAndUpdatesBoundingBox) {
    MeshPrimitive prim;
    prim.vertices.resize(2);
    prim.vertices[0].position = glm::vec3{0.0f, 0.0f, 0.0f};
    prim.vertices[0].normal = glm::vec3{0.0f, 1.0f, 0.0f};
    prim.vertices[1].position = glm::vec3{1.0f, 0.0f, 0.0f};
    prim.vertices[1].normal = glm::vec3{0.0f, 1.0f, 0.0f};
    prim.baseVertices = prim.vertices;

    MorphTarget mt;
    mt.positionDeltas = {glm::vec3{0.0f, 4.0f, 0.0f}, glm::vec3{2.0f, 0.0f, 0.0f}};
    mt.normalDeltas = {glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}};
    mt.tangentDeltas = {glm::vec3{0.5f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.5f, 0.0f}};
    prim.morphTargets.push_back(mt);

    const std::vector<float> weights{0.5f};
    prim.ApplyMorphTargets(weights);

    EXPECT_TRUE(prim.morphDirty);
    EXPECT_NEAR(prim.vertices[0].position.y, 2.0f, 1e-5f);
    EXPECT_NEAR(prim.vertices[1].position.x, 2.0f, 1e-5f);
    EXPECT_NEAR(glm::length(prim.vertices[0].normal), 1.0f, 1e-5f);
    EXPECT_NEAR(prim.boundingBox.maxBound.y, 2.0f, 1e-5f);
    EXPECT_NEAR(prim.boundingBox.maxBound.x, 2.0f, 1e-5f);
}

TEST(SceneMeshTest, PrimitiveCopyMovePreservesCpuGeometryAndResetsHandles) {
    MeshPrimitive orig = MakeSubdividedGrid(2);
    orig.gpuVertexBufferSize = 1024u;
    orig.wireframeIndexCount = 24u;

    MeshPrimitive copied{orig};
    EXPECT_EQ(copied.vertices.size(), orig.vertices.size());
    EXPECT_EQ(copied.indices.size(), orig.indices.size());
    EXPECT_EQ(copied.gpuVertexBufferSize, 0u);
    EXPECT_EQ(copied.wireframeIndexCount, 0u);

    MeshPrimitive moved{std::move(orig)};
    EXPECT_EQ(moved.gpuVertexBufferSize, 1024u);
    EXPECT_EQ(orig.gpuVertexBufferSize, 0u);
    EXPECT_EQ(orig.wireframeIndexCount, 0u);
}

TEST(SceneMeshTest, SceneHierarchyUpdateInstancingAndLookupWorkCleanly) {
    Scene scene;
    auto mesh = std::make_shared<Mesh>();
    mesh->primitives.push_back(MakeSubdividedGrid(1));

    auto parent = std::make_shared<SceneNode>();
    parent->name = "RootParent";
    parent->translation = glm::vec3{10.0f, 0.0f, 0.0f};
    parent->UpdateLocalTransform();

    auto child = std::make_shared<SceneNode>();
    child->name = "InstancedChild";
    child->mesh = mesh;
    child->instanceTransforms = {
        glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, 0.0f, 0.0f}),
        glm::translate(glm::mat4{1.0f}, glm::vec3{20.0f, 0.0f, 0.0f}),
    };
    parent->children.push_back(child);

    scene.rootNodes.push_back(parent);
    scene.allNodes.push_back(parent);
    scene.allNodes.push_back(child);
    const uint64_t prevVer = scene.transformVersion_;
    scene.Update();

    EXPECT_GT(scene.transformVersion_, prevVer);
    EXPECT_EQ(child->GetPrimitives().size(), 1u);
    EXPECT_FLOAT_EQ(child->worldTransform[3][0], 10.0f);
    // Second instance sits at x = 10 + 20 + grid max(5) = 35
    EXPECT_GE(scene.sceneAABB.maxBound.x, 34.9f);
    EXPECT_LE(scene.sceneAABB.minBound.x, 5.1f);
}

} // namespace
