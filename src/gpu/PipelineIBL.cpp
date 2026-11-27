/**
 * @file PipelineIBL.cpp
 * @brief IBL cubemap generation, BRDF LUT & GGX prefilter compute pipelines, and L2 SH irradiance.
 */
#include "core/Log.hpp"
#include "core/ThreadPool.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/PipelineHelpers.hpp"
#include "gpu/ShaderManager.hpp"
#include "scene/Scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

namespace bgl::gpu {
using namespace detail;

std::vector<uint8_t> PipelineManager::GenerateEnvCubemapFaceMip(uint32_t faceIndex, uint32_t faceSize, float roughness,
                                                                const glm::vec3& sunDir) {
    std::vector<uint8_t> pixels(static_cast<size_t>(faceSize) * faceSize * 4);
    const glm::vec3 sun{glm::length(sunDir) > 1e-5f ? glm::normalize(sunDir)
                                                    : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};
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
            const glm::vec3 rimContrib{glm::vec3{0.65f, 0.80f, 1.0f} * std::pow(rimDot, glm::mix(64.0f, 2.0f, rough)) *
                                       glm::mix(0.45f, 0.1f, rough)};

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

std::vector<float> PipelineManager::GenerateProceduralHdrCubemapFace(uint32_t faceIndex, uint32_t faceSize,
                                                                     EnvironmentPreset preset,
                                                                     const glm::vec3& sunDir) {
    std::vector<float> rgba(static_cast<size_t>(faceSize) * faceSize * 4, 1.0f);
    const glm::vec3 baseSun{glm::length(sunDir) > 1e-5f ? glm::normalize(sunDir)
                                                        : glm::normalize(glm::vec3{0.35f, 1.0f, 0.25f})};

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
        zenith = glm::vec3{0.18f, 0.14f, 0.42f};
        horizon = glm::vec3{2.10f, 0.82f, 0.34f};
        ground = glm::vec3{0.22f, 0.14f, 0.12f};
        bounce = glm::vec3{0.65f, 0.30f, 0.18f};
        keyDir = glm::normalize(glm::vec3{0.78f, 0.28f, 0.55f});
        sunColor = glm::vec3{18.0f, 7.5f, 2.8f};
        fillColor = glm::vec3{0.55f, 0.85f, 1.85f};
        break;
    case EnvironmentPreset::OvercastAtelier:
        zenith = glm::vec3{1.35f, 1.42f, 1.50f};
        horizon = glm::vec3{0.95f, 0.98f, 1.04f};
        ground = glm::vec3{0.36f, 0.36f, 0.38f};
        bounce = glm::vec3{0.50f, 0.50f, 0.52f};
        keyDir = glm::normalize(glm::vec3{0.15f, 0.96f, 0.22f});
        sunColor = glm::vec3{4.5f, 4.6f, 4.8f};
        fillColor = glm::vec3{1.4f, 1.45f, 1.55f};
        break;
    case EnvironmentPreset::NightNeon:
        zenith = glm::vec3{0.02f, 0.03f, 0.08f};
        horizon = glm::vec3{0.08f, 0.12f, 0.24f};
        ground = glm::vec3{0.04f, 0.04f, 0.06f};
        bounce = glm::vec3{0.15f, 0.06f, 0.22f};
        keyDir = glm::normalize(glm::vec3{0.65f, 0.45f, 0.60f});
        sunColor = glm::vec3{0.8f, 8.5f, 11.0f}; // Cyan neon softbox
        fillDir = glm::normalize(glm::vec3{-0.70f, 0.35f, -0.62f});
        fillColor = glm::vec3{9.5f, 1.2f, 6.8f}; // Magenta neon softbox
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

std::vector<float> PipelineManager::ConvertEquirectHdrToCubemapFace(const scene::DecodedHdrImage& hdrImg,
                                                                    uint32_t faceIndex, uint32_t faceSize) {
    std::vector<float> rgba(static_cast<size_t>(faceSize) * faceSize * 4, 1.0f);
    if (!hdrImg.IsValid())
        return rgba;

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
            auto sanitizeHdrChannel = [](float v) noexcept {
                return std::isfinite(v) ? std::clamp(v, 0.0f, 65000.0f) : 0.0f;
            };
            rgba[dstIdx + 0] = sanitizeHdrChannel(c.r);
            rgba[dstIdx + 1] = sanitizeHdrChannel(c.g);
            rgba[dstIdx + 2] = sanitizeHdrChannel(c.b);
            rgba[dstIdx + 3] = 1.0f;
        }
    }
    return rgba;
}

std::array<glm::vec4, 9>
PipelineManager::ComputeSphericalHarmonicsL2(const std::array<std::vector<float>, 6>& cubemapFaces,
                                             uint32_t faceSize) noexcept {
    std::array<glm::vec3, 9> shAccum{};
    if (faceSize == 0) {
        return {};
    }

    double weightSum{0.0};
    for (uint32_t face{0}; face < 6; ++face) {
        const auto& faceData = cubemapFaces[face];
        if (faceData.size() < static_cast<size_t>(faceSize) * faceSize * 4)
            continue;

        for (uint32_t y{0}; y < faceSize; ++y) {
            for (uint32_t x{0}; x < faceSize; ++x) {
                const float u{((static_cast<float>(x) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};
                const float v{((static_cast<float>(y) + 0.5f) / static_cast<float>(faceSize)) * 2.0f - 1.0f};

                const float temp{1.0f + u * u + v * v};
                const float dOmega{4.0f / (std::sqrt(temp) * temp)};
                const glm::vec3 dir{CubeFaceDirection(face, u, v)};

                const size_t idx{(static_cast<size_t>(y) * faceSize + x) * 4};
                // Clamp extreme sun spikes and reject NaN/Inf during diffuse SH projection to avoid ringing/poisoning
                auto sanitizeDiffuse = [](float v) noexcept {
                    return std::isfinite(v) ? std::clamp(v, 0.0f, 25.0f) : 0.0f;
                };
                const glm::vec3 radiance{sanitizeDiffuse(faceData[idx + 0]), sanitizeDiffuse(faceData[idx + 1]),
                                         sanitizeDiffuse(faceData[idx + 2])};

                const std::array<float, 9> basis{{0.282095f, 0.488603f * dir.y, 0.488603f * dir.z, 0.488603f * dir.x,
                                                  1.092548f * dir.x * dir.y, 1.092548f * dir.y * dir.z,
                                                  0.315392f * (3.0f * dir.z * dir.z - 1.0f), 1.092548f * dir.x * dir.z,
                                                  0.546274f * (dir.x * dir.x - dir.y * dir.y)}};

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

glm::vec3 PipelineManager::EvaluateSphericalHarmonicsL2(const std::array<glm::vec4, 9>& shCoeffs,
                                                        const glm::vec3& normal) noexcept {
    const glm::vec3 n{glm::length(normal) > 1e-5f ? glm::normalize(normal) : glm::vec3{0.0f, 1.0f, 0.0f}};
    // Ramamoorthi & Hanrahan irradiance divided by pi so multiplying by diffuseColor yields diffuse exitance
    constexpr float kInvPi{1.0f / 3.14159265358979323846f};
    const glm::vec3 irr = glm::vec3{shCoeffs[0]} * 0.886227f + glm::vec3{shCoeffs[1]} * (1.023327f * n.y) +
                          glm::vec3{shCoeffs[2]} * (1.023327f * n.z) + glm::vec3{shCoeffs[3]} * (1.023327f * n.x) +
                          glm::vec3{shCoeffs[4]} * (0.858086f * n.x * n.y) +
                          glm::vec3{shCoeffs[5]} * (0.858086f * n.y * n.z) +
                          glm::vec3{shCoeffs[6]} * (0.247708f * (3.0f * n.z * n.z - 1.0f)) +
                          glm::vec3{shCoeffs[7]} * (0.858086f * n.x * n.z) +
                          glm::vec3{shCoeffs[8]} * (0.429043f * (n.x * n.x - n.y * n.y));
    return glm::max(irr * kInvPi, glm::vec3{0.0f});
}

glm::vec3 PipelineManager::ApplyToneMappingCPU(const glm::vec3& linearHdr, float exposure,
                                               ToneMapOperator op) noexcept {
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
        const glm::mat3 agxMat{0.842479062253094f,  0.0423282422610123f, 0.0423756549057051f,
                               0.0784335999999992f, 0.878468636469772f,  0.0784336f,
                               0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f};
        const glm::mat3 agxMatInv{1.19687900512017f,    -0.0528968517574562f, -0.0529716355144438f,
                                  -0.0980208811401368f, 1.15190312990417f,    -0.0980434501171241f,
                                  -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f};
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
        return glm::clamp(glm::vec3{std::pow(v.x, 2.2f), std::pow(v.y, 2.2f), std::pow(v.z, 2.2f)}, glm::vec3{0.0f},
                          glm::vec3{1.0f});
    }
    // ACES Filmic
    constexpr float a{2.51f};
    constexpr float b{0.03f};
    constexpr float c{2.43f};
    constexpr float d{0.59f};
    constexpr float e{0.14f};
    return glm::clamp((col * (a * col + b)) / (col * (c * col + d) + e), glm::vec3{0.0f}, glm::vec3{1.0f});
}

void PipelineManager::InitializeComputeIBLPipelines() {
    if (!device_)
        return;

    // 1. 2D Split-Sum BRDF LUT Texture (128x128 RGBA16Float with StorageBinding + TextureBinding)
    WGPUTextureDescriptor lutDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    lutDesc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    lutDesc.dimension = WGPUTextureDimension_2D;
    lutDesc.size = WGPUExtent3D{kBrdfLutSize, kBrdfLutSize, 1};
    lutDesc.format = WGPUTextureFormat_RGBA16Float;
    lutDesc.mipLevelCount = 1;
    lutDesc.sampleCount = 1;
    lutDesc.label = ToWgpuStringView("IBL_SplitSum_BRDF_LUT");
    brdfLutTexture_ = wgpuDeviceCreateTexture(device_, &lutDesc);
    if (brdfLutTexture_) {
        brdfLutView_ = wgpuTextureCreateView(brdfLutTexture_, nullptr);
    }

    // 2. BRDF LUT Compute Pipeline
    auto lutModuleRes = ShaderManager::Instance().LoadAndCompile(device_, "brdf_lut.wgsl", "BRDF_LUT_Compute_WGSL");
    if (!lutModuleRes) {
        BGL_LOG_WARN("BRDF LUT shader compilation failed: {}", lutModuleRes.error());
    }

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

    if (lutModuleRes && *lutModuleRes && lutPl) {
        WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cpDesc.layout = lutPl;
        cpDesc.compute.module = *lutModuleRes;
        cpDesc.compute.entryPoint = ToWgpuStringView("cs_brdf_lut");
        cpDesc.label = ToWgpuStringView("BRDF_LUT_ComputePipeline");
        brdfLutComputePipeline_ = wgpuDeviceCreateComputePipeline(device_, &cpDesc);
    }

    DispatchBrdfLutCompute();

    // 3. GGX Specular Cubemap Prefilter Compute Pipeline
    auto ggxModuleRes =
        ShaderManager::Instance().LoadAndCompile(device_, "ggx_prefilter.wgsl", "GGX_Prefilter_Compute_WGSL");
    if (!ggxModuleRes) {
        BGL_LOG_WARN("GGX Prefilter shader compilation failed: {}", ggxModuleRes.error());
    }

    std::array<WGPUBindGroupLayoutEntry, 4> ggxEntries{};
    ggxEntries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    ggxEntries[0].binding = 0;
    ggxEntries[0].visibility = WGPUShaderStage_Compute;
    ggxEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    ggxEntries[0].buffer.hasDynamicOffset = WGPU_TRUE;
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

    if (ggxModuleRes && *ggxModuleRes && ggxPl) {
        WGPUComputePipelineDescriptor cpDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cpDesc.layout = ggxPl;
        cpDesc.compute.module = *ggxModuleRes;
        cpDesc.compute.entryPoint = ToWgpuStringView("cs_ggx_prefilter");
        cpDesc.label = ToWgpuStringView("GGX_Prefilter_ComputePipeline");
        ggxPrefilterComputePipeline_ = wgpuDeviceCreateComputePipeline(device_, &cpDesc);
    }

    WGPUBufferDescriptor paramBufDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramBufDesc.size = 16u * 256u; // Up to 16 GGX mip levels at 256-byte dynamic uniform offset stride
    paramBufDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramBufDesc.label = ToWgpuStringView("GGXPrefilterParamsBuffer");
    ggxPrefilterUniformBuffer_ = wgpuDeviceCreateBuffer(device_, &paramBufDesc);
}

void PipelineManager::DispatchBrdfLutCompute() {
    if (!device_ || !queue_ || !brdfLutComputePipeline_ || !brdfLutView_)
        return;

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

void PipelineManager::BuildEnvironmentFromHdrFaces(const std::array<std::vector<float>, 6>& hdrFaces,
                                                   uint32_t faceSize) {
    if (!device_ || !queue_ || faceSize == 0)
        return;

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
    cubeDesc.size = WGPUExtent3D{faceSize, faceSize, 6};
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
        copyDst.origin = WGPUOrigin3D{0, 0, face};
        copyDst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = faceSize * 8; // 4 * sizeof(uint16_t)
        dataLayout.rowsPerImage = faceSize;

        WGPUExtent3D writeSize{faceSize, faceSize, 1};
        wgpuQueueWriteTexture(queue_, &copyDst, fp16Pixels.data(), fp16Pixels.size() * sizeof(uint16_t), &dataLayout,
                              &writeSize);
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

    // 5. Batch all GGX Specular Prefilter mip passes (1 .. envMipLevels_ - 1) into a single encoder/submit
    if (ggxPrefilterComputePipeline_ && ggxPrefilterComputeBgl_ && ggxPrefilterUniformBuffer_ && mip0CubeView &&
        envMipLevels_ > 1) {
        constexpr uint32_t kParamStride{256u};
        const uint32_t maxMips{std::min(envMipLevels_, 16u)};
        std::array<std::byte, 16u * kParamStride> stagedParams{};
        for (uint32_t mip{1}; mip < maxMips; ++mip) {
            const uint32_t mipSize{std::max(1u, faceSize >> mip)};
            const float roughness{static_cast<float>(mip) / static_cast<float>(envMipLevels_ - 1)};
            const PrefilterComputeParams params{.roughness = roughness,
                                                .faceSize = mipSize,
                                                .sourceSize = static_cast<float>(faceSize),
                                                .sampleCount = 64u};
            std::memcpy(stagedParams.data() + static_cast<size_t>(mip) * kParamStride, &params, sizeof(params));
        }
        wgpuQueueWriteBuffer(queue_, ggxPrefilterUniformBuffer_, 0, stagedParams.data(),
                             static_cast<uint64_t>(maxMips) * kParamStride);

        if (CommandEncoderHandle enc{wgpuDeviceCreateCommandEncoder(device_, nullptr)}) {
            std::vector<TextureViewHandle> dstViews;
            std::vector<BindGroupHandle> bindGroups;
            dstViews.reserve(maxMips);
            bindGroups.reserve(maxMips);

            for (uint32_t mip{1}; mip < maxMips; ++mip) {
                const uint32_t mipSize{std::max(1u, faceSize >> mip)};
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

                const uint32_t dynOffset{mip * kParamStride};
                if (ComputePassEncoderHandle cpass{wgpuCommandEncoderBeginComputePass(enc, nullptr)}) {
                    wgpuComputePassEncoderSetPipeline(cpass, ggxPrefilterComputePipeline_);
                    wgpuComputePassEncoderSetBindGroup(cpass, 0, bg, 1, &dynOffset);
                    wgpuComputePassEncoderDispatchWorkgroups(cpass, (mipSize + 7) / 8, (mipSize + 7) / 8, 6);
                    wgpuComputePassEncoderEnd(cpass);
                }

                dstViews.push_back(std::move(dstMipView));
                bindGroups.push_back(std::move(bg));
            }

            if (CommandBufferHandle cmd{wgpuCommandEncoderFinish(enc, nullptr)}) {
                WGPUCommandBuffer rawCmd{cmd.Get()};
                wgpuQueueSubmit(queue_, 1, &rawCmd);
            }
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
    RebuildSceneGlobalsBindGroup();
}

void PipelineManager::SetEnvironmentPreset(EnvironmentPreset preset, scene::Scene* activeScene) {
    (void)activeScene;
    currentEnvPreset_ = preset;
    if (!device_ || !queue_)
        return;

    std::array<std::vector<float>, 6> hdrFaces{};
#if !defined(__EMSCRIPTEN__)
    core::ThreadPool::Instance().ParallelFor(0, 6, [&](size_t face) {
        hdrFaces[face] =
            GenerateProceduralHdrCubemapFace(static_cast<uint32_t>(face), kEnvCubemapSize, preset, currentLightDir_);
    });
#else
    for (uint32_t face{0}; face < 6; ++face) {
        hdrFaces[face] = GenerateProceduralHdrCubemapFace(face, kEnvCubemapSize, preset, currentLightDir_);
    }
#endif

    BuildEnvironmentFromHdrFaces(hdrFaces, kEnvCubemapSize);
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

    (void)activeScene;
    currentEnvPreset_ = EnvironmentPreset::CustomHdr;
    BuildEnvironmentFromHdrFaces(hdrFaces, faceSize);
    BGL_LOG_INFO("Loaded custom HDR Environment ({}x{} equirect -> {}x{}x6 RGBA16F cubemap, {} GGX mips)",
                 hdrImage.width, hdrImage.height, faceSize, faceSize, envMipLevels_);
    return true;
}

} // namespace bgl::gpu
