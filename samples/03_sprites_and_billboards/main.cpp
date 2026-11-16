// Sample 03: 2D Animated Sprites + 3D HDR Camera-Facing Billboards around a glTF model.
// Run from the repository root:  ./build/Release/sample_03_sprites_and_billboards
#include <array>
#include <bgl/bgl.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// Procedurally builds a crisp 4x1 animated star/diamond sprite sheet in RGBA8.
std::shared_ptr<bgl::SpriteTexture> CreateSparkSheet(uint32_t cellSize) {
    constexpr uint32_t kCols = 4;
    const uint32_t w = cellSize * kCols;
    const uint32_t h = cellSize;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u, 0u);
    const float half = static_cast<float>(cellSize - 1u) * 0.5f;

    for (uint32_t frame = 0; frame < kCols; ++frame) {
        const float radius = (0.35f + 0.16f * static_cast<float>(frame)) * half;
        for (uint32_t y = 0; y < cellSize; ++y) {
            for (uint32_t x = 0; x < cellSize; ++x) {
                const float dx = static_cast<float>(x) - half;
                const float dy = static_cast<float>(y) - half;
                const float dist =
                    (frame % 2u == 0u) ? (std::abs(dx) + std::abs(dy)) * 0.72f : std::sqrt(dx * dx + dy * dy);
                const float alpha = std::clamp(1.0f - (dist / std::max(1.0f, radius)), 0.0f, 1.0f);
                const size_t idx = (static_cast<size_t>(y) * w + (frame * cellSize + x)) * 4u;
                rgba[idx + 0] = 255;
                rgba[idx + 1] = static_cast<uint8_t>(180 + frame * 18);
                rgba[idx + 2] = 90;
                rgba[idx + 3] = static_cast<uint8_t>(alpha * alpha * 255.0f);
            }
        }
    }
    return bgl::SpriteTexture::FromRGBA8(rgba, w, h, true);
}

} // namespace

class SpritesAndBillboardsSample : public bgl::App {
public:
    void OnStart() override {
        bgl::gui::SetDefaultFont("sans");
        if (auto res = GetEngine().LoadModel("assets/ToyCar.glb"); !res) {
            bgl::gui::ShowMessage("Load Failed", res.error(), bgl::gui::MessageType::Error);
        }

        auto sheet = CreateSparkSheet(32);
        auto ringTex = bgl::SpriteTexture::CreateRing(64, 0.70f, 0.90f, {0.25f, 0.85f, 1.0f, 1.0f});
        auto glowTex = bgl::SpriteTexture::CreateRadialGlow(64, {1.0f, 0.65f, 0.2f, 1.0f});

        hudSprite_.texture = sheet;
        hudSprite_.size = {72.0f, 72.0f};
        hudSprite_.position = {56.0f, 56.0f};
        hudSprite_.AddClip(
            bgl::SpriteClip::FromGrid("pulse", 4, 1, 0, 4, 8.0f, bgl::SpritePlayback::PingPong, 128, 32));
        hudSprite_.Play("pulse");

        reticleSprite_.texture = ringTex;
        reticleSprite_.size = {84.0f, 84.0f};
        reticleSprite_.position = {56.0f, 56.0f};

        flameBillboard_.texture = sheet;
        flameBillboard_.size = {0.85f, 0.85f};
        flameBillboard_.position = {0.0f, 1.35f, 0.0f};
        flameBillboard_.anchor = {0.5f, 1.0f};
        flameBillboard_.mode = bgl::BillboardMode::CylindricalY;
        flameBillboard_.blend = bgl::SpriteBlend::Additive;
        flameBillboard_.color = {2.2f, 1.6f, 0.8f, 1.0f};
        flameBillboard_.AddClip(
            bgl::SpriteClip::FromGrid("burn", 4, 1, 0, 4, 10.0f, bgl::SpritePlayback::Loop, 128, 32));
        flameBillboard_.Play("burn");

        orbBillboard_.texture = glowTex;
        orbBillboard_.size = {0.55f, 0.55f};
        orbBillboard_.blend = bgl::SpriteBlend::Additive;
        orbBillboard_.mode = bgl::BillboardMode::Spherical;
        orbBillboard_.color = {0.5f, 1.8f, 2.8f, 0.95f};
    }

    void OnUpdate(float deltaTime) override {
        if (bgl::Input::IsKeyPressed(bgl::Key::Escape)) {
            Quit();
        }
        timeSec_ += deltaTime;
        hudSprite_.Update(deltaTime);
        flameBillboard_.Update(deltaTime);
        reticleSprite_.rotationRad = timeSec_ * 1.4f;

        if (showSprites_) {
            GetEngine().DrawSprite(reticleSprite_);
            GetEngine().DrawSprite(hudSprite_);
        }
        if (showBillboards_) {
            GetEngine().DrawBillboard(flameBillboard_);
            for (int i = 0; i < 4; ++i) {
                const float angle = timeSec_ * 1.5f + static_cast<float>(i) * 1.5708f;
                orbBillboard_.position = {std::cos(angle) * 1.6f, 0.65f + 0.25f * std::sin(timeSec_ * 3.0f + angle),
                                          std::sin(angle) * 1.6f};
                GetEngine().DrawBillboard(orbBillboard_);
            }
        }
    }

    void OnGui() override {
        bgl::gui::Begin("Sprites & Billboards");
        bgl::gui::Toggle("2D Animated Sprites", showSprites_, "Draws HUD sprite & rotating ring");
        bgl::gui::Toggle("3D HDR Billboards", showBillboards_, "Cylindrical flame & spherical glow orbs");
        constexpr std::array<const char*, 3> kModes{"Spherical", "CylindricalY", "ScreenAligned"};
        if (bgl::gui::OptionGroup("Billboard Mode", kModes, selectedMode_, true)) {
            orbBillboard_.mode = static_cast<bgl::BillboardMode>(selectedMode_);
        }
        bgl::gui::End();
    }

private:
    bgl::AnimatedSprite hudSprite_{};
    bgl::Sprite reticleSprite_{};
    bgl::AnimatedBillboard flameBillboard_{};
    bgl::Billboard orbBillboard_{};
    float timeSec_{0.0f};
    bool showSprites_{true};
    bool showBillboards_{true};
    size_t selectedMode_{0};
};

BGL_RUN(SpritesAndBillboardsSample)
