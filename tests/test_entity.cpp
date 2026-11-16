#include "bgl/Entity.hpp"
#include "engine/EntityOps.hpp"
#include <gtest/gtest.h>

using bgl::engine::AddNode;
using bgl::engine::AttachLight;
using bgl::engine::Reparent;

TEST(EntityOps, AddNodeAppendsToAllAndRootNodes) {
    bgl::scene::Scene scene;
    auto a = AddNode(scene, "A");
    auto b = AddNode(scene, "B");
    ASSERT_EQ(scene.allNodes.size(), 2u);
    ASSERT_EQ(scene.rootNodes.size(), 2u);
    EXPECT_EQ(a->nodeIndex, 0);
    EXPECT_EQ(b->nodeIndex, 1);
    EXPECT_EQ(b->name, "B");
}

TEST(EntityOps, ReparentMovesChildAndPropagatesWorldTransform) {
    bgl::scene::Scene scene;
    auto parent = AddNode(scene, "Parent");
    auto child = AddNode(scene, "Child");
    parent->translation = glm::vec3{10.0f, 0.0f, 0.0f};
    parent->UpdateLocalTransform();
    child->translation = glm::vec3{0.0f, 2.0f, 0.0f};
    child->UpdateLocalTransform();

    Reparent(scene, *child, parent.get());
    EXPECT_EQ(scene.rootNodes.size(), 1u);
    ASSERT_EQ(parent->children.size(), 1u);
    EXPECT_EQ(parent->children[0].get(), child.get());
    EXPECT_FLOAT_EQ(child->worldTransform[3][0], 10.0f);
    EXPECT_FLOAT_EQ(child->worldTransform[3][1], 2.0f);

    Reparent(scene, *child, nullptr);
    EXPECT_EQ(scene.rootNodes.size(), 2u);
    EXPECT_TRUE(parent->children.empty());
    EXPECT_FLOAT_EQ(child->worldTransform[3][0], 0.0f);
    EXPECT_FLOAT_EQ(child->worldTransform[3][1], 2.0f);
}

TEST(EntityOps, ReparentPreventsSelfAndDescendantCycles) {
    bgl::scene::Scene scene;
    auto parent = AddNode(scene, "Parent");
    auto child = AddNode(scene, "Child");
    Reparent(scene, *child, parent.get());

    // Self-reparenting and reparenting under own descendant must be ignored
    Reparent(scene, *parent, parent.get());
    Reparent(scene, *parent, child.get());

    ASSERT_EQ(scene.rootNodes.size(), 1u);
    EXPECT_EQ(scene.rootNodes[0].get(), parent.get());
    ASSERT_EQ(parent->children.size(), 1u);
    EXPECT_EQ(parent->children[0].get(), child.get());
    EXPECT_TRUE(child->children.empty());
}

TEST(EntityOps, AttachLightSetsValidLightIndex) {
    bgl::scene::Scene scene;
    auto node = AddNode(scene, "Lamp");
    bgl::scene::PunctualLight light{};
    light.type = bgl::scene::LightType::Point;
    light.color = glm::vec3{1.0f, 0.2f, 0.1f};
    light.intensity = 5.0f;

    AttachLight(scene, *node, light);
    ASSERT_EQ(scene.lights.size(), 1u);
    EXPECT_EQ(node->lightIndex, 0);
    EXPECT_FLOAT_EQ(scene.lights[0].intensity, 5.0f);
}

TEST(EntityHandle, DefaultInvalidHandleIsSafe) {
    bgl::Entity empty;
    EXPECT_FALSE(empty.IsValid());
    EXPECT_TRUE(empty.Name().empty());
    EXPECT_EQ(empty.Position(), glm::vec3(0.0f));
    EXPECT_EQ(empty.Scale(), glm::vec3(1.0f));
    EXPECT_FLOAT_EQ(empty.Rotation().w, 1.0f);

    // Mutating an invalid handle must be a safe no-op
    empty.SetPosition({1.0f, 2.0f, 3.0f});
    empty.SetRotationEuler({0.0f, 90.0f, 0.0f});
    empty.SetScale({2.0f, 2.0f, 2.0f});
    empty.SetParent(empty);
    empty.AddLight({});
    EXPECT_FALSE(empty.IsValid());
}
