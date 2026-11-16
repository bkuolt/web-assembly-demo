// Cinematic Post-Processing Pipeline:
// Depth of Field, Hi-Z Screen-Space Reflections, Depth-Gated Volumetric Sun God Rays,
// Multi-Mip Bloom, Half-Res GTAO, GPU Auto-Exposure, Tonemapping (PBR Neutral / AgX / ACES),
// Radial Chromatic Aberration, Filmic Color Grading Presets, Vignette, and Film Grain / Dither.

struct TonemapUniforms {
    exposure : f32,
    operatorId : u32,
    isSrgbSurface : u32,
    lensFlareEnabled : u32,
    sunScreenPos : vec4<f32>,
    flareParams : vec4<f32>,
    bloomParams : vec4<f32>,
    ssaoParams : vec4<f32>,
};

struct PostProcessExtraUniforms {
    proj : mat4x4<f32>,
    invProj : mat4x4<f32>,
    dofParams : vec4<f32>,     // x: enabled, y: focusDist, z: focusRange, w: maxBlurPx
    ssrParams : vec4<f32>,     // x: enabled, y: intensity, z: maxDist, w: thickness
    godRayParams : vec4<f32>,  // x: enabled, y: intensity, z: decay, w: density
    gradingParams : vec4<f32>, // x: presetId (0..4), y: vignetteIntensity, z: chromaAberr, w: grainAmount
    screenParams : vec4<f32>,  // x: width, y: height, z: timeSec, w: autoExposureEnabled
};

@group(0) @binding(0) var<uniform> ubo : TonemapUniforms;
@group(0) @binding(1) var srcSampler : sampler;
@group(0) @binding(2) var hdrTex : texture_2d<f32>;
@group(0) @binding(3) var bloomTex : texture_2d<f32>;
@group(0) @binding(4) var linearDepthTex : texture_2d<f32>;
@group(0) @binding(5) var gtaoTex : texture_2d<f32>;
@group(0) @binding(6) var<uniform> fxUbo : PostProcessExtraUniforms;
@group(0) @binding(7) var<storage, read> exposureBuf : vec4<f32>;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_tonemap(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

fn linearToSrgb(c : vec3<f32>) -> vec3<f32> {
    let clamped = clamp(c, vec3<f32>(0.0), vec3<f32>(1.0));
    let lo = clamped * 12.92;
    let hi = 1.055 * pow(clamped, vec3<f32>(1.0 / 2.4)) - 0.055;
    return select(hi, lo, clamped <= vec3<f32>(0.0031308));
}

fn tonemapKhronosPbrNeutral(color : vec3<f32>) -> vec3<f32> {
    let startCompression = 0.8 - 0.04;
    let desaturation = 0.15;
    let x = min(color.r, min(color.g, color.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    var col = color - offset;
    let peak = max(col.r, max(col.g, col.b));
    if (peak < startCompression) {
        return clamp(col, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    let d = 1.0 - startCompression;
    let newPeak = 1.0 - d * d / (peak + d - startCompression);
    col *= newPeak / peak;
    let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return clamp(mix(col, vec3<f32>(newPeak), g), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn tonemapAgX(color : vec3<f32>) -> vec3<f32> {
    let agxMat = mat3x3<f32>(
        vec3<f32>(0.84247906, 0.04232824, 0.04237565),
        vec3<f32>(0.07843360, 0.87846864, 0.07843360),
        vec3<f32>(0.07922375, 0.07916613, 0.87914297));
    var val = agxMat * max(color, vec3<f32>(1e-6));
    val = clamp((log2(val) - (-12.47393)) / (4.026069 - (-12.47393)), vec3<f32>(0.0), vec3<f32>(1.0));
    let v2 = val * val;
    let v4 = v2 * v2;
    let poly = 15.5 * v4 * v2 - 40.14 * v4 * val + 31.96 * v4 - 6.868 * v2 * val + 0.4298 * v2 + 0.1191 * val - 0.00232;
    let invMat = mat3x3<f32>(
        vec3<f32>(1.1968790, -0.0528969, -0.0529716),
        vec3<f32>(-0.0980209, 1.1519031, -0.0980435),
        vec3<f32>(-0.0990297, -0.0989612, 1.1510737));
    return clamp(invMat * poly, vec3<f32>(0.0), vec3<f32>(1.0));
}

fn tonemapACES(color : vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn interleavedNoise(px : vec2<f32>) -> f32 {
    return fract(52.9829189 * fract(dot(px, vec2<f32>(0.06711056, 0.00583715))));
}

fn loadLinearDepth(uv : vec2<f32>) -> f32 {
    let dims = vec2<i32>(textureDimensions(linearDepthTex));
    let coord = clamp(vec2<i32>(uv * vec2<f32>(dims)), vec2<i32>(0), dims - vec2<i32>(1));
    return textureLoad(linearDepthTex, coord, 0).r;
}

fn uvDepthToViewPos(uv : vec2<f32>, linearZ : f32) -> vec3<f32> {
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    let farV = fxUbo.invProj * ndc;
    let ray = farV.xyz / max(-farV.z, 1e-4);
    return ray * linearZ;
}

fn evaluateColorGrading(col : vec3<f32>, preset : u32) -> vec3<f32> {
    let luma = dot(col, vec3<f32>(0.2126, 0.7152, 0.0722));
    if (preset == 1u) {
        // Warm Cinema: rich golden highlights + warm shadow separation + gentle S-curve
        let shadows = mix(vec3<f32>(0.96, 0.94, 0.90), vec3<f32>(1.08, 1.01, 0.89), smoothstep(0.0, 0.85, luma));
        let graded = clamp(col * shadows, vec3<f32>(0.0), vec3<f32>(1.0));
        return mix(graded, smoothstep(vec3<f32>(0.0), vec3<f32>(1.0), graded), 0.22);
    }
    if (preset == 2u) {
        // Cool Sci-Fi: deep teal shadows + crisp silver-cyan highlights
        let tint = mix(vec3<f32>(0.86, 0.98, 1.08), vec3<f32>(0.96, 1.02, 1.10), smoothstep(0.2, 0.9, luma));
        return clamp(mix(vec3<f32>(luma), col, 0.90) * tint, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    if (preset == 3u) {
        // Bleach Bypass: silver-rich desaturated high contrast
        let contrast = smoothstep(vec3<f32>(0.03), vec3<f32>(0.97), col);
        return clamp(mix(vec3<f32>(luma), contrast, 0.48), vec3<f32>(0.0), vec3<f32>(1.0));
    }
    if (preset == 4u) {
        // Noir: classic crisp monochrome with cool silver shadow tint
        let mono = smoothstep(0.02, 0.96, luma);
        return clamp(vec3<f32>(mono) * mix(vec3<f32>(0.92, 0.95, 1.03), vec3<f32>(1.04, 1.02, 0.98), mono),
                     vec3<f32>(0.0), vec3<f32>(1.0));
    }
    return col;
}

@fragment
fn fs_tonemap(in : VsOut) -> @location(0) vec4<f32> {
    let uv = in.uv;
    let screenSize = max(fxUbo.screenParams.xy, vec2<f32>(1.0));
    let invScreen = 1.0 / screenSize;
    let centerZ = loadLinearDepth(uv);
    let isSky = centerZ > 500000.0;

    // 1. HDR Sampling with optional Edge Chromatic Aberration & Vogel Bokeh Depth of Field
    var hdrColor = textureSampleLevel(hdrTex, srcSampler, uv, 0.0).rgb;

    if (fxUbo.gradingParams.z > 1e-4) {
        let dirFromCenter = uv - vec2<f32>(0.5);
        let r2 = dot(dirFromCenter, dirFromCenter);
        let chromaShift = dirFromCenter * r2 * fxUbo.gradingParams.z * 0.022;
        hdrColor.r = textureSampleLevel(hdrTex, srcSampler, clamp(uv + chromaShift, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0).r;
        hdrColor.b = textureSampleLevel(hdrTex, srcSampler, clamp(uv - chromaShift, vec2<f32>(0.001), vec2<f32>(0.999)), 0.0).b;
    }

    if (fxUbo.dofParams.x > 0.5) {
        let effZ = select(centerZ, 80.0, isSky);
        let focusDist = max(fxUbo.dofParams.y, 0.2);
        let focusRange = max(fxUbo.dofParams.z, 0.25);
        let coc = clamp((abs(effZ - focusDist) - 0.35) / focusRange, 0.0, 1.0);
        if (coc > 0.04) {
            let maxPx = clamp(fxUbo.dofParams.w, 1.0, 12.0) * coc;
            let kGoldenAngle = 2.39996323;
            var dofAccum = hdrColor;
            var dofW = 1.0;
            for (var i = 1; i <= 12; i = i + 1) {
                let rad = sqrt((f32(i) + 0.5) / 12.5) * maxPx;
                let ang = f32(i) * kGoldenAngle;
                let tapUv = clamp(uv + vec2<f32>(cos(ang), sin(ang)) * rad * invScreen, vec2<f32>(0.001), vec2<f32>(0.999));
                let tapZ = select(loadLinearDepth(tapUv), 80.0, loadLinearDepth(tapUv) > 500000.0);
                let tapCoc = clamp((abs(tapZ - focusDist) - 0.35) / focusRange, 0.0, 1.0);
                let w = select(clamp(tapCoc, 0.15, 1.0), 1.0, tapZ >= effZ - 0.4);
                dofAccum += textureSampleLevel(hdrTex, srcSampler, tapUv, 0.0).rgb * w;
                dofW += w;
            }
            hdrColor = mix(hdrColor, dofAccum / dofW, coc * 0.92);
        }
    }

    // 2. GTAO Ambient Occlusion (half-res edge-blurred occlusion factor)
    if (ubo.ssaoParams.w > 0.5 && !isSky) {
        let gtao = textureSampleLevel(gtaoTex, srcSampler, uv, 0.0).r;
        let aoFactor = mix(1.0, gtao, clamp(ubo.ssaoParams.x, 0.0, 1.5));
        hdrColor *= clamp(aoFactor, 0.18, 1.0);
    }

    // 3. Hi-Z View-Space Screen-Space Reflections (SSR) on smooth upward / grazing surfaces
    if (fxUbo.ssrParams.x > 0.5 && !isSky) {
        let depthDims = vec2<i32>(textureDimensions(linearDepthTex));
        let pixCoord = clamp(vec2<i32>(uv * vec2<f32>(depthDims)), vec2<i32>(1), depthDims - vec2<i32>(2));
        let zL = textureLoad(linearDepthTex, pixCoord + vec2<i32>(-1, 0), 0).r;
        let zR = textureLoad(linearDepthTex, pixCoord + vec2<i32>(1, 0), 0).r;
        let zU = textureLoad(linearDepthTex, pixCoord + vec2<i32>(0, -1), 0).r;
        let zD = textureLoad(linearDepthTex, pixCoord + vec2<i32>(0, 1), 0).r;

        let cPos = uvDepthToViewPos(uv, centerZ);
        let pL = uvDepthToViewPos(uv + vec2<f32>(-invScreen.x, 0.0), zL);
        let pR = uvDepthToViewPos(uv + vec2<f32>(invScreen.x, 0.0), zR);
        let pU = uvDepthToViewPos(uv + vec2<f32>(0.0, -invScreen.y), zU);
        let pD = uvDepthToViewPos(uv + vec2<f32>(0.0, invScreen.y), zD);
        let ddx = select(pR - cPos, cPos - pL, abs(zL - centerZ) < abs(zR - centerZ));
        let ddy = select(pD - cPos, cPos - pU, abs(zU - centerZ) < abs(zD - centerZ));
        let vNorm = normalize(cross(ddy, ddx));
        let vDir = normalize(-cPos);

        let fresnel = pow(clamp(1.0 - max(dot(vNorm, vDir), 0.0), 0.0, 1.0), 3.2);
        if (fresnel > 0.06) {
            let reflDir = normalize(reflect(-vDir, vNorm));
            let maxDist = clamp(fxUbo.ssrParams.z, 2.0, 40.0);
            let thickness = clamp(fxUbo.ssrParams.w, 0.05, 1.5);
            let kSteps = 14;
            let stepLen = maxDist / f32(kSteps);
            var hitFound = false;
            var hitUv = vec2<f32>(0.0);

            for (var s = 1; s <= kSteps; s = s + 1) {
                let rayPos = cPos + reflDir * (f32(s) * stepLen);
                if (rayPos.z >= -0.05) {
                    break;
                }
                let clip = fxUbo.proj * vec4<f32>(rayPos, 1.0);
                let ndc = clip.xyz / max(clip.w, 1e-5);
                let rUv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                if (rUv.x < 0.0 || rUv.x > 1.0 || rUv.y < 0.0 || rUv.y > 1.0) {
                    break;
                }
                let sceneZ = loadLinearDepth(rUv);
                let rayZ = -rayPos.z;
                if (rayZ > sceneZ + 0.02 && rayZ < sceneZ + thickness * (1.0 + sceneZ * 0.08)) {
                    // 3-step binary refinement around hit span
                    var aPos = cPos + reflDir * (f32(s - 1) * stepLen);
                    var bPos = rayPos;
                    for (var b = 0; b < 3; b = b + 1) {
                        let mid = (aPos + bPos) * 0.5;
                        let mc = fxUbo.proj * vec4<f32>(mid, 1.0);
                        let mNdc = mc.xyz / max(mc.w, 1e-5);
                        let mUv = vec2<f32>(mNdc.x * 0.5 + 0.5, 0.5 - mNdc.y * 0.5);
                        let mz = loadLinearDepth(clamp(mUv, vec2<f32>(0.001), vec2<f32>(0.999)));
                        if (-mid.z > mz) {
                            bPos = mid;
                            hitUv = mUv;
                        } else {
                            aPos = mid;
                        }
                    }
                    hitFound = true;
                    break;
                }
            }

            if (hitFound) {
                let edgeFade = smoothstep(0.0, 0.12, hitUv.x) * smoothstep(1.0, 0.88, hitUv.x) *
                               smoothstep(0.0, 0.12, hitUv.y) * smoothstep(1.0, 0.88, hitUv.y);
                let reflSample = min(textureSampleLevel(hdrTex, srcSampler, hitUv, 0.0).rgb, vec3<f32>(6.0));
                hdrColor += reflSample * fresnel * edgeFade * clamp(fxUbo.ssrParams.y, 0.0, 2.0);
            }
        }
    }

    // 4. Multi-Mip Physically Based Bloom
    if (ubo.bloomParams.w > 0.5) {
        let bloomColor = textureSampleLevel(bloomTex, srcSampler, uv, 0.0).rgb;
        hdrColor += bloomColor * clamp(ubo.bloomParams.x, 0.0, 4.0);
    }

    // 5. Depth-Gated Volumetric Sun God Rays + Lens Flares
    let sunVis = ubo.sunScreenPos.z;
    if (fxUbo.godRayParams.x > 0.5 && sunVis > 0.01) {
        let sunUv = ubo.sunScreenPos.xy;
        let kRaySamples = 16;
        let density = clamp(fxUbo.godRayParams.w, 0.1, 1.5);
        let decay = clamp(fxUbo.godRayParams.z, 0.80, 0.99);
        var deltaUv = (uv - sunUv) * (density / f32(kRaySamples));
        var sampleUv = uv - deltaUv * interleavedNoise(in.pos.xy);
        var illumDecay = 1.0;
        var rays = vec3<f32>(0.0);

        for (var i = 0; i < kRaySamples; i = i + 1) {
            sampleUv -= deltaUv;
            if (sampleUv.x >= 0.0 && sampleUv.x <= 1.0 && sampleUv.y >= 0.0 && sampleUv.y <= 1.0) {
                let tapDepth = loadLinearDepth(sampleUv);
                let skyMask = select(0.0, 1.0, tapDepth > 500000.0);
                let skySample = min(textureSampleLevel(hdrTex, srcSampler, sampleUv, 0.0).rgb, vec3<f32>(3.0));
                rays += skySample * skyMask * illumDecay;
            }
            illumDecay *= decay;
        }
        let radialFalloff = clamp(1.1 - length(uv - sunUv) * 0.85, 0.0, 1.0);
        hdrColor += (rays / f32(kRaySamples)) * sunVis * radialFalloff * clamp(fxUbo.godRayParams.y, 0.0, 3.0);
    }

    if (ubo.lensFlareEnabled == 1u && sunVis > 0.01) {
        let sunUV = ubo.sunScreenPos.xy;
        let aspect = ubo.sunScreenPos.w;
        let delta = sunUV - uv;
        let aspectDelta = vec2<f32>(delta.x * aspect, delta.y);
        let distSun = length(aspectDelta);
        let starburst = exp(-distSun * 11.0) * 0.65;
        let dirFromCenter = vec2<f32>(0.5) - sunUV;
        var ghosts = vec3<f32>(0.0);
        for (var i = 1; i <= 4; i = i + 1) {
            let fi = f32(i);
            let ghostPos = sunUV + dirFromCenter * (fi * 0.45);
            let gDelta = vec2<f32>((ghostPos.x - uv.x) * aspect, ghostPos.y - uv.y);
            let gDist = length(gDelta);
            let gMask = smoothstep(0.14 / (fi * 0.45 + 0.5), 0.0, gDist);
            let chromaCol = vec3<f32>(0.9 - fi * 0.12, 0.65 + fi * 0.06, 0.45 + fi * 0.14);
            ghosts += chromaCol * pow(gMask, 3.0) * (0.3 / fi);
        }
        hdrColor += (vec3<f32>(1.0, 0.86, 0.68) * starburst + ghosts) * sunVis * ubo.flareParams.x;
    }

    // 6. Active Exposure (manual or smoothed GPU Auto-Exposure) & Tone Mapping
    let activeExposure = select(ubo.exposure, exposureBuf.x, fxUbo.screenParams.w > 0.5);
    let exposed = max(hdrColor * activeExposure, vec3<f32>(0.0));

    var mapped = vec3<f32>(0.0);
    if (ubo.operatorId == 0u) {
        mapped = tonemapKhronosPbrNeutral(exposed);
    } else if (ubo.operatorId == 1u) {
        mapped = tonemapAgX(exposed);
    } else {
        mapped = tonemapACES(exposed);
    }

    // 7. Color Grading Presets, Vignette & Anti-Banding Film Grain
    let preset = u32(max(fxUbo.gradingParams.x + 0.5, 0.0));
    mapped = evaluateColorGrading(mapped, preset);

    if (fxUbo.gradingParams.y > 1e-4) {
        let centered = uv - vec2<f32>(0.5);
        let vign = 1.0 - smoothstep(0.28, 0.85, length(centered)) * clamp(fxUbo.gradingParams.y, 0.0, 1.0) * 0.72;
        mapped *= vign;
    }

    // Animated triangular-PDF film grain + 8-bit dither
    let noiseT = fract(fxUbo.screenParams.z * 11.73);
    let n1 = interleavedNoise(in.pos.xy + vec2<f32>(noiseT * 137.0, noiseT * 293.0));
    let n2 = interleavedNoise(in.pos.xy + vec2<f32>(noiseT * 71.0, noiseT * 419.0));
    let triNoise = (n1 + n2) - 1.0;
    let grainScale = (1.0 / 255.0) * 0.85 + fxUbo.gradingParams.w * 0.032;
    mapped = clamp(mapped + vec3<f32>(triNoise * grainScale), vec3<f32>(0.0), vec3<f32>(1.0));

    let outColor = select(linearToSrgb(mapped), mapped, ubo.isSrgbSurface == 1u);
    return vec4<f32>(outColor, 1.0);
}
