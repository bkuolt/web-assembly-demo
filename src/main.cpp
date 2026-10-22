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
void bgl_load_file_buffer(const char* name, const uint8_t* buffer, size_t size) {
    if (g_pApp && name && buffer && size > 0) {
        g_pApp->OnFileDrop(name, std::span<const uint8_t>(buffer, size));
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

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    bgl::app::Application app;
    if (auto res = app.Initialize(1280, 720); !res) {
        BGL_LOG_ERROR("Initialization error: {}", res.error());
        return -1;
    }

    BGL_LOG_INFO("bgl_viewer native runner started. Simulating frame ticks...");
    for (int frame = 0; frame < 5; ++frame) {
        app.Update(0.016f);
        app.Render();
    }

    app.Shutdown();
    BGL_LOG_INFO("bgl_viewer native execution finished normally.");
    return 0;
}

#endif

