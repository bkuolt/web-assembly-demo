/**
 * @file WebExports.cpp
 * @brief Emscripten HTML5 main loop and C-ABI exports for the WebAssembly viewer build.
 */
#include "viewer/Application.hpp"

#if defined(BGL_PLATFORM_WASM) && defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

static bgl::app::Application* g_pApp = nullptr;

static void EmscriptenMainLoop() {
    if (g_pApp) {
        g_pApp->Update(1.0f / 60.0f);
        g_pApp->Render();
    }
}

extern "C" {
EMSCRIPTEN_KEEPALIVE
void bgl_toggle_bbox() {
    if (g_pApp)
        g_pApp->ToggleBoundingBoxes();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_quadtree() {
    if (g_pApp)
        g_pApp->ToggleQuadtreeBounds();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_skybox() {
    if (g_pApp)
        g_pApp->ToggleSkybox();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_debug_overlay() {
    if (g_pApp)
        g_pApp->ToggleDebugOverlay();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_bump() {
    if (g_pApp)
        g_pApp->ToggleBumpMapping();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_mipmaps() {
    if (g_pApp)
        g_pApp->ToggleMipmapping();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_shadows() {
    if (g_pApp)
        g_pApp->ToggleShadows();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_reflections() {
    if (g_pApp)
        g_pApp->ToggleReflections();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_ao() {
    if (g_pApp)
        g_pApp->ToggleAmbientOcclusion();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_pom() {
    if (g_pApp)
        g_pApp->ToggleParallaxOcclusion();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_lens_flare() {
    if (g_pApp)
        g_pApp->ToggleLensFlare();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_ssao() {
    if (g_pApp)
        g_pApp->ToggleSSAO();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_bloom() {
    if (g_pApp)
        g_pApp->ToggleBloom();
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_ao_intensity(float val) {
    if (g_pApp)
        g_pApp->SetAoIntensity(val);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_ssao_intensity(float val) {
    if (g_pApp)
        g_pApp->SetSsaoIntensity(val);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_bloom_intensity(float val) {
    if (g_pApp)
        g_pApp->SetBloomIntensity(val);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_parallax_scale(float val) {
    if (g_pApp)
        g_pApp->SetParallaxScale(val);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_lens_flare_intensity(float val) {
    if (g_pApp)
        g_pApp->SetLensFlareIntensity(val);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_weather_mode(int mode) {
    if (g_pApp) {
        const auto m = (mode == 1)   ? bgl::gpu::WeatherMode::Rain
                       : (mode == 2) ? bgl::gpu::WeatherMode::Snow
                       : (mode == 3) ? bgl::gpu::WeatherMode::Fog
                       : (mode == 4) ? bgl::gpu::WeatherMode::Smoke
                                     : bgl::gpu::WeatherMode::None;
        g_pApp->SetWeatherMode(m);
    }
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_rain() {
    if (g_pApp)
        g_pApp->ToggleRain();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_snow() {
    if (g_pApp)
        g_pApp->ToggleSnow();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_fog() {
    if (g_pApp)
        g_pApp->ToggleFog();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_smoke() {
    if (g_pApp)
        g_pApp->ToggleSmoke();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_day_night_cycle() {
    if (g_pApp)
        g_pApp->ToggleDayNightCycle();
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_day_night_speed(float hoursPerSec) {
    if (g_pApp)
        g_pApp->SetDayNightSpeed(hoursPerSec);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_weather_intensity(float val) {
    if (g_pApp)
        g_pApp->SetWeatherIntensity(val);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_wind_strength(float val) {
    if (g_pApp)
        g_pApp->SetWindStrength(val);
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_move(float forward, float right, float up) {
    if (g_pApp) {
        if (std::abs(forward) > 1e-4f)
            g_pApp->GetCamera().MoveForward(forward);
        if (std::abs(right) > 1e-4f)
            g_pApp->GetCamera().MoveRight(right);
        if (std::abs(up) > 1e-4f)
            g_pApp->GetCamera().MoveUp(up);
    }
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_orbit(float deltaYaw, float deltaPitch) {
    if (g_pApp)
        g_pApp->GetCamera().Orbit(deltaYaw, deltaPitch);
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_pan(float deltaX, float deltaY) {
    if (g_pApp)
        g_pApp->GetCamera().Pan(deltaX, deltaY);
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_focus_selection() {
    if (g_pApp)
        g_pApp->FocusSelection();
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_frame_all() {
    if (g_pApp)
        g_pApp->FrameAll();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_camera_mode() {
    if (g_pApp)
        g_pApp->ToggleCameraMode();
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_save_bookmark(int slot) {
    if (g_pApp && slot >= 0)
        g_pApp->GetCamera().SaveBookmark(static_cast<size_t>(slot));
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_restore_bookmark(int slot) {
    if (g_pApp && slot >= 0)
        g_pApp->GetCamera().RestoreBookmark(static_cast<size_t>(slot));
}

EMSCRIPTEN_KEEPALIVE
int bgl_pick_node(float screenX, float screenY, float screenWidth, float screenHeight) {
    if (g_pApp)
        return g_pApp->PickNodeAt(screenX, screenY, screenWidth, screenHeight);
    return -1;
}

EMSCRIPTEN_KEEPALIVE
void bgl_select_node(int nodeIndex) {
    if (g_pApp)
        g_pApp->SelectNodeByIndex(nodeIndex);
}

EMSCRIPTEN_KEEPALIVE
void bgl_clear_selection() {
    if (g_pApp)
        g_pApp->ClearSelection();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_turntable() {
    if (g_pApp)
        g_pApp->ToggleTurntable();
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_turntable(int enabled) {
    if (g_pApp)
        g_pApp->SetTurntable(enabled != 0);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_turntable_speed(float speed) {
    if (g_pApp)
        g_pApp->SetTurntableSpeed(speed);
}

EMSCRIPTEN_KEEPALIVE
int bgl_get_camera_count() {
    if (g_pApp)
        return g_pApp->GetCameraCount();
    return 0;
}

EMSCRIPTEN_KEEPALIVE
const char* bgl_get_camera_name(int index) {
    static std::string s_camName;
    if (g_pApp) {
        s_camName = std::string(g_pApp->GetCameraName(index));
        return s_camName.c_str();
    }
    return "";
}

EMSCRIPTEN_KEEPALIVE
void bgl_select_camera(int index) {
    if (g_pApp)
        g_pApp->SelectCamera(index);
}

EMSCRIPTEN_KEEPALIVE
void bgl_cycle_camera() {
    if (g_pApp)
        g_pApp->CycleCamera();
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_time_of_day(float hour) {
    if (g_pApp)
        g_pApp->SetTimeOfDay(hour);
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_sun_angle(float azimuthRad, float elevationRad) {
    if (g_pApp)
        g_pApp->SetSunAzimuthElevation(azimuthRad, elevationRad);
}

EMSCRIPTEN_KEEPALIVE
void bgl_gamepad_state(float lx, float ly, float rx, float ry, float lt, float rt, int btnA, int btnB, int btnX,
                       int btnY) {
    if (g_pApp) {
        bgl::core::GamepadState raw{};
        raw.connected = true;
        raw.leftStick = glm::vec2{lx, ly};
        raw.rightStick = glm::vec2{rx, ry};
        raw.leftTrigger = lt;
        raw.rightTrigger = rt;
        raw.buttonA = btnA != 0;
        raw.buttonB = btnB != 0;
        raw.buttonX = btnX != 0;
        raw.buttonY = btnY != 0;
        g_pApp->GetGamepad().SetRawState(raw);
    }
}

EMSCRIPTEN_KEEPALIVE
void bgl_camera_zoom(float deltaZoom) {
    if (g_pApp)
        g_pApp->GetCamera().Zoom(deltaZoom);
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_occlusion() {
    if (g_pApp)
        g_pApp->ToggleOcclusionCulling();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_lod() {
    if (g_pApp)
        g_pApp->ToggleLod();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_auto_lod() {
    if (g_pApp)
        g_pApp->ToggleAutoLod();
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_lod_bias(float bias) {
    if (g_pApp)
        g_pApp->SetLodBias(bias);
}

EMSCRIPTEN_KEEPALIVE
void bgl_cycle_debug_view() {
    if (g_pApp)
        g_pApp->CycleDebugViewMode();
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_debug_view(int mode) {
    if (g_pApp) {
        const auto m = (mode == 1)   ? bgl::gpu::DebugViewMode::LodHeatmap
                       : (mode == 2) ? bgl::gpu::DebugViewMode::Normals
                       : (mode == 3) ? bgl::gpu::DebugViewMode::MetallicRoughnessAo
                       : (mode == 4) ? bgl::gpu::DebugViewMode::UvChecker
                                     : bgl::gpu::DebugViewMode::LitPbr;
        g_pApp->SetDebugViewMode(m);
    }
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_lod_heatmap() {
    if (g_pApp)
        g_pApp->ToggleLodHeatmap();
}

EMSCRIPTEN_KEEPALIVE
void bgl_load_file_buffer(const char* name, const uint8_t* buffer, size_t size) {
    if (g_pApp && name && buffer && size > 0) {
        g_pApp->OnFileDrop(name, std::span<const uint8_t>(buffer, size));
    }
}

EMSCRIPTEN_KEEPALIVE
void bgl_load_url(const char* url) {
    if (g_pApp && url) {
        (void)g_pApp->LoadSceneFromUri(url);
    }
}

EMSCRIPTEN_KEEPALIVE
int bgl_get_animation_count() {
    return g_pApp ? static_cast<int>(g_pApp->GetAnimationCount()) : 0;
}

EMSCRIPTEN_KEEPALIVE
int bgl_get_active_animation() {
    return g_pApp ? g_pApp->GetActiveAnimationIndex() : -1;
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_animation(int index) {
    if (g_pApp) {
        g_pApp->SetAnimation(index);
    }
}

EMSCRIPTEN_KEEPALIVE
void bgl_set_animation_playing(int playing) {
    if (g_pApp) {
        g_pApp->SetAnimationPlaying(playing != 0);
    }
}
}

int main() {
    static bgl::app::Application app;
    g_pApp = &app;
    if (auto res = app.Initialize(1280, 720); !res) {
        return -1;
    }
    emscripten_set_main_loop(EmscriptenMainLoop, 0, 1);
    return 0;
}

#endif
