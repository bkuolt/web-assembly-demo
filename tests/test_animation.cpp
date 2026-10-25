/**
 * @file test_animation.cpp
 * @brief Comprehensive unit tests for glTF 2.0 animation system:
 *        - AnimationSampler interpolation (Step, Linear vectors/rotations, CubicSpline Hermite)
 *        - TRS decoupling and hierarchical matrix recalculation
 *        - Morph target delta blending and dirty flag propagation
 *        - Skeletal skinning joint matrix computation
 *        - Scene animation timeline evaluation and loop wrapping
 */

#include "scene/Animation.hpp"
#include "scene/Scene.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

using namespace bgl;
using namespace bgl::scene;

TEST(AnimationTest, SamplerStepInterpolation) {
    AnimationSampler sampler;
    sampler.interpolation = AnimationInterpolation::Step;
    sampler.timestamps = {0.0f, 1.0f, 2.0f};
    sampler.keyframeValues = {0.0f, 0.0f, 0.0f, 10.0f, 20.0f, 30.0f, 100.0f, 200.0f, 300.0f};

    std::array<float, 3> result{};

    // Before first keyframe -> clamps to first
    sampler.Sample(-0.5f, 3, AnimationPath::Translation, result);
    EXPECT_FLOAT_EQ(result[0], 0.0f);
    EXPECT_FLOAT_EQ(result[1], 0.0f);
    EXPECT_FLOAT_EQ(result[2], 0.0f);

    // Midpoint between keyframe 0 and 1 -> Step holds keyframe 0
    sampler.Sample(0.5f, 3, AnimationPath::Translation, result);
    EXPECT_FLOAT_EQ(result[0], 0.0f);
    EXPECT_FLOAT_EQ(result[1], 0.0f);
    EXPECT_FLOAT_EQ(result[2], 0.0f);

    // Exact keyframe 1
    sampler.Sample(1.0f, 3, AnimationPath::Translation, result);
    EXPECT_FLOAT_EQ(result[0], 10.0f);
    EXPECT_FLOAT_EQ(result[1], 20.0f);
    EXPECT_FLOAT_EQ(result[2], 30.0f);

    // Just before keyframe 2
    sampler.Sample(1.99f, 3, AnimationPath::Translation, result);
    EXPECT_FLOAT_EQ(result[0], 10.0f);
    EXPECT_FLOAT_EQ(result[1], 20.0f);
    EXPECT_FLOAT_EQ(result[2], 30.0f);

    // After last keyframe -> clamps to last
    sampler.Sample(3.0f, 3, AnimationPath::Translation, result);
    EXPECT_FLOAT_EQ(result[0], 100.0f);
    EXPECT_FLOAT_EQ(result[1], 200.0f);
    EXPECT_FLOAT_EQ(result[2], 300.0f);
}

TEST(AnimationTest, SamplerLinearVectorInterpolation) {
    AnimationSampler sampler;
    sampler.interpolation = AnimationInterpolation::Linear;
    sampler.timestamps = {0.0f, 2.0f};
    sampler.keyframeValues = {0.0f, 10.0f, 20.0f, 10.0f, 30.0f, 40.0f};

    std::array<float, 3> result{};

    // Midpoint t = 1.0 (alpha = 0.5)
    sampler.Sample(1.0f, 3, AnimationPath::Translation, result);
    EXPECT_NEAR(result[0], 5.0f, 1e-5f);
    EXPECT_NEAR(result[1], 20.0f, 1e-5f);
    EXPECT_NEAR(result[2], 30.0f, 1e-5f);

    // t = 0.5 (alpha = 0.25)
    sampler.Sample(0.5f, 3, AnimationPath::Translation, result);
    EXPECT_NEAR(result[0], 2.5f, 1e-5f);
    EXPECT_NEAR(result[1], 15.0f, 1e-5f);
    EXPECT_NEAR(result[2], 25.0f, 1e-5f);
}

TEST(AnimationTest, SamplerLinearRotationSlerp) {
    AnimationSampler sampler;
    sampler.interpolation = AnimationInterpolation::Linear;
    sampler.timestamps = {0.0f, 1.0f};

    // Rotation from identity (0, 0, 0, 1) to 90 deg around Y axis (0, sin(45 deg), 0, cos(45 deg))
    glm::quat q0 = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // (w, x, y, z)
    glm::quat q1 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // glTF outputs store [x, y, z, w]
    sampler.keyframeValues = {q0.x, q0.y, q0.z, q0.w, q1.x, q1.y, q1.z, q1.w};

    std::array<float, 4> result{};

    // Midpoint t = 0.5 -> expect 45 deg around Y axis
    sampler.Sample(0.5f, 4, AnimationPath::Rotation, result);
    glm::quat interpolatedQ(result[3], result[0], result[1], result[2]); // (w, x, y, z)

    glm::quat expectedQ = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(std::abs(glm::dot(interpolatedQ, expectedQ)), 1.0f, 1e-4f);

    // Ensure output quaternion is normalized
    float len = glm::length(interpolatedQ);
    EXPECT_NEAR(len, 1.0f, 1e-5f);
}

TEST(AnimationTest, SamplerCubicSplineInterpolation) {
    AnimationSampler sampler;
    sampler.interpolation = AnimationInterpolation::CubicSpline;
    sampler.timestamps = {0.0f, 1.0f};

    // glTF cubic spline layout per keyframe:
    // [in-tangent, value, out-tangent]
    // Keyframe 0 (t = 0): in=(0), val=(0), out=(0)
    // Keyframe 1 (t = 1): in=(0), val=(10), out=(0)
    sampler.keyframeValues = {
        0.0f, 0.0f,  0.0f, // Keyframe 0: in, val, out
        0.0f, 10.0f, 0.0f  // Keyframe 1: in, val, out
    };

    std::array<float, 1> result{};

    // At t = 0.5, Hermite basis polynomials:
    // h00(0.5) = 2(0.125) - 3(0.25) + 1 = 0.5
    // h01(0.5) = -2(0.125) + 3(0.25) = 0.5
    // Tangents are 0, so result = 0.5 * 0 + 0.5 * 10 = 5.0
    sampler.Sample(0.5f, 1, AnimationPath::Translation, result);
    EXPECT_NEAR(result[0], 5.0f, 1e-5f);

    // At t = 0.0 -> exact value 0.0
    sampler.Sample(0.0f, 1, AnimationPath::Translation, result);
    EXPECT_NEAR(result[0], 0.0f, 1e-5f);

    // At t = 1.0 -> exact value 10.0
    sampler.Sample(1.0f, 1, AnimationPath::Translation, result);
    EXPECT_NEAR(result[0], 10.0f, 1e-5f);
}

TEST(AnimationTest, SceneNodeTRSDecoupling) {
    SceneNode node;
    node.translation = glm::vec3(2.0f, 3.0f, 4.0f);
    node.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    node.scale = glm::vec3(2.0f, 2.0f, 2.0f);

    node.UpdateLocalTransform();

    // Verify origin transforms to translation
    glm::vec4 origin(0.0f, 0.0f, 0.0f, 1.0f);
    glm::vec4 transformedOrigin = node.localTransform * origin;
    EXPECT_NEAR(transformedOrigin.x, 2.0f, 1e-5f);
    EXPECT_NEAR(transformedOrigin.y, 3.0f, 1e-5f);
    EXPECT_NEAR(transformedOrigin.z, 4.0f, 1e-5f);

    // Verify unit X (1, 0, 0, 0) rotated by 90 deg around Z and scaled by 2 becomes (0, 2, 0, 0)
    glm::vec4 vecX(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec4 transformedX = node.localTransform * vecX;
    EXPECT_NEAR(transformedX.x, 0.0f, 1e-5f);
    EXPECT_NEAR(transformedX.y, 2.0f, 1e-5f);
    EXPECT_NEAR(transformedX.z, 0.0f, 1e-5f);
}

TEST(AnimationTest, HierarchicalTransformPropagation) {
    auto parent = std::make_shared<SceneNode>();
    parent->translation = glm::vec3(10.0f, 0.0f, 0.0f);
    parent->UpdateLocalTransform();

    auto child = std::make_shared<SceneNode>();
    child->translation = glm::vec3(0.0f, 5.0f, 0.0f);
    child->UpdateLocalTransform();
    parent->children.push_back(child);

    parent->UpdateTransforms(glm::mat4(1.0f));

    // Parent world position should be (10, 0, 0)
    glm::vec4 pPos = parent->worldTransform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(pPos.x, 10.0f, 1e-5f);
    EXPECT_NEAR(pPos.y, 0.0f, 1e-5f);

    // Child world position should be (10, 5, 0)
    glm::vec4 cPos = child->worldTransform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(cPos.x, 10.0f, 1e-5f);
    EXPECT_NEAR(cPos.y, 5.0f, 1e-5f);
    EXPECT_NEAR(cPos.z, 0.0f, 1e-5f);
}

TEST(AnimationTest, MorphTargetBlending) {
    MeshPrimitive prim;

    core::Vertex v0;
    v0.position = glm::vec3(1.0f, 2.0f, 3.0f);
    v0.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    prim.vertices = {v0};
    prim.baseVertices = {v0};

    // Target 0: moves position by (+1, 0, 0)
    MorphTarget t0;
    t0.positionDeltas = {glm::vec3(1.0f, 0.0f, 0.0f)};
    t0.normalDeltas = {glm::vec3(0.0f, 0.0f, 0.0f)};

    // Target 1: moves position by (0, +2, 0)
    MorphTarget t1;
    t1.positionDeltas = {glm::vec3(0.0f, 2.0f, 0.0f)};
    t1.normalDeltas = {glm::vec3(0.0f, 0.0f, 0.0f)};

    prim.morphTargets = {t0, t1};

    // Apply weights: [0.5, 0.25]
    std::vector<float> weights = {0.5f, 0.25f};
    prim.ApplyMorphTargets(weights);

    EXPECT_TRUE(prim.morphDirty);
    // Expected: pos = (1 + 0.5 * 1, 2 + 0.25 * 2, 3) = (1.5, 2.5, 3.0)
    EXPECT_NEAR(prim.vertices[0].position.x, 1.5f, 1e-5f);
    EXPECT_NEAR(prim.vertices[0].position.y, 2.5f, 1e-5f);
    EXPECT_NEAR(prim.vertices[0].position.z, 3.0f, 1e-5f);
}

TEST(AnimationTest, SceneTimelinePlaybackAndLooping) {
    Scene scene;

    auto node = std::make_shared<SceneNode>();
    node->translation = glm::vec3(0.0f);
    node->UpdateLocalTransform();

    scene.rootNodes.push_back(node);
    scene.allNodes.push_back(node);

    Animation anim;
    anim.name = "MoveX";
    anim.duration = 2.0f;

    AnimationSampler sampler;
    sampler.interpolation = AnimationInterpolation::Linear;
    sampler.timestamps = {0.0f, 2.0f};
    sampler.keyframeValues = {0.0f, 0.0f, 0.0f, 20.0f, 0.0f, 0.0f};
    anim.samplers.push_back(sampler);

    AnimationChannel channel;
    channel.samplerIndex = 0;
    channel.targetNodeIndex = 0;
    channel.path = AnimationPath::Translation;
    anim.channels.push_back(channel);

    scene.animations.push_back(anim);

    scene.SetAnimation(0);
    EXPECT_EQ(scene.activeAnimationIndex, 0);
    EXPECT_FLOAT_EQ(scene.animationTime, 0.0f);
    EXPECT_TRUE(scene.animationPlaying);

    // Advance 0.5 seconds (25% progress)
    scene.UpdateAnimation(0.5f);
    EXPECT_NEAR(scene.animationTime, 0.5f, 1e-5f);
    EXPECT_NEAR(node->translation.x, 5.0f, 1e-4f);

    // Advance 1.0 more second (total 1.5s, 75% progress)
    scene.UpdateAnimation(1.0f);
    EXPECT_NEAR(scene.animationTime, 1.5f, 1e-5f);
    EXPECT_NEAR(node->translation.x, 15.0f, 1e-4f);

    // Advance 1.0 second (total 2.5s -> wraps to 0.5s due to loop)
    scene.UpdateAnimation(1.0f);
    EXPECT_NEAR(scene.animationTime, 0.5f, 1e-5f);
    EXPECT_NEAR(node->translation.x, 5.0f, 1e-4f);
}

TEST(AnimationTest, SkinJointMatrixComputation) {
    Skin skin;
    skin.joints = {0, 1};

    // Joint 0: bind pose at origin, identity IBM
    glm::mat4 ibm0 = glm::mat4(1.0f);

    // Joint 1: bind pose translated by (0, 2, 0) -> IBM translates by (0, -2, 0)
    glm::mat4 ibm1 = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -2.0f, 0.0f));

    skin.inverseBindMatrices = {ibm0, ibm1};

    // Node 0: joint 0 moved to (5, 0, 0)
    auto node0 = std::make_shared<SceneNode>();
    node0->worldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));

    // Node 1: joint 1 moved to (5, 3, 0)
    auto node1 = std::make_shared<SceneNode>();
    node1->worldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 3.0f, 0.0f));

    std::vector<std::shared_ptr<SceneNode>> allNodes = {node0, node1};

    // Mesh node at origin
    glm::mat4 meshWorld = glm::mat4(1.0f);
    std::array<glm::mat4, 4> computedJoints{};
    uint32_t count = skin.ComputeJointMatrices(meshWorld, allNodes, computedJoints);
    EXPECT_EQ(count, 2u);

    // Expected joint matrices: invMeshWorld * jointWorld * ibm
    const glm::mat4& jMat0 = computedJoints[0];
    const glm::mat4& jMat1 = computedJoints[1];

    // Test a vertex bound to joint 0 with weight 1.0:
    glm::vec4 pos0(0.0f, 0.0f, 0.0f, 1.0f);
    glm::vec4 skinnedPos0 = jMat0 * pos0;
    EXPECT_NEAR(skinnedPos0.x, 5.0f, 1e-5f);
    EXPECT_NEAR(skinnedPos0.y, 0.0f, 1e-5f);

    // Test a vertex bound to joint 1 at its local position (0, 2, 0) with weight 1.0:
    // IBM transforms (0, 2, 0) to (0, 0, 0), then joint1World transforms it to (5, 3, 0)
    glm::vec4 pos1(0.0f, 2.0f, 0.0f, 1.0f);
    glm::vec4 skinnedPos1 = jMat1 * pos1;
    EXPECT_NEAR(skinnedPos1.x, 5.0f, 1e-5f);
    EXPECT_NEAR(skinnedPos1.y, 3.0f, 1e-5f);
    EXPECT_NEAR(skinnedPos1.z, 0.0f, 1e-5f);
}
