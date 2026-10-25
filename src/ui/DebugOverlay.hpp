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
    float deltaTime{0.016f};
    glm::vec3 cameraPos{0.0f};
    glm::vec3 cameraTarget{0.0f};
    float cameraDistance{0.0f};

    std::string sceneName{"None"};
    size_t primitiveCount{0};
    size_t vertexCount{0};
    size_t indexCount{0};

    size_t quadtreeNodes{0};
    size_t visiblePrimitives{0};
    size_t occludedPrimitives{0};

    bool boundingBoxesEnabled{true};
    bool quadtreeBoundsEnabled{false};
    bool skyboxEnabled{true};
    bool bumpMappingEnabled{true};
    bool mipmappingEnabled{true};
    bool shadowsEnabled{true};
    bool reflectionsEnabled{true};
    bool ambientOcclusionEnabled{true};
    bool occlusionCullingEnabled{true};

    // System Memory & Hardware Telemetry
    size_t residentRamBytes{0};
    size_t vramBytes{0};
    bool gamepadConnected{false};
    bool isDualSense{false};
    std::string gamepadName{"None"};
};

/**
 * @enum OverlayMode
 * @brief Presentation mode for the in-engine debug HUD.
 */
enum class OverlayMode : uint8_t {
    Full = 0,    ///< Comprehensive multi-section telemetry & controls card
    Minimal = 1, ///< Single-line compact floating status pill
    Hidden = 2   ///< Fully hidden HUD
};

/**
 * @class DebugOverlay
 * @brief Renders an interactive, glassmorphism HUD panel over the 3D scene.
 */
class DebugOverlay {
public:
    DebugOverlay();
    ~DebugOverlay();

    /**
     * @brief Initializes font atlas (system LiberationMono or embedded fallback) and TextRenderer.
     * @param device WebGPU device handle.
     * @param queue WebGPU queue handle.
     * @param surfaceFormat Swapchain presentation format.
     * @param sampleCount Multisample count for the presentation surface.
     * @return Result indicating success or error string.
     */
    core::Result<void> Initialize(WGPUDevice device,
                                  WGPUQueue queue,
                                  WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount);

    /**
     * @brief Ingests current engine telemetry metrics and updates exponential moving averages for FPS.
     * @param deltaTime Elapsed frame time in seconds.
     * @param stats Live telemetry metrics snapshot.
     */
    void Update(float deltaTime, const OverlayStats& stats);

    /**
     * @brief Renders the glassmorphism telemetry panel and shaped text over the presentation pass.
     * @param pass Active render pass encoder targeting the presentation swapchain.
     * @param viewportWidth Framebuffer width in pixels.
     * @param viewportHeight Framebuffer height in pixels.
     */
    void Render(WGPURenderPassEncoder pass, uint32_t viewportWidth, uint32_t viewportHeight);

    /**
     * @brief Cycles overlay mode: Full -> Minimal -> Hidden -> Full.
     */
    void Toggle() noexcept;

    void SetMode(OverlayMode mode) noexcept { mode_ = mode; }
    [[nodiscard]] OverlayMode GetMode() const noexcept { return mode_; }

    void SetVisible(bool visible) noexcept { mode_ = visible ? OverlayMode::Full : OverlayMode::Hidden; }
    [[nodiscard]] bool IsVisible() const noexcept { return mode_ != OverlayMode::Hidden; }

private:
    OverlayMode mode_{OverlayMode::Full};
    text::FontAtlas fontAtlas_{};
    text::TextRenderer textRenderer_{};

    // Smoothed metrics
    float smoothedFps_{60.0f};
    float smoothedFrameTimeMs_{16.6f};
    OverlayStats currentStats_{};
};

} // namespace bgl::ui

