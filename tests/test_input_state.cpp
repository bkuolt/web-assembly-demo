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

TEST(InputState, UnknownKeyIsIgnored) {
    InputState in;
    in.BeginFrame();
    in.SetKey(Key::Unknown, true);
    EXPECT_FALSE(in.IsKeyDown(Key::Unknown) && in.IsKeyPressed(Key::A));
}
