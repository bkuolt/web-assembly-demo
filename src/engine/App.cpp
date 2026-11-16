/**
 * @file App.cpp
 * @brief Implementation of bgl::RunApp main loop driving bgl::App lifecycle hooks.
 */
#include "bgl/App.hpp"
#include "core/CrashHandler.hpp"
#include "core/Log.hpp"
#include <chrono>

namespace bgl {

int RunApp(std::unique_ptr<App> app, int argc, char** argv) {
    (void)argc;
    (void)argv;
#if !defined(BGL_PLATFORM_NATIVE)
    (void)app;
    return -1;
#else
    if (!app) {
        return 1;
    }

    core::InstallCrashHandler();
    core::InitLogging("info");

    const EngineConfig config = app->Configure();
    auto created = Engine::Create(config);
    if (!created) {
        BGL_LOG_ERROR("Engine creation failed: {}", created.error());
        core::UninstallCrashHandler();
        return 1;
    }

    Engine& engine = **created;
    app->engine_ = &engine;
    engine.SetResizeCallback([&](uint32_t w, uint32_t h) { app->OnResize(w, h); });
    engine.SetFileDropCallback([&](std::string_view path) { app->OnFileDrop(path); });

    app->OnStart();

    auto prevTime = std::chrono::steady_clock::now();
    while (engine.PollEvents()) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;

        engine.Update(dt);
        app->OnUpdate(dt);
        if (!engine.IsRunning()) {
            break;
        }
        engine.BeginGui();
        app->OnGui();
        engine.EndGui();
        engine.Render();
    }

    app->OnShutdown();
    app->engine_ = nullptr;
    created->reset();
    core::UninstallCrashHandler();
    return 0;
#endif
}

} // namespace bgl
