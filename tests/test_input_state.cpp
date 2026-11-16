#include "bgl/Input.hpp"
#include "engine/InputState.hpp"
#include <gtest/gtest.h>

using namespace bgl;
using bgl::engine::InputState;

TEST(InputState, PressedOnlyInFirstFrame) {
    InputState in;
    in.BeginFrame();
    in.SetKey(Key::W, true);
    EXPECT_TRUE(in.IsKeyDown(Key::W));
    EXPECT_TRUE(in.IsKeyPressed(Key::W));
    in.BeginFrame();
    in.SetKey(Key::W, true);
    EXPECT_TRUE(in.IsKeyDown(Key::W));
    EXPECT_FALSE(in.IsKeyPressed(Key::W));
}

TEST(InputState, ReleasedOnlyInFrameOfRelease) {
    InputState in;
    in.BeginFrame();
    in.SetKey(Key::Space, true);
    in.BeginFrame();
    in.SetKey(Key::Space, false);
    EXPECT_FALSE(in.IsKeyDown(Key::Space));
    EXPECT_TRUE(in.IsKeyReleased(Key::Space));
    in.BeginFrame();
    EXPECT_FALSE(in.IsKeyReleased(Key::Space));
}

TEST(InputState, MouseDeltaAndWheelResetEachFrame) {
    InputState in;
    in.BeginFrame();
    in.AddMouseMotion({10.0f, 20.0f}, {3.0f, -2.0f});
    in.AddMouseMotion({12.0f, 21.0f}, {2.0f, 1.0f});
    in.AddWheel(1.0f);
    EXPECT_FLOAT_EQ(in.MouseDelta().x, 5.0f);
    EXPECT_FLOAT_EQ(in.MouseDelta().y, -1.0f);
    EXPECT_FLOAT_EQ(in.MouseWheel(), 1.0f);
    EXPECT_FLOAT_EQ(in.MousePosition().x, 12.0f);
    in.BeginFrame();
    EXPECT_FLOAT_EQ(in.MouseDelta().x, 0.0f);
    EXPECT_FLOAT_EQ(in.MouseWheel(), 0.0f);
}

TEST(InputState, MouseButtonsPressedHeldAndReleased) {
    InputState in;
    in.BeginFrame();
    in.SetMouseButton(MouseButton::Left, true);
    in.SetMouseButton(MouseButton::Right, true);
    EXPECT_TRUE(in.IsMouseDown(MouseButton::Left));
    EXPECT_TRUE(in.IsMousePressed(MouseButton::Left));
    EXPECT_TRUE(in.IsMousePressed(MouseButton::Right));
    EXPECT_FALSE(in.IsMouseDown(MouseButton::Middle));

    in.BeginFrame();
    in.SetMouseButton(MouseButton::Left, true);
    in.SetMouseButton(MouseButton::Right, false);
    EXPECT_TRUE(in.IsMouseDown(MouseButton::Left));
    EXPECT_FALSE(in.IsMousePressed(MouseButton::Left));
    EXPECT_FALSE(in.IsMouseDown(MouseButton::Right));
    EXPECT_TRUE(in.IsMouseReleased(MouseButton::Right));

    in.BeginFrame();
    EXPECT_FALSE(in.IsMouseReleased(MouseButton::Right));
}

TEST(InputState, SetMousePositionDoesNotAffectDelta) {
    InputState in;
    in.BeginFrame();
    in.SetMousePosition({120.0f, 240.0f});
    EXPECT_FLOAT_EQ(in.MousePosition().x, 120.0f);
    EXPECT_FLOAT_EQ(in.MousePosition().y, 240.0f);
    EXPECT_FLOAT_EQ(in.MouseDelta().x, 0.0f);
    EXPECT_FLOAT_EQ(in.MouseDelta().y, 0.0f);
}

TEST(InputState, OutOfBoundsAndUnknownKeysAreSafe) {
    InputState in;
    in.BeginFrame();
    in.SetKey(Key::Unknown, true);
    in.SetKey(Key::Count, true);
    in.SetMouseButton(MouseButton::Count, true);
    EXPECT_FALSE(in.IsKeyDown(Key::Count));
    EXPECT_FALSE(in.IsKeyPressed(Key::Count));
    EXPECT_FALSE(in.IsKeyReleased(Key::Count));
    EXPECT_FALSE(in.IsMouseDown(MouseButton::Count));
    EXPECT_FALSE(in.IsMousePressed(MouseButton::Count));
    EXPECT_FALSE(in.IsMouseReleased(MouseButton::Count));
}

TEST(InputState, PublicInputForwardsToGlobalState) {
    auto& state = bgl::engine::GetInputState();
    state.BeginFrame();
    state.SetKey(Key::F1, true);
    state.SetMouseButton(MouseButton::Middle, true);
    state.AddMouseMotion({50.0f, 60.0f}, {4.0f, 5.0f});
    state.AddWheel(-2.0f);

    EXPECT_TRUE(Input::IsKeyDown(Key::F1));
    EXPECT_TRUE(Input::IsKeyPressed(Key::F1));
    EXPECT_TRUE(Input::IsMouseDown(MouseButton::Middle));
    EXPECT_TRUE(Input::IsMousePressed(MouseButton::Middle));
    EXPECT_FLOAT_EQ(Input::MousePosition().x, 50.0f);
    EXPECT_FLOAT_EQ(Input::MouseDelta().y, 5.0f);
    EXPECT_FLOAT_EQ(Input::MouseWheel(), -2.0f);

    state.BeginFrame();
    state.SetKey(Key::F1, false);
    state.SetMouseButton(MouseButton::Middle, false);
    EXPECT_TRUE(Input::IsKeyReleased(Key::F1));
    EXPECT_TRUE(Input::IsMouseReleased(MouseButton::Middle));
    state.BeginFrame();
}
