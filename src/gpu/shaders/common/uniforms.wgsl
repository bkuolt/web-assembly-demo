// Shared PerDrawUniforms, MaterialUniforms, VertexInput, and KHR_texture_transform helpers
#pragma once

struct PerDrawUniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    lightViewProj : mat4x4<f32>,
    morphWeights0 : vec4<f32>,
    morphWeights1 : vec4<f32>,
    jointCount : u32,
    morphTargetCount : u32,
    vertexCount : u32,
    drawFlags : u32,
    drawParams : vec4<f32>,
};

struct MaterialUniforms {
    baseColorFactor : vec4<f32>,
    emissiveFactor : vec4<f32>,
    sheenColorFactor : vec4<f32>,
    specularColorFactor : vec4<f32>,
    attenuationColor : vec4<f32>,
    metallicFactor : f32,
    roughnessFactor : f32,
    normalScale : f32,
    occlusionStrength : f32,
    bumpScale : f32,
    alphaCutoff : f32,
    alphaMode : u32,
    materialFlags : u32,
    ior : f32,
    clearcoatFactor : f32,
    clearcoatRoughness : f32,
    transmissionFactor : f32,
    specularFactor : f32,
    thicknessFactor : f32,
    attenuationDistance : f32,
    reflectionIntensity : f32,
    baseColorUvTransform : vec4<f32>,
    metallicRoughnessUvTransform : vec4<f32>,
    normalUvTransform : vec4<f32>,
    occlusionUvTransform : vec4<f32>,
    emissiveUvTransform : vec4<f32>,
    uvRotations0 : vec4<f32>,
    uvRotations1 : vec4<f32>,
};

struct VertexInput {
    @builtin(vertex_index) vertexIndex : u32,
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
    @location(4) joints : vec4<u32>,
    @location(5) weights : vec4<f32>,
    @location(6) instCol0 : vec4<f32>,
    @location(7) instCol1 : vec4<f32>,
    @location(8) instCol2 : vec4<f32>,
    @location(9) instCol3 : vec4<f32>,
};

fn resolveWorldModel(in : VertexInput, drawModel : mat4x4<f32>) -> mat4x4<f32> {
    let instModel = mat4x4<f32>(in.instCol0, in.instCol1, in.instCol2, in.instCol3);
    return drawModel * instModel;
}

fn transformUv(uv : vec2<f32>, scaleOffset : vec4<f32>, rot : f32) -> vec2<f32> {
    let s = sin(rot);
    let c = cos(rot);
    let su = uv * scaleOffset.xy;
    return vec2<f32>(c * su.x + s * su.y, -s * su.x + c * su.y) + scaleOffset.zw;
}

fn transformUvDeriv(d : vec2<f32>, scaleOffset : vec4<f32>, rot : f32) -> vec2<f32> {
    let s = sin(rot);
    let c = cos(rot);
    let sd = d * scaleOffset.xy;
    return vec2<f32>(c * sd.x + s * sd.y, -s * sd.x + c * sd.y);
}

fn safeNormalize(v : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let lenSq = dot(v, v);
    return select(fallback, v * inverseSqrt(lenSq), lenSq > 1e-12);
}

