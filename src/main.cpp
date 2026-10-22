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
    if (g_pApp) g_pApp->ToggleBoundingBoxes();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_quadtree() {
    if (g_pApp) g_pApp->ToggleQuadtreeBounds();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_skybox() {
    if (g_pApp) g_pApp->ToggleSkybox();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_debug_overlay() {
    if (g_pApp) g_pApp->ToggleDebugOverlay();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_bump() {
    if (g_pApp) g_pApp->ToggleBumpMapping();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_mipmaps() {
    if (g_pApp) g_pApp->ToggleMipmapping();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_shadows() {
    if (g_pApp) g_pApp->ToggleShadows();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_reflections() {
    if (g_pApp) g_pApp->ToggleReflections();
}

EMSCRIPTEN_KEEPALIVE
void bgl_toggle_occlusion() {
    if (g_pApp) g_pApp->ToggleOcclusionCulling();
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

#include <chrono>
#include <thread>
#include "app/CommandLine.hpp"
#include "core/CrashHandler.hpp"

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
    config.enableOcclusionCulling = cli.enableOcclusionCulling;

    bgl::app::Application app;
    if (auto res = app.Initialize(config); !res) {
        BGL_LOG_ERROR("Initialization error: {}", res.error());
        return -1;
    }

    BGL_LOG_INFO("bgl_viewer interactive loop running. Close the window or press ESC to exit.");
    auto prevTime = std::chrono::high_resolution_clock::now();

    while (app.IsRunning()) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - prevTime).count();
        prevTime = currentTime;

        app.Update(deltaTime);
        app.Render();
    }

    app.Shutdown();
    bgl::core::UninstallCrashHandler();
    BGL_LOG_INFO("bgl_viewer execution finished cleanly.");
    return 0;
}

#endif

