/**
 * @file ApplicationInput.cpp
 * @brief Table-driven desktop SDL2 keyboard shortcut handler for the bgl reference viewer.
 */
#include "core/Log.hpp"
#include "viewer/Application.hpp"
#include <array>
#include <cmath>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::app {

#if defined(BGL_PLATFORM_NATIVE)
namespace {

struct SimpleKeyAction {
    int32_t key;
    int32_t altKey;
    void (Application::*handler)();
};

} // namespace

void Application::HandleKeyDown(int32_t keySym, uint16_t keyMod) {
    const bool hasCtrl = (keyMod & KMOD_CTRL) != 0;
    const bool hasAlt = (keyMod & KMOD_ALT) != 0;
    const bool hasShift = (keyMod & KMOD_SHIFT) != 0;

    // 1. Camera bookmarks: Ctrl+1..9 saves, 1..9 restores (never trigger on Alt combinations)
    if (!hasAlt && keySym >= SDLK_1 && keySym <= SDLK_9) {
        const size_t slot = static_cast<size_t>(keySym - SDLK_1);
        if (hasCtrl) {
            camera_.SaveBookmark(slot);
            BGL_LOG_INFO("Saved camera bookmark to slot #{}", slot + 1);
        } else if (camera_.RestoreBookmark(slot)) {
            BGL_LOG_INFO("Restored camera bookmark from slot #{}", slot + 1);
        }
        return;
    }

    // 2. Modifier-sensitive shortcuts (Ctrl+R/P, +/- exposure vs zoom, exit, wireframe)
    switch (keySym) {
    case SDLK_ESCAPE:
        if (!hasCtrl && !hasAlt) {
            running_ = false;
        }
        return;
    case SDLK_F1:
        if (!hasCtrl && !hasAlt) {
            wireframeMode_ = !wireframeMode_;
            BGL_LOG_INFO("Wireframe overlay: {}", wireframeMode_ ? "ON" : "OFF");
        }
        return;
    case SDLK_r:
        if (hasAlt) {
            return;
        }
        if (hasCtrl) {
            pipelineManager_.SetRayTracingEnabled(!pipelineManager_.IsRayTracingEnabled());
            BGL_LOG_INFO("Desktop Ray Tracing: {}", pipelineManager_.IsRayTracingEnabled() ? "ON" : "OFF");
        } else {
            ToggleReflections();
            BGL_LOG_INFO("Environment reflections (IBL): {}", pipelineManager_.IsReflectionsEnabled());
        }
        return;
    case SDLK_p:
        if (hasAlt) {
            return;
        }
        if (hasCtrl) {
            if (!pipelineManager_.IsRayTracingEnabled()) {
                pipelineManager_.SetRayTracingEnabled(true);
            }
            pipelineManager_.SetRtPathTracingEnabled(!pipelineManager_.IsRtPathTracingEnabled());
            BGL_LOG_INFO("Desktop Path Tracer: {}", pipelineManager_.IsRtPathTracingEnabled() ? "ON" : "OFF");
        } else {
            ToggleParallaxOcclusion();
            BGL_LOG_INFO("Parallax Occlusion Mapping (POM): {}", pipelineManager_.IsParallaxEnabled());
        }
        return;
    case SDLK_z:
        if (!hasCtrl && !hasAlt) {
            pipelineManager_.ToggleBloom();
            BGL_LOG_INFO("Emissive HDR Bloom: {}", pipelineManager_.IsBloomEnabled());
        }
        return;
    case SDLK_g:
        if (!hasCtrl && !hasAlt) {
            pipelineManager_.ToggleSSAO();
            BGL_LOG_INFO("SSAO/GTAO: {}", pipelineManager_.IsSsaoEnabled());
        }
        return;
    case SDLK_PLUS:
    case SDLK_KP_PLUS:
    case SDLK_EQUALS:
        if (hasCtrl || hasAlt) {
            return;
        }
        if (hasShift) {
            AdjustExposure(0.15f);
            BGL_LOG_INFO("Exposure: {:.2f}", pipelineManager_.GetExposure());
        } else {
            camera_.Zoom(camera_.GetDistance() * 0.15f);
        }
        return;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        if (hasCtrl || hasAlt) {
            return;
        }
        if (hasShift) {
            AdjustExposure(-0.15f);
            BGL_LOG_INFO("Exposure: {:.2f}", pipelineManager_.GetExposure());
        } else {
            camera_.Zoom(-camera_.GetDistance() * 0.15f);
        }
        return;
    case SDLK_SEMICOLON:
        if (hasCtrl || hasAlt) {
            return;
        }
        SetLodBias(pipelineManager_.GetLodBias() - 0.15f);
        BGL_LOG_INFO("LOD bias: {:.2f}x", pipelineManager_.GetLodBias());
        return;
    case SDLK_QUOTE:
        if (hasCtrl || hasAlt) {
            return;
        }
        SetLodBias(pipelineManager_.GetLodBias() + 0.15f);
        BGL_LOG_INFO("LOD bias: {:.2f}x", pipelineManager_.GetLodBias());
        return;
    case SDLK_COMMA:
        if (!hasCtrl && !hasAlt) {
            SetTimeOfDay(timeOfDay_ - 0.5f);
        }
        return;
    case SDLK_PERIOD:
        if (!hasCtrl && !hasAlt) {
            SetTimeOfDay(timeOfDay_ + 0.5f);
        }
        return;
    case SDLK_LEFTBRACKET:
        if (!hasCtrl && !hasAlt) {
            RotateEnvironment(-0.0872665f);
        }
        return;
    case SDLK_RIGHTBRACKET:
        if (!hasCtrl && !hasAlt) {
            RotateEnvironment(0.0872665f);
        }
        return;
    default:
        break;
    }

    if (hasCtrl || hasAlt) {
        return;
    }

    // 3. Zero-argument toggle and cycle hotkeys
    static constexpr std::array<SimpleKeyAction, 24> kSimpleActions{{
        {SDLK_F2, SDLK_UNKNOWN, &Application::ToggleSettingsUI},
        {SDLK_o, SDLK_F3, &Application::ToggleDebugOverlay},
        {SDLK_b, SDLK_UNKNOWN, &Application::ToggleBoundingBoxes},
        {SDLK_q, SDLK_UNKNOWN, &Application::ToggleQuadtreeBounds},
        {SDLK_v, SDLK_UNKNOWN, &Application::ToggleBumpMapping},
        {SDLK_l, SDLK_UNKNOWN, &Application::ToggleMipmapping},
        {SDLK_m, SDLK_UNKNOWN, &Application::ToggleShadows},
        {SDLK_u, SDLK_UNKNOWN, &Application::ToggleAmbientOcclusion},
        {SDLK_h, SDLK_UNKNOWN, &Application::ToggleOcclusionCulling},
        {SDLK_x, SDLK_UNKNOWN, &Application::CycleTonemapper},
        {SDLK_e, SDLK_UNKNOWN, &Application::CycleEnvironmentPreset},
        {SDLK_k, SDLK_UNKNOWN, &Application::CycleSkyboxMode},
        {SDLK_j, SDLK_UNKNOWN, &Application::ToggleLensFlare},
        {SDLK_y, SDLK_UNKNOWN, &Application::CycleWeatherMode},
        {SDLK_F4, SDLK_UNKNOWN, &Application::ToggleRain},
        {SDLK_F5, SDLK_UNKNOWN, &Application::ToggleSnow},
        {SDLK_F6, SDLK_UNKNOWN, &Application::ToggleFog},
        {SDLK_F7, SDLK_UNKNOWN, &Application::ToggleSmoke},
        {SDLK_i, SDLK_F8, &Application::ToggleDayNightCycle},
        {SDLK_n, SDLK_F9, &Application::CycleDebugViewMode},
        {SDLK_F10, SDLK_UNKNOWN, &Application::ToggleLodHeatmap},
        {SDLK_F11, SDLK_UNKNOWN, &Application::ToggleLod},
        {SDLK_F12, SDLK_UNKNOWN, &Application::ToggleAutoLod},
        {SDLK_f, SDLK_UNKNOWN, &Application::FocusSelection},
    }};

    for (const auto& entry : kSimpleActions) {
        if (keySym == entry.key || (entry.altKey != SDLK_UNKNOWN && keySym == entry.altKey)) {
            (this->*entry.handler)();
            return;
        }
    }

    if (keySym == SDLK_c) {
        ToggleCameraMode();
        BGL_LOG_INFO("Camera mode: {}", camera_.GetMode() == core::CameraMode::Fly ? "Fly" : "Orbit");
    } else if (keySym == SDLK_t) {
        ToggleTurntable();
        BGL_LOG_INFO("Turntable autorotation: {}", turntableActive_ ? "ON" : "OFF");
    }
}
#endif

} // namespace bgl::app
