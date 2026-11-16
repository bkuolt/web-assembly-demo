/**
 * @file CommandLine.hpp
 * @brief Command-line argument parsing and configuration for bgl_viewer.
 */
#ifndef BGL_VIEWER_COMMANDLINE_HPP
#define BGL_VIEWER_COMMANDLINE_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgl::app {

/**
 * @struct CliOptions
 * @brief Processed command-line arguments for the desktop viewer.
 */
struct CliOptions {
    bool showHelp{false};
    bool showVersion{false};
    bool listFormats{false};
    bool testCrash{false};
    int testCrashSignal{0}; // 0=SIGSEGV, 1=SIGABRT, 2=SIGFPE
    bool headless{false};   // Headless CI smoke-test mode (renders 10 frames and terminates)

    uint32_t width{1280};
    uint32_t height{720};
    std::string assetPath{};
    std::string logLevel{"info"};

    bool enableSkybox{true};
    bool enableBoundingBoxes{true};
    bool enableQuadtreeBounds{false};
    bool enableDebugOverlay{true};
    bool enableBumpMapping{true};
    bool enableMipmapping{true};
    bool enableShadows{true};
    bool enableReflections{true};
    bool enableAmbientOcclusion{true};
    bool enableOcclusionCulling{true};
    bool enableLod{true};
    bool enableAutoLod{false};
    float lodBias{1.0f};
    uint32_t debugViewMode{0}; // 0=LitPbr, 1=LodHeatmap, 2=Normals, 3=MetallicRoughnessAo, 4=UvChecker

    uint32_t weatherMode{0}; // 0=None/Clear, 1=Rain, 2=Snow, 3=Fog, 4=Smoke
    float weatherIntensity{1.0f};
    float windStrength{0.3f};
    float timeOfDay{14.0f};
    bool enableDayNightCycle{false};
    float dayNightSpeed{1.0f};

    /**
     * @brief Parses a span of command-line arguments into a validated CliOptions structure.
     */
    static CliOptions Parse(std::span<const char* const> args);

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

#endif // BGL_VIEWER_COMMANDLINE_HPP
