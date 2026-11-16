/**
 * @file main.cpp
 * @brief Entrypoint for the native desktop bgl_viewer application.
 */
#include "core/CrashHandler.hpp"
#include "core/Log.hpp"
#include "viewer/Application.hpp"
#include "viewer/CommandLine.hpp"
#include <chrono>

#if !defined(BGL_PLATFORM_WASM)
int main(int argc, char** argv) {
    // 1. Install crash signal handling immediately
    bgl::core::InstallCrashHandler();

    // 2. Parse command-line arguments and run early-exit info commands
    const std::string_view progName{(argc > 0 && argv && argv[0]) ? argv[0] : "bgl_viewer"};
    const auto cli = bgl::app::CliOptions::Parse(argc, argv);
    if (const auto exitCode = cli.DispatchInfoAction(progName); exitCode.has_value()) {
        return *exitCode;
    }

    // 3. Initialize logging and handle optional intentional crash test
    bgl::core::InitLogging(cli.logLevel);
    if (cli.testCrash) {
        BGL_LOG_WARN("Executing intentional test crash (signal type {})...", cli.testCrashSignal);
        bgl::core::TriggerTestCrash(cli.testCrashSignal);
    }

    // 4. Configure and initialize Application directly from CliOptions (derives from ApplicationConfig)
    bgl::app::Application app;
    if (auto res = app.Initialize(cli); !res) {
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
