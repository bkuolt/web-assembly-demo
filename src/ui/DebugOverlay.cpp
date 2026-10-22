/**
 * @file DebugOverlay.cpp
 * @brief In-engine 2D Debug Overlay displaying real-time FPS, camera, scene, and spatial metrics.
 */
#include "ui/DebugOverlay.hpp"
#include "text/EmbeddedFont.hpp"
#include "core/Log.hpp"
#include <filesystem>
#include <format>
#include <algorithm>

namespace bgl::ui {

DebugOverlay::DebugOverlay() = default;

DebugOverlay::~DebugOverlay() = default;

core::Result<void> DebugOverlay::Initialize(WGPUDevice device,
                                            WGPUQueue queue,
                                            WGPUTextureFormat surfaceFormat,
                                            uint32_t sampleCount) {
    // 1. Try to load LiberationMono from disk, or fallback to zero-dependency embedded font
    std::string fontPath;
    const std::vector<std::string> searchPaths = {
        "assets/fonts/LiberationMono-Regular.ttf",
        "../assets/fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
    };

    for (const auto& path : searchPaths) {
        if (std::filesystem::exists(path)) {
            fontPath = path;
            break;
        }
    }

    core::Result<void> initRes;
    if (!fontPath.empty()) {
        BGL_LOG_INFO("Loading debug overlay font from '{}'...", fontPath);
        initRes = fontAtlas_.InitializeFromFile(fontPath, 16, 1024);
    } else {
        BGL_LOG_INFO("Loading debug overlay font from built-in embedded font data...");
        auto embeddedData = text::GetEmbeddedFontData();
        initRes = fontAtlas_.InitializeFromMemory(embeddedData, 16, 1024);
    }

    if (!initRes) {
        return initRes;
    }

    // 2. Initialize TextRenderer WebGPU pipeline
    return textRenderer_.Initialize(device, queue, surfaceFormat, sampleCount, fontAtlas_);
}

void DebugOverlay::Update(float deltaTime, const OverlayStats& stats) {
    currentStats_ = stats;

    float currentMs = deltaTime * 1000.0f;
    smoothedFrameTimeMs_ = smoothedFrameTimeMs_ * 0.92f + currentMs * 0.08f;

    float currentFps = (deltaTime > 0.00001f) ? (1.0f / deltaTime) : 60.0f;
    smoothedFps_ = smoothedFps_ * 0.92f + currentFps * 0.08f;
}

void DebugOverlay::Render(WGPURenderPassEncoder pass, uint32_t viewportWidth, uint32_t viewportHeight) {
    if (!visible_ || !pass || viewportWidth == 0 || viewportHeight == 0) return;

    const float panelX = 16.0f;
    const float panelY = 16.0f;
    const float panelW = 500.0f;
    const float panelH = 244.0f;

    // 1. Translucent slate background card (#0f172a at 88% alpha)
    const glm::vec4 bgColor(0.06f, 0.09f, 0.16f, 0.88f);
    const glm::vec4 borderColor(0.22f, 0.74f, 0.97f, 0.35f);
    textRenderer_.DrawRectWithBorder(panelX, panelY, panelW, panelH, bgColor, borderColor, 1.5f);

    // 2. Top accent glow line in vivid cyan (#38bdf8)
    textRenderer_.DrawRect(panelX, panelY, panelW, 3.0f, glm::vec4(0.22f, 0.74f, 0.97f, 1.0f));

    // 3. Render telemetry text lines
    float textX = panelX + 16.0f;
    float textY = panelY + 14.0f;
    const float lineSpacing = 24.0f;

    // Line 1: Header / Engine Info
    textRenderer_.DrawText("WebAssembly an WebGPU Demo v0.7.0 [4x MSAA - 16x Aniso]",
                           textX, textY, glm::vec4{0.22f, 0.74f, 0.97f, 1.0f});
    textY += lineSpacing;

    // Line 2: Real-time Frame Performance
    glm::vec4 fpsColor = (smoothedFps_ >= 55.0f) ? glm::vec4(0.29f, 0.87f, 0.50f, 1.0f)   // Green
                       : (smoothedFps_ >= 30.0f) ? glm::vec4(0.98f, 0.80f, 0.08f, 1.0f)   // Yellow
                                                 : glm::vec4(0.94f, 0.27f, 0.24f, 1.0f);  // Red
    std::string fpsText = std::format("Performance: {:.1f} FPS ({:.2f} ms)", smoothedFps_, smoothedFrameTimeMs_);
    textRenderer_.DrawText(fpsText, textX, textY, fpsColor);
    textY += lineSpacing;

    // Line 3: Active Scene Info
    std::string sceneText = std::format("Scene: {} | Prims: {} | Verts: {}",
                                        currentStats_.sceneName,
                                        currentStats_.primitiveCount,
                                        currentStats_.vertexCount);
    textRenderer_.DrawText(sceneText, textX, textY, glm::vec4(0.97f, 0.98f, 0.99f, 1.0f));
    textY += lineSpacing;

    // Line 4: Camera Coordinates
    std::string camText = std::format("Camera: [{:.1f}, {:.1f}, {:.1f}] (Dist: {:.1f})",
                                      currentStats_.cameraPos.x, currentStats_.cameraPos.y, currentStats_.cameraPos.z,
                                      currentStats_.cameraDistance);
    textRenderer_.DrawText(camText, textX, textY, glm::vec4(0.80f, 0.85f, 0.92f, 1.0f));
    textY += lineSpacing;

    // Line 5: Spatial Quadtree & Occlusion Culling
    std::string spatialText = std::format("Quadtree: {} Nodes | Vis: {}/{} (Occl: {})",
                                          currentStats_.quadtreeNodes,
                                          currentStats_.visiblePrimitives,
                                          currentStats_.primitiveCount,
                                          currentStats_.occludedPrimitives);
    textRenderer_.DrawText(spatialText, textX, textY, glm::vec4(0.20f, 0.83f, 0.60f, 1.0f));
    textY += lineSpacing;

    // Line 6: Spatial & Scene Toggles
    std::string toggleText = std::format("[B] AABBs: {}  [Q] Quad: {}  [C] Occl: {}  [K] Sky: {}",
                                         currentStats_.boundingBoxesEnabled ? "ON" : "OFF",
                                         currentStats_.quadtreeBoundsEnabled ? "ON" : "OFF",
                                         currentStats_.occlusionCullingEnabled ? "ON" : "OFF",
                                         currentStats_.skyboxEnabled ? "ON" : "OFF");
    textRenderer_.DrawText(toggleText, textX, textY, glm::vec4(0.99f, 0.73f, 0.22f, 1.0f));
    textY += lineSpacing;

    // Line 7: PBR Feature Toggles (Bump, Mips, Shadows, Reflections)
    std::string pbrToggleText = std::format("[V] Bump: {}  [L] Mips: {}  [M] Shadows: {}  [R] Refl: {}",
                                            currentStats_.bumpMappingEnabled ? "ON" : "OFF",
                                            currentStats_.mipmappingEnabled ? "ON" : "OFF",
                                            currentStats_.shadowsEnabled ? "ON" : "OFF",
                                            currentStats_.reflectionsEnabled ? "ON" : "OFF");
    textRenderer_.DrawText(pbrToggleText, textX, textY, glm::vec4(0.68f, 0.62f, 0.98f, 1.0f));
    textY += lineSpacing;

    // Line 8: Controls Navigation Help
    textRenderer_.DrawText("[O/F3] HUD  | LMB: Orbit  | RMB: Pan  | Wheel: Zoom",
                           textX, textY, glm::vec4(0.60f, 0.65f, 0.73f, 0.95f));

    // Flush GPU draw call
    textRenderer_.Render(pass, viewportWidth, viewportHeight);
}

} // namespace bgl::ui
