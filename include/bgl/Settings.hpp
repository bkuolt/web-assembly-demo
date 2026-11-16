/**
 * @file Settings.hpp
 * @brief Plain-data render settings of the bgl Engine. Modify and the engine applies changes on the next frame.
 */
#ifndef BGL_PUBLIC_SETTINGS_HPP
#define BGL_PUBLIC_SETTINGS_HPP

#include <cstdint>

namespace bgl {

/** @brief Tone mapping operator. Underlying values match `gpu::ToneMapOperator`. */
enum class ToneMap : uint32_t {
    KhronosPbrNeutral = 0, ///< Color-preserving neutral PBR tone curve.
    AgX = 1,               ///< Filmic highlight-desaturating curve (Blender 4 style).
    ACES = 2               ///< High-contrast Academy Color Encoding System curve.
};
/** @brief Atmospheric weather effect. Underlying values match `gpu::WeatherMode`. */
enum class Weather : uint32_t {
    Clear = 0, ///< Clear sky with no weather particles.
    Rain = 1,  ///< Streaking raindrops and procedural surface puddles.
    Snow = 2,  ///< Drifting snowflakes and horizontal accumulation tint.
    Fog = 3,   ///< Exponential height/distance atmospheric mist.
    Smoke = 4  ///< Buoyant turbulent volumetric smoke plumes.
};
/** @brief Diagnostic shading view. Underlying values match `gpu::DebugViewMode`. */
enum class DebugView : uint32_t {
    LitPbr = 0,              ///< Final physically based shaded & post-processed output.
    LodHeatmap = 1,          ///< Color-coded mesh LOD tier heatmap.
    Normals = 2,             ///< World-space perturbed shading normals.
    MetallicRoughnessAo = 3, ///< Split visualization of metallic, roughness, and occlusion channels.
    UvChecker = 4,           ///< Procedural UV distortion grid overlay.
    ClayWireframe = 5,       ///< Matte neutral clay shading with wireframe edges.
    ToonCel = 6              ///< Quantized step-shaded cel/toon lighting mode.
};
/** @brief Built-in HDR environment preset. Underlying values match `gpu::EnvironmentPreset` (without `CustomHdr`). */
enum class Environment : uint32_t {
    MediterraneanCourtyard = 0, ///< Warm sunlit outdoor courtyard IBL.
    SunsetStudio = 1,           ///< Low golden-hour horizon rim lighting.
    OvercastAtelier = 2,        ///< Soft neutral diffused studio light.
    NightNeon = 3               ///< Cool midnight sky with vibrant cyan/magenta accents.
};
/** @brief Skybox background display mode. Underlying values match `gpu::SkyboxMode`. */
enum class SkyboxStyle : uint32_t {
    Sharp = 0,   ///< Crisp procedural sky & sun disc.
    Blurred = 1, ///< Pre-filtered soft environment backdrop.
    Hidden = 2   ///< Solid clear background with skybox hidden.
};

/**
 * @struct RenderSettings
 * @brief Plain-data tuning knobs of the bgl Engine. Defaults match the engine's built-in defaults.
 */
struct RenderSettings {
    // Lighting & materials
    bool shadows{true};                  ///< Enables 2048x2048 directional PCF shadow mapping.
    bool reflections{true};              ///< Enables specular/diffuse HDR IBL cubemap reflections.
    bool bumpMapping{true};              ///< Enables tangent-space normal maps and derivative bump mapping.
    bool parallaxOcclusion{true};        ///< Enables view-adaptive Parallax Occlusion Mapping (POM).
    float parallaxScale{0.035f};         ///< Maximum parallax raymarch displacement depth `[0.0, 0.1]`.
    bool mipmaps{true};                  ///< Enables full trilinear/16x anisotropic mip-chain sampling.
    bool materialAmbientOcclusion{true}; ///< Enables glTF material occlusion maps and contact AO.
    float materialAoIntensity{1.0f};     ///< Multiplier on material/contact ambient occlusion `[0.0, 2.0]`.
    // Environment & sky
    Environment environment{Environment::MediterraneanCourtyard}; ///< Active HDR lighting preset.
    float environmentRotationRad{0.0f};          ///< Azimuth rotation applied to the environment cubemap in radians.
    float timeOfDayHours{14.0f};                 ///< Diurnal sun position in `[0.0, 24.0)` hours (`12.0` = solar noon).
    bool skybox{true};                           ///< Toggles background skybox rendering.
    SkyboxStyle skyboxStyle{SkyboxStyle::Sharp}; ///< Sharp, blurred, or hidden skybox presentation.
    // Tone mapping & post-processing
    ToneMap toneMap{ToneMap::KhronosPbrNeutral}; ///< Active HDR-to-LDR display transform.
    float exposure{1.0f};                        ///< Manual exposure EV scale multiplier (`1.0` = neutral).
    bool bloom{true};                            ///< Enables 5-mip HDR Jimenez emissive bloom.
    float bloomIntensity{0.65f};                 ///< Additive bloom scatter intensity `[0.0, 3.0]`.
    float bloomThreshold{1.05f};                 ///< Linear HDR luminance cutoff before bloom extraction.
    bool ssao{true};                             ///< Enables half-resolution horizon Ground-Truth AO (GTAO).
    float ssaoIntensity{0.75f};                  ///< Screen-space ambient occlusion darkening strength `[0.0, 2.0]`.
    float ssaoRadius{0.02f};                     ///< World/view-space GTAO sampling kernel radius.
    bool lensFlare{true};                        ///< Enables directional solar corona & optical lens flares.
    float lensFlareIntensity{0.75f};             ///< Lens flare brightness multiplier `[0.0, 2.0]`.
    // Culling & level of detail
    bool occlusionCulling{true}; ///< Enables hierarchical Quadtree / Hi-Z occlusion culling.
    bool lod{true};              ///< Enables distance/error-based multi-tier mesh LOD selection.
    bool autoLod{false};         ///< Automatically adapts `lodBias` toward a 60 FPS frame budget.
    float lodBias{1.0f};         ///< Screen-space error multiplier (`>1.0` keeps higher detail longer).
    // Atmosphere
    Weather weather{Weather::Clear}; ///< Active precipitation/fog/smoke weather simulation.
    float weatherIntensity{1.0f};    ///< Particle / density scaling factor for the active weather mode.
    float windStrength{0.3f};        ///< Normalized horizontal wind strength affecting rain/snow/smoke.
    // Diagnostics
    DebugView debugView{DebugView::LitPbr}; ///< Final PBR view or diagnostic inspection shading mode.

    /**
     * @brief Component-wise equality comparison used by the engine to apply incremental diffs per frame.
     * @param rhs Right-hand side settings struct to compare against.
     * @return `true` if all configuration fields match identically.
     */
    bool operator==(const RenderSettings& rhs) const = default;
};

} // namespace bgl

#endif // BGL_PUBLIC_SETTINGS_HPP
