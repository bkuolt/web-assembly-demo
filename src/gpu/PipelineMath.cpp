/**
 * @file PipelineMath.cpp
 * @brief Pure CPU shading, weather, diurnal sun, LOD heatmap, and blending helpers for PipelineManager.
 */
#include "gpu/Pipeline.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace bgl::gpu {

glm::vec3 PipelineManager::EvaluateBeerLambertVolumeAttenuation(const glm::vec3& attenuationColor,
                                                                float attenuationDistance, float thickness,
                                                                float NdotV) noexcept {
    if (thickness <= 0.0f || attenuationDistance >= 1e8f) {
        return glm::vec3{1.0f};
    }
    const float attDist = std::max(attenuationDistance, 1e-4f);
    const glm::vec3 safeColor = glm::clamp(attenuationColor, glm::vec3{1e-4f}, glm::vec3{1.0f});
    const glm::vec3 extinction = -glm::log(safeColor) / attDist;
    const float opticalPath = thickness / std::max(NdotV, 0.1f);
    return glm::exp(-extinction * opticalPath);
}

glm::vec3 PipelineManager::EvaluateBloomThreshold(const glm::vec3& color, float exposure, float threshold) noexcept {
    const glm::vec3 exposed = color * exposure;
    const float lum = glm::dot(exposed, glm::vec3{0.2126f, 0.7152f, 0.0722f});
    if (lum <= threshold) {
        return glm::vec3{0.0f};
    }
    const float factor = (lum - threshold) / std::max(lum, 1e-4f);
    return exposed * factor;
}

float PipelineManager::EvaluateScreenSpaceOcclusionFactor(float depthCenter, float depthNeighbor, float radius,
                                                          float bias) noexcept {
    const float diff = depthCenter - depthNeighbor;
    if (diff > bias && diff < radius) {
        const float occ = (diff - bias) / std::max(radius - bias, 1e-4f);
        return std::clamp(1.0f - occ, 0.0f, 1.0f);
    }
    return 1.0f;
}

glm::vec3 PipelineManager::EvaluateRainWetAlbedo(const glm::vec3& dryAlbedo, float metallic, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f)
        return dryAlbedo;
    const float wetWeight{std::clamp(clampedInt * 0.8f, 0.0f, 0.75f) * (1.0f - std::clamp(metallic, 0.0f, 1.0f))};
    return glm::mix(dryAlbedo, dryAlbedo * 0.65f, wetWeight);
}

float PipelineManager::EvaluateRainWetRoughness(float dryRoughness, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f)
        return dryRoughness;
    const float glazeWeight{std::clamp(clampedInt * 0.85f, 0.0f, 0.88f)};
    return glm::mix(dryRoughness, 0.08f, glazeWeight);
}

float PipelineManager::EvaluateSnowAccumulationFactor(float normalY, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f || normalY <= 0.2f)
        return 0.0f;
    const float t{std::clamp((normalY - 0.2f) / (0.75f - 0.2f), 0.0f, 1.0f)};
    const float smooth{t * t * (3.0f - 2.0f * t)};
    return smooth * std::clamp(clampedInt, 0.0f, 1.5f);
}

namespace {

[[nodiscard]] inline float WrapHour24(float hour) noexcept {
    float wrapped = std::fmod(hour, 24.0f);
    return (wrapped < 0.0f) ? (wrapped + 24.0f) : wrapped;
}

[[nodiscard]] inline float ComputeSolarElevation(float hour) noexcept {
    return -std::cos((WrapHour24(hour) / 24.0f) * 2.0f * std::numbers::pi_v<float>);
}

} // namespace

void PipelineManager::SetTimeOfDay(float hour) noexcept {
    if (!std::isfinite(hour)) {
        return;
    }
    timeOfDay_ = WrapHour24(hour);
    solarElevation_ = ComputeSolarElevation(timeOfDay_);
}

float PipelineManager::EvaluateFogFactor(float distance, float worldY, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f || distance <= 0.0f) {
        return 0.0f;
    }
    const float heightDensity{std::exp(-std::max(worldY + 2.0f, 0.0f) * 0.12f)};
    const float opticalDepth{distance * (0.045f + 0.055f * heightDensity) * clampedInt};
    return std::clamp(1.0f - std::exp(-opticalDepth), 0.0f, 0.96f);
}

float PipelineManager::EvaluateSmokeDensity(float worldY, float intensity) noexcept {
    const float clampedInt{std::clamp(intensity, 0.0f, 2.0f)};
    if (clampedInt <= 0.001f) {
        return 0.0f;
    }
    const float plumeEnvelope{std::exp(-std::max(worldY + 3.0f, 0.0f) * 0.08f)};
    return std::clamp(plumeEnvelope * clampedInt * 0.75f, 0.0f, 0.95f);
}

glm::vec3 PipelineManager::EvaluateDiurnalSunRadiance(float timeOfDayHours) noexcept {
    const float solarElev = ComputeSolarElevation(timeOfDayHours);
    const glm::vec3 noonSun{1.0f, 0.98f, 0.95f};
    const glm::vec3 goldenSun{1.0f, 0.48f, 0.18f};
    const glm::vec3 nightMoon{0.28f, 0.44f, 0.78f};

    if (solarElev >= 0.35f) {
        const float t = std::clamp((solarElev - 0.35f) / 0.65f, 0.0f, 1.0f);
        return glm::mix(goldenSun * 2.2f, noonSun * 3.14159265f, t);
    } else if (solarElev >= -0.10f) {
        const float t = std::clamp((solarElev + 0.10f) / 0.45f, 0.0f, 1.0f);
        return glm::mix(nightMoon * 0.55f, goldenSun * 2.2f, t);
    }
    return nightMoon * 0.55f;
}

glm::vec3 PipelineManager::EvaluateDiurnalAmbientScale(float timeOfDayHours) noexcept {
    const float solarElev = ComputeSolarElevation(timeOfDayHours);
    const glm::vec3 dayScale{1.0f, 1.0f, 1.0f};
    const glm::vec3 duskScale{0.82f, 0.60f, 0.52f};
    const glm::vec3 nightScale{0.16f, 0.22f, 0.38f};

    if (solarElev >= 0.25f) {
        const float t = std::clamp((solarElev - 0.25f) / 0.75f, 0.0f, 1.0f);
        return glm::mix(duskScale, dayScale, t);
    } else if (solarElev >= -0.15f) {
        const float t = std::clamp((solarElev + 0.15f) / 0.40f, 0.0f, 1.0f);
        return glm::mix(nightScale, duskScale, t);
    }
    return nightScale;
}

void PipelineManager::UpdateAutoLod(float deltaTime) noexcept {
    if (!autoLodEnabled_ || deltaTime <= 0.0f) {
        return;
    }
    // Target 60 FPS (16.67 ms): if frame time exceeds 18.0 ms, increase LOD bias to simplify earlier;
    // if frame time is comfortably below 14.0 ms, relax LOD bias back toward 1.0f.
    constexpr float kSlowFrameSec{1.0f / 55.0f};
    constexpr float kFastFrameSec{1.0f / 72.0f};
    if (deltaTime > kSlowFrameSec) {
        lodBias_ = std::clamp(lodBias_ + deltaTime * 0.85f, 0.25f, 4.0f);
    } else if (deltaTime < kFastFrameSec && lodBias_ > 1.0f) {
        lodBias_ = std::max(1.0f, lodBias_ - deltaTime * 0.45f);
    }
}

glm::vec3 PipelineManager::EvaluateLodHeatmapColor(uint32_t lodLevel, float transition) noexcept {
    constexpr std::array<glm::vec3, 4> kPalette{{
        {0.10f, 0.92f, 0.38f}, // LOD0: Emerald Green
        {0.96f, 0.82f, 0.12f}, // LOD1: Amber Yellow
        {0.98f, 0.45f, 0.10f}, // LOD2: Deep Orange
        {0.95f, 0.16f, 0.22f}  // LOD3: Crimson Red
    }};
    const uint32_t cur = std::min(lodLevel, 3u);
    const uint32_t next = std::min(cur + 1u, 3u);
    return glm::mix(kPalette[cur], kPalette[next], std::clamp(transition, 0.0f, 1.0f));
}

float PipelineManager::EvaluateBayerDither4x4(uint32_t x, uint32_t y) noexcept {
    constexpr std::array<float, 16> kBayer{{0.0f / 16.0f, 8.0f / 16.0f, 2.0f / 16.0f, 10.0f / 16.0f, 12.0f / 16.0f,
                                            4.0f / 16.0f, 14.0f / 16.0f, 6.0f / 16.0f, 3.0f / 16.0f, 11.0f / 16.0f,
                                            1.0f / 16.0f, 9.0f / 16.0f, 15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f,
                                            5.0f / 16.0f}};
    const uint32_t idx = ((y & 3u) * 4u) + (x & 3u);
    return kBayer[idx];
}

bool PipelineManager::IsMaterialTransparentOrTransmissive(const scene::Material& mat) noexcept {
    return mat.alphaMode == scene::AlphaMode::Blend || mat.transmissionFactor > 0.001f;
}

WGPUBlendState PipelineManager::GetTransparentBlendState() noexcept {
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    return blend;
}

} // namespace bgl::gpu
