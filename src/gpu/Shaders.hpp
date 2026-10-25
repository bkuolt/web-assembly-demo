/**
 * @file Shaders.hpp
 * @brief Embedded WGSL Shaders:
 *        - Compute Split-Sum BRDF LUT (`kBrdfLutComputeWGSL`)
 *        - Compute GGX Prefiltered Environment Cubemap (`kGgxPrefilterComputeWGSL`)
 *        - Full glTF 2.0 PBR + Spherical Harmonics (SH) Irradiance + BRDF LUT + Clustered Forward+ `KHR_lights_punctual` (`kPbrShaderWGSL`)
 *        - Fullscreen HDR Tonemapping Post-Processing (`kTonemapShaderWGSL`: Khronos PBR Neutral, AgX, ACES)
 *        - Directional Shadow Mapping (`kShadowShaderWGSL`) & Wireframe/AABB Shaders
 */
#pragma once

#include <string_view>

namespace bgl::gpu {

/**
 * @brief WebGPU Compute Shader for 2D Split-Sum BRDF Integration LUT (RGBA16Float, 128x128).
 */
inline constexpr std::string_view kBrdfLutComputeWGSL = R"(
@group(0) @binding(0) var brdfLut : texture_storage_2d<rgba16float, write>;

fn radicalInverseVdC(bitsIn : u32) -> f32 {
    var bits = bitsIn;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return f32(bits) * 2.3283064365386963e-10;
}

fn hammersley(i : u32, N : u32) -> vec2<f32> {
    return vec2<f32>(f32(i) / f32(N), radicalInverseVdC(i));
}

fn importanceSampleGGX(Xi : vec2<f32>, roughness : f32) -> vec3<f32> {
    let a = roughness * roughness;
    let phi = 6.28318530718 * Xi.x;
    let cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    let sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    return vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

fn geometrySchlickGGX_IBL(NdotV : f32, roughness : f32) -> f32 {
    let a = roughness;
    let k = (a * a) * 0.5;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

fn geometrySmith_IBL(NdotV : f32, NdotL : f32, roughness : f32) -> f32 {
    return geometrySchlickGGX_IBL(NdotV, roughness) * geometrySchlickGGX_IBL(NdotL, roughness);
}

@compute @workgroup_size(8, 8, 1)
fn cs_brdf_lut(@builtin(global_invocation_id) id : vec3<u32>) {
    let dims = textureDimensions(brdfLut);
    if (id.x >= dims.x || id.y >= dims.y) {
        return;
    }

    let NdotV = max((f32(id.x) + 0.5) / f32(dims.x), 0.001);
    let roughness = max((f32(id.y) + 0.5) / f32(dims.y), 0.001);

    let V = vec3<f32>(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    var A = 0.0;
    var B = 0.0;

    let sampleCount : u32 = 128u;
    for (var i : u32 = 0u; i < sampleCount; i = i + 1u) {
        let Xi = hammersley(i, sampleCount);
        let H = importanceSampleGGX(Xi, roughness);
        let L = normalize(2.0 * dot(V, H) * H - V);

        let NdotL = max(L.z, 0.0);
        let NdotH = max(H.z, 0.0);
        let VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            let G = geometrySmith_IBL(NdotV, NdotL, roughness);
            let G_Vis = (G * VdotH) / max(NdotH * NdotV, 1e-4);
            let Fc = pow(1.0 - VdotH, 5.0);

            A = A + (1.0 - Fc) * G_Vis;
            B = B + Fc * G_Vis;
        }
    }

    A = A / f32(sampleCount);
    B = B / f32(sampleCount);

    textureStore(brdfLut, vec2<i32>(i32(id.x), i32(id.y)), vec4<f32>(A, B, 0.0, 1.0));
}
)";

/**
 * @brief WebGPU Compute Shader for GGX Importance-Sampled Environment Cubemap Prefiltering.
 */
inline constexpr std::string_view kGgxPrefilterComputeWGSL = R"(
struct PrefilterParams {
    roughness : f32,
    faceSize : u32,
    sourceSize : f32,
    sampleCount : u32,
};

@group(0) @binding(0) var<uniform> params : PrefilterParams;
@group(0) @binding(1) var envSampler : sampler;
@group(0) @binding(2) var srcCubemap : texture_cube<f32>;
@group(0) @binding(3) var dstMipFaces : texture_storage_2d_array<rgba16float, write>;

fn radicalInverseVdC(bitsIn : u32) -> f32 {
    var bits = bitsIn;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return f32(bits) * 2.3283064365386963e-10;
}

fn hammersley(i : u32, N : u32) -> vec2<f32> {
    return vec2<f32>(f32(i) / f32(N), radicalInverseVdC(i));
}

fn cubeFaceDir(face : u32, u : f32, v : f32) -> vec3<f32> {
    switch (face) {
        case 0u: { return normalize(vec3<f32>( 1.0,   -v,   -u)); }
        case 1u: { return normalize(vec3<f32>(-1.0,   -v,    u)); }
        case 2u: { return normalize(vec3<f32>(   u,  1.0,    v)); }
        case 3u: { return normalize(vec3<f32>(   u, -1.0,   -v)); }
        case 4u: { return normalize(vec3<f32>(   u,   -v,  1.0)); }
        default: { return normalize(vec3<f32>(  -u,   -v, -1.0)); }
    }
}

fn distributionGGX_Prefilter(NdotH : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265359 * d * d, 1e-6);
}

fn importanceSampleGGX_World(Xi : vec2<f32>, N : vec3<f32>, roughness : f32) -> vec3<f32> {
    let a = roughness * roughness;
    let phi = 6.28318530718 * Xi.x;
    let cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    let sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    let H_tan = vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(N.z) < 0.999);
    let T = normalize(cross(up, N));
    let B = cross(N, T);
    return normalize(T * H_tan.x + B * H_tan.y + N * H_tan.z);
}

@compute @workgroup_size(8, 8, 1)
fn cs_ggx_prefilter(@builtin(global_invocation_id) id : vec3<u32>) {
    if (id.x >= params.faceSize || id.y >= params.faceSize || id.z >= 6u) {
        return;
    }

    let u = ((f32(id.x) + 0.5) / f32(params.faceSize)) * 2.0 - 1.0;
    let v = ((f32(id.y) + 0.5) / f32(params.faceSize)) * 2.0 - 1.0;
    let N = cubeFaceDir(id.z, u, v);
    let R = N;
    let V = R;

    var prefilteredColor = vec3<f32>(0.0);
    var totalWeight = 0.0;

    let sampleCount = max(params.sampleCount, 16u);
    for (var i : u32 = 0u; i < sampleCount; i = i + 1u) {
        let Xi = hammersley(i, sampleCount);
        let H = importanceSampleGGX_World(Xi, N, params.roughness);
        let L = normalize(2.0 * dot(V, H) * H - V);

        let NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            let sampleCol = min(textureSampleLevel(srcCubemap, envSampler, L, 0.0).rgb, vec3<f32>(64.0));
            prefilteredColor = prefilteredColor + sampleCol * NdotL;
            totalWeight = totalWeight + NdotL;
        }
    }

    prefilteredColor = prefilteredColor / max(totalWeight, 1e-4);
    textureStore(dstMipFaces, vec2<i32>(i32(id.x), i32(id.y)), i32(id.z), vec4<f32>(prefilteredColor, 1.0));
}
)";

/**
 * @brief Fullscreen HDR Tonemapping Post-Processing WGSL Shader.
 *        Supports Khronos PBR Neutral (0), AgX (1), and ACES Filmic (2) with Exposure control,
 *        real-time Directional Sun Lens Flares (anamorphic streaks, corona, chromatic ghosts),
 *        and sRGB conversion.
 */
inline constexpr std::string_view kTonemapShaderWGSL = R"(
struct TonemapUniforms {
    exposure : f32,
    operatorId : u32,
    isSrgbSurface : u32,
    lensFlareEnabled : u32,
    sunScreenPos : vec4<f32>, // xy = sun UV [0..1], z = visibility [0..1], w = aspect ratio (width / height)
    flareParams : vec4<f32>,  // x = intensity, y = haloRadius, z = chromaticDispersal, w = unused
    bloomParams : vec4<f32>,  // x = intensity, y = threshold, z = scatter, w = enabled
    ssaoParams : vec4<f32>,   // x = intensity, y = radius, z = bias, w = enabled
};

@group(0) @binding(0) var<uniform> ubo : TonemapUniforms;
@group(0) @binding(1) var hdrSampler : sampler;
@group(0) @binding(2) var hdrTexture : texture_2d<f32>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_tonemap(@builtin(vertex_index) vIdx : u32) -> VertexOutput {
    var out : VertexOutput;
    let x = f32(i32(vIdx << 1u) & 2) * 2.0 - 1.0;
    let y = f32(i32(vIdx & 2u)) * 2.0 - 1.0;
    out.clipPos = vec4<f32>(x, y, 0.0, 1.0);
    out.uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
    return out;
}

// 0: Khronos PBR Neutral Tonemapper (Official Khronos specification)
fn tonemapKhronosPbrNeutral(colorIn : vec3<f32>) -> vec3<f32> {
    var color = max(colorIn, vec3<f32>(0.0));
    let startCompression : f32 = 0.8 - 0.04;
    let desaturation : f32 = 0.15;

    let x = min(color.r, min(color.g, color.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    color = color - vec3<f32>(offset);

    let peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) {
        return color;
    }

    let d = 1.0 - startCompression;
    let newPeak = 1.0 - d * d / (peak + d - startCompression);
    color = color * (newPeak / max(peak, 1e-6));

    let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3<f32>(newPeak), g);
}

// 1: AgX Tonemapper (Troy Sobotka / Blender 4.0)
fn agxDefaultContrastApprox(x : vec3<f32>) -> vec3<f32> {
    let x2 = x * x;
    let x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

fn tonemapAgX(colorIn : vec3<f32>) -> vec3<f32> {
    let agxMat = mat3x3<f32>(
        vec3<f32>(0.842479062253094,  0.0423282422610123, 0.0423756549057051),
        vec3<f32>(0.0784335999999992, 0.878468636469772,  0.0784336),
        vec3<f32>(0.0792237451477643, 0.0791661274605434, 0.879142973793104)
    );
    let agxMatInv = mat3x3<f32>(
        vec3<f32>( 1.19687900512017,   -0.0528968517574562, -0.0529716355144438),
        vec3<f32>(-0.0980208811401368,  1.15190312990417,   -0.0980434501171241),
        vec3<f32>(-0.0990297440797205, -0.0989611768448433,  1.15107367264116)
    );

    let minEv : f32 = -12.47393;
    let maxEv : f32 = 4.026069;

    var val = agxMat * max(colorIn, vec3<f32>(1e-10));
    val = clamp(log2(val), vec3<f32>(minEv), vec3<f32>(maxEv));
    val = (val - vec3<f32>(minEv)) / (maxEv - minEv);
    val = agxDefaultContrastApprox(val);
    val = agxMatInv * val;
    return clamp(pow(max(val, vec3<f32>(0.0)), vec3<f32>(2.2)), vec3<f32>(0.0), vec3<f32>(1.0));
}

// 2: ACES Filmic Tonemapper (Stephen Hill RRT+ODT fit)
fn tonemapACES(colorIn : vec3<f32>) -> vec3<f32> {
    let x = max(colorIn, vec3<f32>(0.0));
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn linearToSrgb(linearCol : vec3<f32>) -> vec3<f32> {
    let c = clamp(linearCol, vec3<f32>(0.0), vec3<f32>(1.0));
    let lo = c * 12.92;
    let hi = 1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - vec3<f32>(0.055);
    return select(hi, lo, c <= vec3<f32>(0.0031308));
}

/**
 * @brief Evaluates cinematic directional sun lens flares with corona, anamorphic streaks, and chromatic ghosts.
 */
fn evaluateLensFlare(uv : vec2<f32>, sunUV : vec2<f32>, visibility : f32, aspect : f32, intensity : f32) -> vec3<f32> {
    if (visibility <= 0.001 || intensity <= 0.001) {
        return vec3<f32>(0.0);
    }
    let aspectVec = vec2<f32>(max(aspect, 0.1), 1.0);
    let delta = (uv - sunUV) * aspectVec;
    let distToSun = length(delta);

    // 1. Primary Sun Core Glow & Radial Corona
    let corona = 0.75 / (distToSun * 12.0 + 1.0);
    let broadGlow = 0.35 / (distToSun * 3.2 + 1.0);
    let sunColor = vec3<f32>(1.0, 0.88, 0.65);
    var flareColor = (corona * corona + broadGlow) * sunColor;

    // 2. Anamorphic Horizontal Streak (Cinematic flare)
    let streakY = abs(delta.y);
    let streakX = abs(delta.x);
    let anamorphicStreak = exp(-streakY * 42.0) * exp(-streakX * 1.8) * 0.5;
    let streakColor = vec3<f32>(0.55, 0.78, 1.0);
    flareColor = flareColor + anamorphicStreak * streakColor;

    // 3. Multi-Element Lens Ghosts along the optical axis passing through the screen center (0.5, 0.5)
    let centerUV = vec2<f32>(0.5, 0.5);
    let sunToCenter = (centerUV - sunUV);

    // Ghost 1: Small bright violet disc (distance 0.45 along axis)
    let g1Pos = (sunUV + sunToCenter * 0.45) * aspectVec;
    let d1 = length(uv * aspectVec - g1Pos);
    let ghost1 = exp(-d1 * 22.0) * vec3<f32>(0.35, 0.65, 0.95);

    // Ghost 2: Warm soft halo disc (distance 0.85 along axis)
    let g2Pos = (sunUV + sunToCenter * 0.85) * aspectVec;
    let d2 = length(uv * aspectVec - g2Pos);
    let ghost2 = exp(-d2 * 10.0) * vec3<f32>(0.85, 0.55, 0.25);

    // Ghost 3: Chromatic green/cyan aperture element (distance 1.35)
    let g3Pos = (sunUV + sunToCenter * 1.35) * aspectVec;
    let d3 = length(uv * aspectVec - g3Pos);
    let ghost3 = exp(-d3 * 16.0) * vec3<f32>(0.25, 0.85, 0.55);

    // Ghost 4: Diffuse outer chromatic reflection (distance 1.75)
    let g4Pos = (sunUV + sunToCenter * 1.75) * aspectVec;
    let d4 = length(uv * aspectVec - g4Pos);
    let ghost4 = exp(-d4 * 6.0) * vec3<f32>(0.45, 0.7, 1.0) * 0.5;

    // 4. Optical Ring / Halo centered around screen center
    let centerDist = length((uv - centerUV) * aspectVec);
    let haloDist = abs(centerDist - 0.38);
    let halo = exp(-haloDist * 28.0) * 0.15 * vec3<f32>(0.8, 0.45, 0.9);

    let ghosts = (ghost1 + ghost2 + ghost3 + ghost4 + halo) * 0.55;

    // Smooth border attenuation as the sun leaves the screen viewport
    let borderFade = smoothstep(-0.1, 0.1, sunUV.x) * smoothstep(1.1, 0.9, sunUV.x)
                   * smoothstep(-0.1, 0.1, sunUV.y) * smoothstep(1.1, 0.9, sunUV.y);

    return (flareColor + ghosts) * (visibility * intensity * borderFade);
}

// Screen-Space Ambient Occlusion (SSAO/GTAO complement to material occlusion)
fn evaluateSSAO(uv : vec2<f32>, radius : f32, intensity : f32) -> f32 {
    let centerCol = textureSample(hdrTexture, hdrSampler, uv).rgb;
    let centerLum = dot(centerCol, vec3<f32>(0.2126, 0.7152, 0.0722));

    var occlusion = 0.0;
    let r = max(radius, 0.002);
    let offsets = array<vec2<f32>, 8>(
        vec2<f32>( 0.707,  0.707) * r * 0.45,
        vec2<f32>(-0.707, -0.707) * r * 0.45,
        vec2<f32>(-0.707,  0.707) * r * 0.75,
        vec2<f32>( 0.707, -0.707) * r * 0.75,
        vec2<f32>( 1.0,    0.0  ) * r * 1.0,
        vec2<f32>(-1.0,    0.0  ) * r * 1.0,
        vec2<f32>( 0.0,    1.0  ) * r * 1.0,
        vec2<f32>( 0.0,   -1.0  ) * r * 1.0
    );

    for (var i = 0u; i < 8u; i = i + 1u) {
        let sampleUV = clamp(uv + offsets[i], vec2<f32>(0.001), vec2<f32>(0.999));
        let sampleCol = textureSample(hdrTexture, hdrSampler, sampleUV).rgb;
        let sampleLum = dot(sampleCol, vec3<f32>(0.2126, 0.7152, 0.0722));
        let diff = centerLum - sampleLum;
        if (diff > 0.02) {
            occlusion = occlusion + clamp(diff * 2.5, 0.0, 1.0);
        }
    }

    let ao = clamp(1.0 - (occlusion / 8.0) * intensity, 0.15, 1.0);
    return ao;
}

// HDR Bloom for emissive materials (golden-angle multi-ring kernel with luminance threshold)
fn evaluateBloom(uv : vec2<f32>, exposure : f32, threshold : f32, intensity : f32) -> vec3<f32> {
    var bloomAcc = vec3<f32>(0.0);
    var totalWeight = 0.0;

    let ringSteps = array<f32, 4>(0.0035, 0.0085, 0.0185, 0.0385);
    let ringWeights = array<f32, 4>(0.40, 0.28, 0.20, 0.12);

    for (var ring = 0u; ring < 4u; ring = ring + 1u) {
        let r = ringSteps[ring];
        let w = ringWeights[ring];
        for (var tap = 0u; tap < 3u; tap = tap + 1u) {
            let angle = f32(ring * 3u + tap) * 2.39996;
            let offset = vec2<f32>(cos(angle), sin(angle)) * r;
            let sampleUV = clamp(uv + offset, vec2<f32>(0.001), vec2<f32>(0.999));
            let sampleCol = textureSample(hdrTexture, hdrSampler, sampleUV).rgb;
            let exposedSample = sampleCol * exposure;
            let lum = dot(exposedSample, vec3<f32>(0.2126, 0.7152, 0.0722));
            if (lum > threshold) {
                let factor = (lum - threshold) / max(lum, 1e-4);
                let brightColor = exposedSample * factor;
                bloomAcc = bloomAcc + brightColor * w;
            }
            totalWeight = totalWeight + w;
        }
    }

    return (bloomAcc / max(totalWeight, 1e-4)) * intensity;
}

@fragment
fn fs_tonemap(in : VertexOutput) -> @location(0) vec4<f32> {
    let hdrSample = textureSample(hdrTexture, hdrSampler, in.uv);
    var exposed = hdrSample.rgb * ubo.exposure;

    // 1. Complementary Screen-Space Ambient Occlusion (SSAO)
    if (ubo.ssaoParams.w > 0.5) {
        let ssao = evaluateSSAO(in.uv, ubo.ssaoParams.y, ubo.ssaoParams.x);
        exposed = exposed * ssao;
    }

    // 2. Additive HDR Bloom for emissive and bright materials
    if (ubo.bloomParams.w > 0.5 && ubo.bloomParams.x > 0.001) {
        let bloom = evaluateBloom(in.uv, ubo.exposure, ubo.bloomParams.y, ubo.bloomParams.x);
        exposed = exposed + bloom;
    }

    // 3. Evaluate additive sun lens flare in linear HDR space prior to tonemapping
    if (ubo.lensFlareEnabled != 0u && ubo.sunScreenPos.z > 0.001) {
        let flare = evaluateLensFlare(
            in.uv,
            ubo.sunScreenPos.xy,
            ubo.sunScreenPos.z,
            ubo.sunScreenPos.w,
            ubo.flareParams.x
        );
        exposed = exposed + flare;
    }

    var mapped = exposed;
    if (ubo.operatorId == 0u) {
        mapped = tonemapKhronosPbrNeutral(exposed);
    } else if (ubo.operatorId == 1u) {
        mapped = tonemapAgX(exposed);
    } else {
        mapped = tonemapACES(exposed);
    }

    if (ubo.isSrgbSurface == 0u) {
        mapped = linearToSrgb(mapped);
    }
    return vec4<f32>(mapped, 1.0);
}
)";

/**
 * @brief Depth-only directional Shadow Map WGSL shader.
 */
inline constexpr std::string_view kShadowShaderWGSL = R"(
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    lightViewProj : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    baseColorFactor : vec4<f32>,
    metallicFactor : f32,
    roughnessFactor : f32,
    normalScale : f32,
    occlusionStrength : f32,
    emissiveFactor : vec4<f32>,
    alphaCutoff : f32,
    alphaMode : u32,
    flags : u32,
    ior : f32,
    clearcoatFactor : f32,
    clearcoatRoughness : f32,
    transmissionFactor : f32,
    specularFactor : f32,
    sheenColorFactor : vec4<f32>,
    specularColorFactor : vec4<f32>,
    bumpScale : f32,
    reflectionIntensity : f32,
    shadowBias : f32,
    envMipCount : f32,
    thicknessFactor : f32,
    attenuationDistance : f32,
    transmissionRoughness : f32,
    paddingVol1 : f32,
    attenuationColor : vec4<f32>,
    jointCount : u32,
    morphTargetCount : u32,
    padAnim1 : f32,
    padAnim2 : f32,
    morphWeights : vec4<f32>,
    jointMatrices : array<mat4x4<f32>, 128>,
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var defaultSampler : sampler;
@group(0) @binding(2) var baseColorTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
    @location(4) joints : vec4<u32>,
    @location(5) weights : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_shadow(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    var localPos = in.position;

    if (ubo.jointCount > 0u) {
        let i0 = in.joints.x;
        let i1 = in.joints.y;
        let i2 = in.joints.z;
        let i3 = in.joints.w;
        let skinMat = in.weights.x * ubo.jointMatrices[i0] +
                      in.weights.y * ubo.jointMatrices[i1] +
                      in.weights.z * ubo.jointMatrices[i2] +
                      in.weights.w * ubo.jointMatrices[i3];
        localPos = (skinMat * vec4<f32>(localPos, 1.0)).xyz;
    }

    let worldPos = ubo.model * vec4<f32>(localPos, 1.0);
    out.clipPos = ubo.lightViewProj * worldPos;
    out.uv = in.uv;
    return out;
}

@fragment
fn fs_shadow(in : VertexOutput) {
    var alpha = ubo.baseColorFactor.a;
    if ((ubo.flags & 1u) != 0u) { // MaterialFlags::HasBaseColorTex
        let texColor = textureSample(baseColorTexture, defaultSampler, in.uv);
        alpha = alpha * texColor.a;
    }

    // alphaMode: 0 = Opaque, 1 = Mask, 2 = Blend
    if (ubo.alphaMode == 1u) {
        if (alpha < ubo.alphaCutoff) {
            discard;
        }
    } else if (ubo.alphaMode == 2u) {
        if (alpha < max(ubo.alphaCutoff, 0.5)) {
            discard;
        }
    }
}
)";

/**
 * @brief Complete WGSL PBR (Physically Based Rendering) shader code supporting all glTF 2.0
 * and KHR extensions (Clearcoat, Transmission, Sheen, Specular, Emissive Strength, Normal,
 * Derivative Bump Mapping, Trilinear Mipmapping, PCF Shadow Mapping, True HDR IBL with
 * L2 Spherical Harmonics Irradiance + GGX Prefiltered Cubemap + Split-Sum BRDF LUT, and
 * Clustered Forward+ KHR_lights_punctual).
 */
inline constexpr std::string_view kPbrShaderWGSL = R"(
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    lightViewProj : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    baseColorFactor : vec4<f32>,
    metallicFactor : f32,
    roughnessFactor : f32,
    normalScale : f32,
    occlusionStrength : f32,
    emissiveFactor : vec4<f32>,
    alphaCutoff : f32,
    alphaMode : u32,
    flags : u32,
    ior : f32,
    clearcoatFactor : f32,
    clearcoatRoughness : f32,
    transmissionFactor : f32,
    specularFactor : f32,
    sheenColorFactor : vec4<f32>,
    specularColorFactor : vec4<f32>,
    bumpScale : f32,
    reflectionIntensity : f32,
    shadowBias : f32,
    envMipCount : f32,
    thicknessFactor : f32,
    attenuationDistance : f32,
    transmissionRoughness : f32,
    paddingVol1 : f32,
    attenuationColor : vec4<f32>,
    jointCount : u32,
    morphTargetCount : u32,
    padAnim1 : f32,
    padAnim2 : f32,
    morphWeights : vec4<f32>,
    jointMatrices : array<mat4x4<f32>, 128>,
};

struct GpuPunctualLight {
    posOrDir : vec4<f32>,  // xyz = position (Point/Spot) or directionToLight (Directional), w = lightType (0=Dir, 1=Point, 2=Spot)
    colorInt : vec4<f32>,  // xyz = color * intensity, w = range (0 = infinite)
    spotParams : vec4<f32> // xyz = spotDirection (light-to-target), w = packed angleScale / angleOffset
};

struct SceneLightingUniforms {
    shIrradiance : array<vec4<f32>, 9>,
    lights : array<GpuPunctualLight, 16>,
    spotAngles : array<vec4<f32>, 16>, // x = lightAngleScale, y = lightAngleOffset, z = castsShadow, w = pad
    clusterMasks : array<vec4<u32>, 256>, // 1024 u32 bitmasks for 16 x 8 x 8 Forward+ frustum clusters
    gridParams : vec4<f32>, // x = viewportW, y = viewportH, z = zNear, w = zFar
    envParams : vec4<f32>,  // x = envRotation (rad), y = punctualLightCount, z = useClusteredCulling, w = hasPrimarySun
    viewMatrix : mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var defaultSampler : sampler;
@group(0) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(3) var metallicRoughnessTexture : texture_2d<f32>;
@group(0) @binding(4) var normalTexture : texture_2d<f32>;
@group(0) @binding(5) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(6) var occlusionTexture : texture_2d<f32>;
@group(0) @binding(7) var clearcoatTexture : texture_2d<f32>;
@group(0) @binding(8) var transmissionTexture : texture_2d<f32>;
@group(0) @binding(9) var sheenTexture : texture_2d<f32>;
@group(0) @binding(10) var envCubemap : texture_cube<f32>;
@group(0) @binding(11) var shadowMap : texture_depth_2d;
@group(0) @binding(12) var shadowSampler : sampler_comparison;
@group(0) @binding(13) var brdfLutTexture : texture_2d<f32>;
@group(0) @binding(14) var<uniform> sceneLighting : SceneLightingUniforms;
@group(0) @binding(15) var opaqueColorTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
    @location(4) joints : vec4<u32>,
    @location(5) weights : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) worldPos : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
};

fn rotateY(v : vec3<f32>, angle : f32) -> vec3<f32> {
    let c = cos(angle);
    let s = sin(angle);
    return vec3<f32>(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

fn evaluateSHIrradiance(n : vec3<f32>) -> vec3<f32> {
    // Ramamoorthi & Hanrahan L2 Spherical Harmonics irradiance reconstruction
    var irr = sceneLighting.shIrradiance[0].xyz * 0.886227
            + sceneLighting.shIrradiance[1].xyz * (1.023327 * n.y)
            + sceneLighting.shIrradiance[2].xyz * (1.023327 * n.z)
            + sceneLighting.shIrradiance[3].xyz * (1.023327 * n.x)
            + sceneLighting.shIrradiance[4].xyz * (0.858086 * n.x * n.y)
            + sceneLighting.shIrradiance[5].xyz * (0.858086 * n.y * n.z)
            + sceneLighting.shIrradiance[6].xyz * (0.247708 * (3.0 * n.z * n.z - 1.0))
            + sceneLighting.shIrradiance[7].xyz * (0.858086 * n.x * n.z)
            + sceneLighting.shIrradiance[8].xyz * (0.429043 * (n.x * n.x - n.y * n.y));
    return max(irr, vec3<f32>(0.0));
}

fn getClusterLightMask(fragCoord : vec4<f32>, worldPos : vec3<f32>) -> u32 {
    if (sceneLighting.envParams.z < 0.5) {
        return 0xFFFFFFFFu;
    }
    let vpW = max(sceneLighting.gridParams.x, 1.0);
    let vpH = max(sceneLighting.gridParams.y, 1.0);
    let zNear = max(sceneLighting.gridParams.z, 0.05);
    let zFar = max(sceneLighting.gridParams.w, zNear + 1.0);

    let tileX = clamp(u32((fragCoord.x / vpW) * 16.0), 0u, 15u);
    let tileY = clamp(u32((fragCoord.y / vpH) * 8.0), 0u, 7u);

    let viewPos = sceneLighting.viewMatrix * vec4<f32>(worldPos, 1.0);
    let viewZ = max(-viewPos.z, zNear);
    let logRatio = log(viewZ / zNear) / max(log(zFar / zNear), 1e-4);
    let sliceZ = clamp(u32(clamp(logRatio, 0.0, 0.9999) * 8.0), 0u, 7u);

    let clusterIdx = (sliceZ * 8u + tileY) * 16u + tileX;
    let vecIdx = clusterIdx >> 2u;
    let compIdx = clusterIdx & 3u;
    let maskVec = sceneLighting.clusterMasks[vecIdx];
    if (compIdx == 0u) { return maskVec.x; }
    if (compIdx == 1u) { return maskVec.y; }
    if (compIdx == 2u) { return maskVec.z; }
    return maskVec.w;
}

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    var localPos = in.position;
    var localNorm = in.normal;
    var localTan = in.tangent.xyz;

    if (ubo.jointCount > 0u) {
        let i0 = in.joints.x;
        let i1 = in.joints.y;
        let i2 = in.joints.z;
        let i3 = in.joints.w;
        let skinMat = in.weights.x * ubo.jointMatrices[i0] +
                      in.weights.y * ubo.jointMatrices[i1] +
                      in.weights.z * ubo.jointMatrices[i2] +
                      in.weights.w * ubo.jointMatrices[i3];

        localPos = (skinMat * vec4<f32>(localPos, 1.0)).xyz;
        localNorm = (skinMat * vec4<f32>(localNorm, 0.0)).xyz;
        localTan = (skinMat * vec4<f32>(localTan, 0.0)).xyz;
    }

    let world = ubo.model * vec4<f32>(localPos, 1.0);
    out.worldPos = world.xyz;
    out.clipPos = ubo.viewProj * world;

    let m0 = ubo.model[0].xyz;
    let m1 = ubo.model[1].xyz;
    let m2 = ubo.model[2].xyz;
    let adj0 = cross(m1, m2);
    let adj1 = cross(m2, m0);
    let adj2 = cross(m0, m1);
    let det = dot(m0, adj0);
    let rawNorm = select((ubo.model * vec4<f32>(localNorm, 0.0)).xyz,
                         adj0 * localNorm.x + adj1 * localNorm.y + adj2 * localNorm.z,
                         abs(det) > 1e-12);
    let normLen = length(rawNorm);
    out.normal = select(vec3<f32>(0.0, 1.0, 0.0), rawNorm / normLen, normLen > 1e-8);

    let worldTan = (ubo.model * vec4<f32>(localTan, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    let detSign = select(1.0, -1.0, det < 0.0);
    out.tangent = vec4<f32>(normTan, in.tangent.w * detSign);
    out.uv = in.uv;
    return out;
}

fn getTBN(worldPos : vec3<f32>, geomNormal : vec3<f32>, uv : vec2<f32>, tangent : vec4<f32>, dp1 : vec3<f32>, dp2 : vec3<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> mat3x3<f32> {
    let N = normalize(geomNormal);
    let orthoT = tangent.xyz - N * dot(N, tangent.xyz);
    let orthoLenSq = dot(orthoT, orthoT);
    if (abs(tangent.w) > 0.001 && orthoLenSq > 1e-6) {
        let T = orthoT * inverseSqrt(orthoLenSq);
        let B = normalize(cross(N, T) * sign(tangent.w));
        return mat3x3<f32>(T, B, N);
    }
    let dp2perp = cross(dp2, N);
    let dp1perp = cross(N, dp1);
    let T = dp2perp * duv1.x + dp1perp * duv2.x;
    let B = dp2perp * duv1.y + dp1perp * duv2.y;
    let maxLenSq = max(dot(T, T), dot(B, B));
    if (maxLenSq > 1e-18) {
        let invmax = inverseSqrt(maxLenSq);
        return mat3x3<f32>(normalize(T * invmax), normalize(B * invmax), N);
    }
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(N.y) < 0.999);
    let fallbackT = normalize(cross(up, N));
    let fallbackB = normalize(cross(N, fallbackT));
    return mat3x3<f32>(fallbackT, fallbackB, N);
}

fn distributionGGX(NdotH : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265359 * d * d);
}

fn visibilitySmithGGX(NdotL : f32, NdotV : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let ggxL = NdotL + sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
    let ggxV = NdotV + sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
    return 1.0 / max(ggxL * ggxV, 0.0001);
}

fn fresnelSchlick(VdotH : f32, F0 : vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(cosTheta : f32, F0 : vec3<f32>, roughness : f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn computePCFShadow(worldPos : vec3<f32>, N : vec3<f32>, L : vec3<f32>) -> f32 {
    let NdotL = max(dot(N, L), 0.0);
    let normalOffset = N * 0.015 * (1.0 - NdotL);
    let lightClip = ubo.lightViewProj * vec4<f32>(worldPos + normalOffset, 1.0);
    let projCoords = lightClip.xyz / lightClip.w;

    let uv = vec2<f32>(projCoords.x * 0.5 + 0.5, 0.5 - projCoords.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 1.0;
    }

    let slopeBias = max(ubo.shadowBias * (1.0 - NdotL), ubo.shadowBias * 0.35);
    let compareDepth = projCoords.z - slopeBias;
    let texelSize = 1.0 / 2048.0;

    var shadow = 0.0;
    for (var y : i32 = -1; y <= 1; y = y + 1) {
        for (var x : i32 = -1; x <= 1; x = x + 1) {
            let offset = vec2<f32>(f32(x), f32(y)) * texelSize;
            shadow = shadow + textureSampleCompareLevel(shadowMap, shadowSampler, uv + offset, compareDepth);
        }
    }
    shadow = shadow / 9.0;

    let edgeDist = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    let borderFade = smoothstep(0.0, 0.04, edgeDist);
    return mix(1.0, shadow, borderFade);
}

fn evaluateDirectLobe(
    L : vec3<f32>,
    V : vec3<f32>,
    N : vec3<f32>,
    radiance : vec3<f32>,
    F0 : vec3<f32>,
    diffuseColor : vec3<f32>,
    roughness : f32,
    metallic : f32,
    transFactor : f32,
    ccFactor : f32,
    ccRoughness : f32,
    sheenColor : vec3<f32>,
    sheenRoughness : f32,
    bumpCavity : f32
) -> vec3<f32> {
    let NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) {
        return vec3<f32>(0.0);
    }
    let NdotV = max(dot(N, V), 0.001);
    let H = normalize(L + V);
    let NdotH = max(dot(N, H), 0.0);
    let VdotH = max(dot(V, H), 0.0);

    let D = distributionGGX(NdotH, roughness);
    let Vis = visibilitySmithGGX(NdotL, NdotV, roughness);
    let F = fresnelSchlick(VdotH, F0);
    let specularBRDF = D * Vis * F;

    let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
    let diffuseBRDF = kD * (diffuseColor / 3.14159265359) * (1.0 - transFactor);

    var contrib = (diffuseBRDF + specularBRDF) * radiance * NdotL * bumpCavity;

    if (ccFactor > 0.001) {
        let Dcc = distributionGGX(NdotH, ccRoughness);
        let VisCc = visibilitySmithGGX(NdotL, NdotV, ccRoughness);
        let clearcoatF = fresnelSchlick(VdotH, vec3<f32>(0.04));
        let clearcoatLobe = vec3<f32>(Dcc * VisCc) * clearcoatF * ccFactor;
        contrib = contrib * (1.0 - clearcoatF * ccFactor) + clearcoatLobe * radiance * NdotL;
    }

    if (length(sheenColor) > 0.001) {
        let invAlpha = 1.0 / sheenRoughness;
        let sin2 = max(1.0 - NdotH * NdotH, 0.0);
        let Dcharlie = (2.0 + invAlpha) / (2.0 * 3.14159265359) * pow(sin2, invAlpha * 0.5);
        let sheenLobe = sheenColor * Dcharlie / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 0.001);
        contrib = contrib + sheenLobe * radiance * NdotL;
    }

    return contrib;
}

/**
 * @brief Samples normalized surface displacement height from occlusion map, normal alpha, or base color luma.
 * @param uv Normalized 2D texture coordinates.
 * @param duv1 Screen-space horizontal derivative of UV coordinates.
 * @param duv2 Screen-space vertical derivative of UV coordinates.
 * @return Normalized height value in [0.0, 1.0] where 1.0 represents the outer surface plane.
 */
fn getSurfaceHeight(uv : vec2<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> f32 {
    var height = 0.5;
    if ((ubo.flags & 16u) != 0u) {
        // Occlusion texture red channel represents cavity depth (0 = deep crevice, 1 = surface plane)
        height = textureSampleGrad(occlusionTexture, defaultSampler, uv, duv1, duv2).r;
    } else {
        // Base color luminance fallback for materials without dedicated height/occlusion maps
        let col = textureSampleGrad(baseColorTexture, defaultSampler, uv, duv1, duv2).rgb;
        height = dot(col, vec3<f32>(0.299, 0.587, 0.114));
    }
    return clamp(height, 0.0, 1.0);
}

/**
 * @brief Steep Parallax Occlusion Mapping (POM) with adaptive view-dependent sampling and linear secant interpolation.
 * @param initialUV Original interpolated UV coordinates.
 * @param viewDirTangent Camera view direction normalized in tangent space.
 * @param duv1 Screen-space horizontal derivative of UV coordinates.
 * @param duv2 Screen-space vertical derivative of UV coordinates.
 * @param scale Parallax displacement depth scale factor.
 * @return Displaced texture coordinates giving true 3D surface depth.
 */
fn parallaxOcclusionMapping(
    initialUV : vec2<f32>,
    viewDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> vec2<f32> {
    // Dynamic step count between 8 and 32 based on viewing angle (steep grazing angles need more samples)
    let minLayers : f32 = 8.0;
    let maxLayers : f32 = 32.0;
    let numLayers = mix(maxLayers, minLayers, clamp(abs(viewDirTangent.z), 0.0, 1.0));
    let layerDepth = 1.0 / numLayers;
    var currentLayerDepth : f32 = 0.0;

    // Tangent space ray delta per step (depth downwards: 1.0 is surface, 0.0 is base)
    let P = (viewDirTangent.xy / max(abs(viewDirTangent.z), 0.15)) * scale;
    let deltaUV = P / numLayers;

    var currentUV = initialUV;
    var currentHeight = getSurfaceHeight(currentUV, duv1, duv2);

    var prevUV = currentUV;
    var prevHeight = currentHeight;
    var prevLayerDepth = currentLayerDepth;

    for (var i : i32 = 0; i < 32; i = i + 1) {
        if (f32(i) >= numLayers || currentLayerDepth >= (1.0 - currentHeight)) {
            break;
        }
        prevUV = currentUV;
        prevHeight = currentHeight;
        prevLayerDepth = currentLayerDepth;

        currentUV = currentUV - deltaUV;
        currentHeight = getSurfaceHeight(currentUV, duv1, duv2);
        currentLayerDepth = currentLayerDepth + layerDepth;
    }

    // Parallax Occlusion interpolation between previous layer and current layer
    let afterDepth = currentLayerDepth - (1.0 - currentHeight);
    let beforeDepth = (1.0 - prevHeight) - prevLayerDepth;
    let denom = afterDepth + beforeDepth;
    let weight = select(0.5, afterDepth / max(denom, 1e-5), abs(denom) > 1e-6);

    return mix(currentUV, prevUV, clamp(weight, 0.0, 1.0));
}

/**
 * @brief Parallax self-shadowing raymarch from surface point along tangent-space light direction.
 * @param startUV Displaced surface texture coordinate.
 * @param lightDirTangent Normalized light direction in tangent space.
 * @param duv1 Screen-space horizontal derivative of UV coordinates.
 * @param duv2 Screen-space vertical derivative of UV coordinates.
 * @param scale Parallax displacement depth scale factor.
 * @return Shadow attenuation factor in [0.25, 1.0].
 */
fn computeParallaxShadow(
    startUV : vec2<f32>,
    lightDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> f32 {
    if (lightDirTangent.z <= 0.0) {
        return 0.25;
    }
    let initialHeight = getSurfaceHeight(startUV, duv1, duv2);
    let numShadowSteps = 8.0;
    let stepDepth = (1.0 - initialHeight) / numShadowSteps;
    let deltaUV = (lightDirTangent.xy / max(lightDirTangent.z, 0.15)) * (scale / numShadowSteps);

    var currentUV = startUV;
    var currentDepth = 1.0 - initialHeight;
    var shadowFactor = 1.0;

    for (var i : i32 = 0; i < 8; i = i + 1) {
        currentUV = currentUV + deltaUV;
        currentDepth = currentDepth - stepDepth;
        let h = getSurfaceHeight(currentUV, duv1, duv2);
        let sampleDepth = 1.0 - h;
        if (sampleDepth < currentDepth) {
            shadowFactor = min(shadowFactor, clamp((currentDepth - sampleDepth) * 3.5, 0.0, 1.0));
        }
    }
    return clamp(shadowFactor, 0.25, 1.0);
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    let dp1 = dpdx(in.worldPos);
    let dp2 = dpdy(in.worldPos);
    let duv1 = dpdx(in.uv);
    let duv2 = dpdy(in.uv);

    var geomN = normalize(in.normal);
    let viewDir = normalize(ubo.cameraPos.xyz - in.worldPos);
    if ((ubo.flags & 512u) != 0u && dot(geomN, viewDir) < 0.0) {
        geomN = -geomN;
    }

    let tbn = getTBN(in.worldPos, geomN, in.uv, in.tangent, dp1, dp2, duv1, duv2);

    // Tangent-space view vector (TBN transpose * V)
    let V_tangent = vec3<f32>(
        dot(tbn[0], viewDir),
        dot(tbn[1], viewDir),
        dot(tbn[2], viewDir)
    );

    let envRot = sceneLighting.envParams.x;
    let sunL = rotateY(normalize(ubo.lightDir.xyz), -envRot);

    // Parallax Occlusion Mapping (POM) if flag bit 15 is active and bump mapping is enabled
    var uv = in.uv;
    var pomShadow = 1.0;
    if ((ubo.flags & 32768u) != 0u && (ubo.flags & 1024u) != 0u) {
        let pomScale = max(ubo.bumpScale * 0.035, 0.005);
        uv = parallaxOcclusionMapping(in.uv, V_tangent, duv1, duv2, pomScale);

        let L_tangent = vec3<f32>(
            dot(tbn[0], sunL),
            dot(tbn[1], sunL),
            dot(tbn[2], sunL)
        );
        pomShadow = computeParallaxShadow(uv, L_tangent, duv1, duv2, pomScale);
    }

    let baseTexSample = textureSampleGrad(baseColorTexture, defaultSampler, uv, duv1, duv2);
    let mrTexSample = textureSampleGrad(metallicRoughnessTexture, defaultSampler, uv, duv1, duv2);
    let normTexSample = textureSampleGrad(normalTexture, defaultSampler, uv, duv1, duv2);
    let emTexSample = textureSampleGrad(emissiveTexture, defaultSampler, uv, duv1, duv2);
    let occTexSample = textureSampleGrad(occlusionTexture, defaultSampler, uv, duv1, duv2);
    let ccTexSample = textureSampleGrad(clearcoatTexture, defaultSampler, uv, duv1, duv2);
    let transTexSample = textureSampleGrad(transmissionTexture, defaultSampler, uv, duv1, duv2);
    let sheenTexSample = textureSampleGrad(sheenTexture, defaultSampler, uv, duv1, duv2);

    let texDims = vec2<f32>(textureDimensions(baseColorTexture, 0));
    let texelStep = 1.0 / clamp(texDims, vec2<f32>(64.0), vec2<f32>(2048.0));
    let lumaWeights = vec3<f32>(0.299, 0.587, 0.114);

    let hR_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uv + vec2<f32>(texelStep.x, 0.0), duv1, duv2).rgb;
    let hL_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uv - vec2<f32>(texelStep.x, 0.0), duv1, duv2).rgb;
    let hU_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uv + vec2<f32>(0.0, texelStep.y), duv1, duv2).rgb;
    let hD_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uv - vec2<f32>(0.0, texelStep.y), duv1, duv2).rgb;

    var dHdU = dot(hR_rgb - hL_rgb, lumaWeights);
    var dHdV = dot(hU_rgb - hD_rgb, lumaWeights);
    if ((ubo.flags & 16u) != 0u) {
        let occR = textureSampleGrad(occlusionTexture, defaultSampler, uv + vec2<f32>(texelStep.x, 0.0), duv1, duv2).r;
        let occL = textureSampleGrad(occlusionTexture, defaultSampler, uv - vec2<f32>(texelStep.x, 0.0), duv1, duv2).r;
        let occU = textureSampleGrad(occlusionTexture, defaultSampler, uv + vec2<f32>(0.0, texelStep.y), duv1, duv2).r;
        let occD = textureSampleGrad(occlusionTexture, defaultSampler, uv - vec2<f32>(0.0, texelStep.y), duv1, duv2).r;
        dHdU = dHdU + (occR - occL) * 0.75;
        dHdV = dHdV + (occU - occD) * 0.75;
    }

    var baseColor = ubo.baseColorFactor;
    if ((ubo.flags & 1u) != 0u) {
        baseColor = baseColor * baseTexSample;
    }

    if (ubo.alphaMode == 1u && baseColor.a < ubo.alphaCutoff) {
        discard;
    }

    if ((ubo.flags & 256u) != 0u) {
        return baseColor;
    }

    var N = geomN;
    var bumpCavity = 1.0;
    var tangentXY = vec2<f32>(0.0, 0.0);
    if ((ubo.flags & 1024u) != 0u) {
        var tangentZ = 1.0;
        if ((ubo.flags & 4u) != 0u) {
            let normTex = normTexSample.xyz * 2.0 - 1.0;
            tangentXY = normTex.xy * (ubo.normalScale * ubo.bumpScale);
            tangentZ = max(normTex.z, 0.15);
        } else if ((ubo.flags & 16u) != 0u) {
            tangentXY = vec2<f32>(-dHdU, -dHdV) * 2.0 * ubo.bumpScale;
            tangentZ = 1.0;
        }
        let scaledNorm = normalize(vec3<f32>(tangentXY, tangentZ));
        N = normalize(tbn * scaledNorm);
        if (dot(N, geomN) < 0.05) {
            N = normalize(mix(geomN, N, 0.65));
        }
        let slopeSq = dot(tangentXY, tangentXY);
        bumpCavity = clamp(1.0 - 0.25 * slopeSq, 0.5, 1.0);
    }

    var roughness = ubo.roughnessFactor;
    var metallic = ubo.metallicFactor;
    if ((ubo.flags & 2u) != 0u) {
        roughness = roughness * mrTexSample.g;
        metallic = metallic * mrTexSample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    let ior = max(ubo.ior, 1.0);
    let f0_dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    let specularColor = ubo.specularFactor * ubo.specularColorFactor.rgb;
    let F0 = mix(vec3<f32>(f0_dielectric) * specularColor, baseColor.rgb, metallic);
    let diffuseColor = baseColor.rgb * (1.0 - metallic);

    var ccFactor = ubo.clearcoatFactor;
    var ccRoughness = ubo.clearcoatRoughness;
    if ((ubo.flags & 32u) != 0u) {
        ccFactor = ccFactor * ccTexSample.r;
        ccRoughness = ccRoughness * ccTexSample.g;
    }
    ccRoughness = clamp(ccRoughness, 0.04, 1.0);

    var sheenColor = ubo.sheenColorFactor.rgb;
    let sheenRoughness = clamp(ubo.sheenColorFactor.w, 0.04, 1.0);
    if ((ubo.flags & 128u) != 0u) {
        sheenColor = sheenColor * sheenTexSample.rgb;
    }

    var transmission = ubo.transmissionFactor;
    if ((ubo.flags & 64u) != 0u) {
        transmission = transmission * transTexSample.r;
    }
    let transFactor = clamp(transmission, 0.0, 1.0);

    // Ground-truth Ambient Occlusion (glTF occlusion map + micro-cavity + horizon contact + multi-bounce)
    var ao = 1.0;
    if ((ubo.flags & 16384u) != 0u) { // Bit 14 = EnableAmbientOcclusion
        if ((ubo.flags & 16u) != 0u) {
            let occSample = occTexSample.r;
            ao = 1.0 + ubo.occlusionStrength * (occSample - 1.0);
        }
        let horizonOcclusion = clamp(dot(N, geomN) * 1.25, 0.35, 1.0);
        let combinedAO = clamp(ao * bumpCavity * horizonOcclusion, 0.0, 1.0);
        // Jimenez multi-bounce approximation to preserve saturated diffuse tone in crevices
        ao = mix(combinedAO, combinedAO * (diffuseColor * (1.0 - combinedAO) + 1.0).r, 0.35);
    }

    var emissive = ubo.emissiveFactor.xyz * ubo.emissiveFactor.w;
    if ((ubo.flags & 8u) != 0u) {
        emissive = emissive * emTexSample.rgb;
    }

    let V = viewDir;
    let NdotV = max(dot(N, V), 0.001);

    // 1. Primary Sun Light (Rotated with Environment) + Shadow Map
    var shadowFactor = 1.0;
    if ((ubo.flags & 4096u) != 0u) {
        shadowFactor = computePCFShadow(in.worldPos, geomN, sunL);
    }

    var directLighting = vec3<f32>(0.0);
    if (sceneLighting.envParams.w > 0.5) {
        let sunRadiance = vec3<f32>(1.0, 0.98, 0.95) * 3.14159265359;
        directLighting = evaluateDirectLobe(
            sunL, V, N, sunRadiance, F0, diffuseColor, roughness, metallic,
            transFactor, ccFactor, ccRoughness, sheenColor, sheenRoughness, bumpCavity
        ) * (shadowFactor * pomShadow);
    }

    // 2. KHR_lights_punctual Evaluated via Clustered Forward+ Frustum Culling
    let punctualCount = min(u32(sceneLighting.envParams.y), 16u);
    if (punctualCount > 0u) {
        let clusterMask = getClusterLightMask(in.clipPos, in.worldPos);
        for (var li : u32 = 0u; li < punctualCount; li = li + 1u) {
            let light = sceneLighting.lights[li];
            let lightType = u32(light.posOrDir.w + 0.5);

            var L_p = vec3<f32>(0.0, 1.0, 0.0);
            var attenuation = 1.0;

            if (lightType == 0u) {
                // Directional light (always active across all clusters)
                L_p = normalize(light.posOrDir.xyz);
            } else {
                // Point or Spot light: check Forward+ 3D cluster mask first
                if ((clusterMask & (1u << li)) == 0u) {
                    continue;
                }
                let toLight = light.posOrDir.xyz - in.worldPos;
                let distSq = dot(toLight, toLight);
                let dist = sqrt(max(distSq, 1e-6));
                L_p = toLight / dist;

                let range = light.colorInt.w;
                var rangeAtten = 1.0 / max(distSq, 0.01 * 0.01);
                if (range > 0.0) {
                    let ratio = dist / range;
                    let ratio4 = ratio * ratio * ratio * ratio;
                    let window = clamp(1.0 - ratio4, 0.0, 1.0);
                    rangeAtten = rangeAtten * (window * window);
                }
                attenuation = rangeAtten;

                if (lightType == 2u) {
                    // Spot light angular falloff (KHR_lights_punctual spec)
                    let spotDir = normalize(light.spotParams.xyz);
                    let cd = dot(spotDir, -L_p);
                    let angleScale = sceneLighting.spotAngles[li].x;
                    let angleOffset = sceneLighting.spotAngles[li].y;
                    let angularAtten = clamp(cd * angleScale + angleOffset, 0.0, 1.0);
                    attenuation = attenuation * (angularAtten * angularAtten);
                }
            }

            if (attenuation > 1e-5) {
                let lightRadiance = light.colorInt.xyz * attenuation;
                var pShadow = 1.0;
                if (lightType == 0u && sceneLighting.spotAngles[li].z > 0.5) {
                    pShadow = shadowFactor;
                }
                directLighting = directLighting + evaluateDirectLobe(
                    L_p, V, N, lightRadiance, F0, diffuseColor, roughness, metallic,
                    transFactor, ccFactor, ccRoughness, sheenColor, sheenRoughness, bumpCavity
                ) * pShadow;
            }
        }
    }

    // 3. True HDR Image-Based Lighting (IBL):
    //    - Diffuse Irradiance via L2 Spherical Harmonics (9 coefficients)
    //    - Specular IBL via Compute GGX Prefiltered Cubemap + Split-Sum 2D BRDF LUT
    let rotN = rotateY(N, envRot);
    let R = rotateY(reflect(-V, N), envRot);

    let shIrradiance = evaluateSHIrradiance(rotN);
    let brdfSample = textureSampleLevel(brdfLutTexture, defaultSampler, vec2<f32>(NdotV, roughness), 0.0).rg;
    let F_ibl = fresnelSchlickRoughness(NdotV, F0, roughness);
    let kD_ibl = (vec3<f32>(1.0) - F_ibl) * (1.0 - metallic);

    var ambientDiffuse = kD_ibl * diffuseColor * shIrradiance * ao * (1.0 - transFactor);
    var ambientSpecular = vec3<f32>(0.0);

    if ((ubo.flags & 8192u) != 0u) {
        let maxMip = max(ubo.envMipCount - 1.0, 0.0);
        let specMip = clamp(roughness * maxMip, 0.0, maxMip);
        let prefilteredEnv = textureSampleLevel(envCubemap, defaultSampler, R, specMip).rgb;
        let specBrdf = F_ibl * brdfSample.x + vec3<f32>(brdfSample.y);
        ambientSpecular = prefilteredEnv * specBrdf * ubo.reflectionIntensity * mix(0.35, 1.0, ao);

        if (ccFactor > 0.001) {
            let R_cc = rotateY(reflect(-V, geomN), envRot);
            let ccMip = clamp(ccRoughness * maxMip, 0.0, maxMip);
            let ccEnv = textureSampleLevel(envCubemap, defaultSampler, R_cc, ccMip).rgb;
            let ccBrdf = textureSampleLevel(brdfLutTexture, defaultSampler, vec2<f32>(NdotV, ccRoughness), 0.0).rg;
            let ccFresnel = vec3<f32>(0.04) * ccBrdf.x + vec3<f32>(ccBrdf.y);
            ambientSpecular = ambientSpecular * (1.0 - ccFresnel * ccFactor) + ccEnv * ccFresnel * ccFactor * ubo.reflectionIntensity;
        }
    } else {
        ambientSpecular = (F_ibl * brdfSample.x + vec3<f32>(brdfSample.y)) * shIrradiance * (1.0 - roughness) * 0.25 * ao;
    }

    var finalColor = directLighting + ambientDiffuse + ambientSpecular + emissive;

    var outAlpha = baseColor.a;
    if (transFactor > 0.001) {
        let fresnelGlass = fresnelSchlick(NdotV, vec3<f32>(f0_dielectric)).r;

        // Screen-space refraction coordinate
        let screenDim = max(sceneLighting.gridParams.xy, vec2<f32>(1.0, 1.0));
        let screenUV = in.clipPos.xy / screenDim;

        // Physical Snell's law refraction in view space
        let ior = max(ubo.ior, 1.0);
        let eta = 1.0 / ior;
        let viewPos = sceneLighting.viewMatrix * vec4<f32>(in.worldPos, 1.0);
        let viewZ = max(-viewPos.z, 0.1);
        let N_view = normalize((sceneLighting.viewMatrix * vec4<f32>(N, 0.0)).xyz);
        let thickness = max(ubo.thicknessFactor, 0.05);

        // Chromatic dispersion & refraction UV offset
        let refrOffset = (N_view.xy * (1.0 - eta)) * (thickness / viewZ) * 0.12;
        let refrUV = clamp(screenUV + refrOffset, vec2<f32>(0.001), vec2<f32>(0.999));

        var bgSample = textureSample(opaqueColorTexture, defaultSampler, refrUV).rgb;

        // Beer-Lambert volume attenuation (KHR_materials_volume)
        if (ubo.thicknessFactor > 0.0 && ubo.attenuationDistance < 1e8) {
            let attDist = max(ubo.attenuationDistance, 1e-4);
            let attColor = clamp(ubo.attenuationColor.rgb, vec3<f32>(1e-4), vec3<f32>(1.0));
            let extinction = -log(attColor) / attDist;
            let opticalPath = ubo.thicknessFactor / max(NdotV, 0.1);
            let transmittance = exp(-extinction * opticalPath);
            bgSample = bgSample * transmittance;
        }

        let transmittedColor = bgSample * baseColor.rgb;
        let transMix = (1.0 - fresnelGlass) * transFactor;
        finalColor = mix(directLighting + ambientSpecular, transmittedColor, transMix) + emissive;
        outAlpha = 1.0;
    }

    return vec4<f32>(finalColor, outAlpha);
}
)";

/**
 * @brief 3D Bounding-Box (AABB) and spatial volume wireframe debug shader.
 */
inline constexpr std::string_view kAabbShaderWGSL = R"(
struct AabbUniforms {
    viewProj : mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> ubo : AabbUniforms;

struct LineVertexInput {
    @location(0) pos : vec3<f32>,
    @location(1) color : vec4<f32>,
};

struct LineVertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) color : vec4<f32>,
};

@vertex
fn vs_aabb(in : LineVertexInput) -> LineVertexOutput {
    var out : LineVertexOutput;
    out.clipPos = ubo.viewProj * vec4<f32>(in.pos, 1.0);
    out.color = in.color;
    return out;
}

@fragment
fn fs_aabb(in : LineVertexOutput) -> @location(0) vec4<f32> {
    return in.color;
}
)";

/**
 * @brief Wireframe and Bounding-Box debug shader.
 */
inline constexpr std::string_view kWireframeShaderWGSL = R"(
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    lightViewProj : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    baseColorFactor : vec4<f32>,
    metallicFactor : f32,
    roughnessFactor : f32,
    normalScale : f32,
    occlusionStrength : f32,
    emissiveFactor : vec4<f32>,
    alphaCutoff : f32,
    alphaMode : u32,
    flags : u32,
    ior : f32,
    clearcoatFactor : f32,
    clearcoatRoughness : f32,
    transmissionFactor : f32,
    specularFactor : f32,
    sheenColorFactor : vec4<f32>,
    specularColorFactor : vec4<f32>,
    bumpScale : f32,
    reflectionIntensity : f32,
    shadowBias : f32,
    envMipCount : f32,
    thicknessFactor : f32,
    attenuationDistance : f32,
    transmissionRoughness : f32,
    paddingVol1 : f32,
    attenuationColor : vec4<f32>,
    jointCount : u32,
    morphTargetCount : u32,
    padAnim1 : f32,
    padAnim2 : f32,
    morphWeights : vec4<f32>,
    jointMatrices : array<mat4x4<f32>, 128>,
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var defaultSampler : sampler;
@group(0) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(3) var metallicRoughnessTexture : texture_2d<f32>;
@group(0) @binding(4) var normalTexture : texture_2d<f32>;
@group(0) @binding(5) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(6) var occlusionTexture : texture_2d<f32>;
@group(0) @binding(7) var clearcoatTexture : texture_2d<f32>;
@group(0) @binding(8) var transmissionTexture : texture_2d<f32>;
@group(0) @binding(9) var sheenTexture : texture_2d<f32>;
@group(0) @binding(10) var envCubemap : texture_cube<f32>;
@group(0) @binding(11) var shadowMap : texture_depth_2d;
@group(0) @binding(12) var shadowSampler : sampler_comparison;
@group(0) @binding(13) var brdfLutTexture : texture_2d<f32>;
@group(0) @binding(14) var<uniform> sceneLighting : array<vec4<f32>, 305>;
@group(0) @binding(15) var opaqueColorTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
};

@vertex
fn vs_main(in : VertexInput) -> @builtin(position) vec4<f32> {
    return ubo.viewProj * ubo.model * vec4<f32>(in.position, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(0.22, 0.74, 0.97, 1.0);
}
)";

} // namespace bgl::gpu
