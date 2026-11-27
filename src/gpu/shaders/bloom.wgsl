// 5-Mip Physically Based Bloom (Jimenez 13-tap Karis Downsample + Tent Upsample).

struct BloomUniforms {
    params0 : vec4<f32>, // x: texelSize.x, y: texelSize.y, z: threshold, w: softKnee
    params1 : vec4<f32>, // x: isFirstMip (>0.5), y: filterRadius, z: boost, w: unused
};

@group(0) @binding(0) var<uniform> ubo : BloomUniforms;
@group(0) @binding(1) var srcSampler : sampler;
@group(0) @binding(2) var srcTex : texture_2d<f32>;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_bloom(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

fn luminance(c : vec3<f32>) -> f32 {
    return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

fn karisAverage(c : vec3<f32>) -> f32 {
    let finiteC = select(vec3<f32>(0.0), c, all(c >= vec3<f32>(0.0)) && all(c < vec3<f32>(65504.0)));
    return 1.0 / (1.0 + luminance(finiteC));
}

fn prefilterSoftThreshold(hdr : vec3<f32>, threshold : f32, knee : f32) -> vec3<f32> {
    let finiteHdr = select(vec3<f32>(0.0), hdr, all(hdr >= vec3<f32>(0.0)) && all(hdr < vec3<f32>(65504.0)));
    let clamped = min(finiteHdr, vec3<f32>(64.0));
    let br = max(clamped.r, max(clamped.g, clamped.b));
    let soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    let softQuad = (soft * soft) / max(4.0 * knee, 1e-5);
    let contrib = max(softQuad, br - threshold) / max(br, 1e-5);
    return clamped * clamp(contrib, 0.0, 16.0);
}

@fragment
fn fs_bloom_downsample(in : VsOut) -> @location(0) vec4<f32> {
    let d = ubo.params0.xy;
    let uv = in.uv;

    let a = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-2.0, 2.0) * d, 0.0).rgb;
    let b = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 0.0, 2.0) * d, 0.0).rgb;
    let c = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 2.0, 2.0) * d, 0.0).rgb;

    let D = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-1.0, 1.0) * d, 0.0).rgb;
    let e = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 1.0, 1.0) * d, 0.0).rgb;

    let f = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-2.0, 0.0) * d, 0.0).rgb;
    let g = textureSampleLevel(srcTex, srcSampler, uv, 0.0).rgb;
    let h = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 2.0, 0.0) * d, 0.0).rgb;

    let i = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-1.0, -1.0) * d, 0.0).rgb;
    let j = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 1.0, -1.0) * d, 0.0).rgb;

    let k = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-2.0, -2.0) * d, 0.0).rgb;
    let l = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 0.0, -2.0) * d, 0.0).rgb;
    let m = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 2.0, -2.0) * d, 0.0).rgb;

    var down : vec3<f32>;
    if (ubo.params1.x > 0.5) {
        // Mip 0: Apply soft threshold first, then Karis partial averages to suppress fireflies
        let th = ubo.params0.z;
        let kn = ubo.params0.w;
        var g0 = prefilterSoftThreshold((D + e + i + j) * 0.25, th, kn);
        var g1 = prefilterSoftThreshold((a + b + g + f) * 0.25, th, kn);
        var g2 = prefilterSoftThreshold((b + c + h + g) * 0.25, th, kn);
        var g3 = prefilterSoftThreshold((f + g + l + k) * 0.25, th, kn);
        var g4 = prefilterSoftThreshold((g + h + m + l) * 0.25, th, kn);

        let w0 = karisAverage(g0) * 0.5;
        let w1 = karisAverage(g1) * 0.125;
        let w2 = karisAverage(g2) * 0.125;
        let w3 = karisAverage(g3) * 0.125;
        let w4 = karisAverage(g4) * 0.125;
        down = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / max(w0 + w1 + w2 + w3 + w4, 1e-5);
    } else {
        down  = e * 0.125 + D * 0.125 + i * 0.125 + j * 0.125;
        down += (a + c + k + m) * 0.03125;
        down += (b + f + h + l) * 0.0625;
        down += g * 0.125;
    }

    return vec4<f32>(max(down, vec3<f32>(0.0)), 1.0);
}

@fragment
fn fs_bloom_upsample(in : VsOut) -> @location(0) vec4<f32> {
    let radius = max(ubo.params1.y, 0.5) * ubo.params0.xy;
    let uv = in.uv;

    let a = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-1.0,  1.0) * radius, 0.0).rgb;
    let b = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 0.0,  1.0) * radius, 0.0).rgb;
    let c = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 1.0,  1.0) * radius, 0.0).rgb;

    let d = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-1.0,  0.0) * radius, 0.0).rgb;
    let e = textureSampleLevel(srcTex, srcSampler, uv, 0.0).rgb;
    let f = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 1.0,  0.0) * radius, 0.0).rgb;

    let g = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>(-1.0, -1.0) * radius, 0.0).rgb;
    let h = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 0.0, -1.0) * radius, 0.0).rgb;
    let i = textureSampleLevel(srcTex, srcSampler, uv + vec2<f32>( 1.0, -1.0) * radius, 0.0).rgb;

    var up = e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i);
    up *= (1.0 / 16.0);
    return vec4<f32>(max(up, vec3<f32>(0.0)), 1.0);
}
