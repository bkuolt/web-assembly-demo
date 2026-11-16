// Single-pass FXAA 3.11 edge antialiasing applied to post-tonemapped 3D scene before HUD/UI.

struct FxaaUniforms {
    invScreenSize : vec4<f32>, // x: 1/w, y: 1/h, z: subpixQuality (0.75), w: edgeThreshold (0.125)
    flags : vec4<f32>,         // x: isSrgbTarget (>0.5), y: edgeThresholdMin (0.0312), z: enabled, w: unused
};

@group(0) @binding(0) var<uniform> ubo : FxaaUniforms;
@group(0) @binding(1) var linearSampler : sampler;
@group(0) @binding(2) var ldrTex : texture_2d<f32>;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_fxaa(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

fn srgbToLinear(c : vec3<f32>) -> vec3<f32> {
    let lo = c / 12.92;
    let hi = pow(max((c + 0.055) / 1.055, vec3<f32>(0.0)), vec3<f32>(2.4));
    return select(hi, lo, c <= vec3<f32>(0.04045));
}

fn fxaaLuma(rgb : vec3<f32>) -> f32 {
    return dot(rgb, vec3<f32>(0.299, 0.587, 0.114));
}

@fragment
fn fs_fxaa(in : VsOut) -> @location(0) vec4<f32> {
    let texel = ubo.invScreenSize.xy;
    let uv = in.uv;
    let centerSample = textureSampleLevel(ldrTex, linearSampler, uv, 0.0);
    var resultRgb = centerSample.rgb;

    if (ubo.flags.z > 0.5) {
        let rgbN = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>(0.0, -texel.y), 0.0).rgb;
        let rgbS = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>(0.0,  texel.y), 0.0).rgb;
        let rgbW = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>(-texel.x, 0.0), 0.0).rgb;
        let rgbE = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>( texel.x, 0.0), 0.0).rgb;

        let lumM = fxaaLuma(resultRgb);
        let lumN = fxaaLuma(rgbN);
        let lumS = fxaaLuma(rgbS);
        let lumW = fxaaLuma(rgbW);
        let lumE = fxaaLuma(rgbE);

        let rangeMin = min(lumM, min(min(lumN, lumS), min(lumW, lumE)));
        let rangeMax = max(lumM, max(max(lumN, lumS), max(lumW, lumE)));
        let range = rangeMax - rangeMin;

        if (range >= max(ubo.flags.y, rangeMax * ubo.invScreenSize.w)) {
            let rgbNW = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>(-texel.x, -texel.y), 0.0).rgb;
            let rgbNE = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>( texel.x, -texel.y), 0.0).rgb;
            let rgbSW = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>(-texel.x,  texel.y), 0.0).rgb;
            let rgbSE = textureSampleLevel(ldrTex, linearSampler, uv + vec2<f32>( texel.x,  texel.y), 0.0).rgb;

            let lumNW = fxaaLuma(rgbNW);
            let lumNE = fxaaLuma(rgbNE);
            let lumSW = fxaaLuma(rgbSW);
            let lumSE = fxaaLuma(rgbSE);

            let lumNS = lumN + lumS;
            let lumWE = lumW + lumE;
            let lumCorners = lumNW + lumNE + lumSW + lumSE;

            let subpixRcp = ((2.0 * (lumNS + lumWE) + lumCorners) * (1.0 / 12.0)) - lumM;
            let subpixBlend = smoothstep(0.0, 1.0, clamp(abs(subpixRcp) / range, 0.0, 1.0));
            let subpixFactor = subpixBlend * subpixBlend * ubo.invScreenSize.z;

            let edgeH = abs(lumNW - 2.0 * lumW + lumSW) + abs(lumN - 2.0 * lumM + lumS) * 2.0 +
                        abs(lumNE - 2.0 * lumE + lumSE);
            let edgeV = abs(lumNW - 2.0 * lumN + lumNE) + abs(lumW - 2.0 * lumM + lumE) * 2.0 +
                        abs(lumSW - 2.0 * lumS + lumSE);
            let isHoriz = edgeH >= edgeV;

            let lum1 = select(lumW, lumN, isHoriz);
            let lum2 = select(lumE, lumS, isHoriz);
            let grad1 = abs(lum1 - lumM);
            let grad2 = abs(lum2 - lumM);
            let stepDir = select(1.0, -1.0, grad1 >= grad2);
            let stepLength = select(texel.x, texel.y, isHoriz) * stepDir;
            let gradScaled = 0.25 * max(grad1, grad2);
            let lumaLocalAvg = 0.5 * (select(lum2, lum1, grad1 >= grad2) + lumM);

            var posB = uv + select(vec2<f32>(stepLength * 0.5, 0.0), vec2<f32>(0.0, stepLength * 0.5), isHoriz);
            let offNP = select(vec2<f32>(0.0, texel.y), vec2<f32>(texel.x, 0.0), isHoriz);

            var uv1 = posB - offNP;
            var uv2 = posB + offNP;
            var end1 = fxaaLuma(textureSampleLevel(ldrTex, linearSampler, uv1, 0.0).rgb) - lumaLocalAvg;
            var end2 = fxaaLuma(textureSampleLevel(ldrTex, linearSampler, uv2, 0.0).rgb) - lumaLocalAvg;
            var done1 = abs(end1) >= gradScaled;
            var done2 = abs(end2) >= gradScaled;

            for (var s = 0; s < 5; s = s + 1) {
                if (!done1) {
                    uv1 -= offNP * 1.5;
                    end1 = fxaaLuma(textureSampleLevel(ldrTex, linearSampler, uv1, 0.0).rgb) - lumaLocalAvg;
                    done1 = abs(end1) >= gradScaled;
                }
                if (!done2) {
                    uv2 += offNP * 1.5;
                    end2 = fxaaLuma(textureSampleLevel(ldrTex, linearSampler, uv2, 0.0).rgb) - lumaLocalAvg;
                    done2 = abs(end2) >= gradScaled;
                }
            }

            let dist1 = select(uv.y - uv1.y, uv.x - uv1.x, isHoriz);
            let dist2 = select(uv2.y - uv.y, uv2.x - uv.x, isHoriz);
            let minDist = min(dist1, dist2);
            let spanLen = max(dist1 + dist2, 1e-5);
            let goodSpan = select((end2 < 0.0) != ((lumM - lumaLocalAvg) < 0.0),
                                  (end1 < 0.0) != ((lumM - lumaLocalAvg) < 0.0),
                                  dist1 < dist2);
            let pixelOffset = select(0.0, 0.5 - minDist / spanLen, goodSpan);
            let finalOffset = max(pixelOffset, subpixFactor);

            let finalUv = uv + select(vec2<f32>(finalOffset * stepLength, 0.0),
                                      vec2<f32>(0.0, finalOffset * stepLength), isHoriz);
            resultRgb = textureSampleLevel(ldrTex, linearSampler, finalUv, 0.0).rgb;
        }
    }

    if (ubo.flags.x > 0.5) {
        resultRgb = srgbToLinear(resultRgb);
    }
    return vec4<f32>(resultRgb, 1.0);
}
