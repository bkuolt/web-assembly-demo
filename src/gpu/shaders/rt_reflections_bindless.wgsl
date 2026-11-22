#include "common/rt_traversal.wgsl"
#include "common/weather.wgsl"

@group(2) @binding(0) var rtBindlessSampler : sampler;
@group(2) @binding(1) var rtTextures : binding_array<texture_2d<f32>, 128>;

fn rtSampleBaseColor(mat : RtMaterial, uv : vec2<f32>) -> vec3<f32> {
    var col = mat.baseColorFactor.rgb;
    if (mat.texIndices.x > 0) {
        let idx = min(u32(mat.texIndices.x), 127u);
        col = col * textureSampleLevel(rtTextures[idx], rtBindlessSampler, uv, 0.0).rgb;
    }
    return col;
}

fn rtSampleEmissive(mat : RtMaterial, uv : vec2<f32>) -> vec3<f32> {
    var em = mat.emissiveFactor.rgb;
    if (mat.texIndices.w > 0) {
        let idx = min(u32(mat.texIndices.w), 127u);
        em = em * textureSampleLevel(rtTextures[idx], rtBindlessSampler, uv, 0.0).rgb;
    }
    return em;
}

#include "common/rt_reflections_core.wgsl"
