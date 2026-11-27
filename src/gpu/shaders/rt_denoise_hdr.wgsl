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
@group(0) @binding(3) var srcTex : texture_2d<f32>;
@group(0) @binding(4) var historyTex : texture_2d<f32>;
@group(0) @binding(5) var dstTex16 : texture_storage_2d<rgba16float, write>;

fn rtComputePrevUv16(worldPos : vec3<f32>) -> vec2<f32> {
    let prevClip = ubo.prevViewProj * vec4<f32>(worldPos, 1.0);
    if (prevClip.w <= 1e-4) {
        return vec2<f32>(-1.0);
    }
    let prevNdc = prevClip.xyz / prevClip.w;
    return vec2<f32>(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

@compute @workgroup_size(8, 8)
fn cs_temporal_16(@builtin(global_invocation_id) gid : vec3<u32>) {
    let traceW = i32(ubo.params0.z);
    let traceH = i32(ubo.params0.w);
    if (i32(gid.x) >= traceW || i32(gid.y) >= traceH) {
        return;
    }

    let px = vec2<i32>(gid.xy);
    let curr = textureLoad(srcTex, px, 0);

    let fullDims = vec2<i32>(i32(ubo.params0.x), i32(ubo.params0.y));
    let scale = select(1, 2, ubo.params2.z > 0.5);
    let fullPx = clamp(px * scale, vec2<i32>(0), fullDims - vec2<i32>(1));
    let depth = textureLoad(gbufferDepth, fullPx, 0);
    if (depth >= 0.99999) {
        textureStore(dstTex16, px, vec4<f32>(0.0));
        return;
    }

    let uv = (vec2<f32>(fullPx) + vec2<f32>(0.5)) / vec2<f32>(fullDims);
    let worldPos = rtWorldPosFromDepth(uv, depth, ubo.invViewProj);
    let prevUv = rtComputePrevUv16(worldPos);
    if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0 || ubo.cameraPos.w < 1.5) {
        textureStore(dstTex16, px, curr);
        return;
    }

    var nMin = curr.rgb;
    var nMax = curr.rgb;
    for (var dy : i32 = -1; dy <= 1; dy = dy + 1) {
        for (var dx : i32 = -1; dx <= 1; dx = dx + 1) {
            let np = clamp(px + vec2<i32>(dx, dy), vec2<i32>(0), vec2<i32>(traceW - 1, traceH - 1));
            let s = textureLoad(srcTex, np, 0).rgb;
            nMin = min(nMin, s);
            nMax = max(nMax, s);
        }
    }

    let prevPx = clamp(vec2<i32>(prevUv * vec2<f32>(f32(traceW), f32(traceH))), vec2<i32>(0),
                       vec2<i32>(traceW - 1, traceH - 1));
    let hist = textureLoad(historyTex, prevPx, 0);
    let clampedHist = clamp(hist.rgb, nMin * 0.85, nMax * 1.15 + vec3<f32>(0.02));
    let roughness = textureLoad(gbufferNormalRoughness, fullPx, 0).w;
    let blendAlpha = mix(0.35, 0.16, clamp(roughness * 2.0, 0.0, 1.0));
    let blended = mix(clampedHist, curr.rgb, blendAlpha);
    textureStore(dstTex16, px, vec4<f32>(blended, curr.a));
}

@compute @workgroup_size(8, 8)
fn cs_atrous_16(@builtin(global_invocation_id) gid : vec3<u32>) {
    let traceW = i32(ubo.params0.z);
    let traceH = i32(ubo.params0.w);
    if (i32(gid.x) >= traceW || i32(gid.y) >= traceH) {
        return;
    }

    let px = vec2<i32>(gid.xy);
    let centerVal = textureLoad(srcTex, px, 0);
    let fullDims = vec2<i32>(i32(ubo.params0.x), i32(ubo.params0.y));
    let scale = select(1, 2, ubo.params2.z > 0.5);
    let fullPx = clamp(px * scale, vec2<i32>(0), fullDims - vec2<i32>(1));
    let centerDepth = textureLoad(gbufferDepth, fullPx, 0);
    if (centerDepth >= 0.99999) {
        textureStore(dstTex16, px, vec4<f32>(0.0));
        return;
    }

    let cNR = textureLoad(gbufferNormalRoughness, fullPx, 0);
    let centerN = rtSafeNormalize(cNR.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let centerRough = cNR.w;
    let stride = max(i32(ubo.params3.w), 1);

    let roughScale = smoothstep(0.04, 0.35, centerRough);
    if (roughScale < 0.02) {
        textureStore(dstTex16, px, centerVal);
        return;
    }

    let kernel = array<f32, 3>(1.0, 2.0 / 3.0, 1.0 / 6.0);
    var sumCol = centerVal.rgb;
    var sumW : f32 = 1.0;

    for (var dy : i32 = -1; dy <= 1; dy = dy + 1) {
        for (var dx : i32 = -1; dx <= 1; dx = dx + 1) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            let sp = clamp(px + vec2<i32>(dx * stride, dy * stride), vec2<i32>(0),
                           vec2<i32>(traceW - 1, traceH - 1));
            let sFullPx = clamp(sp * scale, vec2<i32>(0), fullDims - vec2<i32>(1));
            let sDepth = textureLoad(gbufferDepth, sFullPx, 0);
            if (sDepth >= 0.99999) {
                continue;
            }
            let sNR = textureLoad(gbufferNormalRoughness, sFullPx, 0);
            let sN = rtSafeNormalize(sNR.xyz, vec3<f32>(0.0, 1.0, 0.0));
            let nW = pow(max(dot(centerN, sN), 0.0), 48.0);
            let dW = exp(-abs(centerDepth - sDepth) * 450.0);
            let rW = exp(-abs(centerRough - sNR.w) * 16.0);
            let kW = kernel[abs(dx)] * kernel[abs(dy)];
            let w = kW * nW * dW * rW * roughScale;
            sumCol = sumCol + textureLoad(srcTex, sp, 0).rgb * w;
            sumW = sumW + w;
        }
    }

    textureStore(dstTex16, px, vec4<f32>(sumCol / max(sumW, 1e-4), centerVal.a));
}
