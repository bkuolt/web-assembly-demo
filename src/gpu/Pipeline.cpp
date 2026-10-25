/**
 * @file Pipeline.cpp
 * @brief Implementation of hardware WebGPU RenderPipeline, HDR RGBA16Float Post-Processing Tonemapping
 *        (Khronos PBR Neutral, AgX, ACES), True HDR IBL (L2 Spherical Harmonics, Compute GGX Prefiltered Cubemap,
 *        Compute Split-Sum BRDF LUT), Clustered Forward+ KHR_lights_punctual, and Native GPU Compressed Textures.
 */
#include "gpu/Pipeline.hpp"
#include "gpu/Shaders.hpp"
#include "scene/Scene.hpp"
#include "scene/Quadtree.hpp"
#include "scene/TextureDecoder.hpp"
#include "core/Log.hpp"
#include "core/ThreadPool.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <span>
#include <bit>
#include <utility>
#include <atomic>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif
#include <glm/gtc/matrix_transform.hpp>

namespace bgl::gpu {

namespace {

[[nodiscard]] uint16_t FloatToHalfBits(float val) noexcept {
    if (std::isnan(val)) return 0x7E00u;
    if (std::isinf(val)) return (val < 0.0f) ? 0xFC00u : 0x7C00u;
    val = std::clamp(val, -65504.0f, 65504.0f);
    const uint32_t f32{std::bit_cast<uint32_t>(val)};
    const uint32_t sign{(f32 >> 16u) & 0x8000u};
    const int32_t exp{static_cast<int32_t>((f32 >> 23u) & 0xFFu) - 127 + 15};
    uint32_t mant{f32 & 0x007FFFFFu};

    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant = (mant | 0x00800000u) >> static_cast<uint32_t>(1 - exp);
        return static_cast<uint16_t>(sign | (mant >> 13u));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10u) | (mant >> 13u));
}

[[nodiscard]] std::vector<uint16_t> PackRgba32FloatToRgba16Float(std::span<const float> rgba32f) {
    std::vector<uint16_t> out(rgba32f.size());
    for (size_t i{0}; i < rgba32f.size(); ++i) {
        out[i] = FloatToHalfBits(rgba32f[i]);
    }
    return out;
}

[[nodiscard]] glm::vec3 CubeFaceDirection(uint32_t faceIndex, float u, float v) noexcept {
    glm::vec3 dir{0.0f};
    switch (faceIndex) {
        case 0: dir = glm::vec3{ 1.0f,   -v,   -u}; break; // +X
        case 1: dir = glm::vec3{-1.0f,   -v,    u}; break; // -X
        case 2: dir = glm::vec3{    u, 1.0f,    v}; break; // +Y
        case 3: dir = glm::vec3{    u,-1.0f,   -v}; break; // -Y
        case 4: dir = glm::vec3{    u,   -v, 1.0f}; break; // +Z
        default:dir = glm::vec3{   -u,   -v,-1.0f}; break; // -Z
    }
    return glm::normalize(dir);
}

[[nodiscard]] bool IsSrgbSwapchainFormat(WGPUTextureFormat fmt) noexcept {
    return fmt == WGPUTextureFormat_BGRA8UnormSrgb || fmt == WGPUTextureFormat_RGBA8UnormSrgb;
}

} // anonymous namespace

PipelineManager::~PipelineManager() {
    Shutdown();
}

void PipelineManager::Shutdown() noexcept {
    postProcessPass_.Shutdown();
    shadowPass_.Shutdown();
    aabbRenderer_.Shutdown();
    gpuCuller_.Shutdown();
    gpuRayPicker_.Shutdown();

    tonemapBindGroup_.Reset();
    cachedHdrResolveView_ = nullptr;
    tonemapSampler_.Reset();
    tonemapUniformBuffer_.Reset();
    tonemapPipeline_.Reset();
    tonemapBindGroupLayout_.Reset();

    sceneLightingBuffer_.Reset();
    ggxPrefilterUniformBuffer_.Reset();
    ggxPrefilterComputePipeline_.Reset();
    ggxPrefilterComputeBgl_.Reset();
    brdfLutComputePipeline_.Reset();
    brdfLutComputeBgl_.Reset();
    brdfLutView_.Reset();
    brdfLutTexture_.Reset();

    noMipSampler_.Reset();
    defaultSampler_.Reset();
    shadowSampler_.Reset();
    shadowDepthView_.Reset();
    shadowDepthTexture_.Reset();
    shadowPipeline_.Reset();
    shadowBindGroupLayout_.Reset();
    envCubemapView_.Reset();
    envCubemapTexture_.Reset();
    defaultBlackTextureView_.Reset();
    defaultBlackTexture_.Reset();
    defaultNormalTextureView_.Reset();
    defaultNormalTexture_.Reset();
    defaultWhiteTextureView_.Reset();
    defaultWhiteTexture_.Reset();
    aabbBindGroup_.Reset();
    aabbUniformBuffer_.Reset();
    aabbVertexBuffer_.Reset();
    aabbPipeline_.Reset();
    aabbBindGroupLayout_.Reset();
    wireframePipeline_.Reset();
    opaqueBackCulledPipeline_.Reset();
    opaquePipeline_.Reset();
    bindGroupLayout_.Reset();
    device_ = nullptr;
    queue_ = nullptr;
    initialized_ = false;
}

uint32_t PipelineManager::CalculateMipLevels(uint32_t width, uint32_t height) noexcept {
    const uint32_t maxDim{std::max(width, height)};
    return std::max(1u, static_cast<uint32_t>(std::bit_width(maxDim)));
}

std::vector<uint8_t> PipelineManager::GenerateNextMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, bool isNormalMap) {
    if (!src || srcW == 0 || srcH == 0) {
        return std::vector<uint8_t>(static_cast<size_t>(dstW) * dstH * 4);
    }
    return GenerateNextMipLevel(std::span<const uint8_t>{src, static_cast<size_t>(srcW) * srcH * 4}, srcW, srcH, dstW, dstH, isNormalMap);
}

std::vector<uint8_t> PipelineManager::GenerateNextMipLevel(std::span<const uint8_t> src, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, bool isNormalMap) {
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * 4);
    if (src.empty() || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0 || src.size() < static_cast<size_t>(srcW) * srcH * 4) {
        return dst;
    }

    auto processRows = [&](uint32_t yStart, uint32_t yEnd) {
        for (uint32_t y{yStart}; y < yEnd; ++y) {
            for (uint32_t x{0}; x < dstW; ++x) {
                const uint32_t srcX0{std::min(x * 2, srcW - 1)};
                const uint32_t srcX1{std::min(x * 2 + 1, srcW - 1)};
                const uint32_t srcY0{std::min(y * 2, srcH - 1)};
                const uint32_t srcY1{std::min(y * 2 + 1, srcH - 1)};

                std::span<const uint8_t> p00{src.subspan((static_cast<size_t>(srcY0) * srcW + srcX0) * 4, 4)};
                std::span<const uint8_t> p10{src.subspan((static_cast<size_t>(srcY0) * srcW + srcX1) * 4, 4)};
                std::span<const uint8_t> p01{src.subspan((static_cast<size_t>(srcY1) * srcW + srcX0) * 4, 4)};
                std::span<const uint8_t> p11{src.subspan((static_cast<size_t>(srcY1) * srcW + srcX1) * 4, 4)};

                const size_t dstIdx{(static_cast<size_t>(y) * dstW + x) * 4};
                if (isNormalMap) {
                    auto unpackN = [](std::span<const uint8_t> p) -> glm::vec3 {
                        return glm::vec3{
                            (static_cast<float>(p[0]) / 255.0f) * 2.0f - 1.0f,
                            (static_cast<float>(p[1]) / 255.0f) * 2.0f - 1.0f,
                            (static_cast<float>(p[2]) / 255.0f) * 2.0f - 1.0f
                        };
                    };
                    glm::vec3 avgN{unpackN(p00) + unpackN(p10) + unpackN(p01) + unpackN(p11)};
                    const float len{glm::length(avgN)};
                    if (len > 1e-5f) {
                        avgN /= len;
                    } else {
                        avgN = glm::vec3{0.0f, 0.0f, 1.0f};
                    }
                    dst[dstIdx + 0] = static_cast<uint8_t>(std::clamp((avgN.x * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[dstIdx + 1] = static_cast<uint8_t>(std::clamp((avgN.y * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[dstIdx + 2] = static_cast<uint8_t>(std::clamp((avgN.z * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
                    const uint32_t sumA{static_cast<uint32_t>(p00[3]) + p10[3] + p01[3] + p11[3]};
                    dst[dstIdx + 3] = static_cast<uint8_t>((sumA + 2) / 4);
                } else {
                    for (int c{0}; c < 4; ++c) {
                        const uint32_t sum{
                            static_cast<uint32_t>(p00[c]) +
                            static_cast<uint32_t>(p10[c]) +
                            static_cast<uint32_t>(p01[c]) +
                            static_cast<uint32_t>(p11[c])
                        };
                        dst[dstIdx + c] = static_cast<uint8_t>((sum + 2) / 4);
                    }
                }
            }
        }
    };

    processRows(0, dstH);
    return dst;
}

glm::mat4 PipelineManager::ComputeDirectionalLightMatrix(const core::AABB& sceneBounds, const glm::vec3& lightDir) noexcept {
    glm::vec3 center{0.0f};
    float radius{20.0f};
    if (sceneBounds.minBound.x <= sceneBounds.maxBound.x) {
        center = sceneBounds.Center();
        radius = std::max(glm::length(sceneBounds.Extents()), 5.0f);
    }

    const glm::vec3 dir{glm::length(lightDir) > 1e-5f ? glm::normalize(lightDir) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
    const glm::vec3 up{(std::abs(dir.y) < 0.99f) ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{0.0f, 0.0f, 1.0f}};

    const glm::vec3 lightPos{center + dir * (radius * 1.5f)};
    const glm::mat4 lightView{glm::lookAtRH(lightPos, center, up)};
    const glm::mat4 lightProj{glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.1f, radius * 3.5f)};
    return lightProj * lightView;
}

std::vector<uint8_t> PipelineManager::GenerateEnvCubemapFaceMip(uint32_t faceIndex, uint32_t faceSize, float roughness, const glm::vec3& sunDir) {
    std::vector<uint8_t> pixels(static_cast<size_t>(faceSize) * faceSize * 4);
    const glm::vec3 sun{glm::length(sunDir) > 1e-5f ? glm::normalize(sunDir) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
    const float rough{std::clamp(roughness, 0.0f, 1.0f)};

    const glm::vec3 zenithColor{0.14f, 0.38f, 0.78f};
    const glm::vec3 horizonColor{0.72f, 0.84f, 0.96f};
    const glm::vec3 groundColor{0.28f, 0.23f, 0.19f};
    const glm::vec3 warmBounce{0.42f, 0.34f, 0.26f};

    for (uint32_t y{0}; y < faceSize; ++y) {
        for (uint32_t x{0}; x < faceSize; ++x) {
            const float u{((static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
            const float v{((static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
            const glm::vec3 dir{CubeFaceDirection(faceIndex, u, v)};

            const float elev{dir.y};
            glm::vec3 envColor{};
            if (elev >= 0.0f) {
                const float horizonBlend{std::pow(std::clamp(1.0f - elev, 0.0f, 1.0f), glm::mix(2.2f, 1.0f, rough))};
                envColor = glm::mix(zenithColor, horizonColor, horizonBlend);
            } else {
                const float groundBlend{std::clamp(-elev * glm::mix(3.5f, 1.2f, rough), 0.0f, 1.0f)};
                envColor = glm::mix(horizonColor, groundColor, groundBlend);
                const float azimuthWave{0.5f + 0.5f * std::sin(dir.x * 4.0f) * std::cos(dir.z * 4.0f)};
                envColor = glm::mix(envColor, warmBounce, azimuthWave * 0.25f * (1.0f - rough * 0.5f));
            }

            const float sunDot{std::max(glm::dot(dir, sun), 0.0f)};
            const float specPower{glm::mix(380.0f, 2.5f, std::sqrt(rough))};
            const float specScale{glm::mix(2.2f, 0.25f, rough)};
            const glm::vec3 sunContrib{glm::vec3{1.0f, 0.95f, 0.85f} * std::pow(sunDot, specPower) * specScale};

            const glm::vec3 rimDir{glm::normalize(glm::vec3{-sun.x, 0.45f, -sun.z})};
            const float rimDot{std::max(glm::dot(dir, rimDir), 0.0f)};
            const glm::vec3 rimContrib{glm::vec3{0.65f, 0.80f, 1.0f} * std::pow(rimDot, glm::mix(64.0f, 2.0f, rough)) * glm::mix(0.45f, 0.1f, rough)};

            glm::vec3 finalCol{envColor + sunContrib + rimContrib};
            finalCol = finalCol / (finalCol + glm::vec3{0.65f});

            const size_t idx{(static_cast<size_t>(y) * faceSize + x) * 4};
            pixels[idx + 0] = static_cast<uint8_t>(std::clamp(finalCol.r * 255.0f + 0.5f, 0.0f, 255.0f));
            pixels[idx + 1] = static_cast<uint8_t>(std::clamp(finalCol.g * 255.0f + 0.5f, 0.0f, 255.0f));
            pixels[idx + 2] = static_cast<uint8_t>(std::clamp(finalCol.b * 255.0f + 0.5f, 0.0f, 255.0f));
            pixels[idx + 3] = 255;
        }
    }
    return pixels;
}

std::vector<float> PipelineManager::GenerateProceduralHdrCubemapFace(uint32_t faceIndex, uint32_t faceSize, EnvironmentPreset preset, const glm::vec3& sunDir) {
    std::vector<float> rgba(static_cast<size_t>(faceSize) * faceSize * 4, 1.0f);
    const glm::vec3 baseSun{glm::length(sunDir) > 1e-5f ? glm::normalize(sunDir) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};

    glm::vec3 zenith{0.16f, 0.42f, 0.92f};
    glm::vec3 horizon{0.85f, 0.96f, 1.15f};
    glm::vec3 ground{0.32f, 0.26f, 0.21f};
    glm::vec3 bounce{0.48f, 0.38f, 0.28f};
    glm::vec3 sunColor{12.0f, 11.2f, 9.6f};
    glm::vec3 keyDir{baseSun};
    glm::vec3 fillDir{glm::normalize(glm::vec3{-baseSun.x, 0.5f, -baseSun.z})};
    glm::vec3 fillColor{1.2f, 1.5f, 2.1f};

    switch (preset) {
        case EnvironmentPreset::SunsetStudio:
            zenith    = glm::vec3{0.18f, 0.14f, 0.42f};
            horizon   = glm::vec3{2.10f, 0.82f, 0.34f};
            ground    = glm::vec3{0.22f, 0.14f, 0.12f};
            bounce    = glm::vec3{0.65f, 0.30f, 0.18f};
            keyDir    = glm::normalize(glm::vec3{0.78f, 0.28f, 0.55f});
            sunColor  = glm::vec3{18.0f, 7.5f, 2.8f};
            fillColor = glm::vec3{0.55f, 0.85f, 1.85f};
            break;
        case EnvironmentPreset::OvercastAtelier:
            zenith    = glm::vec3{1.35f, 1.42f, 1.50f};
            horizon   = glm::vec3{0.95f, 0.98f, 1.04f};
            ground    = glm::vec3{0.36f, 0.36f, 0.38f};
            bounce    = glm::vec3{0.50f, 0.50f, 0.52f};
            keyDir    = glm::normalize(glm::vec3{0.15f, 0.96f, 0.22f});
            sunColor  = glm::vec3{4.5f, 4.6f, 4.8f};
            fillColor = glm::vec3{1.4f, 1.45f, 1.55f};
            break;
        case EnvironmentPreset::NightNeon:
            zenith    = glm::vec3{0.02f, 0.03f, 0.08f};
            horizon   = glm::vec3{0.08f, 0.12f, 0.24f};
            ground    = glm::vec3{0.04f, 0.04f, 0.06f};
            bounce    = glm::vec3{0.15f, 0.06f, 0.22f};
            keyDir    = glm::normalize(glm::vec3{0.65f, 0.45f, 0.60f});
            sunColor  = glm::vec3{0.8f, 8.5f, 11.0f}; // Cyan neon softbox
            fillDir   = glm::normalize(glm::vec3{-0.70f, 0.35f, -0.62f});
            fillColor = glm::vec3{9.5f, 1.2f, 6.8f};  // Magenta neon softbox
            break;
        case EnvironmentPreset::MediterraneanCourtyard:
        case EnvironmentPreset::CustomHdr:
        default:
            break;
    }

    for (uint32_t y{0}; y < faceSize; ++y) {
        for (uint32_t x{0}; x < faceSize; ++x) {
            const float u{((static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
            const float v{((static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
            const glm::vec3 dir{CubeFaceDirection(faceIndex, u, v)};

            glm::vec3 col{};
            if (dir.y >= 0.0f) {
                const float t{std::pow(std::clamp(1.0f - dir.y, 0.0f, 1.0f), 2.2f)};
                col = glm::mix(zenith, horizon, t);
            } else {
                const float t{std::clamp(-dir.y * 3.5f, 0.0f, 1.0f)};
                col = glm::mix(horizon, ground, t);
                const float wave{0.5f + 0.5f * std::sin(dir.x * 4.0f) * std::cos(dir.z * 4.0f)};
                col = glm::mix(col, bounce, wave * 0.3f);
            }

            const float keyDot{std::max(glm::dot(dir, keyDir), 0.0f)};
            const float keyCore{std::pow(keyDot, 250.0f)};
            const float keyHalo{std::pow(keyDot, 16.0f) * 0.18f};
            col += sunColor * (keyCore + keyHalo);

            const float fillDot{std::max(glm::dot(dir, fillDir), 0.0f)};
            col += fillColor * std::pow(fillDot, 24.0f) * 0.45f;

            const size_t idx{(static_cast<size_t>(y) * faceSize + x) * 4};
            rgba[idx + 0] = col.r;
            rgba[idx + 1] = col.g;
            rgba[idx + 2] = col.b;
            rgba[idx + 3] = 1.0f;
        }
    }
    return rgba;
}

std::vector<float> PipelineManager::ConvertEquirectHdrToCubemapFace(const scene::DecodedHdrImage& hdrImg, uint32_t faceIndex, uint32_t faceSize) {
    std::vector<float> rgba(static_cast<size_t>(faceSize) * faceSize * 4, 1.0f);
    if (!hdrImg.IsValid()) return rgba;

    constexpr float kInvPi{1.0f / 3.14159265358979323846f};
    constexpr float kInvTwoPi{1.0f / 6.28318530717958647692f};

    for (uint32_t y{0}; y < faceSize; ++y) {
        for (uint32_t x{0}; x < faceSize; ++x) {
            const float u{((static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
            const float v{((static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
            const glm::vec3 d{CubeFaceDirection(faceIndex, u, v)};

            const float phi{std::atan2(d.z, d.x)};
            const float theta{std::acos(std::clamp(d.y, -1.0f, 1.0f))};

            float eqU{phi * kInvTwoPi + 0.5f};
            eqU = eqU - std::floor(eqU);
            const float eqV{std::clamp(theta * kInvPi, 0.0f, 1.0f)};

            const float fx{eqU * static_cast<float>(hdrImg.width - 1)};
            const float fy{eqV * static_cast<float>(hdrImg.height - 1)};
            const uint32_t x0{static_cast<uint32_t>(fx)};
            const uint32_t y0{static_cast<uint32_t>(fy)};
            const uint32_t x1{(x0 + 1) % hdrImg.width};
            const uint32_t y1{std::min(y0 + 1, hdrImg.height - 1)};
            const float tx{fx - static_cast<float>(x0)};
            const float ty{fy - static_cast<float>(y0)};

            auto fetchTexel = [&](uint32_t px, uint32_t py) -> glm::vec3 {
                const size_t idx{(static_cast<size_t>(py) * hdrImg.width + px) * 4};
                return glm::vec3{hdrImg.rgbaFloat[idx + 0], hdrImg.rgbaFloat[idx + 1], hdrImg.rgbaFloat[idx + 2]};
            };

            const glm::vec3 c00{fetchTexel(x0, y0)};
            const glm::vec3 c10{fetchTexel(x1, y0)};
            const glm::vec3 c01{fetchTexel(x0, y1)};
            const glm::vec3 c11{fetchTexel(x1, y1)};
            const glm::vec3 c{glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty)};

            const size_t dstIdx{(static_cast<size_t>(y) * faceSize + x) * 4};
            rgba[dstIdx + 0] = std::clamp(c.r, 0.0f, 65000.0f);
            rgba[dstIdx + 1] = std::clamp(c.g, 0.0f, 65000.0f);
            rgba[dstIdx + 2] = std::clamp(c.b, 0.0f, 65000.0f);
            rgba[dstIdx + 3] = 1.0f;
        }
    }
    return rgba;
}

std::array<glm::vec4, 9> PipelineManager::ComputeSphericalHarmonicsL2(const std::array<std::vector<float>, 6>& cubemapFaces, uint32_t faceSize) noexcept {
    std::array<glm::vec3, 9> shAccum{};
    if (faceSize == 0) {
        return {};
    }

    double weightSum{0.0};
    for (uint32_t face{0}; face < 6; ++face) {
        const auto& faceData = cubemapFaces[face];
        if (faceData.size() < static_cast<size_t>(faceSize) * faceSize * 4) continue;

        for (uint32_t y{0}; y < faceSize; ++y) {
            for (uint32_t x{0}; x < faceSize; ++x) {
                const float u{((static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
                const float v{((static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};

                const float temp{1.0f + u * u + v * v};
                const float dOmega{4.0f / (std::sqrt(temp) * temp)};
                const glm::vec3 dir{CubeFaceDirection(face, u, v)};

                const size_t idx{(static_cast<size_t>(y) * faceSize + x) * 4};
                // Clamp extreme sun spikes during diffuse SH projection to avoid ringing
                const glm::vec3 radiance{
                    std::clamp(faceData[idx + 0], 0.0f, 25.0f),
                    std::clamp(faceData[idx + 1], 0.0f, 25.0f),
                    std::clamp(faceData[idx + 2], 0.0f, 25.0f)
                };

                const std::array<float, 9> basis{{
                    0.282095f,
                    0.488603f * dir.y,
                    0.488603f * dir.z,
                    0.488603f * dir.x,
                    1.092548f * dir.x * dir.y,
                    1.092548f * dir.y * dir.z,
                    0.315392f * (3.0f * dir.z * dir.z - 1.0f),
                    1.092548f * dir.x * dir.z,
                    0.546274f * (dir.x * dir.x - dir.y * dir.y)
                }};

                for (size_t i{0}; i < 9; ++i) {
                    shAccum[i] += radiance * (basis[i] * dOmega);
                }
                weightSum += static_cast<double>(dOmega);
            }
        }
    }

    const float normFactor{(weightSum > 1e-6) ? static_cast<float>((4.0 * 3.14159265358979323846) / weightSum) : 1.0f};
    std::array<glm::vec4, 9> out{};
    for (size_t i{0}; i < 9; ++i) {
        out[i] = glm::vec4{shAccum[i] * normFactor, 0.0f};
    }
    return out;
}

glm::vec3 PipelineManager::EvaluateSphericalHarmonicsL2(const std::array<glm::vec4, 9>& shCoeffs, const glm::vec3& normal) noexcept {
    const glm::vec3 n{glm::length(normal) > 1e-5f ? glm::normalize(normal) : glm::vec3{0.0f, 1.0f, 0.0f}};
    // Ramamoorthi & Hanrahan irradiance divided by pi so multiplying by diffuseColor yields diffuse exitance
    constexpr float kInvPi{1.0f / 3.14159265358979323846f};
    const glm::vec3 irr =
          glm::vec3{shCoeffs[0]} * 0.886227f
        + glm::vec3{shCoeffs[1]} * (1.023327f * n.y)
        + glm::vec3{shCoeffs[2]} * (1.023327f * n.z)
        + glm::vec3{shCoeffs[3]} * (1.023327f * n.x)
        + glm::vec3{shCoeffs[4]} * (0.858086f * n.x * n.y)
        + glm::vec3{shCoeffs[5]} * (0.858086f * n.y * n.z)
        + glm::vec3{shCoeffs[6]} * (0.247708f * (3.0f * n.z * n.z - 1.0f))
        + glm::vec3{shCoeffs[7]} * (0.858086f * n.x * n.z)
        + glm::vec3{shCoeffs[8]} * (0.429043f * (n.x * n.x - n.y * n.y));
    return glm::max(irr * kInvPi, glm::vec3{0.0f});
}

glm::vec3 PipelineManager::ApplyToneMappingCPU(const glm::vec3& linearHdr, float exposure, ToneMapOperator op) noexcept {
    glm::vec3 col{glm::max(linearHdr * exposure, glm::vec3{0.0f})};
    if (op == ToneMapOperator::KhronosPbrNeutral) {
        constexpr float startCompression{0.8f - 0.04f};
        constexpr float desaturation{0.15f};
        const float x{std::min({col.r, col.g, col.b})};
        const float offset{(x < 0.08f) ? (x - 6.25f * x * x) : 0.04f};
        col -= glm::vec3{offset};
        const float peak{std::max({col.r, col.g, col.b})};
        if (peak < startCompression) {
            return glm::clamp(col, glm::vec3{0.0f}, glm::vec3{1.0f});
        }
        const float d{1.0f - startCompression};
        const float newPeak{1.0f - d * d / (peak + d - startCompression)};
        col *= (newPeak / std::max(peak, 1e-6f));
        const float g{1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f)};
        return glm::clamp(glm::mix(col, glm::vec3{newPeak}, g), glm::vec3{0.0f}, glm::vec3{1.0f});
    }
    if (op == ToneMapOperator::AgX) {
        const glm::mat3 agxMat{
            0.842479062253094f,  0.0423282422610123f, 0.0423756549057051f,
            0.0784335999999992f, 0.878468636469772f,  0.0784336f,
            0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f
        };
        const glm::mat3 agxMatInv{
             1.19687900512017f,   -0.0528968517574562f, -0.0529716355144438f,
            -0.0980208811401368f,  1.15190312990417f,   -0.0980434501171241f,
            -0.0990297440797205f, -0.0989611768448433f,  1.15107367264116f
        };
        constexpr float minEv{-12.47393f};
        constexpr float maxEv{4.026069f};
        glm::vec3 v{agxMat * glm::max(col, glm::vec3{1e-10f})};
        v = glm::vec3{std::log2(v.x), std::log2(v.y), std::log2(v.z)};
        v = glm::clamp(v, glm::vec3{minEv}, glm::vec3{maxEv});
        v = (v - glm::vec3{minEv}) / (maxEv - minEv);
        const glm::vec3 v2{v * v};
        const glm::vec3 v4{v2 * v2};
        v = 15.5f * v4 * v2 - 40.14f * v4 * v + 31.96f * v4 - 6.868f * v2 * v + 0.4298f * v2 + 0.1191f * v - 0.00232f;
        v = agxMatInv * v;
        v = glm::max(v, glm::vec3{0.0f});
        return glm::clamp(glm::vec3{std::pow(v.x, 2.2f), std::pow(v.y, 2.2f), std::pow(v.z, 2.2f)}, glm::vec3{0.0f}, glm::vec3{1.0f});
    }
    // ACES Filmic
    constexpr float a{2.51f};
    constexpr float b{0.03f};
    constexpr float c{2.43f};
    constexpr float d{0.59f};
    constexpr float e{0.14f};
    return glm::clamp((col * (a * col + b)) / (col * (c * col + d) + e), glm::vec3{0.0f}, glm::vec3{1.0f});
}

void PipelineManager::BuildClusteredLightGrid(
    std::span<const GpuPunctualLight> lights,
    uint32_t lightCount,
    const glm::mat4& viewMatrix,
    const glm::mat4& projMatrix,
    float zNear,
    float zFar,
    std::array<glm::uvec4, 256>& outClusterMasks
) noexcept {
    outClusterMasks.fill(glm::uvec4{0u});
    const uint32_t count{std::min<uint32_t>(lightCount, static_cast<uint32_t>(lights.size()))};
    if (count == 0) return;

    const float safeNear{std::max(zNear, 0.05f)};
    const float safeFar{std::max(zFar, safeNear + 1.0f)};
    const float logRatio{std::log(safeFar / safeNear)};

    const float p00{(std::abs(projMatrix[0][0]) > 1e-5f) ? projMatrix[0][0] : 1.0f};
    const float p11{(std::abs(projMatrix[1][1]) > 1e-5f) ? projMatrix[1][1] : 1.0f};

    auto setClusterBit = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t bitMask) {
        const uint32_t clusterIdx{(z * kClusterCountY + y) * kClusterCountX + x};
        const uint32_t vecIdx{clusterIdx >> 2u};
        const uint32_t compIdx{clusterIdx & 3u};
        outClusterMasks[vecIdx][static_cast<glm::length_t>(compIdx)] |= bitMask;
    };

    for (uint32_t li{0}; li < count; ++li) {
        const auto& light = lights[li];
        const uint32_t lightType{static_cast<uint32_t>(light.posOrDir.w + 0.5f)};
        const uint32_t bitMask{1u << li};

        if (lightType == 0u) {
            // Directional light affects all clusters
            for (auto& vecMask : outClusterMasks) {
                vecMask = vecMask | glm::uvec4{bitMask};
            }
            continue;
        }

        // Point or Spot light: transform world position into view space
        const glm::vec3 viewPos{glm::vec3{viewMatrix * glm::vec4{glm::vec3{light.posOrDir}, 1.0f}}};
        // Effective cull radius (use explicit range or fallback radius derived from intensity)
        const float intensityMag{glm::length(glm::vec3{light.colorInt})};
        const float radius{(light.colorInt.w > 0.0f) ? light.colorInt.w : std::clamp(std::sqrt(intensityMag * 25.0f), 2.0f, 100.0f)};

        const float minZ{-viewPos.z - radius};
        const float maxZ{-viewPos.z + radius};
        if (maxZ < safeNear || minZ > safeFar) {
            continue;
        }

        const float normMinZ{std::clamp(std::log(std::max(minZ, safeNear) / safeNear) / logRatio, 0.0f, 0.9999f)};
        const float normMaxZ{std::clamp(std::log(std::clamp(maxZ, safeNear, safeFar) / safeNear) / logRatio, 0.0f, 0.9999f)};
        const uint32_t sliceMin{static_cast<uint32_t>(normMinZ * static_cast<float>(kClusterCountZ))};
        const uint32_t sliceMax{std::min(kClusterCountZ - 1u, static_cast<uint32_t>(normMaxZ * static_cast<float>(kClusterCountZ)))};

        // Project sphere bounds onto NDC [-1, 1] -> Tile [0..15] x [0..7]
        uint32_t tileMinX{0};
        uint32_t tileMaxX{kClusterCountX - 1u};
        uint32_t tileMinY{0};
        uint32_t tileMaxY{kClusterCountY - 1u};

        const float closestDepth{std::max(-viewPos.z - radius, safeNear)};
        if (-viewPos.z > radius) {
            const float halfAngleX{radius * p00 / closestDepth};
            const float halfAngleY{radius * p11 / closestDepth};
            const float centerNdcX{(viewPos.x * p00) / (-viewPos.z)};
            const float centerNdcY{(viewPos.y * p11) / (-viewPos.z)};

            const float minScreenX{std::clamp((centerNdcX - halfAngleX) * 0.5f + 0.5f, 0.0f, 0.9999f)};
            const float maxScreenX{std::clamp((centerNdcX + halfAngleX) * 0.5f + 0.5f, 0.0f, 0.9999f)};
            // WebGPU framebuffer Y is top-down (0 = top, 1 = bottom)
            const float minScreenY{std::clamp((0.5f - (centerNdcY + halfAngleY) * 0.5f), 0.0f, 0.9999f)};
            const float maxScreenY{std::clamp((0.5f - (centerNdcY - halfAngleY) * 0.5f), 0.0f, 0.9999f)};

            if (centerNdcX + halfAngleX < -1.0f || centerNdcX - halfAngleX > 1.0f ||
                centerNdcY + halfAngleY < -1.0f || centerNdcY - halfAngleY > 1.0f) {
                continue;
            }

            tileMinX = static_cast<uint32_t>(minScreenX * static_cast<float>(kClusterCountX));
            tileMaxX = std::min(kClusterCountX - 1u, static_cast<uint32_t>(maxScreenX * static_cast<float>(kClusterCountX)));
            tileMinY = static_cast<uint32_t>(minScreenY * static_cast<float>(kClusterCountY));
            tileMaxY = std::min(kClusterCountY - 1u, static_cast<uint32_t>(maxScreenY * static_cast<float>(kClusterCountY)));
        }

        for (uint32_t z{sliceMin}; z <= sliceMax; ++z) {
            for (uint32_t y{tileMinY}; y <= tileMaxY; ++y) {
                for (uint32_t x{tileMinX}; x <= tileMaxX; ++x) {
                    setClusterBit(x, y, z, bitMask);
                }
            }
        }
    }
}

void PipelineManager::InitializeComputeIBLPipelines() {
    if (!device_) return;

    // 1. 2D Split-Sum BRDF LUT Texture (128x128 RGBA16Float with StorageBinding + TextureBinding)
    WGPUTextureDescriptor lutDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    lutDesc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    lutDesc.dimension = WGPUTextureDimension_2D;
    lutDesc.size = WGPUExtent3D{ kBrdfLutSize, kBrdfLutSize, 1 };
    lutDesc.format = WGPUTextureFormat_RGBA16Float;
    lutDesc.mipLevelCount = 1;
    lutDesc.sampleCount = 1;
    lutDesc.label = ToWgpuStringView("IBL_SplitSum_BRDF_LUT");
    brdfLutTexture_ = wgpuDeviceCreateTexture(device_, &lutDesc);
    if (brdfLutTexture_) {
        brdfLutView_ = wgpuTextureCreateView(brdfLutTexture_, nullptr);
    }

    // 2. BRDF LUT Compute Pipeline
    WGPUShaderSourceWGSL lutWgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    lutWgsl.code = ToWgpuStringView(kBrdfLutComputeWGSL);
    WGPUShaderModuleDescriptor lutSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    lutSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&lutWgsl);
    lutSmDesc.label = ToWgpuStringView("BRDF_LUT_Compute_WGSL");
    ShaderModuleHandle lutModule{wgpuDeviceCreateShaderModule(device_, &lutSmDesc)};

    WGPUBindGroupLayoutEntry lutBglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    lutBglEntry.binding = 0;
    lutBglEntry.visibility = WGPUShaderStage_Compute;
    lutBglEntry.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
    lutBglEntry.storageTexture.format = WGPUTextureFormat_RGBA16Float;
    lutBglEntry.storageTexture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor lutBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    lutBglDesc.entryCount = 1;
    lutBglDesc.entries = &lutBglEntry;
    brdfLutComputeBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &lutBglDesc);

    WGPUPipelineLayoutDescriptor lutPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    lutPlDesc.bindGroupLayoutCount = 1;
    lutPlDesc.bindGroupLayouts = brdfLutComputeBgl_.AddressOf();
    PipelineLayoutHandle lutPl{wgpuDeviceCreatePipelineLayout(device_, &lutPlDesc)};

    if (lutModule && lutPl) {
        WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cpDesc.layout = lutPl;
        cpDesc.compute.module = lutModule;
        cpDesc.compute.entryPoint = ToWgpuStringView("cs_brdf_lut");
        cpDesc.label = ToWgpuStringView("BRDF_LUT_ComputePipeline");
        brdfLutComputePipeline_ = wgpuDeviceCreateComputePipeline(device_, &cpDesc);
    }

    DispatchBrdfLutCompute();

    // 3. GGX Specular Cubemap Prefilter Compute Pipeline
    WGPUShaderSourceWGSL ggxWgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    ggxWgsl.code = ToWgpuStringView(kGgxPrefilterComputeWGSL);
    WGPUShaderModuleDescriptor ggxSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    ggxSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&ggxWgsl);
    ggxSmDesc.label = ToWgpuStringView("GGX_Prefilter_Compute_WGSL");
    ShaderModuleHandle ggxModule{wgpuDeviceCreateShaderModule(device_, &ggxSmDesc)};

    std::array<WGPUBindGroupLayoutEntry, 4> ggxEntries{};
    ggxEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    ggxEntries[0].binding = 0;
    ggxEntries[0].visibility = WGPUShaderStage_Compute;
    ggxEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    ggxEntries[0].buffer.minBindingSize = sizeof(PrefilterComputeParams);

    ggxEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    ggxEntries[1].binding = 1;
    ggxEntries[1].visibility = WGPUShaderStage_Compute;
    ggxEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    ggxEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    ggxEntries[2].binding = 2;
    ggxEntries[2].visibility = WGPUShaderStage_Compute;
    ggxEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    ggxEntries[2].texture.viewDimension = WGPUTextureViewDimension_Cube;
    ggxEntries[2].texture.multisampled = WGPU_FALSE;

    ggxEntries[3] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    ggxEntries[3].binding = 3;
    ggxEntries[3].visibility = WGPUShaderStage_Compute;
    ggxEntries[3].storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
    ggxEntries[3].storageTexture.format = WGPUTextureFormat_RGBA16Float;
    ggxEntries[3].storageTexture.viewDimension = WGPUTextureViewDimension_2DArray;

    WGPUBindGroupLayoutDescriptor ggxBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ggxBglDesc.entryCount = ggxEntries.size();
    ggxBglDesc.entries = ggxEntries.data();
    ggxPrefilterComputeBgl_ = wgpuDeviceCreateBindGroupLayout(device_, &ggxBglDesc);

    WGPUPipelineLayoutDescriptor ggxPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    ggxPlDesc.bindGroupLayoutCount = 1;
    ggxPlDesc.bindGroupLayouts = ggxPrefilterComputeBgl_.AddressOf();
    PipelineLayoutHandle ggxPl{wgpuDeviceCreatePipelineLayout(device_, &ggxPlDesc)};

    if (ggxModule && ggxPl) {
        WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cpDesc.layout = ggxPl;
        cpDesc.compute.module = ggxModule;
        cpDesc.compute.entryPoint = ToWgpuStringView("cs_ggx_prefilter");
        cpDesc.label = ToWgpuStringView("GGX_Prefilter_ComputePipeline");
        ggxPrefilterComputePipeline_ = wgpuDeviceCreateComputePipeline(device_, &cpDesc);
    }

    WGPUBufferDescriptor paramBufDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramBufDesc.size = sizeof(PrefilterComputeParams);
    paramBufDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramBufDesc.label = ToWgpuStringView("GGXPrefilterParamsBuffer");
    ggxPrefilterUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &paramBufDesc);
}

void PipelineManager::DispatchBrdfLutCompute() {
    if (!device_ || !queue_ || !brdfLutComputePipeline_ || !brdfLutView_) return;

    WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
    bgEntry.binding = 0;
    bgEntry.textureView = brdfLutView_;

    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.layout = brdfLutComputeBgl_;
    bgDesc.entryCount = 1;
    bgDesc.entries = &bgEntry;
    BindGroupHandle bindGroup{wgpuDeviceCreateBindGroup(device_, &bgDesc)};

    CommandEncoderHandle enc{wgpuDeviceCreateCommandEncoder(device_, nullptr)};
    ComputePassEncoderHandle cpass{wgpuCommandEncoderBeginComputePass(enc, nullptr)};
    wgpuComputePassEncoderSetPipeline(cpass, brdfLutComputePipeline_);
    wgpuComputePassEncoderSetBindGroup(cpass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cpass, (kBrdfLutSize + 7) / 8, (kBrdfLutSize + 7) / 8, 1);
    wgpuComputePassEncoderEnd(cpass);

    CommandBufferHandle cmd{wgpuCommandEncoderFinish(enc, nullptr)};
    WGPUCommandBuffer rawCmd{cmd.Get()};
    wgpuQueueSubmit(queue_, 1, &rawCmd);
    BGL_LOG_INFO("Computed 128x128 RGBA16Float Split-Sum BRDF LUT on GPU via WebGPU Compute Shader.");
}

void PipelineManager::BuildEnvironmentFromHdrFaces(const std::array<std::vector<float>, 6>& hdrFaces, uint32_t faceSize) {
    if (!device_ || !queue_ || faceSize == 0) return;

    // 1. Compute L2 Spherical Harmonics (9 coefficients) on CPU from the linear HDR cubemap faces
    shIrradiance_ = ComputeSphericalHarmonicsL2(hdrFaces, faceSize);
    // Pre-apply 1/pi Lambertian factor to shIrradiance_ so shader evaluation directly yields diffuse irradiance
    constexpr float kInvPi{1.0f / 3.14159265358979323846f};
    for (auto& coeff : shIrradiance_) {
        coeff *= kInvPi;
    }

    // 2. Create RGBA16Float Environment Cubemap Texture with StorageBinding + TextureBinding + CopyDst
    envMipLevels_ = CalculateMipLevels(faceSize, faceSize);
    WGPUTextureDescriptor cubeDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    cubeDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopyDst;
    cubeDesc.dimension = WGPUTextureDimension_2D;
    cubeDesc.size = WGPUExtent3D{ faceSize, faceSize, 6 };
    cubeDesc.format = WGPUTextureFormat_RGBA16Float;
    cubeDesc.mipLevelCount = envMipLevels_;
    cubeDesc.sampleCount = 1;
    cubeDesc.label = ToWgpuStringView("HDR_EnvironmentCubemap_RGBA16F");
    envCubemapTexture_ = wgpuDeviceCreateTexture(device_, &cubeDesc);

    // 3. Upload Mip 0 (6 faces packed as FP16)
    for (uint32_t face{0}; face < 6; ++face) {
        const auto fp16Pixels = PackRgba32FloatToRgba16Float(hdrFaces[face]);

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = envCubemapTexture_;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{ 0, 0, face };
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = faceSize * 8; // 4 * sizeof(uint16_t)
        dataLayout.rowsPerImage = faceSize;

        WGPUExtent3D writeSize{ faceSize, faceSize, 1 };
        wgpuQueueWriteTexture(queue_, &copyDst, fp16Pixels.data(), fp16Pixels.size() * sizeof(uint16_t), &dataLayout, &writeSize);
    }

    // 4. Create Mip-0 Cube View for Compute Shader input sampling
    WGPUTextureViewDescriptor mip0CubeViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    mip0CubeViewDesc.format = WGPUTextureFormat_RGBA16Float;
    mip0CubeViewDesc.dimension = WGPUTextureViewDimension_Cube;
    mip0CubeViewDesc.baseMipLevel = 0;
    mip0CubeViewDesc.mipLevelCount = 1;
    mip0CubeViewDesc.baseArrayLayer = 0;
    mip0CubeViewDesc.arrayLayerCount = 6;
    mip0CubeViewDesc.aspect = WGPUTextureAspect_All;
    TextureViewHandle mip0CubeView{wgpuTextureCreateView(envCubemapTexture_, &mip0CubeViewDesc)};

    // 5. Dispatch WebGPU Compute Shader per mip level (1 .. envMipLevels_ - 1) for GGX Specular Prefiltering
    if (ggxPrefilterComputePipeline_ && ggxPrefilterComputeBgl_ && ggxPrefilterUniformBuffer_ && mip0CubeView) {
        for (uint32_t mip{1}; mip < envMipLevels_; ++mip) {
            const uint32_t mipSize{std::max(1u, faceSize >> mip)};
            const float roughness{static_cast<float>(mip) / static_cast<float>(envMipLevels_ - 1)};

            PrefilterComputeParams params{
                .roughness = roughness,
                .faceSize = mipSize,
                .sourceSize = static_cast<float>(faceSize),
                .sampleCount = 64u
            };
            wgpuQueueWriteBuffer(queue_, ggxPrefilterUniformBuffer_, 0, &params, sizeof(params));

            WGPUTextureViewDescriptor dstMipViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            dstMipViewDesc.format = WGPUTextureFormat_RGBA16Float;
            dstMipViewDesc.dimension = WGPUTextureViewDimension_2DArray;
            dstMipViewDesc.baseMipLevel = mip;
            dstMipViewDesc.mipLevelCount = 1;
            dstMipViewDesc.baseArrayLayer = 0;
            dstMipViewDesc.arrayLayerCount = 6;
            dstMipViewDesc.aspect = WGPUTextureAspect_All;
            TextureViewHandle dstMipView{wgpuTextureCreateView(envCubemapTexture_, &dstMipViewDesc)};

            std::array<WGPUBindGroupEntry, 4> entries{};
            entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[0].binding = 0;
            entries[0].buffer = ggxPrefilterUniformBuffer_;
            entries[0].offset = 0;
            entries[0].size = sizeof(PrefilterComputeParams);

            entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[1].binding = 1;
            entries[1].sampler = defaultSampler_;

            entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[2].binding = 2;
            entries[2].textureView = mip0CubeView;

            entries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[3].binding = 3;
            entries[3].textureView = dstMipView;

            WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgDesc.layout = ggxPrefilterComputeBgl_;
            bgDesc.entryCount = entries.size();
            bgDesc.entries = entries.data();
            BindGroupHandle bg{wgpuDeviceCreateBindGroup(device_, &bgDesc)};

            CommandEncoderHandle enc{wgpuDeviceCreateCommandEncoder(device_, nullptr)};
            ComputePassEncoderHandle cpass{wgpuCommandEncoderBeginComputePass(enc, nullptr)};
            wgpuComputePassEncoderSetPipeline(cpass, ggxPrefilterComputePipeline_);
            wgpuComputePassEncoderSetBindGroup(cpass, 0, bg, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(cpass, (mipSize + 7) / 8, (mipSize + 7) / 8, 6);
            wgpuComputePassEncoderEnd(cpass);

            CommandBufferHandle cmd{wgpuCommandEncoderFinish(enc, nullptr)};
            WGPUCommandBuffer rawCmd{cmd.Get()};
            wgpuQueueSubmit(queue_, 1, &rawCmd);
        }
    }

    // 6. Create Full Mip-Chain Cube View for PBR & Skybox sampling
    WGPUTextureViewDescriptor fullCubeViewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    fullCubeViewDesc.format = WGPUTextureFormat_RGBA16Float;
    fullCubeViewDesc.dimension = WGPUTextureViewDimension_Cube;
    fullCubeViewDesc.baseMipLevel = 0;
    fullCubeViewDesc.mipLevelCount = envMipLevels_;
    fullCubeViewDesc.baseArrayLayer = 0;
    fullCubeViewDesc.arrayLayerCount = 6;
    fullCubeViewDesc.aspect = WGPUTextureAspect_All;
    envCubemapView_ = wgpuTextureCreateView(envCubemapTexture_, &fullCubeViewDesc);
}

void PipelineManager::SetEnvironmentPreset(EnvironmentPreset preset, scene::Scene* activeScene) {
    currentEnvPreset_ = preset;
    if (!device_ || !queue_) return;

    std::array<std::vector<float>, 6> hdrFaces{};
#if !defined(__EMSCRIPTEN__)
    core::ThreadPool::Instance().ParallelFor(0, 6, [&](size_t face) {
        hdrFaces[face] = GenerateProceduralHdrCubemapFace(static_cast<uint32_t>(face), kEnvCubemapSize, preset, currentLightDir_);
    });
#else
    for (uint32_t face{0}; face < 6; ++face) {
        hdrFaces[face] = GenerateProceduralHdrCubemapFace(face, kEnvCubemapSize, preset, currentLightDir_);
    }
#endif

    BuildEnvironmentFromHdrFaces(hdrFaces, kEnvCubemapSize);
    if (activeScene) {
        RebuildMaterialBindGroups(*activeScene);
    }
    BGL_LOG_INFO("Switched HDR Environment Preset to '{}' ({}x{} RGBA16F, {} GGX Mips, L2 SH Irradiance)",
                 GetEnvironmentPresetName(preset), kEnvCubemapSize, kEnvCubemapSize, envMipLevels_);
}

void PipelineManager::CycleEnvironmentPreset(scene::Scene* activeScene) {
    switch (currentEnvPreset_) {
        case EnvironmentPreset::MediterraneanCourtyard:
            SetEnvironmentPreset(EnvironmentPreset::SunsetStudio, activeScene);
            break;
        case EnvironmentPreset::SunsetStudio:
            SetEnvironmentPreset(EnvironmentPreset::OvercastAtelier, activeScene);
            break;
        case EnvironmentPreset::OvercastAtelier:
            SetEnvironmentPreset(EnvironmentPreset::NightNeon, activeScene);
            break;
        case EnvironmentPreset::NightNeon:
        case EnvironmentPreset::CustomHdr:
            SetEnvironmentPreset(EnvironmentPreset::MediterraneanCourtyard, activeScene);
            break;
    }
}

bool PipelineManager::LoadCustomHdrEnvironment(const scene::DecodedHdrImage& hdrImage, scene::Scene* activeScene) {
    if (!hdrImage.IsValid() || !device_ || !queue_) {
        return false;
    }

    const uint32_t faceSize{std::clamp<uint32_t>(std::bit_floor(std::max(hdrImage.height / 2u, 64u)), 64u, 256u)};
    std::array<std::vector<float>, 6> hdrFaces{};
#if !defined(__EMSCRIPTEN__)
    core::ThreadPool::Instance().ParallelFor(0, 6, [&](size_t face) {
        hdrFaces[face] = ConvertEquirectHdrToCubemapFace(hdrImage, static_cast<uint32_t>(face), faceSize);
    });
#else
    for (uint32_t face{0}; face < 6; ++face) {
        hdrFaces[face] = ConvertEquirectHdrToCubemapFace(hdrImage, face, faceSize);
    }
#endif

    currentEnvPreset_ = EnvironmentPreset::CustomHdr;
    BuildEnvironmentFromHdrFaces(hdrFaces, faceSize);
    if (activeScene) {
        RebuildMaterialBindGroups(*activeScene);
    }
    BGL_LOG_INFO("Loaded custom HDR Environment ({}x{} equirect -> {}x{}x6 RGBA16F cubemap, {} GGX mips)",
                 hdrImage.width, hdrImage.height, faceSize, faceSize, envMipLevels_);
    return true;
}

void PipelineManager::InitializeTonemapPipeline() {
    if (!device_) return;

    WGPUShaderSourceWGSL tmWgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    tmWgsl.code = ToWgpuStringView(kTonemapShaderWGSL);
    WGPUShaderModuleDescriptor tmSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    tmSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&tmWgsl);
    tmSmDesc.label = ToWgpuStringView("Tonemap_WGSL");
    ShaderModuleHandle tmShader{wgpuDeviceCreateShaderModule(device_, &tmSmDesc)};
    if (!tmShader) return;

    std::array<WGPUBindGroupLayoutEntry, 3> bglEntries{};
    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(TonemapUniforms);

    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    bglEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[2].binding = 2;
    bglEntries[2].visibility = WGPUShaderStage_Fragment;
    bglEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntries[2].texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = bglEntries.size();
    bglDesc.entries = bglEntries.data();
    tonemapBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);

    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = tonemapBindGroupLayout_.AddressOf();
    PipelineLayoutHandle pl{wgpuDeviceCreatePipelineLayout(device_, &plDesc)};

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragState = WGPU_FRAGMENT_STATE_INIT;
    fragState.module = tmShader;
    fragState.entryPoint = ToWgpuStringView("fs_tonemap");
    fragState.targetCount = 1;
    fragState.targets = &colorTarget;

    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pl;
    rpDesc.vertex.module = tmShader;
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_tonemap");
    rpDesc.vertex.bufferCount = 0;
    rpDesc.vertex.buffers = nullptr;
    rpDesc.fragment = &fragState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = nullptr;
    rpDesc.multisample.count = 1;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    tonemapPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);

    WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubDesc.size = sizeof(TonemapUniforms);
    ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubDesc.label = ToWgpuStringView("TonemapUniformBuffer");
    tonemapUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &ubDesc);

    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampDesc.maxAnisotropy = 1;
    tonemapSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);
}

void PipelineManager::RenderTonemapPass(
    WGPURenderPassEncoder swapchainPass,
    WGPUTextureView hdrResolveView,
    const glm::mat4& viewProj,
    const glm::vec3& cameraPos,
    const glm::vec3& lightDir,
    float envRotation,
    uint32_t viewportW,
    uint32_t viewportH
) {
    if (postProcessPass_.IsInitialized()) {
        postProcessPass_.Render(swapchainPass, hdrResolveView, viewProj, cameraPos, lightDir, envRotation, viewportW, viewportH);
        return;
    }
    if (!swapchainPass || !hdrResolveView || !tonemapPipeline_ || !tonemapBindGroupLayout_) return;

    if (hdrResolveView != cachedHdrResolveView_ || !tonemapBindGroup_) {
        cachedHdrResolveView_ = hdrResolveView;

        std::array<WGPUBindGroupEntry, 3> entries{};
        entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[0].binding = 0;
        entries[0].buffer = tonemapUniformBuffer_;
        entries[0].offset = 0;
        entries[0].size = sizeof(TonemapUniforms);

        entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[1].binding = 1;
        entries[1].sampler = tonemapSampler_;

        entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[2].binding = 2;
        entries[2].textureView = hdrResolveView;

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = tonemapBindGroupLayout_;
        bgDesc.entryCount = entries.size();
        bgDesc.entries = entries.data();
        tonemapBindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);
    }

    // Project sun vector to screen NDC for real-time lens flares
    const glm::vec3 rotatedSun = glm::rotate(glm::mat4{1.0f}, -envRotation, glm::vec3{0.0f, 1.0f, 0.0f}) * glm::vec4{glm::normalize(lightDir), 0.0f};
    const glm::vec4 sunWorldPos = glm::vec4{cameraPos + rotatedSun * 1000.0f, 1.0f};
    const glm::vec4 sunClip = viewProj * sunWorldPos;

    float sunVisibility = 0.0f;
    glm::vec2 sunUV{0.5f, 0.5f};
    const float aspect = (viewportH > 0) ? (static_cast<float>(viewportW) / static_cast<float>(viewportH)) : (16.0f / 9.0f);

    if (sunClip.w > 0.001f) {
        const glm::vec3 sunNdc = glm::vec3{sunClip} / sunClip.w;
        if (sunNdc.z > 0.0f && sunNdc.z < 1.0f) {
            sunUV = glm::vec2{sunNdc.x * 0.5f + 0.5f, 0.5f - sunNdc.y * 0.5f};
            if (sunUV.x >= -0.2f && sunUV.x <= 1.2f && sunUV.y >= -0.2f && sunUV.y <= 1.2f) {
                const float distCenter = glm::length(sunUV - glm::vec2{0.5f});
                sunVisibility = std::clamp(1.2f - distCenter, 0.0f, 1.0f);
            }
        }
    }

    TonemapUniforms uniforms{
        .exposure = exposure_,
        .operatorId = std::to_underlying(toneMapOperator_),
        .isSrgbSurface = IsSrgbSwapchainFormat(surfaceFormat_) ? 1u : 0u,
        .lensFlareEnabled = lensFlareEnabled_ ? 1u : 0u,
        .sunScreenPos = glm::vec4{sunUV.x, sunUV.y, sunVisibility, aspect},
        .flareParams = glm::vec4{lensFlareIntensity_, 0.38f, 1.0f, 0.0f},
        .bloomParams = glm::vec4{bloomIntensity_, bloomThreshold_, 0.85f, bloomEnabled_ ? 1.0f : 0.0f},
        .ssaoParams = glm::vec4{ssaoIntensity_, ssaoRadius_, 0.005f, ssaoEnabled_ ? 1.0f : 0.0f}
    };
    wgpuQueueWriteBuffer(queue_, tonemapUniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(swapchainPass, tonemapPipeline_);
    wgpuRenderPassEncoderSetBindGroup(swapchainPass, 0, tonemapBindGroup_, 0, nullptr);
    wgpuRenderPassEncoderDraw(swapchainPass, 3, 1, 0, 0);
}

core::Result<void> PipelineManager::Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat, uint32_t sampleCount) {
    device_ = device;
    queue_ = queue;
    surfaceFormat_ = surfaceFormat;
    hdrFormat_ = WGPUTextureFormat_RGBA16Float;
    sampleCount_ = sampleCount;
    if (!device_ || !queue_) {
        return std::unexpected("Cannot initialize PipelineManager without valid WGPUDevice and WGPUQueue");
    }

    // 1. Compile WGSL PBR Shader Module
    WGPUShaderSourceWGSL wgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgslDesc.code = ToWgpuStringView(kPbrShaderWGSL);

    WGPUShaderModuleDescriptor smDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgslDesc);
    smDesc.label = ToWgpuStringView("PBR_WGSL");

    ShaderModuleHandle pbrShaderModule{wgpuDeviceCreateShaderModule(device_, &smDesc)};
    if (!pbrShaderModule) {
        return std::unexpected("Failed to compile embedded WGSL PBR shader module");
    }

    // 2. Compile WGSL Wireframe Shader Module
    WGPUShaderSourceWGSL wfWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    wfWgslDesc.code = ToWgpuStringView(kWireframeShaderWGSL);

    WGPUShaderModuleDescriptor wfSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    wfSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wfWgslDesc);
    wfSmDesc.label = ToWgpuStringView("Wireframe_WGSL");

    ShaderModuleHandle wfShaderModule{wgpuDeviceCreateShaderModule(device_, &wfSmDesc)};

    // 3. Compile WGSL Shadow Shader Module
    WGPUShaderSourceWGSL shadowWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    shadowWgslDesc.code = ToWgpuStringView(kShadowShaderWGSL);

    WGPUShaderModuleDescriptor shadowSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shadowSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&shadowWgslDesc);
    shadowSmDesc.label = ToWgpuStringView("Shadow_WGSL");

    ShaderModuleHandle shadowShaderModule{wgpuDeviceCreateShaderModule(device_, &shadowSmDesc)};

    // 4. Bind Group Layout (16 entries):
    // 0: PrimitiveUniforms
    // 1: Default Sampler
    // 2..9: 2D Material Textures
    // 10: HDR Environment Cubemap (texture_cube<f32>)
    // 11: Shadow Depth Map (texture_depth_2d)
    // 12: Shadow Comparison Sampler (sampler_comparison)
    // 13: Split-Sum BRDF LUT (texture_2d<f32>)
    // 14: SceneLightingUniforms (L2 SH + Clustered Forward+ KHR_lights_punctual)
    // 15: Opaque Scene Color Copy (texture_2d<f32> for Screen-Space Refraction)
    std::array<WGPUBindGroupLayoutEntry, 16> bglEntries{};

    bglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[0].binding = 0;
    bglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[0].buffer.minBindingSize = sizeof(PrimitiveUniforms);

    bglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[1].binding = 1;
    bglEntries[1].visibility = WGPUShaderStage_Fragment;
    bglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    for (uint32_t b{2}; b < 10; ++b) {
        bglEntries[b] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        bglEntries[b].binding = b;
        bglEntries[b].visibility = WGPUShaderStage_Fragment;
        bglEntries[b].texture.sampleType = WGPUTextureSampleType_Float;
        bglEntries[b].texture.viewDimension = WGPUTextureViewDimension_2D;
        bglEntries[b].texture.multisampled = WGPU_FALSE;
    }

    bglEntries[10] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[10].binding = 10;
    bglEntries[10].visibility = WGPUShaderStage_Fragment;
    bglEntries[10].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[10].texture.viewDimension = WGPUTextureViewDimension_Cube;
    bglEntries[10].texture.multisampled = WGPU_FALSE;

    bglEntries[11] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[11].binding = 11;
    bglEntries[11].visibility = WGPUShaderStage_Fragment;
    bglEntries[11].texture.sampleType = WGPUTextureSampleType_Depth;
    bglEntries[11].texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntries[11].texture.multisampled = WGPU_FALSE;

    bglEntries[12] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[12].binding = 12;
    bglEntries[12].visibility = WGPUShaderStage_Fragment;
    bglEntries[12].sampler.type = WGPUSamplerBindingType_Comparison;

    bglEntries[13] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[13].binding = 13;
    bglEntries[13].visibility = WGPUShaderStage_Fragment;
    bglEntries[13].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[13].texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntries[13].texture.multisampled = WGPU_FALSE;

    bglEntries[14] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[14].binding = 14;
    bglEntries[14].visibility = WGPUShaderStage_Fragment;
    bglEntries[14].buffer.type = WGPUBufferBindingType_Uniform;
    bglEntries[14].buffer.minBindingSize = sizeof(SceneLightingUniforms);

    bglEntries[15] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    bglEntries[15].binding = 15;
    bglEntries[15].visibility = WGPUShaderStage_Fragment;
    bglEntries[15].texture.sampleType = WGPUTextureSampleType_Float;
    bglEntries[15].texture.viewDimension = WGPUTextureViewDimension_2D;
    bglEntries[15].texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bglDesc.entryCount = bglEntries.size();
    bglDesc.entries = bglEntries.data();
    bindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &bglDesc);
    if (!bindGroupLayout_) {
        return std::unexpected("Failed to create WebGPU BindGroupLayout for PBR material");
    }

    // 4b. Shadow Pass Bind Group Layout (0: Uniforms, 1: Sampler, 2: BaseColor Texture for Alpha Discard)
    std::array<WGPUBindGroupLayoutEntry, 3> shadowBglEntries{};
    shadowBglEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    shadowBglEntries[0].binding = 0;
    shadowBglEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    shadowBglEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    shadowBglEntries[0].buffer.minBindingSize = sizeof(PrimitiveUniforms);

    shadowBglEntries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    shadowBglEntries[1].binding = 1;
    shadowBglEntries[1].visibility = WGPUShaderStage_Fragment;
    shadowBglEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    shadowBglEntries[2] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    shadowBglEntries[2].binding = 2;
    shadowBglEntries[2].visibility = WGPUShaderStage_Fragment;
    shadowBglEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
    shadowBglEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    shadowBglEntries[2].texture.multisampled = WGPU_FALSE;

    WGPUBindGroupLayoutDescriptor shadowBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    shadowBglDesc.entryCount = shadowBglEntries.size();
    shadowBglDesc.entries = shadowBglEntries.data();
    shadowBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &shadowBglDesc);

    // 5. Pipeline Layouts
    WGPUPipelineLayoutDescriptor plDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = bindGroupLayout_.AddressOf();
    PipelineLayoutHandle pipelineLayout{wgpuDeviceCreatePipelineLayout(device_, &plDesc)};

    WGPUPipelineLayoutDescriptor shadowPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    shadowPlDesc.bindGroupLayoutCount = 1;
    shadowPlDesc.bindGroupLayouts = shadowBindGroupLayout_.AddressOf();
    PipelineLayoutHandle shadowPipelineLayout{wgpuDeviceCreatePipelineLayout(device_, &shadowPlDesc)};

    // 6. Vertex Layout (Position, Normal, Tangent, UV)
    std::array<WGPUVertexAttribute, 4> vertAttrs{};
    vertAttrs[0].format = WGPUVertexFormat_Float32x3;
    vertAttrs[0].offset = offsetof(core::Vertex, position);
    vertAttrs[0].shaderLocation = 0;

    vertAttrs[1].format = WGPUVertexFormat_Float32x3;
    vertAttrs[1].offset = offsetof(core::Vertex, normal);
    vertAttrs[1].shaderLocation = 1;

    vertAttrs[2].format = WGPUVertexFormat_Float32x4;
    vertAttrs[2].offset = offsetof(core::Vertex, tangent);
    vertAttrs[2].shaderLocation = 2;

    vertAttrs[3].format = WGPUVertexFormat_Float32x2;
    vertAttrs[3].offset = offsetof(core::Vertex, uv);
    vertAttrs[3].shaderLocation = 3;

    WGPUVertexBufferLayout vbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbLayout.arrayStride = sizeof(core::Vertex);
    vbLayout.stepMode = WGPUVertexStepMode_Vertex;
    vbLayout.attributeCount = vertAttrs.size();
    vbLayout.attributes = vertAttrs.data();

    // 7. Color Target (RGBA16Float HDR 4x MSAA) & Depth State
    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = hdrFormat_;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    fragmentState.module = pbrShaderModule;
    fragmentState.entryPoint = ToWgpuStringView("fs_main");
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depthStencil.format = WGPUTextureFormat_Depth24Plus;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_True;
    depthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    // 8. Create Opaque Render Pipelines (DoubleSided=CullNone and SingleSided=CullBack)
    WGPURenderPipelineDescriptor rpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.module = pbrShaderModule;
    rpDesc.vertex.entryPoint = ToWgpuStringView("vs_main");
    rpDesc.vertex.bufferCount = 1;
    rpDesc.vertex.buffers = &vbLayout;
    rpDesc.fragment = &fragmentState;
    rpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpDesc.primitive.frontFace = WGPUFrontFace_CCW;
    rpDesc.primitive.cullMode = WGPUCullMode_None;
    rpDesc.depthStencil = &depthStencil;
    rpDesc.multisample.count = sampleCount_;
    rpDesc.multisample.mask = 0xFFFFFFFF;
    rpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    opaquePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);

    rpDesc.primitive.cullMode = WGPUCullMode_Back;
    opaqueBackCulledPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &rpDesc);

    // 8b. Create Shadow Depth Render Pipeline
    if (shadowShaderModule) {
        WGPUDepthStencilState shadowDepthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
        shadowDepthStencil.format = WGPUTextureFormat_Depth32Float;
        shadowDepthStencil.depthWriteEnabled = WGPUOptionalBool_True;
        shadowDepthStencil.depthCompare = WGPUCompareFunction_LessEqual;
        shadowDepthStencil.depthBias = 2;
        shadowDepthStencil.depthBiasSlopeScale = 2.0f;
        shadowDepthStencil.depthBiasClamp = 0.0f;

        WGPUFragmentState shadowFragState = WGPU_FRAGMENT_STATE_INIT;
        shadowFragState.module = shadowShaderModule;
        shadowFragState.entryPoint = ToWgpuStringView("fs_shadow");
        shadowFragState.targetCount = 0;
        shadowFragState.targets = nullptr;

        WGPURenderPipelineDescriptor shadowRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        shadowRpDesc.layout = shadowPipelineLayout;
        shadowRpDesc.vertex.module = shadowShaderModule;
        shadowRpDesc.vertex.entryPoint = ToWgpuStringView("vs_shadow");
        shadowRpDesc.vertex.bufferCount = 1;
        shadowRpDesc.vertex.buffers = &vbLayout;
        shadowRpDesc.fragment = &shadowFragState;
        shadowRpDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        shadowRpDesc.primitive.cullMode = WGPUCullMode_None;
        shadowRpDesc.depthStencil = &shadowDepthStencil;
        shadowRpDesc.multisample.count = 1;
        shadowRpDesc.multisample.mask = 0xFFFFFFFF;
        shadowRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

        shadowPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &shadowRpDesc);
    }

    // 8c. Create Wireframe Render Pipeline
    if (wfShaderModule) {
        WGPUFragmentState wfFragState = WGPU_FRAGMENT_STATE_INIT;
        wfFragState.module = wfShaderModule;
        wfFragState.entryPoint = ToWgpuStringView("fs_main");
        wfFragState.targetCount = 1;
        wfFragState.targets = &colorTarget;

        WGPUDepthStencilState wfDepthStencil = depthStencil;
        wfDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;

        WGPURenderPipelineDescriptor wfRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        wfRpDesc.layout = pipelineLayout;
        wfRpDesc.vertex.module = wfShaderModule;
        wfRpDesc.vertex.entryPoint = ToWgpuStringView("vs_main");
        wfRpDesc.vertex.bufferCount = 1;
        wfRpDesc.vertex.buffers = &vbLayout;
        wfRpDesc.fragment = &wfFragState;
        wfRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
        wfRpDesc.primitive.cullMode = WGPUCullMode_None;
        wfRpDesc.depthStencil = &wfDepthStencil;
        wfRpDesc.multisample.count = sampleCount_;
        wfRpDesc.multisample.mask = 0xFFFFFFFF;
        wfRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

        wireframePipeline_ = wgpuDeviceCreateRenderPipeline(device_, &wfRpDesc);
    }

    // 8d. Create AABB Wireframe Render Pipeline
    WGPUShaderSourceWGSL aabbWgslDesc = WGPU_SHADER_SOURCE_WGSL_INIT;
    aabbWgslDesc.code = ToWgpuStringView(kAabbShaderWGSL);

    WGPUShaderModuleDescriptor aabbSmDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    aabbSmDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&aabbWgslDesc);
    aabbSmDesc.label = ToWgpuStringView("AABB_WGSL");
    ShaderModuleHandle aabbShaderModule{wgpuDeviceCreateShaderModule(device_, &aabbSmDesc)};

    WGPUBindGroupLayoutEntry aabbBglEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    aabbBglEntry.binding = 0;
    aabbBglEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    aabbBglEntry.buffer.type = WGPUBufferBindingType_Uniform;
    aabbBglEntry.buffer.minBindingSize = sizeof(AabbUniforms);

    WGPUBindGroupLayoutDescriptor aabbBglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    aabbBglDesc.entryCount = 1;
    aabbBglDesc.entries = &aabbBglEntry;
    aabbBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(device_, &aabbBglDesc);

    WGPUPipelineLayoutDescriptor aabbPlDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    aabbPlDesc.bindGroupLayoutCount = 1;
    aabbPlDesc.bindGroupLayouts = aabbBindGroupLayout_.AddressOf();
    PipelineLayoutHandle aabbPlLayout{wgpuDeviceCreatePipelineLayout(device_, &aabbPlDesc)};

    std::array<WGPUVertexAttribute, 2> aabbAttrs{};
    aabbAttrs[0].format = WGPUVertexFormat_Float32x3;
    aabbAttrs[0].offset = 0;
    aabbAttrs[0].shaderLocation = 0;

    aabbAttrs[1].format = WGPUVertexFormat_Float32x4;
    aabbAttrs[1].offset = 16;
    aabbAttrs[1].shaderLocation = 1;

    WGPUVertexBufferLayout aabbVbLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    aabbVbLayout.arrayStride = sizeof(AabbVertex);
    aabbVbLayout.stepMode = WGPUVertexStepMode_Vertex;
    aabbVbLayout.attributeCount = aabbAttrs.size();
    aabbVbLayout.attributes = aabbAttrs.data();

    WGPUFragmentState aabbFragState = WGPU_FRAGMENT_STATE_INIT;
    aabbFragState.module = aabbShaderModule;
    aabbFragState.entryPoint = ToWgpuStringView("fs_aabb");
    aabbFragState.targetCount = 1;
    aabbFragState.targets = &colorTarget;

    WGPUDepthStencilState aabbDepthStencil = depthStencil;
    aabbDepthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    aabbDepthStencil.depthCompare = WGPUCompareFunction_LessEqual;

    WGPURenderPipelineDescriptor aabbRpDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    aabbRpDesc.layout = aabbPlLayout;
    aabbRpDesc.vertex.module = aabbShaderModule;
    aabbRpDesc.vertex.entryPoint = ToWgpuStringView("vs_aabb");
    aabbRpDesc.vertex.bufferCount = 1;
    aabbRpDesc.vertex.buffers = &aabbVbLayout;
    aabbRpDesc.fragment = &aabbFragState;
    aabbRpDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
    aabbRpDesc.primitive.cullMode = WGPUCullMode_None;
    aabbRpDesc.depthStencil = &aabbDepthStencil;
    aabbRpDesc.multisample.count = sampleCount_;
    aabbRpDesc.multisample.mask = 0xFFFFFFFF;
    aabbRpDesc.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    aabbPipeline_ = wgpuDeviceCreateRenderPipeline(device_, &aabbRpDesc);

    aabbVertexBufferSize_ = 512 * 1024;
    WGPUBufferDescriptor boxVbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    boxVbDesc.size = aabbVertexBufferSize_;
    boxVbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    boxVbDesc.label = ToWgpuStringView("AABB_DynamicVertexBuffer");
    aabbVertexBuffer_ = wgpuDeviceCreateBuffer(device_, &boxVbDesc);

    WGPUBufferDescriptor aabbUbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    aabbUbDesc.size = sizeof(AabbUniforms);
    aabbUbDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    aabbUbDesc.label = ToWgpuStringView("AABB_Uniforms");
    aabbUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &aabbUbDesc);

    WGPUBindGroupEntry aabbBgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
    aabbBgEntry.binding = 0;
    aabbBgEntry.buffer = aabbUniformBuffer_;
    aabbBgEntry.offset = 0;
    aabbBgEntry.size = sizeof(AabbUniforms);

    WGPUBindGroupDescriptor aabbBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    aabbBgDesc.layout = aabbBindGroupLayout_;
    aabbBgDesc.entryCount = 1;
    aabbBgDesc.entries = &aabbBgEntry;
    aabbBindGroup_ = wgpuDeviceCreateBindGroup(device_, &aabbBgDesc);

    // 9. Create Shadow Map 2048x2048 Depth32Float Texture & Comparison Sampler
    WGPUTextureDescriptor shadowTexDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    shadowTexDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    shadowTexDesc.dimension = WGPUTextureDimension_2D;
    shadowTexDesc.size = WGPUExtent3D{ kShadowMapSize, kShadowMapSize, 1 };
    shadowTexDesc.format = WGPUTextureFormat_Depth32Float;
    shadowTexDesc.mipLevelCount = 1;
    shadowTexDesc.sampleCount = 1;
    shadowTexDesc.label = ToWgpuStringView("DirectionalShadowMap");
    shadowDepthTexture_ = wgpuDeviceCreateTexture(device_, &shadowTexDesc);
    if (shadowDepthTexture_) {
        shadowDepthView_ = wgpuTextureCreateView(shadowDepthTexture_, nullptr);
    }

    WGPUSamplerDescriptor shadowSampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    shadowSampDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    shadowSampDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    shadowSampDesc.addressModeW = WGPUAddressMode_ClampToEdge;
    shadowSampDesc.magFilter = WGPUFilterMode_Linear;
    shadowSampDesc.minFilter = WGPUFilterMode_Linear;
    shadowSampDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    shadowSampDesc.compare = WGPUCompareFunction_LessEqual;
    shadowSampDesc.maxAnisotropy = 1;
    shadowSampler_ = wgpuDeviceCreateSampler(device_, &shadowSampDesc);

    if (shadowDepthView_) {
        CommandEncoderHandle initEnc{wgpuDeviceCreateCommandEncoder(device_, nullptr)};
        WGPURenderPassDepthStencilAttachment clearDepth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        clearDepth.view = shadowDepthView_;
        clearDepth.depthLoadOp = WGPULoadOp_Clear;
        clearDepth.depthStoreOp = WGPUStoreOp_Store;
        clearDepth.depthClearValue = 1.0f;

        WGPURenderPassDescriptor clearPassDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        clearPassDesc.colorAttachmentCount = 0;
        clearPassDesc.depthStencilAttachment = &clearDepth;
        RenderPassEncoderHandle clearPass{wgpuCommandEncoderBeginRenderPass(initEnc, &clearPassDesc)};
        wgpuRenderPassEncoderEnd(clearPass);
        CommandBufferHandle initCmd{wgpuCommandEncoderFinish(initEnc, nullptr)};
        WGPUCommandBuffer rawCmd{initCmd.Get()};
        wgpuQueueSubmit(queue_, 1, &rawCmd);
    }

    // 10. Create Samplers: Trilinear 16x Anisotropic + Base-Mip-Only Sampler
    WGPUSamplerDescriptor sampDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampDesc.addressModeU = WGPUAddressMode_Repeat;
    sampDesc.addressModeV = WGPUAddressMode_Repeat;
    sampDesc.addressModeW = WGPUAddressMode_Repeat;
    sampDesc.magFilter = WGPUFilterMode_Linear;
    sampDesc.minFilter = WGPUFilterMode_Linear;
    sampDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampDesc.lodMinClamp = 0.0f;
    sampDesc.lodMaxClamp = 32.0f;
    sampDesc.maxAnisotropy = 16;
    defaultSampler_ = wgpuDeviceCreateSampler(device_, &sampDesc);

    WGPUSamplerDescriptor noMipDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    noMipDesc.addressModeU = WGPUAddressMode_Repeat;
    noMipDesc.addressModeV = WGPUAddressMode_Repeat;
    noMipDesc.addressModeW = WGPUAddressMode_Repeat;
    noMipDesc.magFilter = WGPUFilterMode_Linear;
    noMipDesc.minFilter = WGPUFilterMode_Linear;
    noMipDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    noMipDesc.lodMinClamp = 0.0f;
    noMipDesc.lodMaxClamp = 0.0f;
    noMipDesc.maxAnisotropy = 1;
    noMipSampler_ = wgpuDeviceCreateSampler(device_, &noMipDesc);

    // 11. Create SceneLightingUniforms buffer (for L2 SH + Clustered Forward+ KHR_lights_punctual)
    WGPUBufferDescriptor slDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    slDesc.size = sizeof(SceneLightingUniforms);
    slDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    slDesc.label = ToWgpuStringView("SceneLightingUniforms");
    sceneLightingBuffer_ = wgpuDeviceCreateBuffer(device_, &slDesc);

    // 12. Initialize Compute IBL Pipelines (BRDF LUT + GGX Prefilter) & Build Default HDR Environment
    InitializeComputeIBLPipelines();
    SetEnvironmentPreset(EnvironmentPreset::MediterraneanCourtyard, nullptr);

    // 13. Initialize Fullscreen HDR Tonemapping Pipeline
    InitializeTonemapPipeline();

    // 14. Helper to create 1x1 fallback textures
    auto create1x1Texture = [&](std::span<const uint8_t, 4> rgba, std::string_view label) -> std::pair<WGPUTexture, WGPUTextureView> {
        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.size = WGPUExtent3D{ 1, 1, 1 };
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.label = WGPUStringView{ label.data(), label.size() };
        WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = tex;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = 4;
        dataLayout.rowsPerImage = 1;

        WGPUExtent3D writeSize{ 1, 1, 1 };
        wgpuQueueWriteTexture(queue_, &copyDst, rgba.data(), rgba.size(), &dataLayout, &writeSize);

        WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
        return { tex, view };
    };

    constexpr std::array<uint8_t, 4> whitePixels{ 255, 255, 255, 255 };
    auto [whiteTex, whiteView] = create1x1Texture(whitePixels, "DefaultWhiteTexture");
    defaultWhiteTexture_ = whiteTex;
    defaultWhiteTextureView_ = whiteView;

    constexpr std::array<uint8_t, 4> normalPixels{ 128, 128, 255, 255 };
    auto [normTex, normView] = create1x1Texture(normalPixels, "DefaultNormalTexture");
    defaultNormalTexture_ = normTex;
    defaultNormalTextureView_ = normView;

    constexpr std::array<uint8_t, 4> blackPixels{ 0, 0, 0, 255 };
    auto [blackTex, blackView] = create1x1Texture(blackPixels, "DefaultBlackTexture");
    defaultBlackTexture_ = blackTex;
    defaultBlackTextureView_ = blackView;

    initialized_ = (opaquePipeline_ != nullptr && tonemapPipeline_ != nullptr);

    // Initialize modular passes and compute engines
    if (auto res = aabbRenderer_.Initialize(device_, queue_, surfaceFormat_, sampleCount_); !res) {
        BGL_LOG_WARN("Failed to initialize AabbRenderer: {}", res.error());
    }
    if (auto res = postProcessPass_.Initialize(device_, queue_, surfaceFormat_); !res) {
        BGL_LOG_WARN("Failed to initialize PostProcessPass: {}", res.error());
    }
    if (auto res = shadowPass_.Initialize(device_, queue_, defaultSampler_.Get(), defaultWhiteTextureView_.Get()); !res) {
        BGL_LOG_WARN("Failed to initialize ShadowPass: {}", res.error());
    }
    if (auto res = gpuCuller_.Initialize(device_, queue_); !res) {
        BGL_LOG_WARN("Failed to initialize GpuCuller: {}", res.error());
    }
    if (auto res = gpuRayPicker_.Initialize(device_, queue_); !res) {
        BGL_LOG_WARN("Failed to initialize GpuRayPicker: {}", res.error());
    }

    BGL_LOG_INFO("Hardware WebGPU RenderPipeline initialized (RGBA16F HDR + Tonemapping + Compute IBL + Forward+ Lights + Native Compressed Textures)!");
    return {};
}

void PipelineManager::SetMipmappingEnabled(bool enabled, scene::Scene* activeScene) {
    if (mipmappingEnabled_ == enabled) return;
    mipmappingEnabled_ = enabled;
    if (activeScene) {
        RebuildMaterialBindGroups(*activeScene);
    }
}

void PipelineManager::RebuildMaterialBindGroups(scene::Scene& scene) {
    if (!device_ || !bindGroupLayout_) return;

    auto getTexView = [&](int texIdx, WGPUTextureView fallback) -> WGPUTextureView {
        if (texIdx >= 0 && texIdx < static_cast<int>(scene.gpuTextures.size())) {
            if (scene.gpuTextures[texIdx].view) {
                return scene.gpuTextures[texIdx].view;
            }
        }
        return fallback;
    };

    WGPUSampler activeSampler{mipmappingEnabled_ ? defaultSampler_.Get() : noMipSampler_.Get()};

    auto rebuildPrimitive = [&](scene::MeshPrimitive& prim) -> void {
        if (!prim.gpuUniformBuffer) return;
        if (prim.gpuBindGroup) {
            wgpuBindGroupRelease(prim.gpuBindGroup);
            prim.gpuBindGroup = nullptr;
        }

        int baseColorTexIdx{-1};
        int mrTexIdx{-1};
        int normalTexIdx{-1};
        int emissiveTexIdx{-1};
        int occlusionTexIdx{-1};
        int clearcoatTexIdx{-1};
        int transmissionTexIdx{-1};
        int sheenTexIdx{-1};

        if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
            const auto& mat = scene.materials[prim.materialIndex];
            baseColorTexIdx = mat.baseColorTextureIndex;
            mrTexIdx = mat.metallicRoughnessTextureIndex;
            normalTexIdx = mat.normalTextureIndex;
            emissiveTexIdx = mat.emissiveTextureIndex;
            occlusionTexIdx = mat.occlusionTextureIndex;
            clearcoatTexIdx = mat.clearcoatTextureIndex;
            transmissionTexIdx = mat.transmissionTextureIndex;
            sheenTexIdx = mat.sheenColorTextureIndex;
        }

        std::array<WGPUBindGroupEntry, 16> bgEntries{};
        bgEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = prim.gpuUniformBuffer;
        bgEntries[0].offset = 0;
        bgEntries[0].size = sizeof(PrimitiveUniforms);

        bgEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[1].binding = 1;
        bgEntries[1].sampler = activeSampler;

        bgEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[2].binding = 2;
        bgEntries[2].textureView = getTexView(baseColorTexIdx, defaultWhiteTextureView_);

        bgEntries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[3].binding = 3;
        bgEntries[3].textureView = getTexView(mrTexIdx, defaultWhiteTextureView_);

        bgEntries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[4].binding = 4;
        bgEntries[4].textureView = getTexView(normalTexIdx, defaultNormalTextureView_);

        bgEntries[5] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[5].binding = 5;
        bgEntries[5].textureView = getTexView(emissiveTexIdx, defaultBlackTextureView_);

        bgEntries[6] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[6].binding = 6;
        bgEntries[6].textureView = getTexView(occlusionTexIdx, defaultWhiteTextureView_);

        bgEntries[7] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[7].binding = 7;
        bgEntries[7].textureView = getTexView(clearcoatTexIdx, defaultWhiteTextureView_);

        bgEntries[8] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[8].binding = 8;
        bgEntries[8].textureView = getTexView(transmissionTexIdx, defaultWhiteTextureView_);

        bgEntries[9] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[9].binding = 9;
        bgEntries[9].textureView = getTexView(sheenTexIdx, defaultWhiteTextureView_);

        bgEntries[10] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[10].binding = 10;
        bgEntries[10].textureView = envCubemapView_;

        bgEntries[11] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[11].binding = 11;
        bgEntries[11].textureView = shadowDepthView_;

        bgEntries[12] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[12].binding = 12;
        bgEntries[12].sampler = shadowSampler_;

        bgEntries[13] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[13].binding = 13;
        bgEntries[13].textureView = brdfLutView_ ? brdfLutView_.Get() : defaultWhiteTextureView_.Get();

        bgEntries[14] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[14].binding = 14;
        bgEntries[14].buffer = sceneLightingBuffer_;
        bgEntries[14].offset = 0;
        bgEntries[14].size = sizeof(SceneLightingUniforms);

        bgEntries[15] = WGPU_BIND_GROUP_ENTRY_INIT;
        bgEntries[15].binding = 15;
        bgEntries[15].textureView = opaqueColorCopyView_ ? opaqueColorCopyView_.Get() : defaultWhiteTextureView_.Get();

        WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgDesc.layout = bindGroupLayout_;
        bgDesc.entryCount = bgEntries.size();
        bgDesc.entries = bgEntries.data();
        prim.gpuBindGroup = wgpuDeviceCreateBindGroup(device_, &bgDesc);

        if (prim.gpuShadowBindGroup) {
            wgpuBindGroupRelease(prim.gpuShadowBindGroup);
            prim.gpuShadowBindGroup = nullptr;
        }
        if (shadowBindGroupLayout_) {
            WGPUTextureView baseView = defaultWhiteTextureView_.Get();
            if (baseColorTexIdx >= 0 && static_cast<size_t>(baseColorTexIdx) < scene.gpuTextures.size() && scene.gpuTextures[baseColorTexIdx].view) {
                baseView = scene.gpuTextures[baseColorTexIdx].view;
            }
            std::array<WGPUBindGroupEntry, 3> shadowEntries{};
            shadowEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
            shadowEntries[0].binding = 0;
            shadowEntries[0].buffer = prim.gpuUniformBuffer;
            shadowEntries[0].offset = 0;
            shadowEntries[0].size = sizeof(PrimitiveUniforms);

            shadowEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
            shadowEntries[1].binding = 1;
            shadowEntries[1].sampler = defaultSampler_.Get();

            shadowEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
            shadowEntries[2].binding = 2;
            shadowEntries[2].textureView = baseView;

            WGPUBindGroupDescriptor shadowBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            shadowBgDesc.layout = shadowBindGroupLayout_;
            shadowBgDesc.entryCount = shadowEntries.size();
            shadowBgDesc.entries = shadowEntries.data();
            prim.gpuShadowBindGroup = wgpuDeviceCreateBindGroup(device_, &shadowBgDesc);
        }
    };

    // Rebuild for all shared meshes
    for (auto& mesh : scene.meshes) {
        if (mesh) {
            for (auto& prim : mesh->primitives) {
                rebuildPrimitive(prim);
            }
        }
    }

    auto rebuildNode = [&](auto& self, scene::SceneNode& node) -> void {
        for (auto& prim : node.GetPrimitives()) {
            rebuildPrimitive(prim);
        }
        for (auto& child : node.children) {
            if (child) self(self, *child);
        }
    };

    for (auto& rootNode : scene.rootNodes) {
        if (rootNode) rebuildNode(rebuildNode, *rootNode);
    }
}

void PipelineManager::PrepareScene(scene::Scene& scene) {
    if (!device_ || !queue_ || !bindGroupLayout_) return;

    std::vector<bool> isNormalTexture(scene.cpuImages.size(), false);
    std::vector<bool> isSrgbTexture(scene.cpuImages.size(), false);
    for (const auto& mat : scene.materials) {
        if (mat.baseColorTextureIndex >= 0 && static_cast<size_t>(mat.baseColorTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.baseColorTextureIndex] = true;
        }
        if (mat.emissiveTextureIndex >= 0 && static_cast<size_t>(mat.emissiveTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.emissiveTextureIndex] = true;
        }
        if (mat.sheenColorTextureIndex >= 0 && static_cast<size_t>(mat.sheenColorTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.sheenColorTextureIndex] = true;
        }
        if (mat.normalTextureIndex >= 0 && static_cast<size_t>(mat.normalTextureIndex) < isNormalTexture.size()) {
            isNormalTexture[mat.normalTextureIndex] = true;
            isSrgbTexture[mat.normalTextureIndex] = false;
        }
        if (mat.metallicRoughnessTextureIndex >= 0 && static_cast<size_t>(mat.metallicRoughnessTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.metallicRoughnessTextureIndex] = false;
        }
        if (mat.occlusionTextureIndex >= 0 && static_cast<size_t>(mat.occlusionTextureIndex) < isSrgbTexture.size()) {
            isSrgbTexture[mat.occlusionTextureIndex] = false;
        }
    }

    // Pre-generate CPU mip pyramids only for uncompressed RGBA8 textures
    struct MipLevelBuffer {
        uint32_t width{0};
        uint32_t height{0};
        std::vector<uint8_t> pixels{};
    };
    std::vector<std::vector<MipLevelBuffer>> precomputedMips(scene.cpuImages.size());

    auto buildMipPyramidForTexture = [&](size_t texIdx) {
        const auto& img = scene.cpuImages[texIdx];
        if (img.isCompressed || img.width == 0 || img.height == 0 || img.rgba.empty()) return;
        if (texIdx >= scene.gpuTextures.size() || scene.gpuTextures[texIdx].view != nullptr) return;

        const uint32_t mipLevels{CalculateMipLevels(img.width, img.height)};
        if (mipLevels <= 1) return;

        auto& chain = precomputedMips[texIdx];
        chain.resize(mipLevels - 1);

        std::span<const uint8_t> prevSpan{img.rgba};
        uint32_t prevW{img.width};
        uint32_t prevH{img.height};

        for (uint32_t level{1}; level < mipLevels; ++level) {
            const uint32_t curW{std::max(1u, prevW / 2)};
            const uint32_t curH{std::max(1u, prevH / 2)};

            chain[level - 1].width = curW;
            chain[level - 1].height = curH;
            chain[level - 1].pixels = GenerateNextMipLevel(prevSpan, prevW, prevH, curW, curH, isNormalTexture[texIdx]);

            prevSpan = std::span<const uint8_t>{chain[level - 1].pixels};
            prevW = curW;
            prevH = curH;
        }
    };

#if !defined(__EMSCRIPTEN__)
    if (scene.cpuImages.size() > 1) {
        core::ThreadPool::Instance().ParallelFor(0, scene.cpuImages.size(), [&](size_t idx) {
            buildMipPyramidForTexture(idx);
        });
    } else if (scene.cpuImages.size() == 1) {
        buildMipPyramidForTexture(0);
    }
#else
    for (size_t i{0}; i < scene.cpuImages.size(); ++i) {
        buildMipPyramidForTexture(i);
    }
#endif

    // 1. Upload CPU decoded images (either native block-compressed BC7/ASTC/ETC2 or RGBA8) to WebGPU Textures
    for (size_t i{0}; i < scene.cpuImages.size(); ++i) {
        const auto& img = scene.cpuImages[i];
        if (img.width == 0 || img.height == 0 || img.rgba.empty()) continue;
        if (i >= scene.gpuTextures.size() || scene.gpuTextures[i].view != nullptr) continue;

        if (img.isCompressed && img.compressionFormat != scene::KtxTranscodeTarget::Rgba32) {
            WGPUTextureFormat wgpuFmt{WGPUTextureFormat_BC7RGBAUnorm};
            std::string_view fmtName{"BC7"};
            switch (static_cast<scene::KtxTranscodeTarget>(img.compressionFormat)) {
                case scene::KtxTranscodeTarget::Bc7:
                    wgpuFmt = isSrgbTexture[i] ? WGPUTextureFormat_BC7RGBAUnormSrgb : WGPUTextureFormat_BC7RGBAUnorm;
                    fmtName = isSrgbTexture[i] ? "BC7RGBAUnormSrgb" : "BC7RGBAUnorm";
                    break;
                case scene::KtxTranscodeTarget::Astc4x4:
                    wgpuFmt = isSrgbTexture[i] ? WGPUTextureFormat_ASTC4x4UnormSrgb : WGPUTextureFormat_ASTC4x4Unorm;
                    fmtName = isSrgbTexture[i] ? "ASTC4x4UnormSrgb" : "ASTC4x4Unorm";
                    break;
                case scene::KtxTranscodeTarget::Etc2Rgba:
                    wgpuFmt = isSrgbTexture[i] ? WGPUTextureFormat_ETC2RGBA8UnormSrgb : WGPUTextureFormat_ETC2RGBA8Unorm;
                    fmtName = isSrgbTexture[i] ? "ETC2RGBA8UnormSrgb" : "ETC2RGBA8Unorm";
                    break;
                default:
                    break;
            }

            // Count valid 4x4 block-aligned mip levels
            uint32_t validCompressedMips{0};
            for (const auto& mip : img.mipLevels) {
                if ((mip.width % 4u) == 0u && (mip.height % 4u) == 0u && !mip.data.empty()) {
                    ++validCompressedMips;
                } else {
                    break;
                }
            }
            validCompressedMips = std::max(1u, validCompressedMips);

            WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
            texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            texDesc.dimension = WGPUTextureDimension_2D;
            texDesc.size = WGPUExtent3D{ img.width, img.height, 1 };
            texDesc.format = wgpuFmt;
            texDesc.mipLevelCount = validCompressedMips;
            texDesc.sampleCount = 1;
            texDesc.label = ToWgpuStringView("glTF_KTX2_CompressedTexture");
            WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

            for (uint32_t level{0}; level < validCompressedMips; ++level) {
                const uint32_t mipW{(level < img.mipLevels.size()) ? img.mipLevels[level].width : img.width};
                const uint32_t mipH{(level < img.mipLevels.size()) ? img.mipLevels[level].height : img.height};
                std::span<const uint8_t> mipBytes{
                    (level < img.mipLevels.size())
                        ? std::span<const uint8_t>{img.mipLevels[level].data.data(), img.mipLevels[level].data.size()}
                        : std::span<const uint8_t>{img.rgba.data(), img.rgba.size()}
                };

                const uint32_t blocksX{std::max(1u, (mipW + 3u) / 4u)};
                const uint32_t blocksY{std::max(1u, (mipH + 3u) / 4u)};

                WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                copyDst.texture = tex;
                copyDst.mipLevel = level;
                copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
                copyDst.aspect = WGPUTextureAspect_All;

                WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                dataLayout.offset = 0;
                dataLayout.bytesPerRow = blocksX * 16u; // BC7, ASTC 4x4, and ETC2 RGBA8 all use 16 bytes per 4x4 block
                dataLayout.rowsPerImage = blocksY;

                WGPUExtent3D writeSize{ mipW, mipH, 1 };
                wgpuQueueWriteTexture(queue_, &copyDst, mipBytes.data(), mipBytes.size(), &dataLayout, &writeSize);
            }

            WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
            scene.gpuTextures[i].texture = tex;
            scene.gpuTextures[i].view = view;
            scene.gpuTextures[i].sampler = defaultSampler_;
            scene.gpuTextures[i].width = img.width;
            scene.gpuTextures[i].height = img.height;
            BGL_LOG_DEBUG("[WebGPU Upload] Texture #{}: Native Compressed 2D Texture ({}x{}, {}, {} Mip Levels)",
                         i, img.width, img.height, fmtName, validCompressedMips);
            continue;
        }

        const uint32_t mipLevels{CalculateMipLevels(img.width, img.height)};

        WGPUTextureDescriptor texDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.size = WGPUExtent3D{ img.width, img.height, 1 };
        texDesc.format = isSrgbTexture[i] ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm;
        texDesc.mipLevelCount = mipLevels;
        texDesc.sampleCount = 1;
        texDesc.label = ToWgpuStringView("glTF_Texture");
        WGPUTexture tex = wgpuDeviceCreateTexture(device_, &texDesc);

        WGPUTexelCopyTextureInfo copyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        copyDst.texture = tex;
        copyDst.mipLevel = 0;
        copyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = img.width * 4;
        dataLayout.rowsPerImage = img.height;

        WGPUExtent3D writeSize{ img.width, img.height, 1 };
        std::span<const uint8_t> mip0Span{img.rgba};
        wgpuQueueWriteTexture(queue_, &copyDst, mip0Span.data(), mip0Span.size(), &dataLayout, &writeSize);

        std::span<const MipLevelBuffer> mipChain{precomputedMips[i]};
        for (size_t mIdx{0}; mIdx < mipChain.size(); ++mIdx) {
            const auto& mipBuf = mipChain[mIdx];
            const uint32_t level{static_cast<uint32_t>(mIdx + 1)};

            WGPUTexelCopyTextureInfo mipCopyDst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            mipCopyDst.texture = tex;
            mipCopyDst.mipLevel = level;
            mipCopyDst.origin = WGPUOrigin3D{ 0, 0, 0 };
            mipCopyDst.aspect = WGPUTextureAspect_All;

            WGPUTexelCopyBufferLayout mipDataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            mipDataLayout.offset = 0;
            mipDataLayout.bytesPerRow = mipBuf.width * 4;
            mipDataLayout.rowsPerImage = mipBuf.height;

            WGPUExtent3D mipWriteSize{ mipBuf.width, mipBuf.height, 1 };
            wgpuQueueWriteTexture(queue_, &mipCopyDst, mipBuf.pixels.data(), mipBuf.pixels.size(), &mipDataLayout, &mipWriteSize);
        }

        WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

        scene.gpuTextures[i].texture = tex;
        scene.gpuTextures[i].view = view;
        scene.gpuTextures[i].sampler = defaultSampler_;
        scene.gpuTextures[i].width = img.width;
        scene.gpuTextures[i].height = img.height;
        BGL_LOG_DEBUG("[WebGPU Upload] Texture #{}: Created 2D Texture ({}x{}, {}, {} Mip Levels, NormalMap={}), wrote {} bytes to GPU",
                     i, img.width, img.height,
                     isSrgbTexture[i] ? "RGBA8UnormSrgb" : "RGBA8Unorm",
                     mipLevels, isNormalTexture[i] ? "Yes" : "No", img.rgba.size());
    }

    // 2. Prepare vertex buffers, index buffers, uniform buffers, and shadow/main bind groups for each primitive
    auto preparePrimitive = [&](scene::MeshPrimitive& prim) -> void {
        if (prim.vertices.empty()) return;

        const size_t vbSize{prim.vertices.size() * sizeof(core::Vertex)};
        const size_t ibSize{prim.indices.size() * sizeof(uint32_t)};

        if (!prim.gpuVertexBuffer) {
            WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            vbDesc.size = (vbSize + 3) & ~3;
            vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            vbDesc.label = ToWgpuStringView("VertexBuffer");
            prim.gpuVertexBuffer = wgpuDeviceCreateBuffer(device_, &vbDesc);
            wgpuQueueWriteBuffer(queue_, prim.gpuVertexBuffer, 0, prim.vertices.data(), vbSize);
        }

        if (!prim.gpuIndexBuffer && !prim.indices.empty()) {
            const bool useUint16 = (prim.vertices.size() < 65536);
            prim.indexFormat = useUint16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
            if (useUint16) {
                std::vector<uint16_t> indices16;
                indices16.reserve(prim.indices.size() + 1);
                for (uint32_t idx : prim.indices) {
                    indices16.push_back(static_cast<uint16_t>(idx));
                }
                if (indices16.size() % 2 != 0) {
                    indices16.push_back(0); // Pad to ensure multiple of 4 bytes for wgpuQueueWriteBuffer
                }
                const size_t ib16Size{indices16.size() * sizeof(uint16_t)};
                WGPUBufferDescriptor ibDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ibDesc.size = ib16Size;
                ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
                ibDesc.label = ToWgpuStringView("IndexBuffer_U16");
                prim.gpuIndexBuffer = wgpuDeviceCreateBuffer(device_, &ibDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuIndexBuffer, 0, indices16.data(), ib16Size);
            } else {
                WGPUBufferDescriptor ibDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                ibDesc.size = (ibSize + 3) & ~3;
                ibDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
                ibDesc.label = ToWgpuStringView("IndexBuffer_U32");
                prim.gpuIndexBuffer = wgpuDeviceCreateBuffer(device_, &ibDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuIndexBuffer, 0, prim.indices.data(), ibSize);
            }
        }

        if (!prim.gpuWireframeIndexBuffer) {
            std::vector<uint32_t> lineIndices;
            if (!prim.indices.empty()) {
                lineIndices.reserve(prim.indices.size() * 2);
                for (size_t i{0}; i + 2 < prim.indices.size(); i += 3) {
                    const uint32_t a{prim.indices[i]};
                    const uint32_t b{prim.indices[i + 1]};
                    const uint32_t c{prim.indices[i + 2]};
                    lineIndices.push_back(a); lineIndices.push_back(b);
                    lineIndices.push_back(b); lineIndices.push_back(c);
                    lineIndices.push_back(c); lineIndices.push_back(a);
                }
            } else {
                lineIndices.reserve(prim.vertices.size() * 2);
                for (size_t i{0}; i + 2 < prim.vertices.size(); i += 3) {
                    const uint32_t a{static_cast<uint32_t>(i)};
                    const uint32_t b{static_cast<uint32_t>(i + 1)};
                    const uint32_t c{static_cast<uint32_t>(i + 2)};
                    lineIndices.push_back(a); lineIndices.push_back(b);
                    lineIndices.push_back(b); lineIndices.push_back(c);
                    lineIndices.push_back(c); lineIndices.push_back(a);
                }
            }
            if (!lineIndices.empty()) {
                prim.wireframeIndexCount = static_cast<uint32_t>(lineIndices.size());
                const size_t wfIbSize{lineIndices.size() * sizeof(uint32_t)};
                WGPUBufferDescriptor wfIbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
                wfIbDesc.size = (wfIbSize + 3) & ~3;
                wfIbDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
                wfIbDesc.label = ToWgpuStringView("WireframeIndexBuffer");
                prim.gpuWireframeIndexBuffer = wgpuDeviceCreateBuffer(device_, &wfIbDesc);
                wgpuQueueWriteBuffer(queue_, prim.gpuWireframeIndexBuffer, 0, lineIndices.data(), wfIbSize);
            }
        }

        if (!prim.gpuUniformBuffer) {
            WGPUBufferDescriptor ubDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            ubDesc.size = sizeof(PrimitiveUniforms);
            ubDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            ubDesc.label = ToWgpuStringView("PrimitiveUniforms");
            prim.gpuUniformBuffer = wgpuDeviceCreateBuffer(device_, &ubDesc);
        }

        if (!prim.gpuShadowBindGroup && shadowBindGroupLayout_) {
            int baseColorTexIdx{-1};
            if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                baseColorTexIdx = scene.materials[prim.materialIndex].baseColorTextureIndex;
            }
            WGPUTextureView baseView = defaultWhiteTextureView_.Get();
            if (baseColorTexIdx >= 0 && static_cast<size_t>(baseColorTexIdx) < scene.gpuTextures.size() && scene.gpuTextures[baseColorTexIdx].view) {
                baseView = scene.gpuTextures[baseColorTexIdx].view;
            }
            std::array<WGPUBindGroupEntry, 3> shadowEntries{};
            shadowEntries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
            shadowEntries[0].binding = 0;
            shadowEntries[0].buffer = prim.gpuUniformBuffer;
            shadowEntries[0].offset = 0;
            shadowEntries[0].size = sizeof(PrimitiveUniforms);

            shadowEntries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
            shadowEntries[1].binding = 1;
            shadowEntries[1].sampler = defaultSampler_.Get();

            shadowEntries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
            shadowEntries[2].binding = 2;
            shadowEntries[2].textureView = baseView;

            WGPUBindGroupDescriptor shadowBgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            shadowBgDesc.layout = shadowBindGroupLayout_;
            shadowBgDesc.entryCount = shadowEntries.size();
            shadowBgDesc.entries = shadowEntries.data();
            prim.gpuShadowBindGroup = wgpuDeviceCreateBindGroup(device_, &shadowBgDesc);
        }
    };

    // Prepare all shared meshes once
    for (auto& mesh : scene.meshes) {
        if (mesh) {
            for (auto& prim : mesh->primitives) {
                preparePrimitive(prim);
            }
        }
    }

    auto prepareNode = [&](auto& self, scene::SceneNode& node) -> void {
        for (auto& prim : node.GetPrimitives()) {
            preparePrimitive(const_cast<scene::MeshPrimitive&>(prim));
        }
        for (auto& child : node.children) {
            if (child) {
                self(self, *child);
            }
        }
    };

    for (auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            prepareNode(prepareNode, *rootNode);
        }
    }

    RebuildMaterialBindGroups(scene);
    shadowMapDirty_ = true;

    // 3. Pre-cache static scene & primitive AABB wireframe vertices
    cachedSceneAabbVertices_.clear();
    batchedAabbVertices_.clear();
    if (scene.sceneAABB.minBound.x <= scene.sceneAABB.maxBound.x) {
        AppendAABBLines(scene.sceneAABB, glm::vec4{1.0f, 0.84f, 0.0f, 1.0f});
    }
    auto cacheNodeAABBs = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform) -> void {
        const glm::mat4 worldMat{parentTransform * node.localTransform};
        for (const auto& prim : node.GetPrimitives()) {
            if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                AppendAABBLines(prim.boundingBox.Transformed(worldMat), glm::vec4{0.2f, 0.85f, 1.0f, 0.8f});
            }
        }
        for (const auto& child : node.children) {
            if (child) self(self, *child, worldMat);
        }
    };
    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode) cacheNodeAABBs(cacheNodeAABBs, *rootNode, glm::mat4{1.0f});
    }
    cachedSceneAabbVertices_ = std::move(batchedAabbVertices_);
    batchedAabbVertices_.clear();
}

void PipelineManager::UpdateSceneLightingUniforms(scene::Scene& scene, const glm::mat4& viewMatrix, const glm::mat4& projMatrix, uint32_t viewportW, uint32_t viewportH, float zNear, float zFar) {
    if (!sceneLightingBuffer_) return;

    SceneLightingUniforms sl{};
    sl.shIrradiance = shIrradiance_;
    sl.viewMatrix = viewMatrix;
    sl.gridParams = glm::vec4{static_cast<float>(std::max(1u, viewportW)), static_cast<float>(std::max(1u, viewportH)), zNear, zFar};

    uint32_t lightCount{0};
    bool hasExplicitDirectional{false};

    auto collectNodeLights = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform) -> void {
        const glm::mat4 worldMat{parentTransform * node.localTransform};
        if (node.lightIndex >= 0 && static_cast<size_t>(node.lightIndex) < scene.lights.size() && lightCount < 16u) {
            const auto& srcLight = scene.lights[static_cast<size_t>(node.lightIndex)];
            auto& dstLight = sl.lights[lightCount];

            const glm::vec3 worldPos{worldMat[3]};
            // In glTF KHR_lights_punctual, directional and spot lights shine along the node's local -Z axis
            const glm::vec3 rawDir{glm::vec3{worldMat * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}}};
            const glm::vec3 lightToTargetDir{glm::length(rawDir) > 1e-5f ? glm::normalize(rawDir) : glm::vec3{0.0f, -1.0f, 0.0f}};

            if (srcLight.type == scene::LightType::Directional) {
                dstLight.posOrDir = glm::vec4{-lightToTargetDir, 0.0f};
                hasExplicitDirectional = true;
            } else if (srcLight.type == scene::LightType::Point) {
                dstLight.posOrDir = glm::vec4{worldPos, 1.0f};
            } else {
                dstLight.posOrDir = glm::vec4{worldPos, 2.0f};
            }

            dstLight.colorInt = glm::vec4{srcLight.color * srcLight.intensity, srcLight.range};
            dstLight.spotParams = glm::vec4{lightToTargetDir, 0.0f};

            const float cosInner{std::cos(srcLight.innerConeAngle)};
            const float cosOuter{std::cos(srcLight.outerConeAngle)};
            const float angleScale{1.0f / std::max(0.001f, cosInner - cosOuter)};
            const float angleOffset{-cosOuter * angleScale};
            sl.spotAngles[lightCount] = glm::vec4{
                angleScale,
                angleOffset,
                (srcLight.type == scene::LightType::Directional) ? 1.0f : 0.0f,
                0.0f
            };
            ++lightCount;
        }

        for (const auto& child : node.children) {
            if (child) self(self, *child, worldMat);
        }
    };

    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode) collectNodeLights(collectNodeLights, *rootNode, glm::mat4{1.0f});
    }

    // Also include any unattached scene.lights if none were referenced by nodes
    if (lightCount == 0 && !scene.lights.empty()) {
        for (const auto& srcLight : scene.lights) {
            if (lightCount >= 16u) break;
            auto& dstLight = sl.lights[lightCount];
            if (srcLight.type == scene::LightType::Directional) {
                dstLight.posOrDir = glm::vec4{currentLightDir_, 0.0f};
                hasExplicitDirectional = true;
            } else {
                dstLight.posOrDir = glm::vec4{0.0f, 2.0f, 0.0f, (srcLight.type == scene::LightType::Point) ? 1.0f : 2.0f};
            }
            dstLight.colorInt = glm::vec4{srcLight.color * srcLight.intensity, srcLight.range};
            dstLight.spotParams = glm::vec4{0.0f, -1.0f, 0.0f, 0.0f};
            const float cosInner{std::cos(srcLight.innerConeAngle)};
            const float cosOuter{std::cos(srcLight.outerConeAngle)};
            const float angleScale{1.0f / std::max(0.001f, cosInner - cosOuter)};
            const float angleOffset{-cosOuter * angleScale};
            sl.spotAngles[lightCount] = glm::vec4{angleScale, angleOffset, 1.0f, 0.0f};
            ++lightCount;
        }
    }

    activePunctualLightCount_ = lightCount;
    BuildClusteredLightGrid(std::span<const GpuPunctualLight>{sl.lights.data(), lightCount}, lightCount, viewMatrix, projMatrix, zNear, zFar, sl.clusterMasks);

    sl.envParams = glm::vec4{
        envRotation_,
        static_cast<float>(lightCount),
        1.0f, // Enable Clustered Forward+ 3D light culling
        hasExplicitDirectional ? 0.0f : 1.0f
    };

    wgpuQueueWriteBuffer(queue_, sceneLightingBuffer_, 0, &sl, sizeof(sl));
}

void PipelineManager::RenderShadowPass(WGPUCommandEncoder encoder, scene::Scene& scene, const glm::vec3& lightDir, const scene::Quadtree* spatialIndex) {
    if (!shadowsEnabled_) return;
    if (shadowPass_.IsInitialized()) {
        const glm::vec3 baseDir{glm::length(lightDir) > 1e-5f ? glm::normalize(lightDir) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
        const float c{std::cos(-envRotation_)};
        const float s{std::sin(-envRotation_)};
        currentLightDir_ = glm::normalize(glm::vec3{c * baseDir.x + s * baseDir.z, baseDir.y, -s * baseDir.x + c * baseDir.z});
        shadowPass_.Render(encoder, scene, currentLightDir_, spatialIndex);
        currentLightViewProj_ = shadowPass_.GetLightViewProjMatrix();
        return;
    }
    (void)spatialIndex;
    const glm::vec3 baseDir{glm::length(lightDir) > 1e-5f ? glm::normalize(lightDir) : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
    // Rotate primary sun direction by -envRotation_ so shadows match rotated HDR environment
    const float c{std::cos(-envRotation_)};
    const float s{std::sin(-envRotation_)};
    currentLightDir_ = glm::normalize(glm::vec3{c * baseDir.x + s * baseDir.z, baseDir.y, -s * baseDir.x + c * baseDir.z});
    currentLightViewProj_ = ComputeDirectionalLightMatrix(scene.sceneAABB, currentLightDir_);

    if (!shadowsEnabled_ || !encoder || !shadowPipeline_ || !shadowDepthView_) {
        return;
    }

    if (!shadowMapDirty_ && lastShadowScene_ == &scene && glm::distance(lastShadowLightDir_, currentLightDir_) < 1e-4f) {
        return;
    }

    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depthAttachment.view = shadowDepthView_;
    depthAttachment.depthLoadOp = WGPULoadOp_Clear;
    depthAttachment.depthStoreOp = WGPUStoreOp_Store;
    depthAttachment.depthClearValue = 1.0f;
    depthAttachment.depthReadOnly = WGPU_FALSE;

    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 0;
    passDesc.colorAttachments = nullptr;
    passDesc.depthStencilAttachment = &depthAttachment;
    passDesc.label = ToWgpuStringView("DirectionalShadowPass");

    RenderPassEncoderHandle shadowPass{wgpuCommandEncoderBeginRenderPass(encoder, &passDesc)};
    if (!shadowPass) return;

    wgpuRenderPassEncoderSetPipeline(shadowPass, shadowPipeline_);

    for (const auto& rootNode : scene.rootNodes) {
        if (rootNode) {
            RenderShadowNode(shadowPass, scene, *rootNode, glm::mat4{1.0f});
        }
    }

    wgpuRenderPassEncoderEnd(shadowPass);

    shadowMapDirty_ = false;
    lastShadowScene_ = &scene;
    lastShadowLightDir_ = currentLightDir_;
}

void PipelineManager::RenderShadowNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& parentTransform) {
    const glm::mat4 worldMat{parentTransform * node.localTransform};

    for (auto& prim : node.GetPrimitives()) {
        if (!prim.gpuVertexBuffer || !prim.gpuShadowBindGroup || !prim.gpuUniformBuffer) continue;

        PrimitiveUniforms uniforms{};
        uniforms.model = worldMat;
        uniforms.lightViewProj = currentLightViewProj_;
        if (prim.materialIndex >= 0 && static_cast<size_t>(prim.materialIndex) < scene.materials.size()) {
            const auto& mat = scene.materials[prim.materialIndex];
            uniforms.baseColorFactor = mat.baseColorFactor;
            uniforms.alphaCutoff = mat.alphaCutoff;
            uniforms.alphaMode = static_cast<uint32_t>(mat.alphaMode);
            if (mat.baseColorTextureIndex >= 0) {
                uniforms.flags |= MaterialFlags::HasBaseColorTex;
            }
        }
        wgpuQueueWriteBuffer(queue_, prim.gpuUniformBuffer, 0, &uniforms, sizeof(uniforms));

        wgpuRenderPassEncoderSetBindGroup(pass, 0, prim.gpuShadowBindGroup, 0, nullptr);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, prim.gpuVertexBuffer, 0, prim.vertices.size() * sizeof(core::Vertex));

        if (prim.gpuIndexBuffer && !prim.indices.empty()) {
            const size_t indexSize = (prim.indexFormat == WGPUIndexFormat_Uint16) ? sizeof(uint16_t) : sizeof(uint32_t);
            wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuIndexBuffer, prim.indexFormat, 0, prim.indices.size() * indexSize);
            wgpuRenderPassEncoderDrawIndexed(pass, static_cast<uint32_t>(prim.indices.size()), 1, 0, 0, 0);
        } else {
            wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(prim.vertices.size()), 1, 0, 0);
        }
    }

    for (const auto& child : node.children) {
        if (child) {
            RenderShadowNode(pass, scene, *child, worldMat);
        }
    }
}

void PipelineManager::RenderScene(WGPURenderPassEncoder pass, scene::Scene& scene, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe, const scene::Quadtree* spatialIndex, const glm::mat4& viewMatrix, const glm::mat4& projMatrix, uint32_t viewportW, uint32_t viewportH, float zNear, float zFar) {
    if (!pass || !initialized_) return;
    aabbFrameByteOffset_ = 0;

    UpdateSceneLightingUniforms(scene, viewMatrix, projMatrix, viewportW, viewportH, zNear, zFar);

    WGPURenderPipeline currentBoundPipeline{wireframe && wireframePipeline_ ? wireframePipeline_ : (opaqueBackCulledPipeline_ ? opaqueBackCulledPipeline_ : opaquePipeline_)};
    wgpuRenderPassEncoderSetPipeline(pass, currentBoundPipeline);

    const core::Frustum frustum{core::Frustum::FromViewProjection(viewProj)};
    lastVisiblePrimitiveCount_ = 0;
    lastOccludedPrimitiveCount_ = 0;

    struct RenderItem {
        std::shared_ptr<scene::SceneNode> node;
        size_t primitiveIndex;
        glm::mat4 worldTransform;
        float distance;
        int materialIndex;
        bool isTransparent;
    };

    if (spatialIndex) {
        auto visibleItems = spatialIndex->QueryVisible(frustum, cameraPos, viewProj, occlusionCullingEnabled_);
        lastVisiblePrimitiveCount_ = visibleItems.size();
        lastOccludedPrimitiveCount_ = spatialIndex->GetLastOccludedCount();

        std::vector<RenderItem> items;
        items.reserve(visibleItems.size());
        for (const auto& vis : visibleItems) {
            if (!vis.node) continue;
            auto prims = vis.node->GetPrimitives();
            if (vis.primitiveIndex >= prims.size()) continue;
            const auto& prim = prims[vis.primitiveIndex];
            const bool isTransparent = (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) &&
                                       (scene.materials[prim.materialIndex].alphaMode == scene::AlphaMode::Blend);
            items.push_back(RenderItem{
                .node = vis.node,
                .primitiveIndex = vis.primitiveIndex,
                .worldTransform = vis.node->worldTransform,
                .distance = vis.distanceToCamera,
                .materialIndex = prim.materialIndex,
                .isTransparent = isTransparent
            });
        }

        // Sort draw calls: Opaques front-to-back (minimizing overdraw and grouping by material),
        // Transparents back-to-front (for correct alpha blending)
        std::sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b) {
            if (a.isTransparent != b.isTransparent) {
                return !a.isTransparent; // opaques first
            }
            if (!a.isTransparent) {
                if (a.materialIndex != b.materialIndex && std::abs(a.distance - b.distance) < 0.2f) {
                    return a.materialIndex < b.materialIndex;
                }
                return a.distance < b.distance; // nearest first
            }
            return a.distance > b.distance; // farthest first
        });

        for (const auto& item : items) {
            auto prims = item.node->GetPrimitives();
            auto& prim = const_cast<scene::MeshPrimitive&>(prims[item.primitiveIndex]);
            RenderPrimitive(pass, scene, prim, item.worldTransform, viewProj, cameraPos, wireframe, currentBoundPipeline);
        }
    } else {
        std::vector<RenderItem> items;
        auto collectNode = [&](auto& self, const std::shared_ptr<scene::SceneNode>& node, const glm::mat4& parentMat) -> void {
            if (!node) return;
            const glm::mat4 worldMat{parentMat * node->localTransform};
            if (node->worldAABB.minBound.x <= node->worldAABB.maxBound.x && !frustum.IntersectsAABB(node->worldAABB)) {
                return;
            }
            auto prims = node->GetPrimitives();
            for (size_t pIdx = 0; pIdx < prims.size(); ++pIdx) {
                const auto& prim = prims[pIdx];
                const float dist = glm::length(prim.boundingBox.Transformed(worldMat).Center() - cameraPos);
                const bool isTransparent = (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) &&
                                           (scene.materials[prim.materialIndex].alphaMode == scene::AlphaMode::Blend);
                items.push_back(RenderItem{
                    .node = node,
                    .primitiveIndex = pIdx,
                    .worldTransform = worldMat,
                    .distance = dist,
                    .materialIndex = prim.materialIndex,
                    .isTransparent = isTransparent
                });
            }
            for (const auto& child : node->children) {
                self(self, child, worldMat);
            }
        };
        for (const auto& root : scene.rootNodes) {
            collectNode(collectNode, root, glm::mat4{1.0f});
        }
        lastVisiblePrimitiveCount_ = items.size();
        std::sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b) {
            if (a.isTransparent != b.isTransparent) return !a.isTransparent;
            if (!a.isTransparent) {
                if (a.materialIndex != b.materialIndex && std::abs(a.distance - b.distance) < 0.2f) {
                    return a.materialIndex < b.materialIndex;
                }
                return a.distance < b.distance;
            }
            return a.distance > b.distance;
        });
        for (const auto& item : items) {
            auto prims = item.node->GetPrimitives();
            auto& prim = const_cast<scene::MeshPrimitive&>(prims[item.primitiveIndex]);
            RenderPrimitive(pass, scene, prim, item.worldTransform, viewProj, cameraPos, wireframe, currentBoundPipeline);
        }
    }
}

void PipelineManager::RenderNode(WGPURenderPassEncoder pass, scene::Scene& scene, scene::SceneNode& node, const glm::mat4& viewProj, const glm::mat4& parentTransform, const glm::vec3& cameraPos, bool wireframe, const core::Frustum& frustum, WGPURenderPipeline& currentBoundPipeline) {
    const glm::mat4 worldMat{parentTransform * node.localTransform};

    if (node.worldAABB.minBound.x <= node.worldAABB.maxBound.x && !frustum.IntersectsAABB(node.worldAABB)) {
        return;
    }

    const auto prims = node.GetPrimitives();
    lastVisiblePrimitiveCount_ += prims.size();
    for (auto& prim : prims) {
        RenderPrimitive(pass, scene, const_cast<scene::MeshPrimitive&>(prim), worldMat, viewProj, cameraPos, wireframe, currentBoundPipeline);
    }

    for (const auto& child : node.children) {
        if (child) {
            RenderNode(pass, scene, *child, viewProj, worldMat, cameraPos, wireframe, frustum, currentBoundPipeline);
        }
    }
}

void PipelineManager::RenderPrimitive(WGPURenderPassEncoder pass, scene::Scene& scene, scene::MeshPrimitive& prim, const glm::mat4& worldMat, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool wireframe, WGPURenderPipeline& currentBoundPipeline) {
    if (!prim.gpuVertexBuffer || !prim.gpuBindGroup || !prim.gpuUniformBuffer) return;

    PrimitiveUniforms uniforms{};
    uniforms.viewProj = viewProj;
    uniforms.model = worldMat;
    uniforms.lightViewProj = currentLightViewProj_;
    uniforms.cameraPos = glm::vec4{cameraPos, 1.0f};
    uniforms.lightDir = glm::vec4{currentLightDir_, 0.0f};
    uniforms.envMipCount = static_cast<float>(envMipLevels_);

    uint32_t flags{0};
    if (bumpMappingEnabled_)      flags |= MaterialFlags::EnableBumpMapping;
    if (mipmappingEnabled_)       flags |= MaterialFlags::EnableMipmapping;
    if (shadowsEnabled_)          flags |= MaterialFlags::EnableShadows;
    if (reflectionsEnabled_)      flags |= MaterialFlags::EnableReflections;
    if (ambientOcclusionEnabled_) flags |= MaterialFlags::EnableAmbientOcclusion;
    if (parallaxEnabled_)         flags |= MaterialFlags::EnableParallaxOcclusion;

    bool isDoubleSided{false};
    if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
        const auto& mat = scene.materials[prim.materialIndex];
        uniforms.baseColorFactor = mat.baseColorFactor;
        uniforms.metallicFactor = mat.metallicFactor;
        uniforms.roughnessFactor = mat.roughnessFactor;
        uniforms.normalScale = mat.normalScale;
        uniforms.bumpScale = mat.bumpScale * (parallaxEnabled_ ? (parallaxScale_ / 0.035f) : 1.0f);
        uniforms.occlusionStrength = mat.occlusionStrength * aoIntensity_;
        uniforms.emissiveFactor = glm::vec4{mat.emissiveFactor, mat.emissiveStrength};
        uniforms.alphaCutoff = mat.alphaCutoff;
        uniforms.alphaMode = std::to_underlying(mat.alphaMode);
        uniforms.ior = mat.ior;
        uniforms.clearcoatFactor = mat.clearcoatFactor;
        uniforms.clearcoatRoughness = mat.clearcoatRoughnessFactor;
        uniforms.transmissionFactor = mat.transmissionFactor;
        uniforms.specularFactor = mat.specularFactor;
        uniforms.sheenColorFactor = glm::vec4{mat.sheenColorFactor, mat.sheenRoughnessFactor};
        uniforms.specularColorFactor = glm::vec4{mat.specularColorFactor, 0.0f};
        uniforms.thicknessFactor = mat.thicknessFactor;
        uniforms.attenuationDistance = mat.attenuationDistance;
        uniforms.attenuationColor = glm::vec4{mat.attenuationColor, 1.0f};

        if (mat.baseColorTextureIndex >= 0) flags |= MaterialFlags::HasBaseColorTex;
        if (mat.metallicRoughnessTextureIndex >= 0) flags |= MaterialFlags::HasMetallicRoughnessTex;
        if (mat.normalTextureIndex >= 0) flags |= MaterialFlags::HasNormalTex;
        if (mat.emissiveTextureIndex >= 0) flags |= MaterialFlags::HasEmissiveTex;
        if (mat.occlusionTextureIndex >= 0) flags |= MaterialFlags::HasOcclusionTex;
        if (mat.clearcoatTextureIndex >= 0) flags |= MaterialFlags::HasClearcoatTex;
        if (mat.transmissionTextureIndex >= 0) flags |= MaterialFlags::HasTransmissionTex;
        if (mat.thicknessTextureIndex >= 0) flags |= MaterialFlags::HasThicknessTex;
        if (mat.sheenColorTextureIndex >= 0) flags |= MaterialFlags::HasSheenTex;
        if (mat.unlit) flags |= MaterialFlags::Unlit;
        if (mat.doubleSided) {
            flags |= MaterialFlags::DoubleSided;
            isDoubleSided = true;
        }
    }
    uniforms.flags = flags;

    if (!wireframe && opaqueBackCulledPipeline_ && opaquePipeline_) {
        WGPURenderPipeline desiredPipeline{isDoubleSided ? opaquePipeline_ : opaqueBackCulledPipeline_};
        if (desiredPipeline != currentBoundPipeline) {
            wgpuRenderPassEncoderSetPipeline(pass, desiredPipeline);
            currentBoundPipeline = desiredPipeline;
        }
    }

    wgpuQueueWriteBuffer(queue_, prim.gpuUniformBuffer, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetBindGroup(pass, 0, prim.gpuBindGroup, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, prim.gpuVertexBuffer, 0, prim.vertices.size() * sizeof(core::Vertex));

    if (wireframe && prim.gpuWireframeIndexBuffer && prim.wireframeIndexCount > 0) {
        wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuWireframeIndexBuffer, WGPUIndexFormat_Uint32, 0, prim.wireframeIndexCount * sizeof(uint32_t));
        wgpuRenderPassEncoderDrawIndexed(pass, prim.wireframeIndexCount, 1, 0, 0, 0);
    } else if (prim.gpuIndexBuffer && !prim.indices.empty()) {
        const size_t indexSize = (prim.indexFormat == WGPUIndexFormat_Uint16) ? sizeof(uint16_t) : sizeof(uint32_t);
        wgpuRenderPassEncoderSetIndexBuffer(pass, prim.gpuIndexBuffer, prim.indexFormat, 0, prim.indices.size() * indexSize);
        wgpuRenderPassEncoderDrawIndexed(pass, static_cast<uint32_t>(prim.indices.size()), 1, 0, 0, 0);
    } else {
        wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(prim.vertices.size()), 1, 0, 0);
    }
}

void PipelineManager::AppendAABBLines(const core::AABB& aabb, const glm::vec4& color) {
    if (aabb.minBound.x > aabb.maxBound.x) return;
    const glm::vec3 minB{aabb.minBound};
    const glm::vec3 maxB{aabb.maxBound};

    const std::array<glm::vec3, 8> c{{
        glm::vec3{minB.x, minB.y, minB.z},
        glm::vec3{maxB.x, minB.y, minB.z},
        glm::vec3{maxB.x, maxB.y, minB.z},
        glm::vec3{minB.x, maxB.y, minB.z},
        glm::vec3{minB.x, minB.y, maxB.z},
        glm::vec3{maxB.x, minB.y, maxB.z},
        glm::vec3{maxB.x, maxB.y, maxB.z},
        glm::vec3{minB.x, maxB.y, maxB.z}
    }};

    static constexpr std::array<size_t, 24> kIndices{{
        0, 1,  1, 2,  2, 3,  3, 0,
        4, 5,  5, 6,  6, 7,  7, 4,
        0, 4,  1, 5,  2, 6,  3, 7
    }};

    for (size_t idx : kIndices) {
        batchedAabbVertices_.push_back(AabbVertex{ .pos = c[idx], ._pad = 0.0f, .color = color });
    }
}

void PipelineManager::FlushAABBLines(WGPURenderPassEncoder pass, const glm::mat4& viewProj) {
    if (!pass || !aabbPipeline_ || batchedAabbVertices_.empty()) {
        batchedAabbVertices_.clear();
        return;
    }

    const size_t requiredBytes{batchedAabbVertices_.size() * sizeof(AabbVertex)};
    size_t writeOffset{aabbFrameByteOffset_};
    const size_t totalNeeded{writeOffset + requiredBytes};

    if (totalNeeded > aabbVertexBufferSize_ || !aabbVertexBuffer_) {
        aabbVertexBufferSize_ = std::max(totalNeeded * 2, size_t{512 * 1024});
        WGPUBufferDescriptor vbDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        vbDesc.size = aabbVertexBufferSize_;
        vbDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        vbDesc.label = ToWgpuStringView("AABB_DynamicVertexBuffer");
        aabbVertexBuffer_ = wgpuDeviceCreateBuffer(device_, &vbDesc);
        writeOffset = 0;
    }

    wgpuQueueWriteBuffer(queue_, aabbVertexBuffer_, writeOffset, batchedAabbVertices_.data(), requiredBytes);

    AabbUniforms uniforms{};
    uniforms.viewProj = viewProj;
    wgpuQueueWriteBuffer(queue_, aabbUniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpuRenderPassEncoderSetPipeline(pass, aabbPipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, aabbBindGroup_, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, aabbVertexBuffer_, writeOffset, requiredBytes);
    wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(batchedAabbVertices_.size()), 1, 0, 0);

    aabbFrameByteOffset_ = (writeOffset + requiredBytes + 255u) & ~size_t{255u};
    batchedAabbVertices_.clear();
}

void PipelineManager::RenderAABB(WGPURenderPassEncoder pass, const core::AABB& aabb, const glm::mat4& viewProj, const glm::vec4& color) {
    AppendAABBLines(aabb, color);
    FlushAABBLines(pass, viewProj);
}

void PipelineManager::BatchSceneAABBs(const scene::Scene& scene, bool showSceneAABB, bool showPrimitiveAABBs) {
    if (showSceneAABB && showPrimitiveAABBs && !cachedSceneAabbVertices_.empty()) {
        batchedAabbVertices_.insert(batchedAabbVertices_.end(), cachedSceneAabbVertices_.begin(), cachedSceneAabbVertices_.end());
        return;
    }

    if (showSceneAABB && scene.sceneAABB.minBound.x <= scene.sceneAABB.maxBound.x) {
        AppendAABBLines(scene.sceneAABB, glm::vec4{1.0f, 0.84f, 0.0f, 1.0f});
    }

    if (showPrimitiveAABBs) {
        auto renderNodeAABBs = [&](auto& self, const scene::SceneNode& node, const glm::mat4& parentTransform) -> void {
            const glm::mat4 worldMat{parentTransform * node.localTransform};
            for (const auto& prim : node.GetPrimitives()) {
                if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                    AppendAABBLines(prim.boundingBox.Transformed(worldMat), glm::vec4{0.2f, 0.85f, 1.0f, 0.8f});
                }
            }
            for (const auto& child : node.children) {
                if (child) self(self, *child, worldMat);
            }
        };

        for (const auto& rootNode : scene.rootNodes) {
            if (rootNode) renderNodeAABBs(renderNodeAABBs, *rootNode, glm::mat4{1.0f});
        }
    }
}

void PipelineManager::BatchQuadtreeBounds(const scene::Quadtree& quadtree) {
    std::vector<core::AABB> bounds{};
    quadtree.GetNodeBounds(bounds);
    for (const auto& b : bounds) {
        AppendAABBLines(b, glm::vec4{0.25f, 0.95f, 0.45f, 0.7f});
    }
}

void PipelineManager::RenderSceneAABBs(WGPURenderPassEncoder pass, const scene::Scene& scene, const glm::mat4& viewProj, bool showSceneAABB, bool showPrimitiveAABBs) {
    if (aabbRenderer_.IsInitialized()) {
        aabbRenderer_.BatchSceneAABBs(scene, showSceneAABB, showPrimitiveAABBs);
        aabbRenderer_.FlushAABBLines(pass, viewProj);
        return;
    }
    if (!pass || !aabbPipeline_) return;
    BatchSceneAABBs(scene, showSceneAABB, showPrimitiveAABBs);
    FlushAABBLines(pass, viewProj);
}

void PipelineManager::RenderQuadtreeBounds(WGPURenderPassEncoder pass, const scene::Quadtree& quadtree, const glm::mat4& viewProj) {
    if (aabbRenderer_.IsInitialized()) {
        aabbRenderer_.BatchQuadtreeBounds(quadtree);
        aabbRenderer_.FlushAABBLines(pass, viewProj);
        return;
    }
    if (!pass || !aabbPipeline_) return;
    BatchQuadtreeBounds(quadtree);
    FlushAABBLines(pass, viewProj);
}

void PipelineManager::EnsureOpaqueCopyTexture(uint32_t width, uint32_t height) {
    if (opaqueColorCopyTexture_ && opaqueCopyWidth_ == width && opaqueCopyHeight_ == height) {
        return;
    }
    opaqueCopyWidth_ = std::max(1u, width);
    opaqueCopyHeight_ = std::max(1u, height);

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = WGPUExtent3D{ opaqueCopyWidth_, opaqueCopyHeight_, 1 };
    desc.format = WGPUTextureFormat_RGBA16Float;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    desc.label = ToWgpuStringView("OpaqueColorCopyTexture");
    opaqueColorCopyTexture_ = wgpuDeviceCreateTexture(device_, &desc);
    if (opaqueColorCopyTexture_) {
        opaqueColorCopyView_ = wgpuTextureCreateView(opaqueColorCopyTexture_, nullptr);
    }
}

glm::vec3 PipelineManager::EvaluateBeerLambertVolumeAttenuation(const glm::vec3& attenuationColor, float attenuationDistance, float thickness, float NdotV) noexcept {
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

float PipelineManager::EvaluateScreenSpaceOcclusionFactor(float depthCenter, float depthNeighbor, float radius, float bias) noexcept {
    const float diff = depthCenter - depthNeighbor;
    if (diff > bias && diff < radius) {
        const float occ = (diff - bias) / std::max(radius - bias, 1e-4f);
        return std::clamp(1.0f - occ, 0.0f, 1.0f);
    }
    return 1.0f;
}

} // namespace bgl::gpu
