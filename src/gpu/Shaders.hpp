/**
 * @file Shaders.hpp
 * @brief Embedded WGSL Full glTF 2.0 PBR, Shadow Mapping, Environment Cubemap Reflections,
 *        Bump/Normal Mapping, and Wireframe/Bounding-Box Shaders.
 */
#pragma once

#include <string_view>

namespace bgl::gpu {

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
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
};

@vertex
fn vs_shadow(in : VertexInput) -> @builtin(position) vec4<f32> {
    let worldPos = ubo.model * vec4<f32>(in.position, 1.0);
    return ubo.lightViewProj * worldPos;
}
)";

/**
 * @brief Complete WGSL PBR (Physically Based Rendering) shader code supporting all glTF 2.0
 * and KHR extensions (Clearcoat, Transmission, Sheen, Specular, Emissive Strength, Normal,
 * Derivative Bump Mapping, Trilinear Mipmapping, PCF Shadow Mapping, and Cubemap Environment Reflections).
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

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) worldPos : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
};

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    let world = ubo.model * vec4<f32>(in.position, 1.0);
    out.worldPos = world.xyz;
    out.clipPos = ubo.viewProj * world;
    out.normal = normalize((ubo.model * vec4<f32>(in.normal, 0.0)).xyz);
    let worldTan = (ubo.model * vec4<f32>(in.tangent.xyz, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    out.tangent = vec4<f32>(normTan, in.tangent.w);
    out.uv = in.uv;
    return out;
}

fn getTBN(worldPos : vec3<f32>, geomNormal : vec3<f32>, uv : vec2<f32>, tangent : vec4<f32>, dp1 : vec3<f32>, dp2 : vec3<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> mat3x3<f32> {
    let N = normalize(geomNormal);
    // Gram-Schmidt orthogonalize vertex tangent against geometric normal
    let orthoT = tangent.xyz - N * dot(N, tangent.xyz);
    let orthoLenSq = dot(orthoT, orthoT);
    if (abs(tangent.w) > 0.001 && orthoLenSq > 1e-6) {
        let T = orthoT * inverseSqrt(orthoLenSq);
        let B = normalize(cross(N, T) * sign(tangent.w));
        return mat3x3<f32>(T, B, N);
    }
    // Screen-space cotangent frame fallback (scale-invariant normalization)
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

fn envBRDFApprox(F0 : vec3<f32>, roughness : f32, NdotV : f32) -> vec3<f32> {
    let c0 = vec4<f32>(-1.0, -0.0275, -0.572, 0.022);
    let c1 = vec4<f32>(1.0, 0.0425, 1.04, -0.04);
    let r = roughness * c0 + c1;
    let a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    let AB = vec2<f32>(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + vec3<f32>(AB.y);
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

    // Smooth fade near shadow frustum boundary
    let edgeDist = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    let borderFade = smoothstep(0.0, 0.04, edgeDist);
    return mix(1.0, shadow, borderFade);
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    // Pre-sample textures and screen-space derivatives in uniform control flow (before discard)
    let dp1 = dpdx(in.worldPos);
    let dp2 = dpdy(in.worldPos);
    let duv1 = dpdx(in.uv);
    let duv2 = dpdy(in.uv);

    let baseTexSample = textureSample(baseColorTexture, defaultSampler, in.uv);
    let mrTexSample = textureSample(metallicRoughnessTexture, defaultSampler, in.uv);
    let normTexSample = textureSample(normalTexture, defaultSampler, in.uv);
    let emTexSample = textureSample(emissiveTexture, defaultSampler, in.uv);
    let occTexSample = textureSample(occlusionTexture, defaultSampler, in.uv);
    let ccTexSample = textureSample(clearcoatTexture, defaultSampler, in.uv);
    let transTexSample = textureSample(transmissionTexture, defaultSampler, in.uv);
    let sheenTexSample = textureSample(sheenTexture, defaultSampler, in.uv);

    // Compute UV-space central-difference height gradients for true texture-space bump mapping
    let texDims = vec2<f32>(textureDimensions(baseColorTexture, 0));
    let texelStep = 1.0 / clamp(texDims, vec2<f32>(64.0), vec2<f32>(2048.0));
    let lumaWeights = vec3<f32>(0.299, 0.587, 0.114);

    let hR_rgb = textureSampleGrad(baseColorTexture, defaultSampler, in.uv + vec2<f32>(texelStep.x, 0.0), duv1, duv2).rgb;
    let hL_rgb = textureSampleGrad(baseColorTexture, defaultSampler, in.uv - vec2<f32>(texelStep.x, 0.0), duv1, duv2).rgb;
    let hU_rgb = textureSampleGrad(baseColorTexture, defaultSampler, in.uv + vec2<f32>(0.0, texelStep.y), duv1, duv2).rgb;
    let hD_rgb = textureSampleGrad(baseColorTexture, defaultSampler, in.uv - vec2<f32>(0.0, texelStep.y), duv1, duv2).rgb;

    var dHdU = dot(hR_rgb - hL_rgb, lumaWeights);
    var dHdV = dot(hU_rgb - hD_rgb, lumaWeights);
    if ((ubo.flags & 16u) != 0u) {
        let occR = textureSampleGrad(occlusionTexture, defaultSampler, in.uv + vec2<f32>(texelStep.x, 0.0), duv1, duv2).r;
        let occL = textureSampleGrad(occlusionTexture, defaultSampler, in.uv - vec2<f32>(texelStep.x, 0.0), duv1, duv2).r;
        let occU = textureSampleGrad(occlusionTexture, defaultSampler, in.uv + vec2<f32>(0.0, texelStep.y), duv1, duv2).r;
        let occD = textureSampleGrad(occlusionTexture, defaultSampler, in.uv - vec2<f32>(0.0, texelStep.y), duv1, duv2).r;
        dHdU = dHdU + (occR - occL) * 0.75;
        dHdV = dHdV + (occU - occD) * 0.75;
    }

    // 1. Base Color & Alpha
    var baseColor = ubo.baseColorFactor;
    if ((ubo.flags & 1u) != 0u) {
        baseColor = baseColor * baseTexSample;
    }

    // Alpha Cutoff (AlphaMode::Mask == 1u)
    if (ubo.alphaMode == 1u && baseColor.a < ubo.alphaCutoff) {
        discard;
    }

    // KHR_materials_unlit
    if ((ubo.flags & 256u) != 0u) {
        return baseColor;
    }

    // 2. Normal & Bump Mapping Calculation
    var geomN = normalize(in.normal);
    let viewDir = normalize(ubo.cameraPos.xyz - in.worldPos);
    if ((ubo.flags & 512u) != 0u || dot(geomN, viewDir) < 0.0) {
        if (dot(geomN, viewDir) < 0.0) {
            geomN = -geomN;
        }
    }

    var N = geomN;
    var bumpCavity = 1.0;
    // Flag bit 10 (1024u) = EnableBumpMapping
    if ((ubo.flags & 1024u) != 0u) {
        let tbn = getTBN(in.worldPos, geomN, in.uv, in.tangent, dp1, dp2, duv1, duv2);
        var tangentXY = vec2<f32>(0.0, 0.0);
        var tangentZ = 1.0;
        if ((ubo.flags & 4u) != 0u) {
            // Tangent-space normal map + UV-space height-map micro-relief
            let normTex = normTexSample.xyz * 2.0 - 1.0;
            let heightBump = vec2<f32>(-dHdU, -dHdV) * 4.2 * ubo.bumpScale;
            tangentXY = normTex.xy * (ubo.normalScale * 1.85 * ubo.bumpScale) + heightBump;
            tangentZ = max(normTex.z, 0.15);
        } else if ((ubo.flags & 1u) != 0u || (ubo.flags & 16u) != 0u) {
            // UV-space central-difference height bump mapping from texture luminance & occlusion
            tangentXY = vec2<f32>(-dHdU, -dHdV) * 7.5 * ubo.bumpScale;
            tangentZ = 1.0;
        }
        let scaledNorm = normalize(vec3<f32>(tangentXY, tangentZ));
        N = normalize(tbn * scaledNorm);
        // Ensure perturbed normal stays in visible hemisphere to prevent black artifacts
        if (dot(N, geomN) < 0.05) {
            N = normalize(mix(geomN, N, 0.65));
        }
        let slopeSq = dot(tangentXY, tangentXY);
        bumpCavity = clamp(1.0 - 0.22 * slopeSq, 0.58, 1.0);
    }

    // 3. Metallic & Roughness
    var roughness = ubo.roughnessFactor;
    var metallic = ubo.metallicFactor;
    if ((ubo.flags & 2u) != 0u) {
        roughness = roughness * mrTexSample.g;
        metallic = metallic * mrTexSample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    // 4. Specular & Dielectric F0
    let ior = max(ubo.ior, 1.0);
    let f0_dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    let specularColor = ubo.specularFactor * ubo.specularColorFactor.rgb;
    let F0 = mix(vec3<f32>(f0_dielectric) * specularColor, baseColor.rgb, metallic);
    let diffuseColor = baseColor.rgb * (1.0 - metallic);

    // 5. Lighting vectors
    let V = viewDir;
    let L = normalize(ubo.lightDir.xyz);
    let H = normalize(L + V);

    let NdotL = max(dot(N, L), 0.0);
    let NdotV = max(dot(N, V), 0.001);
    let NdotH = max(dot(N, H), 0.0);
    let VdotH = max(dot(V, H), 0.0);

    // 6. Cook-Torrance BRDF
    let D = distributionGGX(NdotH, roughness);
    let Vis = visibilitySmithGGX(NdotL, NdotV, roughness);
    let F = fresnelSchlick(VdotH, F0);
    let specularBRDF = D * Vis * F;

    let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
    let diffuseBRDF = kD * (diffuseColor / 3.14159265359);

    // 7. Clearcoat (KHR_materials_clearcoat)
    var ccFactor = ubo.clearcoatFactor;
    var ccRoughness = ubo.clearcoatRoughness;
    if ((ubo.flags & 32u) != 0u) {
        ccFactor = ccFactor * ccTexSample.r;
        ccRoughness = ccRoughness * ccTexSample.g;
    }
    ccRoughness = clamp(ccRoughness, 0.04, 1.0);

    var clearcoatLobe = vec3<f32>(0.0);
    var clearcoatF = vec3<f32>(0.0);
    if (ccFactor > 0.001) {
        let Dcc = distributionGGX(NdotH, ccRoughness);
        let VisCc = visibilitySmithGGX(NdotL, NdotV, ccRoughness);
        clearcoatF = fresnelSchlick(VdotH, vec3<f32>(0.04));
        clearcoatLobe = vec3<f32>(Dcc * VisCc) * clearcoatF * ccFactor;
    }

    // 8. Sheen (KHR_materials_sheen)
    var sheenColor = ubo.sheenColorFactor.rgb;
    let sheenRoughness = clamp(ubo.sheenColorFactor.w, 0.04, 1.0);
    if ((ubo.flags & 128u) != 0u) {
        sheenColor = sheenColor * sheenTexSample.rgb;
    }
    var sheenLobe = vec3<f32>(0.0);
    if (length(sheenColor) > 0.001) {
        let invAlpha = 1.0 / sheenRoughness;
        let sin2 = max(1.0 - NdotH * NdotH, 0.0);
        let Dcharlie = (2.0 + invAlpha) / (2.0 * 3.14159265359) * pow(sin2, invAlpha * 0.5);
        sheenLobe = sheenColor * Dcharlie / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 0.001);
    }

    // 9. Transmission (KHR_materials_transmission)
    var transmission = ubo.transmissionFactor;
    if ((ubo.flags & 64u) != 0u) {
        transmission = transmission * transTexSample.r;
    }
    let transFactor = clamp(transmission, 0.0, 1.0);

    // 10. Ambient Occlusion & Micro-Cavity Self-Shadowing
    var ao = 1.0;
    if ((ubo.flags & 16u) != 0u) {
        let occSample = occTexSample.r;
        ao = 1.0 + ubo.occlusionStrength * (occSample - 1.0);
    }
    ao = clamp(ao * bumpCavity, 0.0, 1.0);

    // 11. Emissive (KHR_materials_emissive_strength)
    var emissive = ubo.emissiveFactor.xyz * ubo.emissiveFactor.w;
    if ((ubo.flags & 8u) != 0u) {
        emissive = emissive * emTexSample.rgb;
    }

    // 12. Directional Shadow Mapping (Flag bit 12 = 4096u = EnableShadows)
    var shadowFactor = 1.0;
    if ((ubo.flags & 4096u) != 0u) {
        shadowFactor = computePCFShadow(in.worldPos, geomN, L);
    }

    // 13. Direct Sun Lighting + Architectural Courtyard Fill Light (reveals bump/normal relief in shadowed areas)
    let lightColor = vec3<f32>(1.0, 0.98, 0.95);
    let directDiffuse = diffuseBRDF * (1.0 - transFactor);
    var directLighting = (directDiffuse + specularBRDF) * lightColor * NdotL * 3.14159265359 * bumpCavity;

    if (ccFactor > 0.001) {
        directLighting = directLighting * (1.0 - clearcoatF * ccFactor) + clearcoatLobe * lightColor * NdotL * 3.14159265359;
    }
    directLighting = (directLighting + sheenLobe * lightColor * NdotL) * shadowFactor;

    // Courtyard Sky & View Fill Light (active in shadowed arcades so normal/bump relief remains crisp)
    let L_fill = normalize(V * 0.55 + vec3<f32>(0.25, 0.85, 0.20));
    let H_fill = normalize(L_fill + V);
    let NdotL_fill = max(dot(N, L_fill), 0.0);
    let NdotH_fill = max(dot(N, H_fill), 0.0);
    let VdotH_fill = max(dot(V, H_fill), 0.0);
    let D_fill = distributionGGX(NdotH_fill, max(roughness * 0.85, 0.08));
    let Vis_fill = visibilitySmithGGX(NdotL_fill, NdotV, max(roughness * 0.85, 0.08));
    let F_fill = fresnelSchlick(VdotH_fill, F0);
    let fillSpec = D_fill * Vis_fill * F_fill;
    let fillDiff = (vec3<f32>(1.0) - F_fill) * (1.0 - metallic) * (diffuseColor / 3.14159265359) * (1.0 - transFactor);
    let fillLightColor = vec3<f32>(0.62, 0.70, 0.82);
    let fillLighting = (fillDiff * 0.85 + fillSpec * 1.35) * fillLightColor * NdotL_fill * 3.14159265359 * ao;

    // 14. Environment Cubemap Reflections & IBL (Flag bit 13 = 8192u = EnableReflections)
    let skyColor = vec3<f32>(0.38, 0.48, 0.62);
    let groundColor = vec3<f32>(0.26, 0.22, 0.18);
    let hemiFactor = N.y * 0.5 + 0.5;
    let ambientLight = mix(groundColor, skyColor, hemiFactor) * 0.95;

    var ambientDiffuse = diffuseColor * ambientLight * ao * (1.0 - transFactor);
    var ambientSpecular = vec3<f32>(0.0);

    if ((ubo.flags & 8192u) != 0u) {
        let maxMip = max(ubo.envMipCount - 1.0, 0.0);
        let R = reflect(-V, N);
        let specMip = clamp(roughness * maxMip, 0.0, maxMip);
        let prefilteredEnv = textureSampleLevel(envCubemap, defaultSampler, R, specMip).rgb;
        let envIrradiance = textureSampleLevel(envCubemap, defaultSampler, N, maxMip).rgb;
        let envBrdf = envBRDFApprox(F0, roughness, NdotV);

        ambientDiffuse = diffuseColor * mix(ambientLight, envIrradiance * 1.15, 0.55) * ao * (1.0 - transFactor);
        ambientSpecular = prefilteredEnv * envBrdf * ubo.reflectionIntensity * mix(0.35, 1.0, ao);

        if (ccFactor > 0.001) {
            let R_cc = reflect(-V, geomN);
            let ccMip = clamp(ccRoughness * maxMip, 0.0, maxMip);
            let ccEnv = textureSampleLevel(envCubemap, defaultSampler, R_cc, ccMip).rgb;
            let ccFresnel = fresnelSchlick(NdotV, vec3<f32>(0.04));
            ambientSpecular = ambientSpecular * (1.0 - ccFresnel * ccFactor) + ccEnv * ccFresnel * ccFactor * ubo.reflectionIntensity;
        }
    } else {
        ambientSpecular = mix(F0, vec3<f32>(1.0), pow(1.0 - NdotV, 5.0)) * ambientLight * (1.0 - roughness) * 0.25 * ao;
    }

    let finalColor = directLighting + fillLighting + ambientDiffuse + ambientSpecular + emissive;

    var outAlpha = baseColor.a;
    if (transFactor > 0.001) {
        let fresnelGlass = fresnelSchlick(NdotV, vec3<f32>(f0_dielectric)).r;
        outAlpha = mix(baseColor.a, 0.25 + 0.75 * fresnelGlass, transFactor);
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
