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
@group(0) @binding(5) var dstTex8 : texture_storage_2d<rgba8unorm, write>;

fn rtComputePrevUv(worldPos : vec3<f32>) -> vec2<f32> {
    let prevClip = ubo.prevViewProj * vec4<f32>(worldPos, 1.0);
    if (prevClip.w <= 1e-4) {
        return vec2<f32>(-1.0);
    }
    let prevNdc = prevClip.xyz / prevClip.w;
    return vec2<f32>(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

@compute @workgroup_size(8, 8)
fn cs_temporal_8(@builtin(global_invocation_id) gid : vec3<u32>) {
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
        textureStore(dstTex8, px, curr);
        return;
    }

    let uv = (vec2<f32>(fullPx) + vec2<f32>(0.5)) / vec2<f32>(fullDims);
    let worldPos = rtWorldPosFromDepth(uv, depth, ubo.invViewProj);
    let prevUv = rtComputePrevUv(worldPos);
    if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0 || ubo.cameraPos.w < 1.5) {
        textureStore(dstTex8, px, curr);
        return;
    }

    var nMin = curr;
    var nMax = curr;
    for (var dy : i32 = -1; dy <= 1; dy = dy + 1) {
        for (var dx : i32 = -1; dx <= 1; dx = dx + 1) {
            let np = clamp(px + vec2<i32>(dx, dy), vec2<i32>(0), vec2<i32>(traceW - 1, traceH - 1));
            let s = textureLoad(srcTex, np, 0);
            nMin = min(nMin, s);
            nMax = max(nMax, s);
        }
    }

    let prevPx = clamp(vec2<i32>(prevUv * vec2<f32>(f32(traceW), f32(traceH))), vec2<i32>(0),
                       vec2<i32>(traceW - 1, traceH - 1));
    let hist = clamp(textureLoad(historyTex, prevPx, 0), nMin - vec4<f32>(0.04), nMax + vec4<f32>(0.04));
    let blended = mix(hist, curr, 0.22);
    textureStore(dstTex8, px, blended);
}

@compute @workgroup_size(8, 8)
fn cs_atrous_8(@builtin(global_invocation_id) gid : vec3<u32>) {
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
        textureStore(dstTex8, px, centerVal);
        return;
    }

    let centerN = normalize(textureLoad(gbufferNormalRoughness, fullPx, 0).xyz);
    let stride = max(i32(ubo.params3.w), 1);

    let kernel = array<f32, 3>(1.0, 2.0 / 3.0, 1.0 / 6.0);
    var sumVal = centerVal;
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
            let sN = normalize(textureLoad(gbufferNormalRoughness, sFullPx, 0).xyz);
            let nW = pow(max(dot(centerN, sN), 0.0), 32.0);
            let dW = exp(-abs(centerDepth - sDepth) * 450.0);
            let kW = kernel[abs(dx)] * kernel[abs(dy)];
            let w = kW * nW * dW;
            sumVal = sumVal + textureLoad(srcTex, sp, 0) * w;
            sumW = sumW + w;
        }
    }

    textureStore(dstTex8, px, sumVal / max(sumW, 1e-4));
}

@compute @workgroup_size(8, 8)
fn cs_upsample_8(@builtin(global_invocation_id) gid : vec3<u32>) {
    let fullW = i32(ubo.params0.x);
    let fullH = i32(ubo.params0.y);
    if (i32(gid.x) >= fullW || i32(gid.y) >= fullH) {
        return;
    }

    let fullPx = vec2<i32>(gid.xy);
    let traceW = i32(ubo.params0.z);
    let traceH = i32(ubo.params0.w);
    let scale = select(1, 2, ubo.params2.z > 0.5);

    if (scale == 1) {
        textureStore(dstTex8, fullPx,
                     textureLoad(srcTex, clamp(fullPx, vec2<i32>(0), vec2<i32>(traceW - 1, traceH - 1)), 0));
        return;
    }

    let cDepth = textureLoad(gbufferDepth, fullPx, 0);
    if (cDepth >= 0.99999) {
        textureStore(dstTex8, fullPx, vec4<f32>(1.0));
        return;
    }
    let cN = normalize(textureLoad(gbufferNormalRoughness, fullPx, 0).xyz);
    let baseLow = fullPx / 2;
    var sumVal = vec4<f32>(0.0);
    var sumW : f32 = 0.0;

    for (var dy : i32 = 0; dy <= 1; dy = dy + 1) {
        for (var dx : i32 = 0; dx <= 1; dx = dx + 1) {
            let lp = clamp(baseLow + vec2<i32>(dx, dy), vec2<i32>(0), vec2<i32>(traceW - 1, traceH - 1));
            let sFull = clamp(lp * 2, vec2<i32>(0), vec2<i32>(fullW - 1, fullH - 1));
            let sDepth = textureLoad(gbufferDepth, sFull, 0);
            let sN = normalize(textureLoad(gbufferNormalRoughness, sFull, 0).xyz);
            let nW = pow(max(dot(cN, sN), 0.0), 16.0);
            let dW = exp(-abs(cDepth - sDepth) * 450.0);
            let w = max(nW * dW, 1e-4);
            sumVal = sumVal + textureLoad(srcTex, lp, 0) * w;
            sumW = sumW + w;
        }
    }

    textureStore(dstTex8, fullPx, sumVal / max(sumW, 1e-4));
}
