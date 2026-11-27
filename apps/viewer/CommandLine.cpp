/**
 * @file CommandLine.cpp
 * @brief Table-driven command-line parser and help/format output for `bgl_viewer`.
 */
#include "viewer/CommandLine.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Version.hpp"
#include "scene/TextureDecoder.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <iostream>
#include <type_traits>

namespace bgl::app {
namespace {

struct EnumChoice {
    std::string_view name;
    uint32_t value;
};

// Keep compact initializer lists (no trailing comma before `}}`) for concise multi-item layout.
constexpr std::array<EnumChoice, 5> kCrashSignalChoices{
    {{"segv", 0}, {"abrt", 1}, {"abort", 1}, {"fpe", 2}, {"div0", 2}}};
constexpr std::array<EnumChoice, 11> kWeatherChoices{{{"clear", 0},
                                                      {"none", 0},
                                                      {"0", 0},
                                                      {"rain", 1},
                                                      {"1", 1},
                                                      {"snow", 2},
                                                      {"2", 2},
                                                      {"fog", 3},
                                                      {"3", 3},
                                                      {"smoke", 4},
                                                      {"4", 4}}};
constexpr std::array<EnumChoice, 20> kDebugViewChoices{
    {{"lit", 0},  {"0", 0},         {"lod", 1},  {"heatmap", 1}, {"1", 1},   {"normals", 2}, {"normal", 2},
     {"2", 2},    {"pbr", 3},       {"mrao", 3}, {"3", 3},       {"uv", 4},  {"checker", 4}, {"4", 4},
     {"clay", 5}, {"wireframe", 5}, {"5", 5},    {"toon", 6},    {"cel", 6}, {"6", 6}}};

struct BoolOpt {
    std::string_view stem, help;
    bool CliOptions::* field;
};
template <typename T>
struct RangeOpt {
    std::string_view shortFlag, longFlag, hint, help;
    T CliOptions::* field;
    T minVal, maxVal;
    bool wrapMod24{false};
};
struct StringOpt {
    std::string_view shortFlag, longFlag, hint, help;
    std::string CliOptions::* field;
};
struct EnumOpt {
    std::string_view shortFlag, longFlag, hint, help;
    uint32_t CliOptions::* field;
    std::span<const EnumChoice> choices;
};
struct PresetOpt {
    std::string_view longFlag, help;
    uint32_t CliOptions::* field;
    uint32_t value;
};

constexpr std::array<StringOpt, 2> kStringOpts{
    {{"-a", "--asset", "<path|url>", "Explicit path or HTTP(S) URL to glTF/GLB asset", &CliOptions::assetPath},
     {"-l", "--log-level", "<level>", "Log level: trace, debug, info, warn, error (default: info)",
      &CliOptions::logLevel}}};
constexpr std::array<RangeOpt<uint32_t>, 2> kUIntOpts{
    {{"-w", "--width", "<pixels>", "Window width in pixels [64-16384] (default: 1280)", &CliOptions::width, 64, 16384},
     {"-H", "--height", "<pixels>", "Window height in pixels [64-16384] (default: 720)", &CliOptions::height, 64,
      16384}}};
constexpr std::array<BoolOpt, 13> kBoolOpts{
    {{"skybox", "Procedural atmospheric skybox", &CliOptions::enableSkybox},
     {"hud", "In-engine Debug Overlay HUD", &CliOptions::enableDebugOverlay},
     {"bounds", "3D wireframe bounding boxes", &CliOptions::enableBoundingBoxes},
     {"quadtree", "3D Quadtree ground boundaries", &CliOptions::enableQuadtreeBounds},
     {"bump", "Bump & tangent-space normal mapping", &CliOptions::enableBumpMapping},
     {"mipmaps", "Trilinear mipmapping", &CliOptions::enableMipmapping},
     {"shadows", "2048x2048 PCF directional shadows", &CliOptions::enableShadows},
     {"reflections", "Environment cubemap reflections", &CliOptions::enableReflections},
     {"ao", "glTF ambient occlusion mapping", &CliOptions::enableAmbientOcclusion},
     {"occlusion", "Quadtree hierarchical occlusion culling", &CliOptions::enableOcclusionCulling},
     {"lod", "Multi-level mesh & shader LOD system", &CliOptions::enableLod},
     {"auto-lod", "Adaptive 60 FPS Auto-LOD scaling", &CliOptions::enableAutoLod},
     {"day-night-cycle", "Automatic day/night sun & moon cycle", &CliOptions::enableDayNightCycle}}};
constexpr std::array<RangeOpt<float>, 5> kFloatOpts{
    {{"", "--lod-bias", "<0.25-4.0>", "Screen-Space Error LOD bias multiplier (default: 1.0)", &CliOptions::lodBias,
      0.25f, 4.0f},
     {"", "--weather-intensity", "<0.0-2.0>", "Weather particle/fog intensity (default: 1.0)",
      &CliOptions::weatherIntensity, 0.0f, 2.0f},
     {"", "--wind", "<-1.0-1.0>", "Lateral wind direction and speed (default: 0.3)", &CliOptions::windStrength, -1.0f,
      1.0f},
     {"", "--time", "<0.0-24.0>", "Initial diurnal time of day in hours (default: 14.0)", &CliOptions::timeOfDay, 0.0f,
      24.0f, true},
     {"", "--day-night-speed", "<0.05-12.0>", "Day/night progression speed in hrs/sec (default: 1.0)",
      &CliOptions::dayNightSpeed, 0.05f, 12.0f}}};
constexpr std::array<EnumOpt, 2> kEnumOpts{
    {{"", "--debug-view", "<lit|lod|normals|pbr|uv|clay|toon>", "Initial diagnostic shading mode (default: lit)",
      &CliOptions::debugViewMode, kDebugViewChoices},
     {"", "--weather", "<clear|rain|snow|fog|smoke>", "Initial atmospheric weather mode (default: clear)",
      &CliOptions::weatherMode, kWeatherChoices}}};
constexpr std::array<PresetOpt, 5> kPresets{
    {{"--lod-heatmap", "Start in LOD Heatmap diagnostic view", &CliOptions::debugViewMode, 1},
     {"--rain", "Start with Rain weather shader active", &CliOptions::weatherMode, 1},
     {"--snow", "Start with Snow weather shader active", &CliOptions::weatherMode, 2},
     {"--fog", "Start with Fog weather shader active", &CliOptions::weatherMode, 3},
     {"--smoke", "Start with Smoke weather shader active", &CliOptions::weatherMode, 4}}};

[[nodiscard]] std::optional<uint32_t> LookupChoice(std::span<const EnumChoice> choices,
                                                   std::string_view token) noexcept {
    const auto it = std::ranges::find(choices, token, &EnumChoice::name);
    return it != choices.end() ? std::optional<uint32_t>{it->value} : std::nullopt;
}

template <typename T>
[[nodiscard]] std::optional<T> TryParseNumber(std::string_view raw) noexcept {
    if (raw.empty()) {
        return std::nullopt;
    }
    T val{};
    const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), val);
    if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
        return std::nullopt;
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(val)) {
            return std::nullopt;
        }
    }
    return val;
}

[[nodiscard]] bool IsCliFlagToken(std::string_view tok) noexcept {
    if (tok.starts_with("--")) {
        return true;
    }
    if (tok.size() >= 2 && tok[0] == '-') {
        const unsigned char ch = static_cast<unsigned char>(tok[1]);
        // Treat `-2.0` / `-.5` as numeric arguments, not short flags.
        return !std::isdigit(ch) && ch != '.';
    }
    return false;
}

[[nodiscard]] std::optional<std::string_view> ConsumeValue(std::optional<std::string_view> inlineVal,
                                                           std::span<const char* const> args, size_t& index) noexcept {
    if (inlineVal.has_value()) {
        return inlineVal;
    }
    if (index + 1 < args.size() && args[index + 1]) {
        const std::string_view candidate{args[index + 1]};
        if (!IsCliFlagToken(candidate)) {
            ++index;
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool MatchFlag(std::string_view flag, std::string_view shortFlag, std::string_view longFlag) noexcept {
    return (!shortFlag.empty() && flag == shortFlag) || flag == longFlag;
}

template <typename Range, typename ApplyFn>
[[nodiscard]] bool DispatchValue(const Range& entries, std::string_view flag, std::optional<std::string_view> inlineVal,
                                 std::span<const char* const> args, size_t& index, ApplyFn&& apply) {
    for (const auto& entry : entries) {
        if (MatchFlag(flag, entry.shortFlag, entry.longFlag)) {
            if (const auto val = ConsumeValue(inlineVal, args, index)) {
                apply(entry, *val);
            } else {
                std::cerr << std::format("Warning: option '{}' requires a value; keeping default\n", flag);
            }
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool TryApplyOption(CliOptions& opt, std::string_view flag, std::optional<std::string_view> inlineVal,
                                  std::span<const char* const> args, size_t& index) {
    for (const auto& entry : kBoolOpts) {
        if (flag.starts_with("--no-") && flag.substr(5) == entry.stem) {
            opt.*(entry.field) = inlineVal.has_value() && (*inlineVal == "false" || *inlineVal == "0");
            return true;
        }
        if (flag.starts_with("--") && flag.substr(2) == entry.stem) {
            opt.*(entry.field) = !inlineVal.has_value() || (*inlineVal != "false" && *inlineVal != "0");
            return true;
        }
    }
    for (const auto& entry : kPresets) {
        if (flag == entry.longFlag) {
            opt.*(entry.field) = entry.value;
            return true;
        }
    }
    if (DispatchValue(kStringOpts, flag, inlineVal, args, index,
                      [&](const StringOpt& e, std::string_view val) { opt.*(e.field) = std::string{val}; })) {
        return true;
    }
    if (DispatchValue(kUIntOpts, flag, inlineVal, args, index, [&](const RangeOpt<uint32_t>& e, std::string_view val) {
            const auto n = TryParseNumber<uint32_t>(val);
            if (!n || *n < e.minVal || *n > e.maxVal) {
                std::cerr << std::format("Warning: invalid value '{}' for '{}' (expected [{}, {}]); keeping default\n",
                                         val, flag, e.minVal, e.maxVal);
                return;
            }
            opt.*(e.field) = *n;
        })) {
        return true;
    }
    if (DispatchValue(kFloatOpts, flag, inlineVal, args, index, [&](const RangeOpt<float>& e, std::string_view val) {
            const auto n = TryParseNumber<float>(val);
            if (!n || !std::isfinite(*n)) {
                std::cerr << std::format("Warning: invalid floating-point value '{}' for '{}'; keeping default\n", val,
                                         flag);
                return;
            }
            if (e.wrapMod24) {
                const float wrapped = std::fmod(*n, 24.0f);
                opt.*(e.field) = wrapped < 0.0f ? wrapped + 24.0f : wrapped;
            } else {
                opt.*(e.field) = std::clamp(*n, e.minVal, e.maxVal);
            }
        })) {
        return true;
    }
    return DispatchValue(kEnumOpts, flag, inlineVal, args, index, [&](const EnumOpt& e, std::string_view val) {
        if (const auto choice = LookupChoice(e.choices, val)) {
            opt.*(e.field) = *choice;
        } else {
            std::cerr << std::format("Warning: unknown choice '{}' for '{}'; keeping default\n", val, flag);
        }
    });
}

void PrintRow(std::string_view label, std::string_view desc) {
    std::cout << std::format("  {:<38} {}\n", label, desc);
}

template <typename OptT>
void PrintOptionRow(const OptT& opt) {
    const std::string flag =
        opt.shortFlag.empty() ? std::string{opt.longFlag} : std::format("{}, {}", opt.shortFlag, opt.longFlag);
    PrintRow(opt.hint.empty() ? flag : std::format("{} {}", flag, opt.hint), opt.help);
}

} // namespace

CliOptions CliOptions::Parse(int argc, char** argv) {
    if (argc <= 0 || !argv) {
        return CliOptions{};
    }
    return Parse(std::span<const char* const>{argv, static_cast<size_t>(argc)});
}

CliOptions CliOptions::Parse(std::span<const char* const> args) {
    CliOptions opt{};
    bool endOfOptions{false};

    for (size_t i{1}; i < args.size(); ++i) {
        const std::string_view rawArg{args[i] ? args[i] : ""};
        if (rawArg.empty()) {
            continue;
        }
        if (!endOfOptions && rawArg == "--") {
            endOfOptions = true;
            continue;
        }
        if (endOfOptions || !rawArg.starts_with('-')) {
            if (opt.assetPath.empty()) {
                opt.assetPath = std::string{rawArg};
            }
            continue;
        }

        std::string_view flag{rawArg};
        std::optional<std::string_view> inlineVal{};
        if (rawArg.starts_with("--")) {
            if (const size_t eq = rawArg.find('='); eq != std::string_view::npos) {
                flag = rawArg.substr(0, eq);
                inlineVal = rawArg.substr(eq + 1);
            }
        }

        if (MatchFlag(flag, "-h", "--help")) {
            opt.showHelp = true;
            return opt;
        }
        if (MatchFlag(flag, "-v", "--version")) {
            opt.showVersion = true;
            return opt;
        }
        if (flag == "--formats") {
            opt.listFormats = true;
            return opt;
        }
        if (flag == "--headless") {
            opt.headless = true;
            continue;
        }
        if (flag == "--test-crash") {
            opt.testCrash = true;
            std::optional<std::string_view> sig = inlineVal;
            if (!sig.has_value() && i + 1 < args.size() && args[i + 1] && args[i + 1][0] != '-') {
                sig = std::string_view{args[++i]};
            }
            if (sig.has_value()) {
                opt.testCrashSignal = static_cast<int>(LookupChoice(kCrashSignalChoices, *sig).value_or(0u));
            }
            continue;
        }

        if (!TryApplyOption(opt, flag, inlineVal, args, i)) {
            std::cerr << std::format("Warning: ignoring unrecognized option '{}'\n", rawArg);
        }
    }

    return opt;
}

std::optional<int> CliOptions::DispatchInfoAction(std::string_view programName) const {
    if (showHelp) {
        PrintHelp(programName);
        return 0;
    }
    if (showVersion) {
        PrintVersion();
        return 0;
    }
    if (listFormats) {
        PrintFormats();
        return 0;
    }
    return std::nullopt;
}

void CliOptions::PrintHelp(std::string_view programName) {
    std::string exec{std::filesystem::path{programName}.filename().string()};
    if (exec.empty()) {
        exec = "bgl_viewer";
    }
    const CliOptions defaults{};

    std::cout << std::format("================================================================================\n"
                             "{} (bgl_viewer v{})\n"
                             "================================================================================\n\n"
                             "USAGE:\n"
                             "  {} [OPTIONS] [ASSET_PATH_OR_URL]\n\n"
                             "GENERAL OPTIONS:\n",
                             kEngineName, kEngineVersion, exec);

    PrintRow("-h, --help", "Show this help message and exit");
    PrintRow("-v, --version", "Print engine version information and exit");
    PrintRow("--formats", "Display compiled asset, texture, font, and audio support");
    std::ranges::for_each(kStringOpts, [](const auto& o) { PrintOptionRow(o); });
    std::ranges::for_each(kUIntOpts, [](const auto& o) { PrintOptionRow(o); });

    std::cout << "\nRENDERING OPTIONS:\n";
    for (const auto& opt : kBoolOpts) {
        PrintRow(std::format("--[no-]{}", opt.stem),
                 std::format("{} (default: {})", opt.help, (defaults.*(opt.field)) ? "ON" : "OFF"));
    }
    std::ranges::for_each(kFloatOpts, [](const auto& o) { PrintOptionRow(o); });
    std::ranges::for_each(kEnumOpts, [](const auto& o) { PrintOptionRow(o); });
    std::ranges::for_each(kPresets, [](const auto& o) { PrintRow(o.longFlag, o.help); });

    std::cout << "\nDEBUG & DIAGNOSTICS:\n";
    PrintRow("--test-crash [segv|abrt|fpe]", "Trigger intentional crash to test signal handler & stacktrace");
    PrintRow("--headless", "Run in headless CI smoke-test mode (10 frames and exit)");

    std::cout
        << "\nINTERACTIVE CONTROLS:\n"
           "  LMB / RMB / Wheel                      Orbit / Pan / Zoom camera\n"
           "  WASD / Arrows, C, T, F, 1-9            Fly/move, Orbit/Fly mode, Turntable, Focus, Bookmarks\n"
           "  B, Q, H, K, V, L, M, R, U              Bounding boxes, Quadtree, Occlusion, Skybox, Bump, Mips, "
           "Shadows, IBL, AO\n"
           "  X, Z, E, + / -, [ / ]                  Tonemapper, Bloom, Environment preset, Exposure, Env rotation\n"
           "  Ctrl+R, Ctrl+P                         Toggle Desktop Ray Tracing, Toggle Progressive Path Tracer\n"
           "  Y / F4-F7, I / F8, , / .               Weather modes, Day/Night cycle, Time step\n"
           "  N / F9, F10-F12, ; / ', F1-F3, ESC     Debug view, LOD controls, Wireframe, Settings UI, HUD, Exit\n"
           "  Double-Click / MMB, Drag & Drop        RayPicker node pick, Load .glb/.gltf or HTTP(S) URL\n"
           "================================================================================\n";
}

void CliOptions::PrintVersion() {
    std::cout << std::format("{} (bgl_viewer version {}) [C++23 WebGPU glTF 2.1 Engine]\n", kEngineName,
                             kEngineVersion);
}

void CliOptions::PrintFormats() {
    struct FeatureRow {
        std::string_view label;
        bool enabled;
        std::string_view backend;
    };
    const std::array<FeatureRow, 8> rows{
        {{"glTF 2.0 / 2.1 (.gltf, .glb)", true, "fastgltf SIMD parser"},
         {"HTTP/HTTPS Asset Streaming", core::NetworkFetcher::IsCurlSupported(), "libcurl / Fetch API"},
         {"PNG textures", scene::TextureDecoder::IsPngSupported(), "libpng"},
         {"JPEG textures", scene::TextureDecoder::IsJpegSupported(), "libjpeg-turbo"},
         {"WebP textures", scene::TextureDecoder::IsWebpSupported(), "libwebp"},
         {"KTX / KTX2 (Basis Universal)", scene::TextureDecoder::IsKtxSupported(), "libktx"},
#if defined(BGL_ENABLE_AUDIO)
         {"Audio (WAV, MP3, FLAC)", true, "miniaudio"},
#else
         {"Audio (WAV, MP3, FLAC)", false, "disabled"},
#endif
         {"Fonts (TTF, OTF)", true, "FreeType + HarfBuzz"}}};

    std::cout << "================================================================================\n"
                 "bgl-Engine Compiled Asset & Texture Decoders:\n"
                 "================================================================================\n";
    for (const auto& row : rows) {
        std::cout << std::format("  - {:<30} {} ({})\n", row.label, row.enabled ? "[ENABLED]" : "[DISABLED]",
                                 row.backend);
    }
    std::cout << "================================================================================\n";
}

} // namespace bgl::app
