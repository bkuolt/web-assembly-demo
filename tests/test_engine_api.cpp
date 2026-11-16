#include "bgl/bgl.hpp"
#include <array>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <type_traits>
#include <vector>

static_assert(std::is_same_v<bgl::Camera, bgl::core::Camera>);
static_assert(std::is_same_v<bgl::CameraMode, bgl::core::CameraMode>);

TEST(GuiPublicApi, CallsOutsideFrameAreSafeNoOps) {
    bool checked = true;
    float value = 1.0f;
    bgl::gui::Begin("NoContext");
    bgl::gui::Label("Hello");
    EXPECT_FALSE(bgl::gui::Button("Click"));
    EXPECT_FALSE(bgl::gui::Checkbox("Toggle", checked));
    EXPECT_FALSE(bgl::gui::Slider("Slider", value, 0.0f, 2.0f));
    bgl::gui::End();
    EXPECT_TRUE(checked);
    EXPECT_FLOAT_EQ(value, 1.0f);
}

TEST(EnginePublicApi, CreateEntityFindAndSceneReloadInvalidatesOldHandles) {
    bgl::EngineConfig cfg{};
    cfg.width = 320;
    cfg.height = 240;
    cfg.title = "bgl_api_test";
    auto engineRes = bgl::Engine::Create(cfg);
    if (!engineRes) {
        GTEST_SKIP() << "Skipping live GPU Engine test: " << engineRes.error();
    }

    auto& engine = **engineRes;
    EXPECT_TRUE(engine.IsRunning());
    EXPECT_EQ(engine.Width(), 320u);
    EXPECT_EQ(engine.Height(), 240u);

    bgl::Entity parent = engine.CreateEntity("TestParent");
    bgl::Entity child = engine.CreateEntity("TestChild");
    ASSERT_TRUE(parent.IsValid());
    ASSERT_TRUE(child.IsValid());
    EXPECT_EQ(parent.Name(), "TestParent");

    parent.SetPosition({1.0f, 2.0f, 3.0f});
    EXPECT_FLOAT_EQ(parent.Position().x, 1.0f);
    EXPECT_FLOAT_EQ(parent.Position().y, 2.0f);
    EXPECT_FLOAT_EQ(parent.Position().z, 3.0f);

    // Zero-length quaternion must safely fall back to finite identity rotation
    parent.SetRotation(glm::quat{0.0f, 0.0f, 0.0f, 0.0f});
    const glm::quat safeRot = parent.Rotation();
    EXPECT_TRUE(std::isfinite(safeRot.w) && std::isfinite(safeRot.x) && std::isfinite(safeRot.y) &&
                std::isfinite(safeRot.z));
    EXPECT_FLOAT_EQ(safeRot.w, 1.0f);

    child.SetParent(parent);
    child.AddLight({.type = bgl::LightType::Point, .color = {1.0f, 0.5f, 0.25f}, .intensity = 3.0f});
    child.AddLight({.type = bgl::LightType::Point, .color = {0.2f, 0.9f, 0.4f}, .intensity = 7.0f});

    bgl::Entity found = engine.FindEntity("TestChild");
    EXPECT_TRUE(found.IsValid());
    EXPECT_TRUE(static_cast<bool>(found));
    EXPECT_EQ(found, child);
    EXPECT_NE(found, parent);
    EXPECT_EQ(found.Name(), "TestChild");
    EXPECT_FALSE(engine.FindEntity("DoesNotExist").IsValid());

    engine.Settings().exposure = 1.5f;
    const bgl::Engine& constEngine = engine;
    EXPECT_FLOAT_EQ(constEngine.Settings().exposure, 1.5f);
    EXPECT_EQ(constEngine.GetCamera().GetMode(), bgl::CameraMode::Orbit);

    engine.Update(0.016f);
    engine.BeginGui();
    bgl::gui::Begin("Test");
    bgl::gui::Label("OK");
    bgl::gui::End();
    engine.EndGui();
    engine.Render();

    // Post-Update camera/settings mutation must sync cleanly on Render() in the same frame
    engine.GetCamera().Orbit(0.25f, 0.1f);
    engine.Settings().exposure = 1.85f;
    engine.Render();

    // Empty byte buffer load returns an explicit Result error without replacing the scene
    const std::array<uint8_t, 0> emptyBytes{};
    EXPECT_FALSE(engine.LoadModel(emptyBytes, "empty.glb").has_value());
    EXPECT_TRUE(parent.IsValid());

    // Loading a new model from memory replaces the scene and invalidates handles from the previous scene
    std::ifstream file("assets/ToyCar.glb", std::ios::binary | std::ios::ate);
    ASSERT_TRUE(file.is_open());
    const auto fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> glbBytes(fileSize);
    ASSERT_TRUE(file.read(reinterpret_cast<char*>(glbBytes.data()), static_cast<std::streamsize>(fileSize)));

    ASSERT_TRUE(engine.LoadModel(glbBytes, "ToyCar.glb").has_value());
    EXPECT_FALSE(parent.IsValid());
    EXPECT_FALSE(child.IsValid());
    EXPECT_EQ(parent, bgl::Entity{});

    // Cross-scene / expired parent handle must be safely ignored instead of unparenting a new-scene entity
    bgl::Entity newParent = engine.CreateEntity("NewSceneParent");
    bgl::Entity newChild = engine.CreateEntity("NewSceneChild");
    newParent.SetPosition({5.0f, 0.0f, 0.0f});
    newChild.SetParent(newParent);
    newChild.SetParent(parent); // `parent` belongs to expired prior scene

    engine.Quit();
    EXPECT_FALSE(engine.IsRunning());
}
