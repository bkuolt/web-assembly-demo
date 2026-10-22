/**
 * @file CommandLine.hpp
 * @brief Command-line argument parsing and configuration for bgl_viewer.
 */
#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <vector>

namespace bgl::app {

/**
 * @struct CliOptions
 * @brief Processed command-line arguments for the desktop viewer.
 */
struct CliOptions {
    bool showHelp = false;
    bool showVersion = false;
    bool listFormats = false;
    bool testCrash = false;
    int testCrashSignal = 0; // 0=SIGSEGV, 1=SIGABRT, 2=SIGFPE

    uint32_t width = 1280;
    uint32_t height = 720;
    std::string assetPath = "";
    std::string logLevel = "info";

    bool enableSkybox = true;
    bool enableBoundingBoxes = true;
    bool enableQuadtreeBounds = false;
    bool enableDebugOverlay = true;
    bool enableBumpMapping = true;
    bool enableMipmapping = true;
    bool enableShadows = true;
    bool enableReflections = true;

    /**
     * @brief Parses argc and argv into a validated CliOptions structure.
     */
    static CliOptions Parse(int argc, char** argv);

    /**
     * @brief Prints standard help text showing all options and hotkey controls.
     */
    static void PrintHelp(std::string_view programName);

    /**
     * @brief Prints version information.
     */
    static void PrintVersion();

    /**
     * @brief Prints supported texture and asset formats.
     */
    static void PrintFormats();
};

} // namespace bgl::app

