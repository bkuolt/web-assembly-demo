/**
 * @file js/shaders.js
 * @brief WGSL Cook-Torrance PBR, Mipmap, POM, Weather, and LOD shader source for BglWebGpuRenderer.
 */
(() => {
  'use strict';

  const pbrWgsl = `
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    normalMatrix : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    baseColorFactor : vec4<f32>,
    pbrFactors : vec4<f32>,      // x=metallic, y=roughness, z=normalScale, w=occlusionStrength
    emissiveFactor : vec4<f32>,  // xyz=emissive, w=alphaCutoff
    uvTransform : vec4<f32>,     // baseColor: xy=scale, zw=offset
    flags : vec4<u32>,           // x=texFlags, y=wireframe, z=doubleSided, w=alphaMode
    params : vec4<f32>,          // x=bumpScale, y=pomScale, z=isPomActive, w=isBumpActive
    weather : vec4<f32>,         // x=weatherMode (0=none, 1=rain, 2=snow), y=intensity, z=wind, w=timeSec
    lodParams : vec4<f32>,       // x=lodLevel, y=lodTransition, z=debugViewMode, w=lodBias
    mrUvTransform : vec4<f32>,   // metallicRoughness: xy=scale, zw=offset
    normalUvTransform : vec4<f32>,    // normal: xy=scale, zw=offset
    occlusionUvTransform : vec4<f32>, // occlusion: xy=scale, zw=offset
    emissiveUvTransform : vec4<f32>,  // emissive: xy=scale, zw=offset
    uvRotations0 : vec4<f32>,    // x=baseColorRot, y=mrRot, z=normalRot, w=occlusionRot
    uvRotations1 : vec4<f32>,    // x=emissiveRot, yzw=reserved
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var defaultSampler : sampler;
@group(0) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(3) var metallicRoughnessTexture : texture_2d<f32>;
@group(0) @binding(4) var normalTexture : texture_2d<f32>;
@group(0) @binding(5) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(6) var occlusionTexture : texture_2d<f32>;

@group(1) @binding(0) var<storage, read> jointMatrices : array<mat4x4<f32>>;

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
};

struct SkinnedVertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
    @location(4) joints : vec4<u32>,
    @location(5) weights : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) worldPos : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
};

struct MipVertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_mip(@builtin(vertex_index) vIdx : u32) -> MipVertexOutput {
    var out : MipVertexOutput;
    let x = f32(i32(vIdx << 1u) & 2) * 2.0 - 1.0;
    let y = f32(i32(vIdx & 2u)) * 2.0 - 1.0;
    out.clipPos = vec4<f32>(x, y, 0.0, 1.0);
    out.uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
    return out;
}

@fragment
fn fs_mip(in : MipVertexOutput) -> @location(0) vec4<f32> {
    return textureSample(baseColorTexture, defaultSampler, in.uv);
}

const PI : f32 = 3.141592653589793;

fn distributionGGX(NdotH : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

fn visibilitySmithGGX(NdotL : f32, NdotV : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let ggxL = NdotL + sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
    let ggxV = NdotV + sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
    return 1.0 / max(ggxL * ggxV, 0.0001);
}

fn fresnelSchlick(VdotH : f32, F0 : vec3<f32>) -> vec3<f32> {
    return F0 + (vec3<f32>(1.0) - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(NdotV : f32, F0 : vec3<f32>, roughness : f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
}

fn safeNormalize(v : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let lenSq = dot(v, v);
    if (lenSq > 1e-12) {
        return v * inverseSqrt(lenSq);
    }
    return fallback;
}

fn getTBN(worldPos : vec3<f32>, geomNormal : vec3<f32>, uv : vec2<f32>, tangent : vec4<f32>, dp1 : vec3<f32>, dp2 : vec3<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> mat3x3<f32> {
    let N = safeNormalize(geomNormal, vec3<f32>(0.0, 1.0, 0.0));
    let orthoT = tangent.xyz - N * dot(N, tangent.xyz);
    let orthoLenSq = dot(orthoT, orthoT);
    if (abs(tangent.w) > 0.001 && orthoLenSq > 1e-6) {
        let T = orthoT * inverseSqrt(orthoLenSq);
        let B = safeNormalize(cross(N, T) * sign(tangent.w), vec3<f32>(0.0, 0.0, 1.0));
        return mat3x3<f32>(T, B, N);
    }
    let dp2perp = cross(dp2, N);
    let dp1perp = cross(N, dp1);
    let T = dp2perp * duv1.x + dp1perp * duv2.x;
    let B = dp2perp * duv1.y + dp1perp * duv2.y;
    let maxLenSq = max(dot(T, T), dot(B, B));
    if (maxLenSq > 1e-18) {
        let invmax = inverseSqrt(maxLenSq);
        return mat3x3<f32>(safeNormalize(T * invmax, vec3<f32>(1.0, 0.0, 0.0)), safeNormalize(B * invmax, vec3<f32>(0.0, 0.0, 1.0)), N);
    }
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(N.y) < 0.999);
    let fallbackT = safeNormalize(cross(up, N), vec3<f32>(1.0, 0.0, 0.0));
    let fallbackB = safeNormalize(cross(N, fallbackT), vec3<f32>(0.0, 0.0, 1.0));
    return mat3x3<f32>(fallbackT, fallbackB, N);
}

fn transformUv(uv : vec2<f32>, scaleOffset : vec4<f32>, rot : f32) -> vec2<f32> {
    let su = uv * scaleOffset.xy;
    if (abs(rot) < 1e-6) {
        return su + scaleOffset.zw;
    }
    let s = sin(rot);
    let c = cos(rot);
    return vec2<f32>(c * su.x + s * su.y, -s * su.x + c * su.y) + scaleOffset.zw;
}

fn transformUvDeriv(d : vec2<f32>, scaleOffset : vec4<f32>, rot : f32) -> vec2<f32> {
    let sd = d * scaleOffset.xy;
    if (abs(rot) < 1e-6) {
        return sd;
    }
    let s = sin(rot);
    let c = cos(rot);
    return vec2<f32>(c * sd.x + s * sd.y, -s * sd.x + c * sd.y);
}

fn getSurfaceHeight(uv : vec2<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> f32 {
    var height = 0.5;
    if ((ubo.flags.x & 16u) != 0u) {
        let occUv = transformUv(uv, ubo.occlusionUvTransform, ubo.uvRotations0.w);
        let occDuv1 = transformUvDeriv(duv1, ubo.occlusionUvTransform, ubo.uvRotations0.w);
        let occDuv2 = transformUvDeriv(duv2, ubo.occlusionUvTransform, ubo.uvRotations0.w);
        height = textureSampleGrad(occlusionTexture, defaultSampler, occUv, occDuv1, occDuv2).r;
    } else if ((ubo.flags.x & 1u) != 0u) {
        let baseUv = transformUv(uv, ubo.uvTransform, ubo.uvRotations0.x);
        let baseDuv1 = transformUvDeriv(duv1, ubo.uvTransform, ubo.uvRotations0.x);
        let baseDuv2 = transformUvDeriv(duv2, ubo.uvTransform, ubo.uvRotations0.x);
        let col = textureSampleGrad(baseColorTexture, defaultSampler, baseUv, baseDuv1, baseDuv2).rgb;
        height = dot(col, vec3<f32>(0.299, 0.587, 0.114));
    }
    return clamp(height, 0.0, 1.0);
}

fn parallaxOcclusionMapping(
    initialUV : vec2<f32>,
    viewDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> vec2<f32> {
    let minLayers : f32 = 8.0;
    let maxLayers : f32 = 32.0;
    let numLayers = mix(maxLayers, minLayers, clamp(abs(viewDirTangent.z), 0.0, 1.0));
    let layerDepth = 1.0 / numLayers;
    var currentLayerDepth : f32 = 0.0;

    let P = (viewDirTangent.xy / max(abs(viewDirTangent.z), 0.15)) * scale;
    let deltaUV = P / numLayers;

    var currentUV = initialUV;
    var currentHeight = getSurfaceHeight(currentUV, duv1, duv2);

    var prevUV = currentUV;
    var prevHeight = currentHeight;
    var prevLayerDepth = currentLayerDepth;

    for (var i : i32 = 0; i < 32; i = i + 1) {
        if (f32(i) >= numLayers || currentLayerDepth >= (1.0 - currentHeight)) {
            break;
        }
        prevUV = currentUV;
        prevHeight = currentHeight;
        prevLayerDepth = currentLayerDepth;

        currentUV = currentUV - deltaUV;
        currentHeight = getSurfaceHeight(currentUV, duv1, duv2);
        currentLayerDepth = currentLayerDepth + layerDepth;
    }

    let afterDepth = currentLayerDepth - (1.0 - currentHeight);
    let beforeDepth = (1.0 - prevHeight) - prevLayerDepth;
    let weight = afterDepth / max(afterDepth + beforeDepth, 0.0001);
    return mix(currentUV, prevUV, clamp(weight, 0.0, 1.0));
}

fn computeParallaxShadow(
    displacedUV : vec2<f32>,
    lightDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> f32 {
    let initialHeight = getSurfaceHeight(displacedUV, duv1, duv2);
    if (lightDirTangent.z <= 0.0) {
        return 0.25;
    }
    let numSamples : f32 = 8.0;
    let stepSize = (1.0 - initialHeight) / numSamples;
    let deltaUV = (lightDirTangent.xy / max(lightDirTangent.z, 0.15)) * scale / numSamples;

    var currentLayerHeight = initialHeight;
    var currentUV = displacedUV;
    var shadowFactor = 1.0;

    for (var i : i32 = 0; i < 8; i = i + 1) {
        currentUV = currentUV + deltaUV;
        currentLayerHeight = currentLayerHeight + stepSize;
        let sampledHeight = getSurfaceHeight(currentUV, duv1, duv2);
        if (sampledHeight > currentLayerHeight) {
            let diff = sampledHeight - currentLayerHeight;
            shadowFactor = min(shadowFactor, 1.0 - diff * 0.85);
        }
    }
    return clamp(shadowFactor, 0.25, 1.0);
}

fn srgbToLinear(c : vec3<f32>) -> vec3<f32> {
    return pow(c, vec3<f32>(2.2));
}

fn linearToSrgb(c : vec3<f32>) -> vec3<f32> {
    return pow(c, vec3<f32>(1.0 / 2.2));
}

fn tonemapKhronosPbrNeutral(color : vec3<f32>) -> vec3<f32> {
    let startCompression = 0.8 - 0.04;
    let desaturation = 0.15;
    let x = min(color.r, min(color.g, color.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    var c = color - offset;
    let peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) {
        return c;
    }
    let d = 1.0 - startCompression;
    let newPeak = 1.0 - d * d / (peak + d - startCompression);
    c = c * (newPeak / peak);
    let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, vec3<f32>(newPeak), g);
}

// ============================================================================
// Procedural Weather Effects (Atmospheric Rain & Snow WGSL Subroutines)
// ============================================================================

fn hash31(p : vec3<f32>) -> f32 {
    let h = fract(sin(dot(p, vec3<f32>(12.9898, 78.233, 45.164))) * 43758.5453123);
    return h;
}

// Procedural dynamic raindrops ripples on surface
fn calculateRainRipples(pos : vec2<f32>, time : f32) -> vec3<f32> {
    let scale = 14.0;
    let uvCoord = pos * scale;
    let iuv = floor(uvCoord);
    let fuv = fract(uvCoord);
    var rippleVec = vec2<f32>(0.0);
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let g = vec2<f32>(f32(x), f32(y));
            let h = fract(sin(dot(iuv + g, vec2<f32>(127.1, 311.7))) * 43758.5453);
            let t = fract(time * 2.2 + h);
            let center = g + vec2<f32>(0.5) + vec2<f32>(sin(h * 6.28), cos(h * 6.28)) * 0.25;
            let d = length(fuv - center);
            let wave = sin(d * 24.0 - t * 14.0) * exp(-d * 4.5) * (1.0 - t);
            let deltaUv = fuv - center;
            let dir = select(vec2<f32>(1.0, 0.0), deltaUv * inverseSqrt(dot(deltaUv, deltaUv)), dot(deltaUv, deltaUv) > 1e-8);
            rippleVec = rippleVec + dir * wave * 0.5;
        }
    }
    return safeNormalize(vec3<f32>(rippleVec, 1.0), vec3<f32>(0.0, 0.0, 1.0));
}

// Volumetric atmospheric rain streaks
fn calculateRainStreaks(worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let rayDist = length(worldPos - cameraPos);
    var streakAccum : f32 = 0.0;
    for (var i = 0; i < 2; i = i + 1) {
        let scale = select(4.0, 8.0, i == 1);
        let speed = select(22.0, 32.0, i == 1);
        let windVec = vec3<f32>(wind * 3.5, -speed, wind * 1.5);
        let samplePos = (worldPos - windVec * time) * scale;
        let cell = floor(vec3<f32>(samplePos.x, samplePos.y * 0.08, samplePos.z));
        let f = fract(vec3<f32>(samplePos.x, samplePos.y * 0.08, samplePos.z));
        let h = hash31(cell);
        let dX = abs(f.x - 0.5);
        let dZ = abs(f.z - 0.5);
        let dY = abs(f.y - 0.5);
        let streak = smoothstep(0.08, 0.0, dX) * smoothstep(0.08, 0.0, dZ) * smoothstep(0.45, 0.0, dY);
        let activeDrop = step(0.70, h);
        streakAccum = streakAccum + streak * activeDrop;
    }
    let mist = (1.0 - exp(-rayDist * 0.08)) * 0.15 * intensity;
    let mistColor = vec3<f32>(0.65, 0.72, 0.82) * mist;
    let streakColor = vec3<f32>(0.85, 0.92, 1.0) * (streakAccum * intensity * 0.75);
    return streakColor + mistColor;
}

// Volumetric atmospheric drifting snow flakes
fn calculateSnowFlakes(worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let rayDist = length(worldPos - cameraPos);
    var flakeAccum : f32 = 0.0;
    for (var i = 0; i < 2; i = i + 1) {
        let scale = select(3.5, 6.5, i == 1);
        let fallSpeed = select(2.4, 3.8, i == 1);
        let freq = select(2.0, 3.2, i == 1);
        let wobbleX = sin(time * freq + worldPos.y * 2.5 + f32(i)) * 0.35;
        let wobbleZ = cos(time * (freq * 0.85) + worldPos.x * 2.0) * 0.3;
        let driftVec = vec3<f32>(wind * 1.8 * time + wobbleX, -fallSpeed * time, wind * 0.8 * time + wobbleZ);
        let samplePos = (worldPos - driftVec) * scale;
        let cell = floor(samplePos);
        let f = fract(samplePos) - 0.5;
        let h = hash31(cell + vec3<f32>(17.3, 41.7, 83.1));
        let dist = length(f);
        let flake = smoothstep(0.20, 0.02, dist) * step(0.66, h);
        flakeAccum = flakeAccum + flake;
    }
    let winterFog = (1.0 - exp(-rayDist * 0.06)) * 0.18 * intensity;
    let winterFogColor = vec3<f32>(0.82, 0.88, 0.96) * winterFog;
    let flakeColor = vec3<f32>(0.96, 0.98, 1.0) * (flakeAccum * intensity * 0.85);
    return flakeColor + winterFogColor;
}

fn noise3D(p : vec3<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let n000 = hash31(i + vec3<f32>(0.0, 0.0, 0.0));
    let n100 = hash31(i + vec3<f32>(1.0, 0.0, 0.0));
    let n010 = hash31(i + vec3<f32>(0.0, 1.0, 0.0));
    let n110 = hash31(i + vec3<f32>(1.0, 1.0, 0.0));
    let n001 = hash31(i + vec3<f32>(0.0, 0.0, 1.0));
    let n101 = hash31(i + vec3<f32>(1.0, 0.0, 1.0));
    let n011 = hash31(i + vec3<f32>(0.0, 1.0, 1.0));
    let n111 = hash31(i + vec3<f32>(1.0, 1.0, 1.0));
    let nx00 = mix(n000, n100, u.x);
    let nx10 = mix(n010, n110, u.x);
    let nx01 = mix(n001, n101, u.x);
    let nx11 = mix(n011, n111, u.x);
    let nxy0 = mix(nx00, nx10, u.y);
    let nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

fn fbm3D(p : vec3<f32>) -> f32 {
    var val = 0.0;
    var amp = 0.5;
    var pos = p;
    for (var i = 0; i < 4; i = i + 1) {
        val = val + amp * noise3D(pos);
        pos = pos * 2.03 + vec3<f32>(13.7, 29.3, 7.1);
        amp = amp * 0.5;
    }
    return val;
}

fn evaluateDiurnalSunRadiance(solarElev : f32) -> vec3<f32> {
    let noonSun = vec3<f32>(1.0, 0.97, 0.92) * 2.6;
    let goldenSun = vec3<f32>(1.0, 0.48, 0.18) * 1.9;
    let nightMoon = vec3<f32>(0.28, 0.44, 0.78) * 0.48;
    if (solarElev >= 0.35) {
        let t = clamp((solarElev - 0.35) / 0.65, 0.0, 1.0);
        return mix(goldenSun, noonSun, t);
    } else if (solarElev >= -0.10) {
        let t = clamp((solarElev + 0.10) / 0.45, 0.0, 1.0);
        return mix(nightMoon, goldenSun, t);
    }
    return nightMoon;
}

fn evaluateDiurnalAmbientScale(solarElev : f32) -> vec3<f32> {
    let dayScale = vec3<f32>(1.0, 1.0, 1.0);
    let duskScale = vec3<f32>(0.82, 0.60, 0.52);
    let nightScale = vec3<f32>(0.16, 0.22, 0.38);
    if (solarElev >= 0.25) {
        let t = clamp((solarElev - 0.25) / 0.75, 0.0, 1.0);
        return mix(duskScale, dayScale, t);
    } else if (solarElev >= -0.15) {
        let t = clamp((solarElev + 0.15) / 0.40, 0.0, 1.0);
        return mix(nightScale, duskScale, t);
    }
    return nightScale;
}

fn calculateFogEffect(sceneColor : vec3<f32>, worldPos : vec3<f32>, cameraPos : vec3<f32>, sunDir : vec3<f32>, solarElev : f32, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let toFrag = worldPos - cameraPos;
    let dist = length(toFrag);
    let viewRay = toFrag / max(dist, 1e-4);

    let windOffset = vec3<f32>(wind * time * 0.6, -time * 0.08, wind * time * 0.3);
    let mistNoise = fbm3D(worldPos * 0.28 - windOffset);
    let heightFalloff = exp(-max(worldPos.y + 2.0, 0.0) * 0.12);
    let density = (0.045 + 0.055 * heightFalloff * (0.65 + 0.7 * mistNoise)) * intensity;
    let fogFactor = clamp(1.0 - exp(-dist * density), 0.0, 0.96);

    let cosTheta = max(dot(viewRay, sunDir), 0.0);
    let mieScatter = pow(cosTheta, 6.0) * 0.45;

    let dayFogColor = vec3<f32>(0.74, 0.81, 0.89);
    let duskFogColor = vec3<f32>(0.82, 0.58, 0.46);
    let nightFogColor = vec3<f32>(0.12, 0.17, 0.28);
    var baseFogColor = dayFogColor;
    if (solarElev < 0.25 && solarElev >= -0.15) {
        baseFogColor = mix(nightFogColor, duskFogColor, clamp((solarElev + 0.15) / 0.40, 0.0, 1.0));
    } else if (solarElev < -0.15) {
        baseFogColor = nightFogColor;
    } else {
        baseFogColor = mix(duskFogColor, dayFogColor, clamp((solarElev - 0.25) / 0.75, 0.0, 1.0));
    }

    let scatterTint = select(vec3<f32>(0.35, 0.50, 0.80), vec3<f32>(1.0, 0.82, 0.55), solarElev >= 0.0);
    let finalFogColor = baseFogColor + scatterTint * mieScatter;
    return mix(sceneColor, finalFogColor, fogFactor);
}

fn calculateSmokePlumes(sceneColor : vec3<f32>, worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let dist = length(worldPos - cameraPos);
    let flow = vec3<f32>(wind * time * 1.1, -time * 1.35, wind * time * 0.45);
    let p1 = worldPos * 0.42 + flow;
    let warp = vec3<f32>(
        fbm3D(p1 + vec3<f32>(5.2, 1.3, 2.8)),
        fbm3D(p1 + vec3<f32>(1.7, 9.2, 4.1)),
        fbm3D(p1 + vec3<f32>(8.3, 2.8, 6.4))
    ) * 1.35;
    let rawTurbulence = fbm3D(p1 + warp);

    let heightEnvelope = exp(-max(worldPos.y + 3.0, 0.0) * 0.08);
    let distEnvelope = 1.0 - exp(-dist * 0.085);
    let plumeShape = smoothstep(0.32, 0.78, rawTurbulence);
    let smokeOpacity = clamp((plumeShape * 0.75 + distEnvelope * 0.35) * heightEnvelope * intensity, 0.0, 0.94);

    let sootColor = vec3<f32>(0.15, 0.16, 0.18);
    let ashHighlight = vec3<f32>(0.34, 0.35, 0.38);
    let emberGlow = vec3<f32>(0.95, 0.38, 0.08) * exp(-max(worldPos.y + 1.5, 0.0) * 0.45) * pow(plumeShape, 2.5) * 0.55 * clamp(intensity, 0.0, 1.5);
    let smokeColor = mix(sootColor, ashHighlight, rawTurbulence) + emberGlow;

    return mix(sceneColor, smokeColor, smokeOpacity);
}

fn bayerDither4x4(fragCoord : vec2<f32>) -> f32 {
    let x = u32(fragCoord.x) & 3u;
    let y = u32(fragCoord.y) & 3u;
    let idx = y * 4u + x;
    let bayer = array<f32, 16>(
         0.0 / 16.0,  8.0 / 16.0,  2.0 / 16.0, 10.0 / 16.0,
        12.0 / 16.0,  4.0 / 16.0, 14.0 / 16.0,  6.0 / 16.0,
         3.0 / 16.0, 11.0 / 16.0,  1.0 / 16.0,  9.0 / 16.0,
        15.0 / 16.0,  7.0 / 16.0, 13.0 / 16.0,  5.0 / 16.0
    );
    return bayer[idx];
}

fn getLodHeatmapColor(lod : u32) -> vec3<f32> {
    if (lod == 0u) {
        return vec3<f32>(0.15, 0.90, 0.35);
    } else if (lod == 1u) {
        return vec3<f32>(0.95, 0.85, 0.15);
    } else if (lod == 2u) {
        return vec3<f32>(0.98, 0.48, 0.12);
    }
    return vec3<f32>(0.95, 0.18, 0.22);
}

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    let worldPos4 = ubo.model * vec4<f32>(in.position, 1.0);
    out.worldPos = worldPos4.xyz;
    let n = (ubo.normalMatrix * vec4<f32>(in.normal, 0.0)).xyz;
    if (dot(n, n) > 0.0001) {
        out.normal = normalize(n);
    } else {
        out.normal = vec3<f32>(0.0, 1.0, 0.0);
    }
    let worldTan = (ubo.model * vec4<f32>(in.tangent.xyz, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    out.tangent = vec4<f32>(normTan, in.tangent.w);
    out.uv = in.uv;
    out.clipPos = ubo.viewProj * worldPos4;
    return out;
}

@vertex
fn vs_main_skinned(in : SkinnedVertexInput) -> VertexOutput {
    var out : VertexOutput;
    var localPos = in.position;
    var localNorm = in.normal;
    var localTan = in.tangent.xyz;

    let weightSum = in.weights.x + in.weights.y + in.weights.z + in.weights.w;
    if (weightSum > 0.0001) {
        let numJoints = arrayLength(&jointMatrices);
        let i0 = min(in.joints.x, numJoints - 1u);
        let i1 = min(in.joints.y, numJoints - 1u);
        let i2 = min(in.joints.z, numJoints - 1u);
        let i3 = min(in.joints.w, numJoints - 1u);
        let skinMat = in.weights.x * jointMatrices[i0] +
                      in.weights.y * jointMatrices[i1] +
                      in.weights.z * jointMatrices[i2] +
                      in.weights.w * jointMatrices[i3];
        localPos = (skinMat * vec4<f32>(in.position, 1.0)).xyz;
        localNorm = (skinMat * vec4<f32>(in.normal, 0.0)).xyz;
        localTan = (skinMat * vec4<f32>(in.tangent.xyz, 0.0)).xyz;
    }

    let worldPos4 = ubo.model * vec4<f32>(localPos, 1.0);
    out.worldPos = worldPos4.xyz;
    let n = (ubo.normalMatrix * vec4<f32>(localNorm, 0.0)).xyz;
    if (dot(n, n) > 0.0001) {
        out.normal = normalize(n);
    } else {
        out.normal = vec3<f32>(0.0, 1.0, 0.0);
    }
    let worldTan = (ubo.model * vec4<f32>(localTan, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    out.tangent = vec4<f32>(normTan, in.tangent.w);
    out.uv = in.uv;
    out.clipPos = ubo.viewProj * worldPos4;
    return out;
}

@fragment
fn fs_main(in : VertexOutput, @builtin(front_facing) frontFacing : bool) -> @location(0) vec4<f32> {
    let dp1 = dpdx(in.worldPos);
    let dp2 = dpdy(in.worldPos);
    let rawDuv1 = dpdx(in.uv);
    let rawDuv2 = dpdy(in.uv);
    let normDuv1 = transformUvDeriv(rawDuv1, ubo.normalUvTransform, ubo.uvRotations0.z);
    let normDuv2 = transformUvDeriv(rawDuv2, ubo.normalUvTransform, ubo.uvRotations0.z);

    // Wireframe debug override
    if (ubo.flags.y != 0u) {
        return vec4<f32>(0.22, 0.74, 0.97, 1.0);
    }

    // 1. Initial Geometry Normal & Orthonormal TBN Basis
    var geomNormal = safeNormalize(in.normal, vec3<f32>(0.0, 1.0, 0.0));
    let V = safeNormalize(ubo.cameraPos.xyz - in.worldPos, vec3<f32>(0.0, 0.0, 1.0));
    if (!frontFacing || (ubo.flags.z != 0u && dot(geomNormal, V) < 0.0)) {
        geomNormal = -geomNormal;
    }

    let TBN = getTBN(in.worldPos, geomNormal, in.uv, in.tangent, dp1, dp2, rawDuv1, rawDuv2);
    let invTBN = transpose(TBN);
    let V_tangent = invTBN * V;

    // 2. Parallax Occlusion Mapping (POM) & Surface Displacement
    var baseRawUv = in.uv;
    var pomShadow = 1.0;
    let bumpScale = ubo.params.x;
    let pomScale = ubo.params.y;
    let activeLodLevel = u32(clamp(ubo.lodParams.x, 0.0, 3.0));
    let lodTransition = clamp(ubo.lodParams.y, 0.0, 1.0);
    let debugViewMode = u32(clamp(ubo.lodParams.z, 0.0, 4.0));
    let isPomActive = (ubo.params.z > 0.5) && (activeLodLevel == 0u);
    let isBumpActive = (ubo.params.w > 0.5) && (activeLodLevel < 2u);

    if (isPomActive && isBumpActive) {
        let isNormalMapped = (ubo.flags.x & 4u) != 0u;
        let effPomScale = select(pomScale, pomScale * 0.14, isNormalMapped);
        baseRawUv = parallaxOcclusionMapping(in.uv, V_tangent, rawDuv1, rawDuv2, effPomScale);
        let L0_tangent = invTBN * safeNormalize(ubo.lightDir.xyz, vec3<f32>(0.0, 1.0, 0.0));
        pomShadow = computeParallaxShadow(baseRawUv, L0_tangent, rawDuv1, rawDuv2, effPomScale);
    }

    // Apply independent per-channel KHR_texture_transform coordinates & derivatives
    let baseUv = transformUv(baseRawUv, ubo.uvTransform, ubo.uvRotations0.x);
    let mrUv = transformUv(baseRawUv, ubo.mrUvTransform, ubo.uvRotations0.y);
    let normUv = transformUv(baseRawUv, ubo.normalUvTransform, ubo.uvRotations0.z);
    let occUv = transformUv(baseRawUv, ubo.occlusionUvTransform, ubo.uvRotations0.w);
    let emUv = transformUv(baseRawUv, ubo.emissiveUvTransform, ubo.uvRotations1.x);

    let baseDuv1 = transformUvDeriv(rawDuv1, ubo.uvTransform, ubo.uvRotations0.x);
    let baseDuv2 = transformUvDeriv(rawDuv2, ubo.uvTransform, ubo.uvRotations0.x);
    let mrDuv1 = transformUvDeriv(rawDuv1, ubo.mrUvTransform, ubo.uvRotations0.y);
    let mrDuv2 = transformUvDeriv(rawDuv2, ubo.mrUvTransform, ubo.uvRotations0.y);
    let occDuv1 = transformUvDeriv(rawDuv1, ubo.occlusionUvTransform, ubo.uvRotations0.w);
    let occDuv2 = transformUvDeriv(rawDuv2, ubo.occlusionUvTransform, ubo.uvRotations0.w);
    let emDuv1 = transformUvDeriv(rawDuv1, ubo.emissiveUvTransform, ubo.uvRotations1.x);
    let emDuv2 = transformUvDeriv(rawDuv2, ubo.emissiveUvTransform, ubo.uvRotations1.x);

    // Sample all PBR material maps using explicit screen-space gradients (uniformity-safe & anisotropic-correct)
    let baseTexSample = textureSampleGrad(baseColorTexture, defaultSampler, baseUv, baseDuv1, baseDuv2);
    let mrTexSample = textureSampleGrad(metallicRoughnessTexture, defaultSampler, mrUv, mrDuv1, mrDuv2);
    let normTexSample = textureSampleGrad(normalTexture, defaultSampler, normUv, normDuv1, normDuv2);
    let duv1 = rawDuv1;
    let duv2 = rawDuv2;
    let emTexSample = textureSampleGrad(emissiveTexture, defaultSampler, emUv, emDuv1, emDuv2);
    let occTexSample = textureSampleGrad(occlusionTexture, defaultSampler, occUv, occDuv1, occDuv2);

    // 3. Base Color & Alpha (glTF baseColorFactor is already linear-space)
    var baseColor = ubo.baseColorFactor;
    if ((ubo.flags.x & 1u) != 0u) {
        baseColor = baseColor * vec4<f32>(srgbToLinear(baseTexSample.rgb), baseTexSample.a);
    }

    // Alpha Cutoff test (glTF 2.0 alphaMode: 0 = OPAQUE, 1 = MASK, 2 = BLEND)
    let alphaMode = ubo.flags.w;
    let alphaCutoff = ubo.emissiveFactor.w;
    if (alphaMode == 1u && baseColor.a < alphaCutoff) {
        discard;
    }

    // 4. High-Detail Sculptural Bump & Normal Mapping
    var N = geomNormal;
    if (isBumpActive) {
        if ((ubo.flags.x & 4u) != 0u) { // HasNormalTex
            let normalSample = normTexSample.xyz * 2.0 - 1.0;
            let totalScale = ubo.pbrFactors.z * bumpScale;
            let tangentNormal = safeNormalize(vec3<f32>(normalSample.xy * totalScale, normalSample.z), vec3<f32>(0.0, 0.0, 1.0));
            N = safeNormalize(TBN * tangentNormal, geomNormal);
        } else if ((ubo.flags.x & 1u) != 0u || (ubo.flags.x & 16u) != 0u) {
            // Surface Gradient Bump Mapping from BaseColor Luminance and Occlusion Cavity
            var dHdU : f32 = 0.0;
            var dHdV : f32 = 0.0;

            if ((ubo.flags.x & 1u) != 0u) {
                let baseDims = vec2<f32>(textureDimensions(baseColorTexture, 0));
                let baseStep = 1.0 / clamp(baseDims, vec2<f32>(64.0), vec2<f32>(2048.0));
                let lumaWeights = vec3<f32>(0.299, 0.587, 0.114);

                let hR = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv + vec2<f32>(baseStep.x, 0.0), baseDuv1, baseDuv2).rgb, lumaWeights);
                let hL = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv - vec2<f32>(baseStep.x, 0.0), baseDuv1, baseDuv2).rgb, lumaWeights);
                let hU = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv + vec2<f32>(0.0, baseStep.y), baseDuv1, baseDuv2).rgb, lumaWeights);
                let hD = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv - vec2<f32>(0.0, baseStep.y), baseDuv1, baseDuv2).rgb, lumaWeights);

                dHdU = dHdU + (hR - hL);
                dHdV = dHdV + (hU - hD);
            }

            if ((ubo.flags.x & 16u) != 0u) {
                let occDims = vec2<f32>(textureDimensions(occlusionTexture, 0));
                let occStep = 1.0 / clamp(occDims, vec2<f32>(64.0), vec2<f32>(2048.0));
                let occR = textureSampleGrad(occlusionTexture, defaultSampler, occUv + vec2<f32>(occStep.x, 0.0), occDuv1, occDuv2).r;
                let occL = textureSampleGrad(occlusionTexture, defaultSampler, occUv - vec2<f32>(occStep.x, 0.0), occDuv1, occDuv2).r;
                let occU = textureSampleGrad(occlusionTexture, defaultSampler, occUv + vec2<f32>(0.0, occStep.y), occDuv1, occDuv2).r;
                let occD = textureSampleGrad(occlusionTexture, defaultSampler, occUv - vec2<f32>(0.0, occStep.y), occDuv1, occDuv2).r;
                dHdU = dHdU + (occR - occL) * 0.75;
                dHdV = dHdV + (occU - occD) * 0.75;
            }

            let bumpStrength = 2.0 * bumpScale;
            let tangentBump = safeNormalize(vec3<f32>(-dHdU * bumpStrength, -dHdV * bumpStrength, 1.0), vec3<f32>(0.0, 0.0, 1.0));
            N = safeNormalize(TBN * tangentBump, geomNormal);
        }
    }

    // 5. Micro-Cavity Factor & Horizon Occlusion
    let cavityFactor = clamp(dot(geomNormal, N), 0.0, 1.0);

    // Weather Parameters
    let weatherMode = ubo.weather.x;
    let weatherIntensity = ubo.weather.y;
    let windStrength = ubo.weather.z;
    let weatherTime = ubo.weather.w;

    if (weatherMode == 1.0 && weatherIntensity > 0.001 && geomNormal.y > 0.15) {
        // Rain droplet ripples on upward-facing surfaces
        let rippleTan = calculateRainRipples(in.uv * 5.0 + in.worldPos.xz * 2.5, weatherTime);
        let rippleWeight = smoothstep(0.15, 0.75, geomNormal.y) * clamp(weatherIntensity * 0.55, 0.0, 0.65);
        N = safeNormalize(mix(N, TBN * rippleTan, rippleWeight), geomNormal);
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomNormal.y > 0.2) {
        // Snow settles and flattens crevices upward
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        N = safeNormalize(mix(N, vec3<f32>(0.0, 1.0, 0.0), snowAccum * 0.6), geomNormal);
    }

    // 6. Metallic & Roughness with Micro-Roughness Relief
    var metallic = ubo.pbrFactors.x;
    var roughness = ubo.pbrFactors.y;
    if ((ubo.flags.x & 2u) != 0u) { // HasMrTex
        roughness = roughness * mrTexSample.g;
        metallic = metallic * mrTexSample.b;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    // Ridge polish & crevice matte modulation for heightened tactile plasticity
    roughness = clamp(roughness * mix(1.15, 0.85, cavityFactor), 0.04, 1.0);

    // Weather surface material modulation (wet absorption glaze, crystalline snow, fog condensation, smoke soot)
    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.08, clamp(weatherIntensity * 0.85, 0.0, 0.88));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            roughness = mix(roughness, 0.70, snowAccum * 0.9);
            metallic = mix(metallic, 0.0, snowAccum * 0.95);
        }
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.30, clamp(weatherIntensity * 0.35, 0.0, 0.45));
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.88, clamp(weatherIntensity * 0.4, 0.0, 0.55));
    }

    // 7. Ambient Occlusion with Micro-Cavity Crevice Deepening
    var ao = 1.0;
    if ((ubo.flags.x & 16u) != 0u) { // HasOcclusionTex
        ao = mix(1.0, occTexSample.r, ubo.pbrFactors.w);
    }
    ao = ao * mix(0.5, 1.0, cavityFactor * cavityFactor);

    // 8. Emissive
    var emissive = ubo.emissiveFactor.rgb;
    if ((ubo.flags.x & 8u) != 0u) { // HasEmissiveTex
        emissive = emissive * srgbToLinear(emTexSample.rgb);
    }

    // 9. PBR Vectors & Dielectric/Metallic Reflectance Setup
    let NdotV = max(dot(N, V), 0.001);
    let F0 = mix(vec3<f32>(0.04), baseColor.rgb, metallic);
    var diffuseColor = baseColor.rgb * (1.0 - metallic);

    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        let wetWeight = clamp(weatherIntensity * 0.8, 0.0, 0.75);
        diffuseColor = mix(diffuseColor, diffuseColor * 0.65, wetWeight * (1.0 - metallic * 0.5));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            let crispSnowColor = vec3<f32>(0.93, 0.95, 0.99);
            diffuseColor = mix(diffuseColor, crispSnowColor, snowAccum * 0.95);
        }
    }

    // 10. Direct Lighting Evaluation with Diurnal Sun/Moon Cycle & Horizon Micro-Shadowing
    var directRadiance = vec3<f32>(0.0);
    let solarElev = ubo.lightDir.w;
    let diurnalAmbientScale = evaluateDiurnalAmbientScale(solarElev);

    // Light 0: Sun / Moon Key Light
    let L0 = safeNormalize(ubo.lightDir.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let NdotL0 = max(dot(N, L0), 0.0);
    let geomNdotL0 = max(dot(geomNormal, L0), 0.0);
    let horizon0 = clamp(geomNdotL0 * 3.0, 0.0, 1.0);
    let light0Shadow = pomShadow * horizon0;

    if (NdotL0 > 0.0 && light0Shadow > 0.001) {
        let H0 = safeNormalize(L0 + V, N);
        let NdotH0 = max(dot(N, H0), 0.0);
        let VdotH0 = max(dot(V, H0), 0.0);

        let D = distributionGGX(NdotH0, roughness);
        let Vis = visibilitySmithGGX(NdotL0, NdotV, roughness);
        let F = fresnelSchlick(VdotH0, F0);
        let specularBRDF = D * Vis * F;

        let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
        let diffuseBRDF = kD * (diffuseColor / PI);

        let sunRadiance = evaluateDiurnalSunRadiance(solarElev);
        directRadiance = directRadiance + (diffuseBRDF + specularBRDF) * sunRadiance * (NdotL0 * light0Shadow);
    }

    // Light 1: Cool Sky Fill Light
    let L1 = safeNormalize(vec3<f32>(-L0.x * 0.7, 0.5, -L0.z * 0.7), vec3<f32>(0.0, 1.0, 0.0));
    let NdotL1 = max(dot(N, L1), 0.0);
    let geomNdotL1 = max(dot(geomNormal, L1), 0.0);
    let horizon1 = clamp(geomNdotL1 * 3.0, 0.0, 1.0);

    if (NdotL1 > 0.0 && horizon1 > 0.001) {
        let H1 = safeNormalize(L1 + V, N);
        let NdotH1 = max(dot(N, H1), 0.0);
        let VdotH1 = max(dot(V, H1), 0.0);

        let D = distributionGGX(NdotH1, roughness);
        let Vis = visibilitySmithGGX(NdotL1, NdotV, roughness);
        let F = fresnelSchlick(VdotH1, F0);
        let specularBRDF = D * Vis * F;

        let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
        let diffuseBRDF = kD * (diffuseColor / PI);

        let fillRadiance = vec3<f32>(0.35, 0.45, 0.60) * 0.8 * diurnalAmbientScale;
        directRadiance = directRadiance + (diffuseBRDF + specularBRDF) * fillRadiance * (NdotL1 * horizon1);
    }

    // Snow crystalline micro-sparkle under direct key illumination
    if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomNormal.y > 0.2) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        let sparkleCoord = floor(in.worldPos * 75.0);
        let sparkleNoise = fract(sin(dot(sparkleCoord, vec3<f32>(27.1, 59.3, 113.7))) * 43758.5453);
        let sparkle = pow(sparkleNoise, 16.0) * 3.5 * snowAccum * max(dot(N, L0), 0.0);
        directRadiance = directRadiance + vec3<f32>(sparkle);
    }

    // 11. Ambient Lighting (Hemispheric Diffuse Irradiance + GGX Environment Specular Reflection)
    let hemiWeight = N.y * 0.5 + 0.5;
    let groundAmbient = vec3<f32>(0.12, 0.11, 0.10) * diurnalAmbientScale;
    let skyAmbient = vec3<f32>(0.35, 0.45, 0.60) * diurnalAmbientScale;
    let irradiance = mix(groundAmbient, skyAmbient, hemiWeight) * 0.9;

    let F_env = fresnelSchlickRoughness(NdotV, F0, roughness);
    let kD_env = (vec3<f32>(1.0) - F_env) * (1.0 - metallic);
    let ambientDiffuse = kD_env * diffuseColor * irradiance;

    // Specular Reflection (Roughness-dependent environmental reflection modulated by cavity factor)
    let R = reflect(-V, N);
    let refHemi = R.y * 0.5 + 0.5;
    let groundReflect = vec3<f32>(0.10, 0.09, 0.08) * diurnalAmbientScale;
    let skyReflect = vec3<f32>(0.55, 0.70, 0.95) * diurnalAmbientScale;
    let envReflectCol = mix(groundReflect, skyReflect, refHemi);
    let roughnessAttenuation = 1.0 - roughness * 0.6;
    let ambientSpecular = envReflectCol * F_env * roughnessAttenuation * cavityFactor;

    let ambientColor = (ambientDiffuse + ambientSpecular) * ao;

    // 12. Total Radiance in Linear HDR Space
    var totalColor = directRadiance + ambientColor + emissive;

    // Atmospheric Weather Effects (Rain, Snow, Fog, Smoke)
    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        totalColor = totalColor + calculateRainStreaks(in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        totalColor = totalColor + calculateSnowFlakes(in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        totalColor = calculateFogEffect(totalColor, in.worldPos, ubo.cameraPos.xyz, L0, solarElev, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        totalColor = calculateSmokePlumes(totalColor, in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    }

    // 13. Diagnostic Render Views & HDR Tonemapping (Khronos PBR Neutral)
    if (debugViewMode == 1u) {
        var heatCol = getLodHeatmapColor(activeLodLevel);
        if (lodTransition > 0.01 && activeLodLevel < 3u) {
            let dither = bayerDither4x4(in.clipPos.xy);
            if (lodTransition > dither) {
                heatCol = getLodHeatmapColor(activeLodLevel + 1u);
            }
        }
        let litLuma = dot(linearToSrgb(tonemapKhronosPbrNeutral(totalColor)), vec3<f32>(0.299, 0.587, 0.114));
        return vec4<f32>(heatCol * (0.35 + 0.65 * litLuma), 1.0);
    } else if (debugViewMode == 2u) {
        return vec4<f32>(N * 0.5 + 0.5, 1.0);
    } else if (debugViewMode == 3u) {
        return vec4<f32>(ao, roughness, metallic, 1.0);
    } else if (debugViewMode == 4u) {
        let grid = floor(baseUv * 16.0);
        let checker = abs((grid.x + grid.y) % 2.0);
        let checkerCol = mix(vec3<f32>(0.16, 0.18, 0.22), vec3<f32>(0.90, 0.92, 0.96), checker);
        return vec4<f32>(checkerCol * (0.4 + 0.6 * max(dot(N, L0), 0.2)), 1.0);
    }

    totalColor = tonemapKhronosPbrNeutral(totalColor);
    let finalColor = linearToSrgb(totalColor);

    return vec4<f32>(finalColor, baseColor.a);
}
`;

  globalThis.BglWebGpuShaders = { pbrWgsl };
})();
