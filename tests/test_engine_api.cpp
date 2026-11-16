#include "bgl/bgl.hpp"
#include <gtest/gtest.h>

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

    child.SetParent(parent);
    child.AddLight({.type = bgl::LightType::Point, .color = {1.0f, 0.5f, 0.25f}, .intensity = 3.0f});

    bgl::Entity found = engine.FindEntity("TestChild");
    EXPECT_TRUE(found.IsValid());
    EXPECT_EQ(found.Name(), "TestChild");
    EXPECT_FALSE(engine.FindEntity("DoesNotExist").IsValid());

    engine.Settings().exposure = 1.5f;
    engine.Update(0.016f);
    engine.BeginGui();
    bgl::gui::Begin("Test");
    bgl::gui::Label("OK");
    bgl::gui::End();
    engine.EndGui();
    engine.Render();

    // Loading a new model replaces the scene and invalidates handles from the previous scene
    ASSERT_TRUE(engine.LoadModel("assets/ToyCar.glb").has_value());
    EXPECT_FALSE(parent.IsValid());
    EXPECT_FALSE(child.IsValid());

    engine.Quit();
    EXPECT_FALSE(engine.IsRunning());
}
