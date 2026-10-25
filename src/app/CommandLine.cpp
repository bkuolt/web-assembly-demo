/**
 * @file CommandLine.cpp
 * @brief Implementation of command-line parsing and CLI utilities.
 */
#include "app/CommandLine.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Version.hpp"
#include "scene/TextureDecoder.hpp"
#include <iostream>
#include <format>
#include <filesystem>
#include <algorithm>

namespace bgl::app {

CliOptions CliOptions::Parse(int argc, char** argv) {
    if (argc <= 0 || !argv) return CliOptions{};
    return Parse(std::span<const char* const>{argv, static_cast<size_t>(argc)});
}

CliOptions CliOptions::Parse(std::span<const char* const> args) {
    CliOptions opt{};

    for (size_t i{1}; i < args.size(); ++i) {
        std::string_view arg{args[i]};

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
            if (i + 1 < args.size() && args[i + 1][0] != '-') {
                std::string_view type{args[++i]};
                if (type == "abrt" || type == "abort") {
                    opt.testCrashSignal = 1;
                } else if (type == "fpe" || type == "div0") {
                    opt.testCrashSignal = 2;
                } else {
                    opt.testCrashSignal = 0;
                }
            }
        } else if ((arg == "-a" || arg == "--asset") && i + 1 < args.size()) {
            opt.assetPath = args[++i];
        } else if ((arg == "-w" || arg == "--width") && i + 1 < args.size()) {
            try {
                const long val = std::stol(args[++i]);
                if (val >= 64 && val <= 16384) {
                    opt.width = static_cast<uint32_t>(val);
                }
            } catch (...) {}
        } else if ((arg == "-H" || arg == "--height") && i + 1 < args.size()) {
            try {
                const long val = std::stol(args[++i]);
                if (val >= 64 && val <= 16384) {
                    opt.height = static_cast<uint32_t>(val);
                }
            } catch (...) {}
        } else if ((arg == "-l" || arg == "--log-level") && i + 1 < args.size()) {
            opt.logLevel = args[++i];
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
        } else if (arg == "--no-bump") {
            opt.enableBumpMapping = false;
        } else if (arg == "--bump") {
            opt.enableBumpMapping = true;
        } else if (arg == "--no-mipmaps") {
            opt.enableMipmapping = false;
        } else if (arg == "--mipmaps") {
            opt.enableMipmapping = true;
        } else if (arg == "--no-shadows") {
            opt.enableShadows = false;
        } else if (arg == "--shadows") {
            opt.enableShadows = true;
        } else if (arg == "--no-reflections") {
            opt.enableReflections = false;
        } else if (arg == "--reflections") {
            opt.enableReflections = true;
        } else if (arg == "--no-ao") {
            opt.enableAmbientOcclusion = false;
        } else if (arg == "--ao") {
            opt.enableAmbientOcclusion = true;
        } else if (arg == "--no-occlusion") {
            opt.enableOcclusionCulling = false;
        } else if (arg == "--occlusion") {
            opt.enableOcclusionCulling = true;
        } else if (arg == "--headless") {
            opt.headless = true;
        } else if (!arg.starts_with("-")) {
            // Positional argument interpreted as asset path if not set
            if (opt.assetPath.empty()) {
                opt.assetPath = std::string{arg};
            }
        }
    }

    return opt;
}

void CliOptions::PrintHelp(std::string_view programName) {
    std::string exec{std::filesystem::path{programName}.filename().string()};
    if (exec.empty()) exec = "bgl_viewer";

    std::cout <<
std::format(R"(================================================================================
{} (bgl_viewer v{})
================================================================================

USAGE:
  {} [OPTIONS] [ASSET_PATH_OR_URL]

ARGUMENTS:
  [ASSET_PATH_OR_URL]           Local path or HTTP/HTTPS URL to .glb/.gltf model
                                (e.g. assets/Sponza.glb or https://.../Model.glb)

GENERAL OPTIONS:
  -h, --help                    Show this help message and exit
  -v, --version                 Print engine version information and exit
  --formats                     Display compiled texture decoders and asset support
  -a, --asset <path|url>        Explicit path or HTTP/HTTPS URL to glTF/GLB asset
  -w, --width <pixels>          Window width in pixels (default: 1280)
  -H, --height <pixels>         Window height in pixels (default: 720)
  -l, --log-level <level>       Set log level: trace, debug, info, warn, error (default: info)

RENDERING TOGGLES:
  --no-skybox                   Start with procedural atmospheric skybox disabled
  --no-hud                      Start with in-engine Debug Overlay HUD hidden
  --no-bounds                   Start with 3D wireframe bounding boxes hidden
  --bounds                      Start with 3D wireframe bounding boxes visible
  --quadtree                    Start with 3D Quadtree ground boundaries visible
  --no-bump / --bump            Disable / enable Bump & Normal Mapping (default: ON)
  --no-mipmaps / --mipmaps      Disable / enable Trilinear Mipmapping (default: ON)
  --no-shadows / --shadows      Disable / enable 2048x2048 PCF Shadow Mapping (default: ON)
  --no-reflections / --reflections Disable / enable Environment Cubemap Reflections (default: ON)
  --no-ao / --ao                Disable / enable glTF Ambient Occlusion (default: ON)
  --no-occlusion / --occlusion  Disable / enable Quadtree Hierarchical Occlusion Culling (default: ON)

DEBUG & DIAGNOSTICS:
  --test-crash [segv|abrt|fpe]  Trigger intentional crash to test signal handler and stacktrace
  --headless                    Run in headless CI smoke-test mode (renders 10 frames and exits)

INTERACTIVE CONTROLS:
  LMB + Drag                    Orbit camera around target
  RMB / Shift + Drag            Pan camera laterally and vertically
  Mouse Wheel                   Smooth zoom in / out
  B                             Toggle 3D Wireframe Bounding Boxes & AABBs
  Q                             Toggle 3D Quadtree Ground Partition Boundaries
  H                             Toggle Quadtree Hierarchical Occlusion Culling (Hi-Z)
  K                             Cycle Skybox Mode (Sharp / Blurred / Hidden)
  V                             Toggle Bump & Tangent-Space Normal Mapping
  L                             Toggle Trilinear Mipmapping (16x Anisotropic vs Mip 0)
  M                             Toggle Directional Shadow Mapping (2048x2048 PCF)
  R                             Toggle Environment Cubemap Reflections & Specular IBL
  U                             Toggle Ambient Occlusion & Micro-Cavity Shading
  X                             Cycle Tonemapper (Khronos PBR Neutral / AgX / ACES)
  Z                             Toggle Emissive HDR Bloom
  C                             Switch Camera Navigation Mode (Orbit / First-Person Fly)
  T                             Toggle Turntable Autorotation
  F                             Focus Camera on Selection or Scene Bounds
  E                             Cycle Environment Preset
  + / -                         Increase / Decrease Exposure
  [ / ]                         Rotate Environment (-5° / +5°)
  O / F3                        Toggle In-Engine FreeType 2 & HarfBuzz Debug Overlay
  Double-Click / MMB            Pick geometry node with RayPicker
  ESC                           Exit viewer
  Drag & Drop                   Drop any .glb/.gltf file or HTTP/HTTPS URL onto the window
================================================================================
)", kEngineName, kEngineVersion, exec);
}

void CliOptions::PrintVersion() {
    std::cout << std::format("{} (bgl_viewer version {}) [C++23 WebGPU glTF 2.1 Engine]\n",
                             kEngineName, kEngineVersion);
}

void CliOptions::PrintFormats() {
    std::cout << "================================================================================\n"
              << "bgl-Engine Compiled Asset & Texture Decoders:\n"
              << "================================================================================\n"
              << "  - glTF 2.0 / 2.1 (.gltf, .glb): [ENABLED] (fastgltf SIMD parser)\n"
              << "  - HTTP/HTTPS Asset Streaming:  " << (core::NetworkFetcher::IsCurlSupported() ? "[ENABLED] (libcurl / Fetch API)" : "[DISABLED]") << "\n"
              << "  - PNG textures:                " << (scene::TextureDecoder::IsPngSupported() ? "[ENABLED] (libpng)" : "[DISABLED]") << "\n"
              << "  - JPEG textures:               " << (scene::TextureDecoder::IsJpegSupported() ? "[ENABLED] (libjpeg-turbo)" : "[DISABLED]") << "\n"
              << "  - WebP textures:               " << (scene::TextureDecoder::IsWebpSupported() ? "[ENABLED] (libwebp)" : "[DISABLED]") << "\n"
              << "  - KTX / KTX2 (Basis Universal): " << (scene::TextureDecoder::IsKtxSupported() ? "[ENABLED] (libktx 4.3.2)" : "[DISABLED]") << "\n"
              << "  - Audio (WAV, MP3, FLAC):      [ENABLED] (miniaudio)\n"
              << "  - Fonts (TTF, OTF):            [ENABLED] (FreeType 2.13.2 + HarfBuzz 8.3.0)\n"
              << "================================================================================\n";
}

} // namespace bgl::app

