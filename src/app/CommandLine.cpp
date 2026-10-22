/**
 * @file CommandLine.cpp
 * @brief Implementation of command-line parsing and CLI utilities.
 */
#include "app/CommandLine.hpp"
#include "scene/TextureDecoder.hpp"
#include <iostream>
#include <format>
#include <filesystem>

namespace bgl::app {

CliOptions CliOptions::Parse(int argc, char** argv) {
    CliOptions opt;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opt.showHelp = true;
            return opt;
        } else if (arg == "-v" || arg == "--version") {
            opt.showVersion = true;
            return opt;
        } else if (arg == "--formats") {
            opt.listFormats = true;
            return opt;
        } else if (arg == "--test-crash") {
            opt.testCrash = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string_view type = argv[++i];
                if (type == "abrt" || type == "abort") {
                    opt.testCrashSignal = 1;
                } else if (type == "fpe" || type == "div0") {
                    opt.testCrashSignal = 2;
                } else {
                    opt.testCrashSignal = 0;
                }
            }
        } else if ((arg == "-a" || arg == "--asset") && i + 1 < argc) {
            opt.assetPath = argv[++i];
        } else if ((arg == "-w" || arg == "--width") && i + 1 < argc) {
            try {
                opt.width = static_cast<uint32_t>(std::stoul(argv[++i]));
            } catch (...) {}
        } else if ((arg == "-H" || arg == "--height") && i + 1 < argc) {
            try {
                opt.height = static_cast<uint32_t>(std::stoul(argv[++i]));
            } catch (...) {}
        } else if ((arg == "-l" || arg == "--log-level") && i + 1 < argc) {
            opt.logLevel = argv[++i];
        } else if (arg == "--no-skybox") {
            opt.enableSkybox = false;
        } else if (arg == "--no-hud") {
            opt.enableDebugOverlay = false;
        } else if (arg == "--no-bounds") {
            opt.enableBoundingBoxes = false;
        } else if (arg == "--bounds") {
            opt.enableBoundingBoxes = true;
        } else if (arg == "--quadtree") {
            opt.enableQuadtreeBounds = true;
        } else if (!arg.starts_with("-")) {
            // Positional argument interpreted as asset path if not set
            if (opt.assetPath.empty()) {
                opt.assetPath = std::string(arg);
            }
        }
    }

    return opt;
}

void CliOptions::PrintHelp(std::string_view programName) {
    std::string exec = std::filesystem::path(programName).filename().string();
    if (exec.empty()) exec = "bgl_viewer";

    std::cout <<
R"(================================================================================
bgl_viewer - C++23 WebGPU & glTF 2.1 High-Performance Renderer (v0.6.8)
================================================================================

USAGE:
  )" << exec << R"( [OPTIONS] [ASSET_PATH]

ARGUMENTS:
  [ASSET_PATH]                  Path to .glb or .gltf model to load (e.g. assets/Sponza.glb)

GENERAL OPTIONS:
  -h, --help                    Show this help message and exit
  -v, --version                 Print engine version information and exit
  --formats                     Display compiled texture decoders and asset support
  -a, --asset <path>            Explicit path to glTF/GLB asset file
  -w, --width <pixels>          Window width in pixels (default: 1280)
  -H, --height <pixels>         Window height in pixels (default: 720)
  -l, --log-level <level>       Set log level: trace, debug, info, warn, error (default: info)

RENDERING TOGGLES:
  --no-skybox                   Start with procedural atmospheric skybox disabled
  --no-hud                      Start with in-engine Debug Overlay HUD hidden
  --no-bounds                   Start with 3D wireframe bounding boxes hidden
  --bounds                      Start with 3D wireframe bounding boxes visible
  --quadtree                    Start with 3D Quadtree ground boundaries visible

DEBUG & DIAGNOSTICS:
  --test-crash [segv|abrt|fpe]  Trigger intentional crash to test signal handler and stacktrace

INTERACTIVE CONTROLS:
  LMB + Drag                    Orbit camera around target
  RMB / Shift + Drag            Pan camera laterally and vertically
  Mouse Wheel                   Smooth zoom in / out
  B                             Toggle 3D Wireframe Bounding Boxes & AABBs
  Q                             Toggle 3D Quadtree Ground Partition Boundaries
  K                             Toggle Procedural Atmospheric Skybox
  O / F3                        Toggle In-Engine FreeType 2 & HarfBuzz Debug Overlay
  ESC                           Exit viewer
  Drag & Drop                   Drop any .glb/.gltf file directly onto the window
================================================================================
)";
}

void CliOptions::PrintVersion() {
    std::cout << "bgl_viewer version 0.6.8 [C++23 WebGPU glTF 2.1 Engine]\n";
}

void CliOptions::PrintFormats() {
    std::cout << "================================================================================\n"
              << "bgl-Engine Compiled Asset & Texture Decoders:\n"
              << "================================================================================\n"
              << "  - glTF 2.0 / 2.1 (.gltf, .glb): [ENABLED] (fastgltf SIMD parser)\n"
              << "  - PNG textures:                " << (scene::TextureDecoder::IsPngSupported() ? "[ENABLED] (libpng)" : "[DISABLED]") << "\n"
              << "  - JPEG textures:               " << (scene::TextureDecoder::IsJpegSupported() ? "[ENABLED] (libjpeg-turbo)" : "[DISABLED]") << "\n"
              << "  - WebP textures:               " << (scene::TextureDecoder::IsWebpSupported() ? "[ENABLED] (libwebp)" : "[DISABLED]") << "\n"
              << "  - KTX / KTX2 (Basis Universal): " << (scene::TextureDecoder::IsKtxSupported() ? "[ENABLED] (libktx 4.3.2)" : "[DISABLED]") << "\n"
              << "  - Audio (WAV, MP3, FLAC):      [ENABLED] (miniaudio)\n"
              << "  - Fonts (TTF, OTF):            [ENABLED] (FreeType 2.13.2 + HarfBuzz 8.3.0)\n"
              << "================================================================================\n";
}

} // namespace bgl::app

