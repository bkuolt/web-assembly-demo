#pragma once

struct SpriteUniforms {
    viewProj : mat4x4<f32>,
    params : vec4<f32>, // x: encodeSrgb (1.0 when drawing onto non-sRGB swapchain, 0.0 for HDR/sRGB)
};

@group(0) @binding(0) var<uniform> uniforms : SpriteUniforms;
@group(1) @binding(0) var spriteSampler : sampler;
@group(1) @binding(1) var spriteTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) pos : vec3<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) color : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
    @location(1) color : vec4<f32>,
};

@vertex
fn vs_sprite(input : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    out.clipPos = uniforms.viewProj * vec4<f32>(input.pos, 1.0);
    out.uv = input.uv;
    out.color = input.color;
    return out;
}

@fragment
fn fs_sprite(input : VertexOutput) -> @location(0) vec4<f32> {
    let sampled = textureSample(spriteTexture, spriteSampler, input.uv);
    var outColor = vec4<f32>(sampled.rgb * input.color.rgb, sampled.a * input.color.a);
    if (outColor.a <= 0.001) {
        discard;
    }
    if (uniforms.params.x > 0.5) {
        outColor = vec4<f32>(pow(max(outColor.rgb, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.2)), outColor.a);
    }
    return outColor;
}
