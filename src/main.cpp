/**
 * @file main.cpp
 * @brief Entrypoint for bgl_viewer native desktop application and Emscripten main loop.
 */
#include "app/Application.hpp"
#include "core/Log.hpp"

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

#else

#include "app/CommandLine.hpp"
#include "core/CrashHandler.hpp"
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    // 1. Install crash signal handling immediately
    bgl::core::InstallCrashHandler();

    // 2. Parse command-line arguments
    auto cli = bgl::app::CliOptions::Parse(argc, argv);

    if (cli.showHelp) {
        bgl::app::CliOptions::PrintHelp(argv[0]);
        return 0;
    }

    if (cli.showVersion) {
        bgl::app::CliOptions::PrintVersion();
        return 0;
    }

    if (cli.listFormats) {
        bgl::app::CliOptions::PrintFormats();
        return 0;
    }

    if (cli.testCrash) {
        bgl::core::InitLogging(cli.logLevel);
        BGL_LOG_WARN("Executing intentional test crash (signal type {})...", cli.testCrashSignal);
        bgl::core::TriggerTestCrash(cli.testCrashSignal);
    }

    // 3. Initialize logging with CLI level
    bgl::core::InitLogging(cli.logLevel);

    // 4. Configure and initialize Application
    bgl::app::ApplicationConfig config;
    config.width = cli.width;
    config.height = cli.height;
    config.assetPath = cli.assetPath;
    config.enableSkybox = cli.enableSkybox;
    config.enableBoundingBoxes = cli.enableBoundingBoxes;
    config.enableQuadtreeBounds = cli.enableQuadtreeBounds;
    config.enableDebugOverlay = cli.enableDebugOverlay;
    config.enableBumpMapping = cli.enableBumpMapping;
    config.enableMipmapping = cli.enableMipmapping;
    config.enableShadows = cli.enableShadows;
    config.enableReflections = cli.enableReflections;
    config.enableAmbientOcclusion = cli.enableAmbientOcclusion;
    config.enableOcclusionCulling = cli.enableOcclusionCulling;

    bgl::app::Application app;
    if (auto res = app.Initialize(config); !res) {
        BGL_LOG_ERROR("Initialization error: {}", res.error());
        return -1;
    }

    BGL_LOG_INFO("bgl_viewer interactive loop running. Close the window or press ESC to exit.");
    auto prevTime = std::chrono::high_resolution_clock::now();
    uint32_t frameCount = 0;

    while (app.IsRunning()) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - prevTime).count();
        prevTime = currentTime;

        app.Update(deltaTime);
        app.Render();

        if (cli.headless && ++frameCount >= 10) {
            BGL_LOG_INFO("Headless CI smoke test completed successfully (10 frames rendered).");
            break;
        }
    }

    app.Shutdown();
    bgl::core::UninstallCrashHandler();
    BGL_LOG_INFO("bgl_viewer execution finished cleanly.");
    return 0;
}

#endif
