#include "common/rt_traversal.wgsl"

struct RtPassUniforms {
    invViewProj : mat4x4<f32>,
    prevViewProj : mat4x4<f32>,
    viewProj : mat4x4<f32>,
    cameraPos : vec4<f32>,
    sunDir : vec4<f32>,
    params0 : vec4<f32>,
    params1 : vec4<f32>,
    params2 : vec4<f32>,
    params3 : vec4<f32>,
    lightsPos : array<vec4<f32>, 4>,
    lightsCol : array<vec4<f32>, 4>,
};

@group(0) @binding(0) var<uniform> ubo : RtPassUniforms;
@group(0) @binding(1) var gbufferDepth : texture_depth_2d;
@group(0) @binding(2) var gbufferNormalRoughness : texture_2d<f32>;
@group(0) @binding(3) var aoOutTex : texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn cs_rt_ao(@builtin(global_invocation_id) gid : vec3<u32>) {
    let traceW = u32(ubo.params0.z);
    let traceH = u32(ubo.params0.w);
    if (gid.x >= traceW || gid.y >= traceH) {
        return;
    }

    let fullDims = vec2<i32>(i32(ubo.params0.x), i32(ubo.params0.y));
    let scale = select(1, 2, ubo.params2.z > 0.5);
    let fullPx = clamp(vec2<i32>(gid.xy) * scale, vec2<i32>(0), fullDims - vec2<i32>(1));
    let depth = textureLoad(gbufferDepth, fullPx, 0);
    if (depth >= 0.99999) {
        textureStore(aoOutTex, vec2<i32>(gid.xy), vec4<f32>(1.0));
        return;
    }

    let uv = (vec2<f32>(fullPx) + vec2<f32>(0.5)) / vec2<f32>(fullDims);
    let worldPos = rtWorldPosFromDepth(uv, depth, ubo.invViewProj);
    let N = rtSafeNormalize(textureLoad(gbufferNormalRoughness, fullPx, 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    let viewDist = length(worldPos - ubo.cameraPos.xyz);
    let origin = worldPos + N * clamp(0.0002 + viewDist * 0.0004, 0.0002, 0.02);

    var rng = rtPcgHash((gid.x * 1973u) ^ (gid.y * 9277u) ^ (u32(ubo.cameraPos.w) * 26699u));
    let radius = max(ubo.params1.x, 0.05);
    let kSamples : u32 = 2u;
    var occSum : f32 = 0.0;

    for (var s : u32 = 0u; s < kSamples; s = s + 1u) {
        let u1 = rtRand01(&rng);
        let u2 = rtRand01(&rng);
        let dir = rtSampleCosineHemisphere(u1, u2, N);
        let hit = rtTraceClosest(origin, dir, 0.001, radius);
        if (hit.hit) {
            let atten = 1.0 - smoothstep(0.0, radius, hit.t);
            occSum = occSum + atten;
        }
    }

    let occ = occSum / f32(kSamples);
    let intensity = clamp(ubo.params1.y, 0.0, 2.0);
    let ao = clamp(1.0 - occ * intensity, 0.05, 1.0);
    textureStore(aoOutTex, vec2<i32>(gid.xy), vec4<f32>(ao, ao, ao, 1.0));
}
