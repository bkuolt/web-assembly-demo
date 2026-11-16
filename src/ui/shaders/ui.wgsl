// Immediate-mode UI shader supporting solid quads, SDF font glyphs,
// analytical rounded rectangles with crisp borders, soft Gaussian shadows,
// optional frosted HDR backdrop glassmorphism, and HSV color picker gradients.

struct UiUniforms {
    proj : mat4x4<f32>,
    sdfParams : vec4<f32>, // x: pxRange, y: threshold, z: hasBackdrop (>0.5), w: timeSec
};

@group(0) @binding(0) var<uniform> ubo : UiUniforms;
@group(0) @binding(1) var atlasSampler : sampler;
@group(0) @binding(2) var atlasTexture : texture_2d<f32>;
@group(0) @binding(3) var backdropSampler : sampler;
@group(0) @binding(4) var backdropTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) pos : vec2<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) color : vec4<f32>,
    @location(3) isSdf : f32,
    @location(4) sdfRect : vec4<f32>,
    @location(5) auxColor : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
    @location(1) color : vec4<f32>,
    @location(2) isSdf : f32,
    @location(3) sdfRect : vec4<f32>,
    @location(4) auxColor : vec4<f32>,
    @location(5) screenUv : vec2<f32>,
};

fn sdRoundedBox(p : vec2<f32>, halfSize : vec2<f32>, radius : f32) -> f32 {
    let safeHalf = max(halfSize, vec2<f32>(1.0));
    let r = clamp(radius, 0.0, min(safeHalf.x, safeHalf.y));
    let q = abs(p) - safeHalf + vec2<f32>(r);
    return length(max(q, vec2<f32>(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

fn hsv2rgb(c : vec3<f32>) -> vec3<f32> {
    let k = vec4<f32>(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    let p = abs(fract(c.xxx + k.xyz) * 6.0 - k.www);
    return c.z * mix(k.xxx, clamp(p - k.xxx, vec3<f32>(0.0), vec3<f32>(1.0)), c.y);
}

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    let clip = ubo.proj * vec4<f32>(in.pos, 0.0, 1.0);
    out.clipPos = clip;
    out.uv = in.uv;
    out.color = in.color;
    out.isSdf = in.isSdf;
    out.sdfRect = in.sdfRect;
    out.auxColor = in.auxColor;
    out.screenUv = vec2<f32>(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    return out;
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    // Evaluate texture sampling and screen-space derivatives in uniform control flow
    // so WebGPU/Naga never rejects non-uniform derivative calls.
    let dist = textureSample(atlasTexture, atlasSampler, in.uv).a;
    let fwDist = max(fwidth(dist), 0.002) * max(ubo.sdfParams.x, 1.0);

    let boxDist = sdRoundedBox(in.uv, in.sdfRect.xy, in.sdfRect.z);
    let aaBox = max(fwidth(boxDist), 0.75);

    let backdropSample = textureSampleLevel(
        backdropTexture, backdropSampler, clamp(in.screenUv, vec2<f32>(0.0), vec2<f32>(1.0)), 0.0);

    let mode = i32(floor(in.isSdf + 0.5));

    // Mode 1: SDF Font Glyph
    if (mode == 1) {
        let sdfAlpha = smoothstep(ubo.sdfParams.y - fwDist, ubo.sdfParams.y + fwDist, dist);
        return vec4<f32>(in.color.rgb, in.color.a * sdfAlpha);
    }

    // Mode 2 / Mode 4: Analytical SDF Rounded Rectangle (+ optional frosted glass backdrop)
    if (mode == 2 || mode == 4) {
        let outerAlpha = 1.0 - smoothstep(-aaBox, aaBox, boxDist);
        let borderW = max(in.sdfRect.w, 0.0);
        let hasBorder = step(0.01, borderW);
        let borderFactor = smoothstep(-aaBox, aaBox, boxDist + borderW) * hasBorder;

        var fillRgb = in.color.rgb;
        var fillAlpha = in.color.a;

        if (mode == 4 && ubo.sdfParams.z > 0.5) {
            let blurHdr = max(backdropSample.rgb, vec3<f32>(0.0));
            let blurLdr = blurHdr / (vec3<f32>(1.0) + blurHdr);
            let tintedGlass = blurLdr * 0.42 + in.color.rgb * in.color.a;
            let topSheen = exp(-max(in.uv.y + in.sdfRect.y, 0.0) * 0.12) * 0.06;
            fillRgb = clamp(tintedGlass + vec3<f32>(topSheen), vec3<f32>(0.0), vec3<f32>(1.0));
            fillAlpha = max(in.color.a, 0.88);
        }

        let finalRgb = mix(fillRgb, in.auxColor.rgb, borderFactor);
        let finalA = mix(fillAlpha, in.auxColor.a, borderFactor) * outerAlpha;
        return vec4<f32>(finalRgb, finalA);
    }

    // Mode 3: Soft Drop Shadow
    if (mode == 3) {
        let blur = max(in.sdfRect.w, 1.0);
        let falloff = exp(-max(boxDist, 0.0) * 3.2 / blur) * (1.0 - smoothstep(0.0, blur, boxDist));
        return vec4<f32>(in.color.rgb, in.color.a * falloff);
    }

    // Mode 5: HSV Color Picker Plane (2D SV or 1D Hue strip)
    if (mode == 5) {
        let isHueBar = in.sdfRect.y >= 0.5;
        let hsvCol = select(
            hsv2rgb(vec3<f32>(fract(in.sdfRect.x), clamp(in.uv.x, 0.0, 1.0), clamp(1.0 - in.uv.y, 0.0, 1.0))),
            hsv2rgb(vec3<f32>(clamp(in.uv.y, 0.0, 1.0), 1.0, 1.0)),
            isHueBar);
        return vec4<f32>(hsvCol, in.color.a);
    }

    // Mode 0: Solid / Vertical Gradient Quad
    let hasGrad = step(0.001, in.auxColor.a);
    return mix(in.color, mix(in.color, in.auxColor, clamp(in.uv.y, 0.0, 1.0)), hasGrad);
}
