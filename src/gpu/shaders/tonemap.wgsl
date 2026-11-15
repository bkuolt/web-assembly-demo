struct TonemapUniforms {
    exposure : f32,
    operatorId : u32,
    isSrgbSurface : u32,
    lensFlareEnabled : u32,
    sunScreenPos : vec4<f32>, // xy = sun UV [0..1], z = visibility [0..1], w = aspect ratio (width / height)
    flareParams : vec4<f32>,  // x = intensity, y = haloRadius, z = chromaticDispersal, w = unused
    bloomParams : vec4<f32>,  // x = intensity, y = threshold, z = scatter, w = enabled
    ssaoParams : vec4<f32>,   // x = intensity, y = radius, z = bias, w = enabled
};

@group(0) @binding(0) var<uniform> ubo : TonemapUniforms;
@group(0) @binding(1) var hdrSampler : sampler;
@group(0) @binding(2) var hdrTexture : texture_2d<f32>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_tonemap(@builtin(vertex_index) vIdx : u32) -> VertexOutput {
    var out : VertexOutput;
    let x = f32(i32(vIdx << 1u) & 2) * 2.0 - 1.0;
    let y = f32(i32(vIdx & 2u)) * 2.0 - 1.0;
    out.clipPos = vec4<f32>(x, y, 0.0, 1.0);
    out.uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
    return out;
}

// 0: Khronos PBR Neutral Tonemapper (Official Khronos specification)
fn tonemapKhronosPbrNeutral(colorIn : vec3<f32>) -> vec3<f32> {
    var color = max(colorIn, vec3<f32>(0.0));
    let startCompression : f32 = 0.8 - 0.04;
    let desaturation : f32 = 0.15;

    let x = min(color.r, min(color.g, color.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    color = color - vec3<f32>(offset);

    let peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) {
        return color;
    }

    let d = 1.0 - startCompression;
    let newPeak = 1.0 - d * d / (peak + d - startCompression);
    color = color * (newPeak / max(peak, 1e-6));

    let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3<f32>(newPeak), g);
}

// 1: AgX Tonemapper (Troy Sobotka / Blender 4.0)
fn agxDefaultContrastApprox(x : vec3<f32>) -> vec3<f32> {
    let x2 = x * x;
    let x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

fn tonemapAgX(colorIn : vec3<f32>) -> vec3<f32> {
    let agxMat = mat3x3<f32>(
        vec3<f32>(0.842479062253094,  0.0423282422610123, 0.0423756549057051),
        vec3<f32>(0.0784335999999992, 0.878468636469772,  0.0784336),
        vec3<f32>(0.0792237451477643, 0.0791661274605434, 0.879142973793104)
    );
    let agxMatInv = mat3x3<f32>(
        vec3<f32>( 1.19687900512017,   -0.0528968517574562, -0.0529716355144438),
        vec3<f32>(-0.0980208811401368,  1.15190312990417,   -0.0980434501171241),
        vec3<f32>(-0.0990297440797205, -0.0989611768448433,  1.15107367264116)
    );

    let minEv : f32 = -12.47393;
    let maxEv : f32 = 4.026069;

    var val = agxMat * max(colorIn, vec3<f32>(1e-10));
    val = clamp(log2(val), vec3<f32>(minEv), vec3<f32>(maxEv));
    val = (val - vec3<f32>(minEv)) / (maxEv - minEv);
    val = agxDefaultContrastApprox(val);
    val = agxMatInv * val;
    return clamp(pow(max(val, vec3<f32>(0.0)), vec3<f32>(2.2)), vec3<f32>(0.0), vec3<f32>(1.0));
}

// 2: ACES Filmic Tonemapper (Stephen Hill RRT+ODT fit)
fn tonemapACES(colorIn : vec3<f32>) -> vec3<f32> {
    let x = max(colorIn, vec3<f32>(0.0));
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn linearToSrgb(linearCol : vec3<f32>) -> vec3<f32> {
    let c = clamp(linearCol, vec3<f32>(0.0), vec3<f32>(1.0));
    let lo = c * 12.92;
    let hi = 1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - vec3<f32>(0.055);
    return select(hi, lo, c <= vec3<f32>(0.0031308));
}

/**
 * @brief Evaluates cinematic directional sun lens flares with corona, anamorphic streaks, and chromatic ghosts.
 */
fn evaluateLensFlare(uv : vec2<f32>, sunUV : vec2<f32>, visibility : f32, aspect : f32, intensity : f32) -> vec3<f32> {
    if (visibility <= 0.001 || intensity <= 0.001) {
        return vec3<f32>(0.0);
    }
    let aspectVec = vec2<f32>(max(aspect, 0.1), 1.0);
    let delta = (uv - sunUV) * aspectVec;
    let distToSun = length(delta);

    // 1. Primary Sun Core Glow & Radial Corona
    let corona = 0.75 / (distToSun * 12.0 + 1.0);
    let broadGlow = 0.35 / (distToSun * 3.2 + 1.0);
    let sunColor = vec3<f32>(1.0, 0.88, 0.65);
    var flareColor = (corona * corona + broadGlow) * sunColor;

    // 2. Anamorphic Horizontal Streak (Cinematic flare)
    let streakY = abs(delta.y);
    let streakX = abs(delta.x);
    let anamorphicStreak = exp(-streakY * 42.0) * exp(-streakX * 1.8) * 0.5;
    let streakColor = vec3<f32>(0.55, 0.78, 1.0);
    flareColor = flareColor + anamorphicStreak * streakColor;

    // 3. Multi-Element Lens Ghosts along the optical axis passing through the screen center (0.5, 0.5)
    let centerUV = vec2<f32>(0.5, 0.5);
    let sunToCenter = (centerUV - sunUV);

    // Ghost 1: Small bright violet disc (distance 0.45 along axis)
    let g1Pos = (sunUV + sunToCenter * 0.45) * aspectVec;
    let d1 = length(uv * aspectVec - g1Pos);
    let ghost1 = exp(-d1 * 22.0) * vec3<f32>(0.35, 0.65, 0.95);

    // Ghost 2: Warm soft halo disc (distance 0.85 along axis)
    let g2Pos = (sunUV + sunToCenter * 0.85) * aspectVec;
    let d2 = length(uv * aspectVec - g2Pos);
    let ghost2 = exp(-d2 * 10.0) * vec3<f32>(0.85, 0.55, 0.25);

    // Ghost 3: Chromatic green/cyan aperture element (distance 1.35)
    let g3Pos = (sunUV + sunToCenter * 1.35) * aspectVec;
    let d3 = length(uv * aspectVec - g3Pos);
    let ghost3 = exp(-d3 * 16.0) * vec3<f32>(0.25, 0.85, 0.55);

    // Ghost 4: Diffuse outer chromatic reflection (distance 1.75)
    let g4Pos = (sunUV + sunToCenter * 1.75) * aspectVec;
    let d4 = length(uv * aspectVec - g4Pos);
    let ghost4 = exp(-d4 * 6.0) * vec3<f32>(0.45, 0.7, 1.0) * 0.5;

    // 4. Optical Ring / Halo centered around screen center
    let centerDist = length((uv - centerUV) * aspectVec);
    let haloDist = abs(centerDist - 0.38);
    let halo = exp(-haloDist * 28.0) * 0.15 * vec3<f32>(0.8, 0.45, 0.9);

    let ghosts = (ghost1 + ghost2 + ghost3 + ghost4 + halo) * 0.55;

    // Smooth border attenuation as the sun leaves the screen viewport
    let borderFade = smoothstep(-0.1, 0.1, sunUV.x) * smoothstep(1.1, 0.9, sunUV.x)
                   * smoothstep(-0.1, 0.1, sunUV.y) * smoothstep(1.1, 0.9, sunUV.y);

    return (flareColor + ghosts) * (visibility * intensity * borderFade);
}

// Screen-Space Ambient Occlusion (SSAO/GTAO complement to material occlusion)
fn evaluateSSAO(uv : vec2<f32>, radius : f32, intensity : f32) -> f32 {
    let centerCol = textureSample(hdrTexture, hdrSampler, uv).rgb;
    let centerLum = dot(centerCol, vec3<f32>(0.2126, 0.7152, 0.0722));

    var occlusion = 0.0;
    let r = max(radius, 0.002);
    let offsets = array<vec2<f32>, 8>(
        vec2<f32>( 0.707,  0.707) * r * 0.45,
        vec2<f32>(-0.707, -0.707) * r * 0.45,
        vec2<f32>(-0.707,  0.707) * r * 0.75,
        vec2<f32>( 0.707, -0.707) * r * 0.75,
        vec2<f32>( 1.0,    0.0  ) * r * 1.0,
        vec2<f32>(-1.0,    0.0  ) * r * 1.0,
        vec2<f32>( 0.0,    1.0  ) * r * 1.0,
        vec2<f32>( 0.0,   -1.0  ) * r * 1.0
    );

    for (var i = 0u; i < 8u; i = i + 1u) {
        let sampleUV = clamp(uv + offsets[i], vec2<f32>(0.001), vec2<f32>(0.999));
        let sampleCol = textureSample(hdrTexture, hdrSampler, sampleUV).rgb;
        let sampleLum = dot(sampleCol, vec3<f32>(0.2126, 0.7152, 0.0722));
        let diff = centerLum - sampleLum;
        if (diff > 0.02) {
            occlusion = occlusion + clamp(diff * 2.5, 0.0, 1.0);
        }
    }

    let ao = clamp(1.0 - (occlusion / 8.0) * intensity, 0.15, 1.0);
    return ao;
}

// HDR Bloom for emissive materials (golden-angle multi-ring kernel with luminance threshold)
fn evaluateBloom(uv : vec2<f32>, exposure : f32, threshold : f32, intensity : f32) -> vec3<f32> {
    var bloomAcc = vec3<f32>(0.0);
    var totalWeight = 0.0;

    let ringSteps = array<f32, 4>(0.0035, 0.0085, 0.0185, 0.0385);
    let ringWeights = array<f32, 4>(0.40, 0.28, 0.20, 0.12);

    for (var ring = 0u; ring < 4u; ring = ring + 1u) {
        let r = ringSteps[ring];
        let w = ringWeights[ring];
        for (var tap = 0u; tap < 3u; tap = tap + 1u) {
            let angle = f32(ring * 3u + tap) * 2.39996;
            let offset = vec2<f32>(cos(angle), sin(angle)) * r;
            let sampleUV = clamp(uv + offset, vec2<f32>(0.001), vec2<f32>(0.999));
            let sampleCol = textureSample(hdrTexture, hdrSampler, sampleUV).rgb;
            let exposedSample = sampleCol * exposure;
            let lum = dot(exposedSample, vec3<f32>(0.2126, 0.7152, 0.0722));
            if (lum > threshold) {
                let factor = (lum - threshold) / max(lum, 1e-4);
                let brightColor = exposedSample * factor;
                bloomAcc = bloomAcc + brightColor * w;
            }
            totalWeight = totalWeight + w;
        }
    }

    return (bloomAcc / max(totalWeight, 1e-4)) * intensity;
}

@fragment
fn fs_tonemap(in : VertexOutput) -> @location(0) vec4<f32> {
    let hdrSample = textureSample(hdrTexture, hdrSampler, in.uv);
    var exposed = hdrSample.rgb * ubo.exposure;

    // 1. Complementary Screen-Space Ambient Occlusion (SSAO)
    if (ubo.ssaoParams.w > 0.5) {
        let ssao = evaluateSSAO(in.uv, ubo.ssaoParams.y, ubo.ssaoParams.x);
        exposed = exposed * ssao;
    }

    // 2. Additive HDR Bloom for emissive and bright materials
    if (ubo.bloomParams.w > 0.5 && ubo.bloomParams.x > 0.001) {
        let bloom = evaluateBloom(in.uv, ubo.exposure, ubo.bloomParams.y, ubo.bloomParams.x);
        exposed = exposed + bloom;
    }

    // 3. Evaluate additive sun lens flare in linear HDR space prior to tonemapping
    if (ubo.lensFlareEnabled != 0u && ubo.sunScreenPos.z > 0.001) {
        let flare = evaluateLensFlare(
            in.uv,
            ubo.sunScreenPos.xy,
            ubo.sunScreenPos.z,
            ubo.sunScreenPos.w,
            ubo.flareParams.x
        );
        exposed = exposed + flare;
    }

    var mapped = exposed;
    if (ubo.operatorId == 0u) {
        mapped = tonemapKhronosPbrNeutral(exposed);
    } else if (ubo.operatorId == 1u) {
        mapped = tonemapAgX(exposed);
    } else {
        mapped = tonemapACES(exposed);
    }

    if (ubo.isSrgbSurface == 0u) {
        mapped = linearToSrgb(mapped);
    }
    return vec4<f32>(mapped, 1.0);
}
