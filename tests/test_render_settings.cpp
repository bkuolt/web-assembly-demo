#include "bgl/Settings.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/Skybox.hpp"
#include <gtest/gtest.h>
#include <utility>

using namespace bgl;

static_assert(std::to_underlying(ToneMap::KhronosPbrNeutral) ==
              std::to_underlying(gpu::ToneMapOperator::KhronosPbrNeutral));
static_assert(std::to_underlying(ToneMap::AgX) == std::to_underlying(gpu::ToneMapOperator::AgX));
static_assert(std::to_underlying(ToneMap::ACES) == std::to_underlying(gpu::ToneMapOperator::ACES));
static_assert(std::to_underlying(Weather::Clear) == std::to_underlying(gpu::WeatherMode::None));
static_assert(std::to_underlying(Weather::Rain) == std::to_underlying(gpu::WeatherMode::Rain));
static_assert(std::to_underlying(Weather::Snow) == std::to_underlying(gpu::WeatherMode::Snow));
static_assert(std::to_underlying(Weather::Fog) == std::to_underlying(gpu::WeatherMode::Fog));
static_assert(std::to_underlying(Weather::Smoke) == std::to_underlying(gpu::WeatherMode::Smoke));
static_assert(std::to_underlying(DebugView::ToonCel) == std::to_underlying(gpu::DebugViewMode::ToonCel));
static_assert(std::to_underlying(Environment::NightNeon) == std::to_underlying(gpu::EnvironmentPreset::NightNeon));
static_assert(std::to_underlying(SkyboxStyle::Sharp) == std::to_underlying(gpu::SkyboxMode::Sharp));
static_assert(std::to_underlying(SkyboxStyle::Blurred) == std::to_underlying(gpu::SkyboxMode::Blurred));
static_assert(std::to_underlying(SkyboxStyle::Hidden) == std::to_underlying(gpu::SkyboxMode::Hidden));

TEST(RenderSettings, DefaultsMatchPipelineDefaults) {
    gpu::PipelineManager pipeline;
    const RenderSettings s{};
    EXPECT_EQ(s.shadows, pipeline.IsShadowsEnabled());
    EXPECT_EQ(s.reflections, pipeline.IsReflectionsEnabled());
    EXPECT_EQ(s.bumpMapping, pipeline.IsBumpMappingEnabled());
    EXPECT_EQ(s.parallaxOcclusion, pipeline.IsParallaxEnabled());
    EXPECT_FLOAT_EQ(s.parallaxScale, pipeline.GetParallaxScale());
    EXPECT_EQ(s.mipmaps, pipeline.IsMipmappingEnabled());
    EXPECT_EQ(s.materialAmbientOcclusion, pipeline.IsAmbientOcclusionEnabled());
    EXPECT_FLOAT_EQ(s.materialAoIntensity, pipeline.GetAoIntensity());
    EXPECT_EQ(std::to_underlying(s.environment), std::to_underlying(pipeline.GetEnvironmentPreset()));
    EXPECT_FLOAT_EQ(s.timeOfDayHours, pipeline.GetTimeOfDay());
    EXPECT_EQ(std::to_underlying(s.toneMap), std::to_underlying(pipeline.GetToneMapOperator()));
    EXPECT_FLOAT_EQ(s.exposure, pipeline.GetExposure());
    EXPECT_EQ(s.bloom, pipeline.IsBloomEnabled());
    EXPECT_FLOAT_EQ(s.bloomIntensity, pipeline.GetBloomIntensity());
    EXPECT_FLOAT_EQ(s.bloomThreshold, pipeline.GetBloomThreshold());
    EXPECT_EQ(s.ssao, pipeline.IsSsaoEnabled());
    EXPECT_FLOAT_EQ(s.ssaoIntensity, pipeline.GetSsaoIntensity());
    EXPECT_FLOAT_EQ(s.ssaoRadius, pipeline.GetSsaoRadius());
    EXPECT_EQ(s.lensFlare, pipeline.IsLensFlareEnabled());
    EXPECT_FLOAT_EQ(s.lensFlareIntensity, pipeline.GetLensFlareIntensity());
    EXPECT_EQ(s.occlusionCulling, pipeline.IsOcclusionCullingEnabled());
    EXPECT_EQ(s.lod, pipeline.IsLodEnabled());
    EXPECT_EQ(s.autoLod, pipeline.IsAutoLodEnabled());
    EXPECT_FLOAT_EQ(s.lodBias, pipeline.GetLodBias());
    EXPECT_EQ(std::to_underlying(s.weather), std::to_underlying(pipeline.GetWeatherMode()));
    EXPECT_FLOAT_EQ(s.weatherIntensity, pipeline.GetWeatherIntensity());
    EXPECT_FLOAT_EQ(s.windStrength, pipeline.GetWindStrength());
    EXPECT_EQ(std::to_underlying(s.debugView), std::to_underlying(pipeline.GetDebugViewMode()));
}

TEST(RenderSettings, EqualityDetectsChanges) {
    const RenderSettings a{};
    RenderSettings b{};
    EXPECT_EQ(a, b);

    b.exposure = 2.0f;
    EXPECT_NE(a, b);
    b = a;

    b.bloom = false;
    EXPECT_NE(a, b);
    b = a;

    b.weather = Weather::Rain;
    EXPECT_NE(a, b);
    b = a;

    b.debugView = DebugView::Normals;
    EXPECT_NE(a, b);
    b = a;

    b.skyboxStyle = SkyboxStyle::Blurred;
    EXPECT_NE(a, b);
}
