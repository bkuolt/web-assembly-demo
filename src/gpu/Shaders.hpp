/**
 * @file Shaders.hpp
 * @brief Embedded WGSL PBR and Wireframe/Bounding-Box Shaders.
 */
#pragma once

#include <string_view>

namespace bgl::gpu {

/**
 * @brief Complete WGSL PBR (Physically Based Rendering) shader code.
 */
inline constexpr std::string_view kPbrShaderWGSL = R"(
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    baseColorFactor : vec4<f32>,
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(2) var baseColorSampler : sampler;

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
    @location(2) uv : vec2<f32>,
};

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    let world = ubo.model * vec4<f32>(in.position, 1.0);
    out.worldPos = world.xyz;
    out.clipPos = ubo.viewProj * world;
    out.normal = normalize((ubo.model * vec4<f32>(in.normal, 0.0)).xyz);
    out.uv = in.uv;
    return out;
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    let N = normalize(in.normal);
    let L = normalize(ubo.lightDir.xyz);
    let V = normalize(ubo.cameraPos.xyz - in.worldPos);
    let H = normalize(L + V);

    // Two-sided lighting
    let NdotL = max(abs(dot(N, L)), 0.25);
    let NdotH = max(dot(N, H), 0.0);
    let spec = pow(NdotH, 32.0) * 0.35;

    let texColor = textureSample(baseColorTexture, baseColorSampler, in.uv);
    let baseColor = texColor * ubo.baseColorFactor;
    let ambient = baseColor.rgb * 0.35;
    let diffuse = baseColor.rgb * NdotL * 0.65;
    let finalColor = ambient + diffuse + vec3<f32>(spec);

    return vec4<f32>(finalColor, baseColor.a);
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
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(2) var baseColorSampler : sampler;

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
    return vec4<f32>(0.2, 0.95, 0.3, 1.0);
}
)";

inline constexpr std::string_view kSkyboxShaderWGSL = R"(
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
};
@group(0) @binding(0) var<uniform> ubo : Uniforms;
@vertex
fn vs_main(@location(0) position : vec3<f32>) -> @builtin(position) vec4<f32> {
    return ubo.viewProj * vec4<f32>(position, 1.0);
}
@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(0.1, 0.12, 0.18, 1.0);
}
)";

} // namespace bgl::gpu
