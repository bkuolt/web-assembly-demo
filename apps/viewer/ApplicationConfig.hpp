/**
 * @file ApplicationConfig.hpp
 * @brief Lightweight, backend-free startup configuration for bgl_viewer and its CLI parser.
 */
#ifndef BGL_VIEWER_APPLICATION_CONFIG_HPP
#define BGL_VIEWER_APPLICATION_CONFIG_HPP

#include <cstdint>
#include <string>

namespace bgl::app {

/**
 * @struct ApplicationConfig
 * @brief Startup window geometry, asset URI, and initial rendering/atmosphere settings for `Application`.
 */
struct ApplicationConfig {
    uint32_t width{1280};
    uint32_t height{720};
    std::string assetPath{};

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
    uint32_t debugViewMode{0}; ///< 0=LitPbr, 1=LodHeatmap, 2=Normals, 3=MetallicRoughnessAo, 4=UvChecker

    uint32_t weatherMode{0}; ///< 0=None/Clear, 1=Rain, 2=Snow, 3=Fog, 4=Smoke
    float weatherIntensity{1.0f};
    float windStrength{0.3f};
    float timeOfDay{14.0f};
    bool enableDayNightCycle{false};
    float dayNightSpeed{1.0f};
};

} // namespace bgl::app

#endif // BGL_VIEWER_APPLICATION_CONFIG_HPP
