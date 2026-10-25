/**
 * @file DebugOverlay.cpp
 * @brief In-engine 2D Debug Overlay displaying real-time FPS, camera, scene, and spatial metrics.
 */
#include "ui/DebugOverlay.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include "text/EmbeddedFont.hpp"
#include <algorithm>
#include <filesystem>
#include <format>

namespace bgl::ui {

DebugOverlay::DebugOverlay() = default;

DebugOverlay::~DebugOverlay() = default;

core::Result<void> DebugOverlay::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                            uint32_t sampleCount) {
    // 1. Try to load LiberationMono from disk, or fallback to zero-dependency embedded font
    std::string fontPath;
    const std::vector<std::string> searchPaths = {"assets/fonts/LiberationMono-Regular.ttf",
                                                  "../assets/fonts/LiberationMono-Regular.ttf",
                                                  "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"};

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

void DebugOverlay::Toggle() noexcept {
    switch (mode_) {
    case OverlayMode::Full:
        mode_ = OverlayMode::Minimal;
        break;
    case OverlayMode::Minimal:
        mode_ = OverlayMode::Hidden;
        break;
    case OverlayMode::Hidden:
        mode_ = OverlayMode::Full;
        break;
    }
}

void DebugOverlay::Update(float deltaTime, const OverlayStats& stats) {
    currentStats_ = stats;

    float currentMs = deltaTime * 1000.0f;
    smoothedFrameTimeMs_ = smoothedFrameTimeMs_ * 0.92f + currentMs * 0.08f;

    float currentFps = (deltaTime > 0.00001f) ? (1.0f / deltaTime) : 60.0f;
    smoothedFps_ = smoothedFps_ * 0.92f + currentFps * 0.08f;
}

void DebugOverlay::Render(WGPURenderPassEncoder pass, uint32_t viewportWidth, uint32_t viewportHeight) {
    if (mode_ == OverlayMode::Hidden || !pass || viewportWidth == 0 || viewportHeight == 0)
        return;

    // Determine FPS status color (Green -> Yellow -> Red)
    const glm::vec4 fpsColor = (smoothedFps_ >= 55.0f)   ? glm::vec4(0.22f, 0.86f, 0.56f, 1.0f)  // Vivid Emerald
                               : (smoothedFps_ >= 30.0f) ? glm::vec4(0.98f, 0.80f, 0.08f, 1.0f)  // Amber
                                                         : glm::vec4(0.94f, 0.27f, 0.24f, 1.0f); // Rose

    // --------------------------------------------------------------------------
    // Minimal Overlay Mode: Floating Top-Left Status Strip
    // --------------------------------------------------------------------------
    if (mode_ == OverlayMode::Minimal) {
        const float pillX = 16.0f;
        const float pillY = 16.0f;
        const float pillW = 500.0f;
        const float pillH = 28.0f;

        const glm::vec4 pillBg(0.04f, 0.06f, 0.10f, 0.88f);
        const glm::vec4 pillBorder(0.22f, 0.74f, 0.97f, 0.35f);
        textRenderer_.DrawRectWithBorder(pillX, pillY, pillW, pillH, pillBg, pillBorder, 1.0f);

        // Vivid Cyan Left Accent Marker
        textRenderer_.DrawRect(pillX, pillY, 3.0f, pillH, glm::vec4(0.22f, 0.74f, 0.97f, 1.0f));

        const float ramMb = static_cast<float>(currentStats_.residentRamBytes) / (1024.0f * 1024.0f);
        std::string minText =
            std::format("bgl v{} | {:.0f} FPS ({:.1f}ms) | RAM {:.0f}M | {} [O:Expand]", bgl::kEngineVersion,
                        smoothedFps_, smoothedFrameTimeMs_, ramMb, currentStats_.sceneName);
        textRenderer_.DrawText(minText, pillX + 12.0f, pillY + 6.0f, glm::vec4(0.92f, 0.95f, 0.98f, 1.0f));

        textRenderer_.Render(pass, viewportWidth, viewportHeight);
        return;
    }

    // --------------------------------------------------------------------------
    // Full Overlay Mode: Structured Glassmorphism Telemetry Card
    // --------------------------------------------------------------------------
    const float panelX = 16.0f;
    const float panelY = 16.0f;
    const float panelW = 490.0f;
    const float panelH = 226.0f;

    // 1. Translucent slate background card (#0a0f1d at 92% alpha)
    const glm::vec4 bgColor(0.04f, 0.06f, 0.11f, 0.92f);
    const glm::vec4 borderColor(0.22f, 0.74f, 0.97f, 0.30f);
    textRenderer_.DrawRectWithBorder(panelX, panelY, panelW, panelH, bgColor, borderColor, 1.0f);

    // 2. Top accent glow line in vivid cyan (#38bdf8)
    textRenderer_.DrawRect(panelX, panelY, panelW, 2.5f, glm::vec4(0.22f, 0.74f, 0.97f, 1.0f));

    // 3. Header: Engine name & Version (Left) and Live FPS Pill (Right)
    const float textX = panelX + 14.0f;
    textRenderer_.DrawText(std::format("{} v{} [4x MSAA]", bgl::kEngineName, bgl::kEngineVersion), textX,
                           panelY + 10.0f, glm::vec4{0.22f, 0.74f, 0.97f, 1.0f});

    // Right-aligned FPS badge pill
    const float fpsPillW = 144.0f;
    const float fpsPillH = 18.0f;
    const float fpsPillX = panelX + panelW - fpsPillW - 14.0f;
    const float fpsPillY = panelY + 8.0f;
    const glm::vec4 fpsPillBg(fpsColor.r * 0.16f, fpsColor.g * 0.16f, fpsColor.b * 0.16f, 0.85f);
    const glm::vec4 fpsPillBorder(fpsColor.r, fpsColor.g, fpsColor.b, 0.45f);
    textRenderer_.DrawRectWithBorder(fpsPillX, fpsPillY, fpsPillW, fpsPillH, fpsPillBg, fpsPillBorder, 1.0f);
    std::string fpsPillText = std::format("{:.1f} FPS  {:.2f}ms", smoothedFps_, smoothedFrameTimeMs_);
    textRenderer_.DrawText(fpsPillText, fpsPillX + 10.0f, fpsPillY + 2.0f, fpsColor);

    // Separator line 1
    textRenderer_.DrawRect(panelX + 10.0f, panelY + 31.0f, panelW - 20.0f, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.07f));

    // 4. Row 1: Hardware RAM / VRAM & Gamepad Status
    const float ramMb = static_cast<float>(currentStats_.residentRamBytes) / (1024.0f * 1024.0f);
    const float vramMb = static_cast<float>(currentStats_.vramBytes) / (1024.0f * 1024.0f);
    std::string memText = std::format("RAM: {:.1f} MB | VRAM: {:.1f} MB", ramMb, vramMb);
    textRenderer_.DrawText(memText, textX, panelY + 39.0f, glm::vec4(0.38f, 0.76f, 0.97f, 1.0f));

    std::string padText =
        currentStats_.gamepadConnected
            ? (currentStats_.isDualSense ? "🎮 PS5 DualSense"
                                         : std::format("🎮 {}", currentStats_.gamepadName.substr(0, 12)))
            : "🎮 Disconnected";
    glm::vec4 padColor =
        currentStats_.gamepadConnected ? glm::vec4(0.25f, 0.88f, 0.65f, 1.0f) : glm::vec4(0.55f, 0.60f, 0.68f, 0.75f);
    textRenderer_.DrawText(padText, panelX + 310.0f, panelY + 39.0f, padColor);

    // 5. Row 2: Active Scene & Primitives
    std::string sceneText = std::format("Scene: {} ({} prims, {} verts)", currentStats_.sceneName,
                                        currentStats_.primitiveCount, currentStats_.vertexCount);
    textRenderer_.DrawText(sceneText, textX, panelY + 61.0f, glm::vec4(0.95f, 0.97f, 0.99f, 1.0f));

    // 6. Row 3: Camera & Spatial Culling
    std::string camText = std::format("Cam: [{:.1f}, {:.1f}, {:.1f}]", currentStats_.cameraPos.x,
                                      currentStats_.cameraPos.y, currentStats_.cameraPos.z);
    textRenderer_.DrawText(camText, textX, panelY + 83.0f, glm::vec4(0.80f, 0.85f, 0.92f, 1.0f));

    std::string cullText = std::format("Vis: {}/{} ({} Occl)", currentStats_.visiblePrimitives,
                                       currentStats_.primitiveCount, currentStats_.occludedPrimitives);
    textRenderer_.DrawText(cullText, panelX + 310.0f, panelY + 83.0f, glm::vec4(0.22f, 0.86f, 0.56f, 1.0f));

    // Separator line 2
    textRenderer_.DrawRect(panelX + 10.0f, panelY + 105.0f, panelW - 20.0f, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.07f));

    // 7. Row 4: Geometry & Spatial Feature Status
    std::string geoToggleText = std::format(
        "Geo: [B]AABB:{}  [Q]Quad:{}  [H]Hi-Z:{}  [K]Sky:{}", currentStats_.boundingBoxesEnabled ? "ON" : "OFF",
        currentStats_.quadtreeBoundsEnabled ? "ON" : "OFF", currentStats_.occlusionCullingEnabled ? "ON" : "OFF",
        currentStats_.skyboxEnabled ? "ON" : "OFF");
    textRenderer_.DrawText(geoToggleText, textX, panelY + 114.0f, glm::vec4(0.99f, 0.73f, 0.22f, 1.0f));

    // 8. Row 5: PBR Shading & Lighting Status
    std::string pbrToggleText = std::format(
        "PBR: [V]Bump:{} [M]Shdw:{} [R]Refl:{} [U]AO:{} [L]Mip:{}", currentStats_.bumpMappingEnabled ? "ON" : "OFF",
        currentStats_.shadowsEnabled ? "ON" : "OFF", currentStats_.reflectionsEnabled ? "ON" : "OFF",
        currentStats_.ambientOcclusionEnabled ? "ON" : "OFF", currentStats_.mipmappingEnabled ? "ON" : "OFF");
    textRenderer_.DrawText(pbrToggleText, textX, panelY + 136.0f, glm::vec4(0.72f, 0.65f, 0.98f, 1.0f));

    // 9. Row 6: Camera Target & Spatial Partitioning
    std::string camDetailText =
        std::format("Target: [{:.1f}, {:.1f}, {:.1f}] | Dist: {:.1f} | Quad: {} nodes", currentStats_.cameraTarget.x,
                    currentStats_.cameraTarget.y, currentStats_.cameraTarget.z, currentStats_.cameraDistance,
                    currentStats_.quadtreeNodes);
    textRenderer_.DrawText(camDetailText, textX, panelY + 158.0f, glm::vec4(0.75f, 0.82f, 0.90f, 1.0f));

    // Separator line 3
    textRenderer_.DrawRect(panelX + 10.0f, panelY + 180.0f, panelW - 20.0f, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.07f));

    // 10. Bottom Navigation Bar Strip
    const float footerH = 24.0f;
    const float footerY = panelY + panelH - footerH;
    textRenderer_.DrawRect(panelX, footerY, panelW, footerH, glm::vec4(0.02f, 0.04f, 0.07f, 0.75f));
    textRenderer_.DrawRect(panelX, footerY, panelW, 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.05f));

    textRenderer_.DrawText("[O] Cycle HUD  |  [F] Focus  |  [T] Turntable  |  [C] Fly/Orbit", textX, footerY + 5.0f,
                           glm::vec4(0.55f, 0.62f, 0.72f, 0.95f));

    // Flush GPU draw call
    textRenderer_.Render(pass, viewportWidth, viewportHeight);
}

} // namespace bgl::ui
