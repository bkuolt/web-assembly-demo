/**
 * @file Settings.hpp
 * @brief Plain-data render settings of the bgl Engine. Modify and the engine applies changes on the next frame.
 */
#ifndef BGL_PUBLIC_SETTINGS_HPP
#define BGL_PUBLIC_SETTINGS_HPP

#include <cstdint>

namespace bgl {

/** @brief Tone mapping operator. Values match gpu::ToneMapOperator. */
enum class ToneMap : uint32_t { KhronosPbrNeutral = 0, AgX = 1, ACES = 2 };
/** @brief Weather effect. Values match gpu::WeatherMode. */
enum class Weather : uint32_t { Clear = 0, Rain = 1, Snow = 2, Fog = 3, Smoke = 4 };
/** @brief Diagnostic shading view. Values match gpu::DebugViewMode. */
enum class DebugView : uint32_t {
    LitPbr = 0,
    LodHeatmap = 1,
    Normals = 2,
    MetallicRoughnessAo = 3,
    UvChecker = 4,
    ClayWireframe = 5,
    ToonCel = 6
};
/** @brief Built-in HDR environment. Values match gpu::EnvironmentPreset (without CustomHdr). */
enum class Environment : uint32_t { MediterraneanCourtyard = 0, SunsetStudio = 1, OvercastAtelier = 2, NightNeon = 3 };
/** @brief Skybox display mode. Values match gpu::SkyboxMode. */
enum class SkyboxStyle : uint32_t { Sharp = 0, Blurred = 1, Hidden = 2 };

/**
 * @struct RenderSettings
 * @brief All user-tunable rendering options. Defaults equal the engine's built-in defaults.
 */
struct RenderSettings {
    // Lighting & materials
    bool shadows{true};
    bool reflections{true};
    bool bumpMapping{true};
    bool parallaxOcclusion{true};
    float parallaxScale{0.035f};
    bool mipmaps{true};
    bool materialAmbientOcclusion{true};
    float materialAoIntensity{1.0f};
    // Environment & sky
    Environment environment{Environment::MediterraneanCourtyard};
    float environmentRotationRad{0.0f};
    float timeOfDayHours{14.0f};
    bool skybox{true};
    SkyboxStyle skyboxStyle{SkyboxStyle::Sharp};
    // Tone mapping & post-processing
    ToneMap toneMap{ToneMap::KhronosPbrNeutral};
    float exposure{1.0f};
    bool bloom{true};
    float bloomIntensity{0.65f};
    float bloomThreshold{1.05f};
    bool ssao{true};
    float ssaoIntensity{0.75f};
    float ssaoRadius{0.02f};
    bool lensFlare{true};
    float lensFlareIntensity{0.75f};
    // Culling & level of detail
    bool occlusionCulling{true};
    bool lod{true};
    bool autoLod{false};
    float lodBias{1.0f};
    // Atmosphere
    Weather weather{Weather::Clear};
    float weatherIntensity{1.0f};
    float windStrength{0.3f};
    // Diagnostics
    DebugView debugView{DebugView::LitPbr};

    bool operator==(const RenderSettings&) const = default;
};

} // namespace bgl

#endif // BGL_PUBLIC_SETTINGS_HPP
