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
    config.enableLod = cli.enableLod;
    config.enableAutoLod = cli.enableAutoLod;
    config.lodBias = cli.lodBias;
    config.debugViewMode = static_cast<bgl::gpu::DebugViewMode>(cli.debugViewMode);
    config.weatherMode = static_cast<bgl::gpu::WeatherMode>(cli.weatherMode);
    config.weatherIntensity = cli.weatherIntensity;
    config.windStrength = cli.windStrength;
    config.timeOfDay = cli.timeOfDay;
    config.enableDayNightCycle = cli.enableDayNightCycle;
    config.dayNightSpeed = cli.dayNightSpeed;

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
