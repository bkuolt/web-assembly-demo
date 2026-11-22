/**
 * @file ApplicationInput.cpp
 * @brief Desktop SDL2 keyboard shortcut handler for the bgl reference viewer.
 */
#include "core/Log.hpp"
#include "viewer/Application.hpp"
#include <cmath>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::app {

#if defined(BGL_PLATFORM_NATIVE)
void Application::HandleKeyDown(int32_t keySym, uint16_t keyMod) {
    if (keySym >= SDLK_1 && keySym <= SDLK_9) {
        const size_t slot = static_cast<size_t>(keySym - SDLK_1);
        if ((keyMod & KMOD_CTRL) != 0) {
            camera_.SaveBookmark(slot);
            BGL_LOG_INFO("Saved camera bookmark to slot #{}", slot + 1);
        } else if (camera_.RestoreBookmark(slot)) {
            BGL_LOG_INFO("Restored camera bookmark from slot #{}", slot + 1);
        }
        return;
    }

    switch (keySym) {
    case SDLK_ESCAPE:
        running_ = false;
        return;
    case SDLK_F1:
        wireframeMode_ = !wireframeMode_;
        BGL_LOG_INFO("Toggled Wireframe Overlay: {}", wireframeMode_ ? "ON" : "OFF");
        break;
    case SDLK_F2:
        ToggleSettingsUI();
        BGL_LOG_INFO("Toggled VRAM & Compression Settings UI: {}", showSettingsUi_ ? "ON" : "OFF");
        break;
    case SDLK_o:
    case SDLK_F3: {
        ToggleDebugOverlay();
        const char* modeStr = (debugOverlay_.GetMode() == ui::OverlayMode::Full)      ? "Full"
                              : (debugOverlay_.GetMode() == ui::OverlayMode::Minimal) ? "Minimal"
                                                                                      : "Hidden";
        BGL_LOG_INFO("Toggled In-Engine Debug Overlay mode: {}", modeStr);
        break;
    }
    case SDLK_b:
        ToggleBoundingBoxes();
        BGL_LOG_INFO("Toggled 3D Bounding Boxes & AABBs visibility: {}", showBoundingBoxes_);
        break;
    case SDLK_q:
        ToggleQuadtreeBounds();
        BGL_LOG_INFO("Toggled 3D Quadtree Spatial Bounds visibility: {}", showQuadtreeBounds_);
        break;
    case SDLK_v:
        ToggleBumpMapping();
        BGL_LOG_INFO("Toggled Bump & Normal Mapping: {}", pipelineManager_.IsBumpMappingEnabled());
        break;
    case SDLK_l:
        ToggleMipmapping();
        BGL_LOG_INFO("Toggled Texture Mipmapping (16x Anisotropic Trilinear vs Mip-0): {}",
                     pipelineManager_.IsMipmappingEnabled());
        break;
    case SDLK_m:
        ToggleShadows();
        BGL_LOG_INFO("Toggled Directional Shadow Mapping (PCF): {}", pipelineManager_.IsShadowsEnabled());
        break;
    case SDLK_r:
        if ((keyMod & KMOD_CTRL) != 0) {
            pipelineManager_.SetRayTracingEnabled(!pipelineManager_.IsRayTracingEnabled());
            BGL_LOG_INFO("Toggled Desktop Ray Tracing: {}",
                         pipelineManager_.IsRayTracingEnabled() ? "ON" : "OFF");
        } else {
            ToggleReflections();
            BGL_LOG_INFO("Toggled Environment Cubemap Reflections (IBL): {}", pipelineManager_.IsReflectionsEnabled());
        }
        break;
    case SDLK_u:
        ToggleAmbientOcclusion();
        BGL_LOG_INFO("Toggled Ambient Occlusion & Micro-Cavities: {}", pipelineManager_.IsAmbientOcclusionEnabled());
        break;
    case SDLK_h:
        ToggleOcclusionCulling();
        BGL_LOG_INFO("Toggled Quadtree Hierarchical Occlusion Culling (Hi-Z): {}",
                     pipelineManager_.IsOcclusionCullingEnabled());
        break;
    case SDLK_x:
        CycleTonemapper();
        BGL_LOG_INFO("Switched Tonemapper: {}",
                     pipelineManager_.GetToneMapOperatorName(pipelineManager_.GetToneMapOperator()));
        break;
    case SDLK_z:
        pipelineManager_.ToggleBloom();
        BGL_LOG_INFO("Toggled Emissive HDR Bloom: {}", pipelineManager_.IsBloomEnabled());
        break;
    case SDLK_e:
        CycleEnvironmentPreset();
        BGL_LOG_INFO("Switched Environment Preset: {}",
                     pipelineManager_.GetEnvironmentPresetName(pipelineManager_.GetEnvironmentPreset()));
        break;
    case SDLK_k:
        CycleSkyboxMode();
        BGL_LOG_INFO("Switched Skybox Mode: {}", skybox_.GetModeName(skybox_.GetMode()));
        break;
    case SDLK_p:
        if ((keyMod & KMOD_CTRL) != 0) {
            if (!pipelineManager_.IsRayTracingEnabled()) {
                pipelineManager_.SetRayTracingEnabled(true);
            }
            pipelineManager_.SetRtPathTracingEnabled(!pipelineManager_.IsRtPathTracingEnabled());
            BGL_LOG_INFO("Toggled Desktop Compute Path Tracer: {}",
                         pipelineManager_.IsRtPathTracingEnabled() ? "ON" : "OFF");
        } else {
            ToggleParallaxOcclusion();
            BGL_LOG_INFO("Toggled Parallax Occlusion Mapping (POM): {}", pipelineManager_.IsParallaxEnabled());
        }
        break;
    case SDLK_j:
        ToggleLensFlare();
        BGL_LOG_INFO("Toggled Directional Sun Lens Flare: {}", pipelineManager_.IsLensFlareEnabled());
        break;
    case SDLK_g:
        pipelineManager_.ToggleSSAO();
        BGL_LOG_INFO("Toggled Screen-Space Ambient Occlusion (SSAO/GTAO): {}", pipelineManager_.IsSsaoEnabled());
        break;
    case SDLK_y:
        CycleWeatherMode();
        BGL_LOG_INFO("Switched Weather Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F4:
        ToggleRain();
        BGL_LOG_INFO("Toggled Rain Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F5:
        ToggleSnow();
        BGL_LOG_INFO("Toggled Snow Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F6:
        ToggleFog();
        BGL_LOG_INFO("Toggled Fog Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F7:
        ToggleSmoke();
        BGL_LOG_INFO("Toggled Smoke Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_i:
    case SDLK_F8:
        ToggleDayNightCycle();
        BGL_LOG_INFO("Toggled Automatic Day/Night Cycle: {} (Time: {:02d}:{:02d})", dayNightCycleActive_ ? "ON" : "OFF",
                     static_cast<int>(timeOfDay_), static_cast<int>((timeOfDay_ - std::floor(timeOfDay_)) * 60.0f));
        break;
    case SDLK_n:
    case SDLK_F9:
        CycleDebugViewMode();
        BGL_LOG_INFO("Switched Diagnostic Debug View: {}",
                     pipelineManager_.GetDebugViewModeName(pipelineManager_.GetDebugViewMode()));
        break;
    case SDLK_F10:
        ToggleLodHeatmap();
        BGL_LOG_INFO("Toggled LOD Heatmap View: {}",
                     pipelineManager_.GetDebugViewModeName(pipelineManager_.GetDebugViewMode()));
        break;
    case SDLK_F11:
        ToggleLod();
        BGL_LOG_INFO("Toggled Multi-Level Mesh & Shader LOD System: {}",
                     pipelineManager_.IsLodEnabled() ? "ON" : "OFF");
        break;
    case SDLK_F12:
        ToggleAutoLod();
        BGL_LOG_INFO("Toggled Adaptive 60 FPS Auto-LOD: {} (LOD Bias: {:.2f}x)",
                     pipelineManager_.IsAutoLodEnabled() ? "ON" : "OFF", pipelineManager_.GetLodBias());
        break;
    case SDLK_SEMICOLON:
        SetLodBias(pipelineManager_.GetLodBias() - 0.15f);
        BGL_LOG_INFO("LOD Bias decreased: {:.2f}x", pipelineManager_.GetLodBias());
        break;
    case SDLK_QUOTE:
        SetLodBias(pipelineManager_.GetLodBias() + 0.15f);
        BGL_LOG_INFO("LOD Bias increased: {:.2f}x", pipelineManager_.GetLodBias());
        break;
    case SDLK_COMMA:
        SetTimeOfDay(timeOfDay_ - 0.5f);
        BGL_LOG_INFO("Time of Day stepped back: {:02d}:{:02d}", static_cast<int>(timeOfDay_),
                     static_cast<int>((timeOfDay_ - std::floor(timeOfDay_)) * 60.0f));
        break;
    case SDLK_PERIOD:
        SetTimeOfDay(timeOfDay_ + 0.5f);
        BGL_LOG_INFO("Time of Day stepped forward: {:02d}:{:02d}", static_cast<int>(timeOfDay_),
                     static_cast<int>((timeOfDay_ - std::floor(timeOfDay_)) * 60.0f));
        break;
    case SDLK_f:
        FocusSelection();
        BGL_LOG_INFO("Framed camera on selection / scene bounds");
        break;
    case SDLK_c:
        ToggleCameraMode();
        BGL_LOG_INFO("Switched camera mode: {}",
                     camera_.GetMode() == core::CameraMode::Fly ? "First-Person Fly" : "Orbit");
        break;
    case SDLK_t:
        ToggleTurntable();
        BGL_LOG_INFO("Turntable autorotation: {}", turntableActive_ ? "ON" : "OFF");
        break;
    case SDLK_PLUS:
    case SDLK_KP_PLUS:
    case SDLK_EQUALS:
        if ((keyMod & KMOD_SHIFT) != 0) {
            AdjustExposure(0.15f);
            BGL_LOG_INFO("Exposure increased: {:.2f}", pipelineManager_.GetExposure());
        } else {
            camera_.Zoom(camera_.GetDistance() * 0.15f);
        }
        break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        if ((keyMod & KMOD_SHIFT) != 0) {
            AdjustExposure(-0.15f);
            BGL_LOG_INFO("Exposure decreased: {:.2f}", pipelineManager_.GetExposure());
        } else {
            camera_.Zoom(-camera_.GetDistance() * 0.15f);
        }
        break;
    case SDLK_LEFTBRACKET:
        RotateEnvironment(-0.0872665f); // -5 degrees
        break;
    case SDLK_RIGHTBRACKET:
        RotateEnvironment(0.0872665f); // +5 degrees
        break;
    default:
        break;
    }
}
#endif

} // namespace bgl::app
