/**
 * @file DebugOverlay.hpp
 * @brief In-engine 2D Debug Overlay displaying real-time FPS, camera, scene, and spatial metrics.
 */
#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>
#include "core/Types.hpp"
#include "text/FontAtlas.hpp"
#include "text/TextRenderer.hpp"

namespace bgl::ui {

/**
 * @struct OverlayStats
 * @brief Snapshot of engine runtime metrics passed to the debug overlay each frame.
 */
struct OverlayStats {
    float deltaTime = 0.016f;
    glm::vec3 cameraPos{0.0f};
    glm::vec3 cameraTarget{0.0f};
    float cameraDistance = 0.0f;

    std::string sceneName = "None";
    size_t primitiveCount = 0;
    size_t vertexCount = 0;
    size_t indexCount = 0;

    size_t quadtreeNodes = 0;
    size_t visiblePrimitives = 0;

    bool boundingBoxesEnabled = true;
    bool quadtreeBoundsEnabled = false;
    bool skyboxEnabled = true;
};

/**
 * @class DebugOverlay
 * @brief Renders an interactive, glassmorphism HUD panel over the 3D scene.
 */
class DebugOverlay {
public:
    DebugOverlay();
    ~DebugOverlay();

    core::Result<void> Initialize(WGPUDevice device,
                                  WGPUQueue queue,
                                  WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount);

    void Update(float deltaTime, const OverlayStats& stats);
    void Render(WGPURenderPassEncoder pass, uint32_t viewportWidth, uint32_t viewportHeight);

    void Toggle() noexcept { visible_ = !visible_; }
    void SetVisible(bool visible) noexcept { visible_ = visible; }
    [[nodiscard]] bool IsVisible() const noexcept { return visible_; }

private:
    bool visible_ = true;
    text::FontAtlas fontAtlas_;
    text::TextRenderer textRenderer_;

    // Smoothed metrics
    float smoothedFps_ = 60.0f;
    float smoothedFrameTimeMs_ = 16.6f;
    OverlayStats currentStats_;
};

} // namespace bgl::ui

