#include "common/rt_traversal.wgsl"

struct RtPassUniforms {
    invViewProj : mat4x4<f32>,
    prevViewProj : mat4x4<f32>,
    viewProj : mat4x4<f32>,
    cameraPos : vec4<f32>,
    sunDir : vec4<f32>,
    params0 : vec4<f32>,   // x=fullW, y=fullH, z=traceW, w=traceH
    params1 : vec4<f32>,   // x=aoRadius, y=aoIntensity, z=reflIntensity, w=envRotation
    params2 : vec4<f32>,   // x=maxBounces, y=sampleCount, z=halfRes, w=hasPrimarySun
    params3 : vec4<f32>,   // x=punctualCount, y=solarElev, z=envMipCount, w=stepStride
    lightsPos : array<vec4<f32>, 4>,
    lightsCol : array<vec4<f32>, 4>,
};

@group(0) @binding(0) var<uniform> ubo : RtPassUniforms;
@group(0) @binding(1) var gbufferDepth : texture_depth_2d;
@group(0) @binding(2) var gbufferNormalRoughness : texture_2d<f32>;
@group(0) @binding(3) var shadowOutTex : texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn cs_rt_shadows(@builtin(global_invocation_id) gid : vec3<u32>) {
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
        textureStore(shadowOutTex, vec2<i32>(gid.xy), vec4<f32>(1.0));
        return;
    }

    let uv = (vec2<f32>(fullPx) + vec2<f32>(0.5)) / vec2<f32>(fullDims);
    let worldPos = rtWorldPosFromDepth(uv, depth, ubo.invViewProj);
    let N = normalize(textureLoad(gbufferNormalRoughness, fullPx, 0).xyz);
    let viewDist = length(worldPos - ubo.cameraPos.xyz);
    let origin = worldPos + N * clamp(0.002 + viewDist * 0.0008, 0.002, 0.03);

    var rng = rtPcgHash(gid.x + gid.y * 4099u + u32(ubo.cameraPos.w) * 16777619u);
    let sunL = normalize(ubo.sunDir.xyz);
    let softness = clamp(ubo.sunDir.w, 0.0, 0.15);

    var sunShadow = 1.0;
    if (ubo.params2.w > 0.5) {
        let ndl = dot(N, sunL);
        if (ndl <= 0.0) {
            sunShadow = 0.0;
        } else {
            let u1 = rtRand01(&rng);
            let u2 = rtRand01(&rng);
            let basis = rtBuildOrthonormalBasis(sunL);
            let diskR = sqrt(u1) * softness;
            let diskA = u2 * 6.2831853;
            let jitterDir = normalize(sunL + basis[0] * (diskR * cos(diskA)) + basis[1] * (diskR * sin(diskA)));
            if (rtTraceAnyHit(origin, jitterDir, 0.001, 2000.0)) {
                sunShadow = 0.0;
            }
        }
    }

    // Punctual lights (up to 3 slots in .g, .b, .a)
    var punctualShadows = vec3<f32>(1.0, 1.0, 1.0);
    let pCount = min(u32(ubo.params3.x), 3u);
    for (var i : u32 = 0u; i < pCount; i = i + 1u) {
        let lp = ubo.lightsPos[i];
        let lType = u32(lp.w + 0.5);
        var toL = normalize(lp.xyz);
        var maxDist = 2000.0;
        if (lType != 0u) {
            let delta = lp.xyz - origin;
            maxDist = length(delta);
            if (maxDist <= 1e-4) {
                continue;
            }
            toL = delta / maxDist;
            let range = ubo.lightsCol[i].w;
            if (range > 0.0 && maxDist > range) {
                continue;
            }
        }
        if (dot(N, toL) <= 0.0) {
            punctualShadows[i] = 0.0;
        } else {
            let j1 = (rtRand01(&rng) - 0.5) * softness * 0.5;
            let j2 = (rtRand01(&rng) - 0.5) * softness * 0.5;
            let basisP = rtBuildOrthonormalBasis(toL);
            let lDir = normalize(toL + basisP[0] * j1 + basisP[1] * j2);
            if (rtTraceAnyHit(origin, lDir, 0.001, max(maxDist - 0.01, 0.002))) {
                punctualShadows[i] = 0.0;
            }
        }
    }

    textureStore(shadowOutTex, vec2<i32>(gid.xy), vec4<f32>(sunShadow, punctualShadows));
}
