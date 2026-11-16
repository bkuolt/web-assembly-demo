/**
 * @file test_sprites.cpp
 * @brief Unit tests for 2D Sprites, AnimatedSprites, SpriteClips, Billboards, SpriteRenderer, and bgl::Engine APIs.
 */
#include "bgl/bgl.hpp"
#include "core/Sprite.hpp"
#include "gpu/SpriteRenderer.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <numbers>
#include <vector>

namespace {

constexpr std::array<uint8_t, 70> kTinyRedPng{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF,
    0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

} // namespace

TEST(SpriteTextureTest, FactoriesValidateInputsAndGenerateUniqueTextures) {
    const std::array<uint8_t, 4> smallBuf{255, 0, 0, 255};
    EXPECT_EQ(bgl::SpriteTexture::FromRGBA8(smallBuf, 0, 1), nullptr);
    EXPECT_EQ(bgl::SpriteTexture::FromRGBA8(smallBuf, 2, 2), nullptr);

    auto solid = bgl::SpriteTexture::CreateSolid({0.2f, 0.4f, 0.8f, 1.0f});
    ASSERT_NE(solid, nullptr);
    EXPECT_TRUE(solid->IsValid());
    EXPECT_EQ(solid->width, 1u);
    EXPECT_EQ(solid->height, 1u);

    auto glow = bgl::SpriteTexture::CreateRadialGlow(32);
    ASSERT_NE(glow, nullptr);
    EXPECT_TRUE(glow->IsValid());
    EXPECT_NE(glow->id, solid->id);
    const size_t centerIdx = (16u * 32u + 16u) * 4u;
    EXPECT_GT(glow->rgbaPixels[centerIdx + 3u], 220u);
    EXPECT_EQ(glow->rgbaPixels[3], 0u);

    auto ring = bgl::SpriteTexture::CreateRing(32, 0.55f, 0.85f);
    ASSERT_NE(ring, nullptr);
    EXPECT_EQ(ring->rgbaPixels[centerIdx + 3u], 0u);

    auto checker = bgl::SpriteTexture::CreateChecker(16, 4);
    ASSERT_NE(checker, nullptr);
    EXPECT_TRUE(checker->nearestFilter);
}

TEST(SpriteClipAndAnimatorTest, GridSlicingAndPlaybackModesOperateCleanly) {
    auto emptyClip = bgl::SpriteClip::FromGrid("invalid", 0, 4);
    EXPECT_TRUE(emptyClip.frames.empty());

    auto outOfRange = bgl::SpriteClip::FromGrid("oob", 2, 2, 10, 2);
    EXPECT_TRUE(outOfRange.frames.empty());

    auto gridClip = bgl::SpriteClip::FromGrid("walk", 4, 2, 1, 3, 10.0f, bgl::SpritePlayback::Loop);
    ASSERT_EQ(gridClip.frames.size(), 3u);
    EXPECT_NEAR(gridClip.TotalDuration(), 0.3f, 1e-4f);
    EXPECT_FLOAT_EQ(gridClip.frames[0].uvMin.x, 0.25f);
    EXPECT_FLOAT_EQ(gridClip.frames[0].uvMax.x, 0.50f);
    EXPECT_FLOAT_EQ(gridClip.frames[0].uvMin.y, 0.0f);
    EXPECT_FLOAT_EQ(gridClip.frames[0].uvMax.y, 0.5f);

    bgl::SpriteClip customClip;
    customClip.name = "once";
    customClip.mode = bgl::SpritePlayback::Once;
    customClip.AddPixelFrame(16, 8, 32, 16, 64, 32, 0.2f);
    customClip.AddPixelFrame(0, 0, 16, 8, 64, 32, 0.2f);
    ASSERT_EQ(customClip.frames.size(), 2u);
    EXPECT_FLOAT_EQ(customClip.frames[0].uvMin.x, 0.25f);
    EXPECT_FLOAT_EQ(customClip.frames[0].uvMax.x, 0.75f);
    EXPECT_FLOAT_EQ(customClip.frames[0].uvMin.y, 0.25f);
    EXPECT_FLOAT_EQ(customClip.frames[0].uvMax.y, 0.75f);

    // Loop mode
    bgl::SpriteAnimator anim;
    anim.AddClip(gridClip);
    anim.AddClip(customClip);
    anim.AddClip(bgl::SpriteClip::FromGrid("ping", 3, 1, 0, 3, 10.0f, bgl::SpritePlayback::PingPong));
    EXPECT_FALSE(anim.Play("missing"));

    ASSERT_TRUE(anim.Play("walk"));
    EXPECT_EQ(anim.ActiveClipName(), "walk");
    EXPECT_EQ(anim.CurrentFrameIndex(), 0u);
    EXPECT_TRUE(anim.Update(0.11f));
    EXPECT_EQ(anim.CurrentFrameIndex(), 1u);
    EXPECT_TRUE(anim.Update(0.10f));
    EXPECT_EQ(anim.CurrentFrameIndex(), 2u);
    EXPECT_TRUE(anim.Update(0.10f));
    EXPECT_EQ(anim.CurrentFrameIndex(), 0u);

    // Pause / Resume / Speed
    anim.Pause();
    EXPECT_FALSE(anim.IsPlaying());
    EXPECT_FALSE(anim.Update(0.5f));
    EXPECT_EQ(anim.CurrentFrameIndex(), 0u);
    anim.Resume();
    anim.SetSpeed(2.0f);
    EXPECT_TRUE(anim.Update(0.06f)); // 0.12 scaled
    EXPECT_EQ(anim.CurrentFrameIndex(), 1u);
    anim.SetSpeed(1.0f);

    // Once mode freezes at final frame
    ASSERT_TRUE(anim.Play("once"));
    EXPECT_TRUE(anim.Update(0.25f));
    EXPECT_EQ(anim.CurrentFrameIndex(), 1u);
    EXPECT_FALSE(anim.IsFinished());
    EXPECT_TRUE(anim.Update(0.25f));
    EXPECT_EQ(anim.CurrentFrameIndex(), 1u);
    EXPECT_TRUE(anim.IsFinished());
    EXPECT_FALSE(anim.IsPlaying());

    // PingPong: 0 -> 1 -> 2 -> 1 -> 0 -> 1
    ASSERT_TRUE(anim.Play("ping"));
    anim.Update(0.11f);
    EXPECT_EQ(anim.CurrentFrameIndex(), 1u);
    anim.Update(0.10f);
    EXPECT_EQ(anim.CurrentFrameIndex(), 2u);
    anim.Update(0.10f);
    EXPECT_EQ(anim.CurrentFrameIndex(), 1u);
    anim.Update(0.10f);
    EXPECT_EQ(anim.CurrentFrameIndex(), 0u);
    anim.Update(0.10f);
    EXPECT_EQ(anim.CurrentFrameIndex(), 1u);
    anim.Stop();
    EXPECT_EQ(anim.CurrentFrameIndex(), 0u);
    EXPECT_FALSE(anim.IsPlaying());
}

TEST(AnimatedSpriteAndBillboardTest, PropagatesAnimatedAndFlippedUVs) {
    auto tex = bgl::SpriteTexture::CreateChecker(64, 8);
    bgl::AnimatedSprite sp;
    sp.texture = tex;
    sp.AddClip(bgl::SpriteClip::FromGrid("run", 4, 1, 0, 4, 10.0f));
    ASSERT_TRUE(sp.Play("run"));
    EXPECT_FLOAT_EQ(sp.uvMin.x, 0.0f);
    EXPECT_FLOAT_EQ(sp.uvMax.x, 0.25f);

    EXPECT_TRUE(sp.Update(0.11f));
    EXPECT_FLOAT_EQ(sp.uvMin.x, 0.25f);
    EXPECT_FLOAT_EQ(sp.uvMax.x, 0.50f);

    sp.flipX = true;
    sp.flipY = true;
    glm::vec2 effMin{};
    glm::vec2 effMax{};
    sp.ComputeEffectiveUVs(effMin, effMax);
    EXPECT_FLOAT_EQ(effMin.x, 0.50f);
    EXPECT_FLOAT_EQ(effMax.x, 0.25f);
    EXPECT_FLOAT_EQ(effMin.y, 1.0f);
    EXPECT_FLOAT_EQ(effMax.y, 0.0f);

    bgl::AnimatedBillboard bb;
    bb.texture = tex;
    bb.SetPixelRegion(32, 16, 16, 32);
    EXPECT_FLOAT_EQ(bb.uvMin.x, 0.5f);
    EXPECT_FLOAT_EQ(bb.uvMin.y, 0.25f);
    EXPECT_FLOAT_EQ(bb.uvMax.x, 0.75f);
    EXPECT_FLOAT_EQ(bb.uvMax.y, 0.75f);

    bb.AddClip(bgl::SpriteClip::FromGrid("spark", 2, 2, 0, 4, 10.0f));
    ASSERT_TRUE(bb.Play("spark"));
    EXPECT_TRUE(bb.Update(0.21f)); // frame 2 -> col 0, row 1
    EXPECT_EQ(bb.CurrentFrameIndex(), 2u);
    EXPECT_FLOAT_EQ(bb.uvMin.x, 0.0f);
    EXPECT_FLOAT_EQ(bb.uvMin.y, 0.5f);
}

TEST(BillboardBasisTest, OrientationsAndRollPreserveOrthonormality) {
    const glm::vec3 camPos{3.0f, 5.0f, 4.0f};
    const glm::mat4 view = glm::lookAt(camPos, glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});

    bgl::Billboard bb;
    bb.position = {1.0f, 0.5f, -2.0f};
    glm::vec3 right{};
    glm::vec3 up{};

    // Spherical
    bb.mode = bgl::BillboardMode::Spherical;
    bb.ComputeBasis(camPos, view, right, up);
    EXPECT_NEAR(glm::length(right), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(up), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(right, up), 0.0f, 1e-4f);
    const glm::vec3 toCam = glm::normalize(camPos - bb.position);
    EXPECT_NEAR(glm::dot(right, toCam), 0.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(up, toCam), 0.0f, 1e-4f);

    // CylindricalY locks vertical axis strictly to +Y
    bb.mode = bgl::BillboardMode::CylindricalY;
    bb.ComputeBasis(camPos, view, right, up);
    EXPECT_NEAR(up.x, 0.0f, 1e-4f);
    EXPECT_NEAR(up.y, 1.0f, 1e-4f);
    EXPECT_NEAR(up.z, 0.0f, 1e-4f);
    EXPECT_NEAR(right.y, 0.0f, 1e-4f);
    EXPECT_NEAR(glm::length(right), 1.0f, 1e-4f);

    // ScreenAligned matches camera Right and Up axes
    bb.mode = bgl::BillboardMode::ScreenAligned;
    bb.ComputeBasis(camPos, view, right, up);
    const glm::vec3 camRight{view[0][0], view[1][0], view[2][0]};
    const glm::vec3 camUp{view[0][1], view[1][1], view[2][1]};
    EXPECT_NEAR(glm::distance(right, camRight), 0.0f, 1e-4f);
    EXPECT_NEAR(glm::distance(up, camUp), 0.0f, 1e-4f);

    // Roll 90 degrees rotates Right -> Up and Up -> -Right cleanly
    bb.rollRad = std::numbers::pi_v<float> * 0.5f;
    glm::vec3 rolledRight{};
    glm::vec3 rolledUp{};
    bb.ComputeBasis(camPos, view, rolledRight, rolledUp);
    EXPECT_NEAR(glm::distance(rolledRight, camUp), 0.0f, 1e-4f);
    EXPECT_NEAR(glm::distance(rolledUp, -camRight), 0.0f, 1e-4f);

    // Degenerate camera == billboard position produces finite basis
    bb.rollRad = 0.0f;
    bb.mode = bgl::BillboardMode::Spherical;
    bb.ComputeBasis(bb.position, view, right, up);
    EXPECT_TRUE(std::isfinite(right.x) && std::isfinite(up.y));
    EXPECT_NEAR(glm::length(right), 1.0f, 1e-4f);
}

TEST(SpriteRendererCpuTest, QuadGeometryBatchSubmissionAndLifecycleSafety) {
    bgl::Sprite sp;
    sp.position = {100.0f, 200.0f};
    sp.size = {40.0f, 20.0f};
    sp.anchor = {0.0f, 0.0f};
    std::vector<bgl::gpu::SpriteVertex> verts;
    bgl::gpu::SpriteRenderer::BuildSpriteQuad(sp, verts);
    ASSERT_EQ(verts.size(), 4u);
    EXPECT_FLOAT_EQ(verts[0].pos.x, 100.0f);
    EXPECT_FLOAT_EQ(verts[0].pos.y, 200.0f);
    EXPECT_FLOAT_EQ(verts[2].pos.x, 140.0f);
    EXPECT_FLOAT_EQ(verts[2].pos.y, 220.0f);
    EXPECT_FLOAT_EQ(verts[0].uv.y, 0.0f);
    EXPECT_FLOAT_EQ(verts[2].uv.y, 1.0f);

    // Bottom-center anchored billboard stands cleanly on position.y .. position.y + size.y
    bgl::Billboard bb;
    bb.position = {0.0f, 2.0f, 0.0f};
    bb.size = {2.0f, 4.0f};
    bb.anchor = {0.5f, 1.0f};
    bb.mode = bgl::BillboardMode::CylindricalY;
    const glm::mat4 view =
        glm::lookAt(glm::vec3{0.0f, 2.0f, 5.0f}, glm::vec3{0.0f, 2.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
    verts.clear();
    bgl::gpu::SpriteRenderer::BuildBillboardQuad(bb, {0.0f, 2.0f, 5.0f}, view, verts);
    ASSERT_EQ(verts.size(), 4u);
    EXPECT_NEAR(verts[0].pos.y, 6.0f, 1e-4f);
    EXPECT_NEAR(verts[2].pos.y, 2.0f, 1e-4f);

    bgl::gpu::SpriteRenderer renderer;
    EXPECT_FALSE(renderer.IsInitialized());

    bgl::Sprite hiddenSp = sp;
    hiddenSp.visible = false;
    bgl::Sprite transparentSp = sp;
    transparentSp.color.a = 0.0f;
    renderer.SubmitSprite(hiddenSp);
    renderer.SubmitSprite(transparentSp);
    renderer.SubmitSprite(sp);
    EXPECT_EQ(renderer.GetQueuedSpriteCount(), 1u);

    bgl::Billboard zeroBb = bb;
    zeroBb.size = {0.0f, 1.0f};
    renderer.SubmitBillboard(zeroBb);
    renderer.SubmitBillboard(bb);
    EXPECT_EQ(renderer.GetQueuedBillboardCount(), 1u);

    // Safe no-ops without GPU initialization
    renderer.Render3D(nullptr, glm::mat4{1.0f}, {0, 0, 5}, view);
    renderer.Render2D(nullptr, 800, 600);
    renderer.ResetFrame();
    EXPECT_EQ(renderer.GetQueuedSpriteCount(), 0u);
    EXPECT_EQ(renderer.GetQueuedBillboardCount(), 0u);
    renderer.Shutdown();
}

TEST(EnginePublicSpriteApiTest, LoadTextureAndRenderSpritesAndBillboards) {
    bgl::EngineConfig cfg{};
    cfg.width = 320;
    cfg.height = 240;
    cfg.title = "bgl_sprite_test";
    auto engineRes = bgl::Engine::Create(cfg);
    if (!engineRes) {
        GTEST_SKIP() << "Skipping live GPU Sprite test: " << engineRes.error();
    }

    auto& engine = **engineRes;
    const std::array<uint8_t, 0> emptyBytes{};
    EXPECT_FALSE(engine.LoadSpriteTexture(emptyBytes, "empty.png").has_value());
    EXPECT_FALSE(engine.LoadSpriteTexture("").has_value());
    EXPECT_FALSE(engine.LoadSpriteTexture("nonexistent_sprite_xyz.png").has_value());

    auto texRes = engine.LoadSpriteTexture(kTinyRedPng, "red.png", true);
    ASSERT_TRUE(texRes.has_value()) << texRes.error();
    auto tex = *texRes;
    ASSERT_NE(tex, nullptr);
    EXPECT_EQ(tex->width, 1u);
    EXPECT_EQ(tex->height, 1u);
    EXPECT_TRUE(tex->nearestFilter);
    EXPECT_EQ(tex->rgbaPixels[0], 255u);

    bgl::AnimatedSprite hud;
    hud.texture = tex;
    hud.position = {64.0f, 64.0f};
    hud.size = {32.0f, 32.0f};
    hud.AddClip(bgl::SpriteClip::FromGrid("idle", 1, 1, 0, 1, 12.0f));
    hud.Play("idle");
    hud.Update(0.016f);

    bgl::AnimatedBillboard marker;
    marker.texture = bgl::SpriteTexture::CreateRadialGlow(32);
    marker.position = {0.0f, 1.0f, 0.0f};
    marker.size = {1.0f, 1.0f};
    marker.blend = bgl::SpriteBlend::Additive;
    marker.color = {1.5f, 1.0f, 0.5f, 1.0f};

    bgl::Billboard solidQuad;
    solidQuad.position = {1.0f, 0.5f, -1.0f};
    solidQuad.mode = bgl::BillboardMode::CylindricalY;

    engine.Update(0.016f);
    engine.DrawSprite(hud);
    engine.DrawBillboard(marker);
    engine.DrawBillboard(solidQuad);
    engine.Render();

    // Subsequent empty frame must not retain stale queued sprites/billboards
    engine.Update(0.016f);
    engine.Render();
    engine.Quit();
}
