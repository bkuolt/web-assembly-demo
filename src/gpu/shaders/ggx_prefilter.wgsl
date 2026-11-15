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

#include "common/ggx_sample.wgsl"

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
