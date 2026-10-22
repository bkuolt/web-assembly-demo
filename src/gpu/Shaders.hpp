/**
 * @file Shaders.hpp
 * @brief Embedded WGSL Full glTF 2.0 PBR and Wireframe/Bounding-Box Shaders.
 */
#pragma once

#include <string_view>

namespace bgl::gpu {

/**
 * @brief Complete WGSL PBR (Physically Based Rendering) shader code supporting all glTF 2.0
 * and KHR extensions (Clearcoat, Transmission, Sheen, Specular, Emissive Strength, Normal, Occlusion, Unlit).
 */
inline constexpr std::string_view kPbrShaderWGSL = R"(
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
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
    out.tangent = vec4<f32>(normalize(worldTan), in.tangent.w);
    out.uv = in.uv;
    return out;
}

fn getTBN(worldPos : vec3<f32>, geomNormal : vec3<f32>, uv : vec2<f32>, tangent : vec4<f32>) -> mat3x3<f32> {
    let N = normalize(geomNormal);
    if (abs(tangent.w) > 0.001 && length(tangent.xyz) > 0.1) {
        let T = normalize(tangent.xyz);
        let B = normalize(cross(N, T) * tangent.w);
        return mat3x3<f32>(T, B, N);
    }
    let dp1 = dpdx(worldPos);
    let dp2 = dpdy(worldPos);
    let duv1 = dpdx(uv);
    let duv2 = dpdy(uv);
    let dp2perp = cross(dp2, N);
    let dp1perp = cross(N, dp1);
    let T = dp2perp * duv1.x + dp1perp * duv2.x;
    let B = dp2perp * duv1.y + dp1perp * duv2.y;
    let invmax = inverseSqrt(max(dot(T, T), dot(B, B)) + 1e-6);
    return mat3x3<f32>(T * invmax, B * invmax, N);
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

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    // 1. Base Color & Alpha
    var baseColor = ubo.baseColorFactor;
    if ((ubo.flags & 1u) != 0u) {
        baseColor = baseColor * textureSample(baseColorTexture, defaultSampler, in.uv);
    }

    // Alpha Cutoff (AlphaMode::Mask == 1u)
    if (ubo.alphaMode == 1u && baseColor.a < ubo.alphaCutoff) {
        discard;
    }

    // KHR_materials_unlit
    if ((ubo.flags & 256u) != 0u) {
        return baseColor;
    }

    // 2. Normal Calculation
    var N = normalize(in.normal);
    let viewDir = normalize(ubo.cameraPos.xyz - in.worldPos);
    if ((ubo.flags & 512u) != 0u || dot(N, viewDir) < 0.0) {
        if (dot(N, viewDir) < 0.0) {
            N = -N;
        }
    }

    if ((ubo.flags & 4u) != 0u) {
        let tbn = getTBN(in.worldPos, N, in.uv, in.tangent);
        let normTex = textureSample(normalTexture, defaultSampler, in.uv).xyz * 2.0 - 1.0;
        let scaledNorm = vec3<f32>(normTex.xy * ubo.normalScale, normTex.z);
        N = normalize(tbn * scaledNorm);
    }

    // 3. Metallic & Roughness
    var roughness = ubo.roughnessFactor;
    var metallic = ubo.metallicFactor;
    if ((ubo.flags & 2u) != 0u) {
        let mrSample = textureSample(metallicRoughnessTexture, defaultSampler, in.uv);
        roughness = roughness * mrSample.g;
        metallic = metallic * mrSample.b;
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
        let ccSample = textureSample(clearcoatTexture, defaultSampler, in.uv);
        ccFactor = ccFactor * ccSample.r;
        ccRoughness = ccRoughness * ccSample.g;
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
        sheenColor = sheenColor * textureSample(sheenTexture, defaultSampler, in.uv).rgb;
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
        transmission = transmission * textureSample(transmissionTexture, defaultSampler, in.uv).r;
    }
    let transFactor = clamp(transmission, 0.0, 1.0);

    // 10. Ambient Occlusion
    var ao = 1.0;
    if ((ubo.flags & 16u) != 0u) {
        let occSample = textureSample(occlusionTexture, defaultSampler, in.uv).r;
        ao = 1.0 + ubo.occlusionStrength * (occSample - 1.0);
    }
    ao = clamp(ao, 0.0, 1.0);

    // 11. Emissive (KHR_materials_emissive_strength)
    var emissive = ubo.emissiveFactor.xyz * ubo.emissiveFactor.w;
    if ((ubo.flags & 8u) != 0u) {
        emissive = emissive * textureSample(emissiveTexture, defaultSampler, in.uv).rgb;
    }

    // 12. Direct & Ambient Lighting Composition
    let lightColor = vec3<f32>(1.0, 0.98, 0.95);
    let directDiffuse = diffuseBRDF * (1.0 - transFactor);
    var directLighting = (directDiffuse + specularBRDF) * lightColor * NdotL * 3.14159265359;

    if (ccFactor > 0.001) {
        directLighting = directLighting * (1.0 - clearcoatF * ccFactor) + clearcoatLobe * lightColor * NdotL * 3.14159265359;
    }
    directLighting = directLighting + sheenLobe * lightColor * NdotL;

    let skyColor = vec3<f32>(0.38, 0.48, 0.62);
    let groundColor = vec3<f32>(0.26, 0.22, 0.18);
    let hemiFactor = N.y * 0.5 + 0.5;
    let ambientLight = mix(groundColor, skyColor, hemiFactor) * 1.5;
    let ambientDiffuse = diffuseColor * ambientLight * ao * (1.0 - transFactor);
    let ambientSpecular = mix(F0, vec3<f32>(1.0), pow(1.0 - NdotV, 5.0)) * ambientLight * (1.0 - roughness) * 0.5;

    let finalColor = directLighting + ambientDiffuse + ambientSpecular + emissive;

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
    model : mat4x4<f32>,
    color : vec4<f32>,
};

@group(0) @binding(0) var<uniform> ubo : AabbUniforms;

@vertex
fn vs_aabb(@location(0) pos : vec3<f32>) -> @builtin(position) vec4<f32> {
    return ubo.viewProj * ubo.model * vec4<f32>(pos, 1.0);
}

@fragment
fn fs_aabb() -> @location(0) vec4<f32> {
    return ubo.color;
}
)";

/**
 * @brief Wireframe and Bounding-Box debug shader.
 */
inline constexpr std::string_view kWireframeShaderWGSL = R"(
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
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
