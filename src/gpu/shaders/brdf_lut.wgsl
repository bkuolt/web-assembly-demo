@group(0) @binding(0) var brdfLut : texture_storage_2d<rgba16float, write>;

#include "common/ggx_sample.wgsl"

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
