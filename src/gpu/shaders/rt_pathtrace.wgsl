#include "common/rt_traversal.wgsl"
#include "common/weather.wgsl"

fn rtSampleBaseColor(mat : RtMaterial, uv : vec2<f32>) -> vec3<f32> {
    return mat.baseColorFactor.rgb;
}

fn rtSampleEmissive(mat : RtMaterial, uv : vec2<f32>) -> vec3<f32> {
    return mat.emissiveFactor.rgb;
}

#include "common/rt_pathtrace_core.wgsl"
