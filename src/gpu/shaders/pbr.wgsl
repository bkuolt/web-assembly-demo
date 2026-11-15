#include "common/uniforms.wgsl"

struct GpuPunctualLight {
    posOrDir : vec4<f32>,  // xyz = position (Point/Spot) or directionToLight (Directional), w = lightType (0=Dir, 1=Point, 2=Spot)
    colorInt : vec4<f32>,  // xyz = color * intensity, w = range (0 = infinite)
    spotParams : vec4<f32> // xyz = spotDirection (light-to-target), w = packed angleScale / angleOffset
};

struct SceneLightingUniforms {
    shIrradiance : array<vec4<f32>, 9>,
    lights : array<GpuPunctualLight, 16>,
    spotAngles : array<vec4<f32>, 16>, // x = lightAngleScale, y = lightAngleOffset, z = castsShadow, w = pad
    clusterMasks : array<vec4<u32>, 256>, // 1024 u32 bitmasks for 16 x 8 x 8 Forward+ frustum clusters
    gridParams : vec4<f32>, // x = viewportW, y = viewportH, z = zNear, w = zFar
    envParams : vec4<f32>,  // x = envRotation (rad), y = punctualLightCount, z = useClusteredCulling, w = hasPrimarySun
    viewMatrix : mat4x4<f32>,
    weatherParams : vec4<f32>, // x = weatherMode (0=None, 1=Rain, 2=Snow), y = intensity, z = wind, w = timeSec
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    renderParams : vec4<f32>, // x = envMipCount, y = shadowBias, z = aoIntensity, w = reserved
};

@group(0) @binding(0) var<uniform> sceneLighting : SceneLightingUniforms;
@group(0) @binding(1) var envCubemap : texture_cube<f32>;
@group(0) @binding(2) var shadowMap : texture_depth_2d;
@group(0) @binding(3) var shadowSampler : sampler_comparison;
@group(0) @binding(4) var brdfLutTexture : texture_2d<f32>;
@group(0) @binding(5) var opaqueColorTexture : texture_2d<f32>;

@group(1) @binding(0) var<uniform> drawUbo : PerDrawUniforms;

@group(2) @binding(0) var<uniform> matUbo : MaterialUniforms;
@group(2) @binding(1) var defaultSampler : sampler;
@group(2) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(2) @binding(3) var metallicRoughnessTexture : texture_2d<f32>;
@group(2) @binding(4) var normalTexture : texture_2d<f32>;
@group(2) @binding(5) var emissiveTexture : texture_2d<f32>;
@group(2) @binding(6) var occlusionTexture : texture_2d<f32>;
@group(2) @binding(7) var clearcoatTexture : texture_2d<f32>;
@group(2) @binding(8) var transmissionTexture : texture_2d<f32>;
@group(2) @binding(9) var sheenTexture : texture_2d<f32>;
@group(2) @binding(10) var<storage, read> morphDeltas : array<vec4<f32>>;

@group(3) @binding(0) var<storage, read> jointMatrices : array<mat4x4<f32>>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) worldPos : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
};

fn rotateY(v : vec3<f32>, angle : f32) -> vec3<f32> {
    let c = cos(angle);
    let s = sin(angle);
    return vec3<f32>(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

fn evaluateSHIrradiance(n : vec3<f32>) -> vec3<f32> {
    // Ramamoorthi & Hanrahan L2 Spherical Harmonics irradiance reconstruction
    var irr = sceneLighting.shIrradiance[0].xyz * 0.886227
            + sceneLighting.shIrradiance[1].xyz * (1.023327 * n.y)
            + sceneLighting.shIrradiance[2].xyz * (1.023327 * n.z)
            + sceneLighting.shIrradiance[3].xyz * (1.023327 * n.x)
            + sceneLighting.shIrradiance[4].xyz * (0.858086 * n.x * n.y)
            + sceneLighting.shIrradiance[5].xyz * (0.858086 * n.y * n.z)
            + sceneLighting.shIrradiance[6].xyz * (0.247708 * (3.0 * n.z * n.z - 1.0))
            + sceneLighting.shIrradiance[7].xyz * (0.858086 * n.x * n.z)
            + sceneLighting.shIrradiance[8].xyz * (0.429043 * (n.x * n.x - n.y * n.y));
    return max(irr, vec3<f32>(0.0));
}

fn getClusterLightMask(fragCoord : vec4<f32>, worldPos : vec3<f32>) -> u32 {
    if (sceneLighting.envParams.z < 0.5) {
        return 0xFFFFFFFFu;
    }
    let vpW = max(sceneLighting.gridParams.x, 1.0);
    let vpH = max(sceneLighting.gridParams.y, 1.0);
    let zNear = max(sceneLighting.gridParams.z, 0.05);
    let zFar = max(sceneLighting.gridParams.w, zNear + 1.0);

    let tileX = clamp(u32((fragCoord.x / vpW) * 16.0), 0u, 15u);
    let tileY = clamp(u32((fragCoord.y / vpH) * 8.0), 0u, 7u);

    let viewPos = sceneLighting.viewMatrix * vec4<f32>(worldPos, 1.0);
    let viewZ = max(-viewPos.z, zNear);
    let logRatio = log(viewZ / zNear) / max(log(zFar / zNear), 1e-4);
    let sliceZ = clamp(u32(clamp(logRatio, 0.0, 0.9999) * 8.0), 0u, 7u);

    let clusterIdx = (sliceZ * 8u + tileY) * 16u + tileX;
    let vecIdx = clusterIdx >> 2u;
    let compIdx = clusterIdx & 3u;
    let maskVec = sceneLighting.clusterMasks[vecIdx];
    if (compIdx == 0u) { return maskVec.x; }
    if (compIdx == 1u) { return maskVec.y; }
    if (compIdx == 2u) { return maskVec.z; }
    return maskVec.w;
}

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    var localPos = in.position;
    var localNorm = in.normal;
    var localTan = in.tangent.xyz;

    let morphCount = min(drawUbo.morphTargetCount, 8u);
    if (morphCount > 0u && drawUbo.vertexCount > 0u && (drawUbo.drawFlags & 262144u) != 0u) {
        for (var t : u32 = 0u; t < morphCount; t = t + 1u) {
            let wVec = select(drawUbo.morphWeights0, drawUbo.morphWeights1, t >= 4u);
            let wComp = t & 3u;
            var w = wVec.x;
            if (wComp == 1u) { w = wVec.y; }
            else if (wComp == 2u) { w = wVec.z; }
            else if (wComp == 3u) { w = wVec.w; }
            if (abs(w) > 1e-6) {
                let baseIdx = (t * drawUbo.vertexCount + in.vertexIndex) * 3u;
                localPos = localPos + w * morphDeltas[baseIdx + 0u].xyz;
                localNorm = localNorm + w * morphDeltas[baseIdx + 1u].xyz;
                localTan = localTan + w * morphDeltas[baseIdx + 2u].xyz;
            }
        }
    }

    if (drawUbo.jointCount > 0u) {
        let maxJoint = drawUbo.jointCount - 1u;
        let i0 = min(in.joints.x, maxJoint);
        let i1 = min(in.joints.y, maxJoint);
        let i2 = min(in.joints.z, maxJoint);
        let i3 = min(in.joints.w, maxJoint);
        let skinMat = in.weights.x * jointMatrices[i0] +
                      in.weights.y * jointMatrices[i1] +
                      in.weights.z * jointMatrices[i2] +
                      in.weights.w * jointMatrices[i3];

        localPos = (skinMat * vec4<f32>(localPos, 1.0)).xyz;
        localNorm = (skinMat * vec4<f32>(localNorm, 0.0)).xyz;
        localTan = (skinMat * vec4<f32>(localTan, 0.0)).xyz;
    }

    let modelMat = resolveWorldModel(in, drawUbo.model);
    let world = modelMat * vec4<f32>(localPos, 1.0);
    out.worldPos = world.xyz;
    out.clipPos = drawUbo.viewProj * world;

    let m0 = modelMat[0].xyz;
    let m1 = modelMat[1].xyz;
    let m2 = modelMat[2].xyz;
    let adj0 = cross(m1, m2);
    let adj1 = cross(m2, m0);
    let adj2 = cross(m0, m1);
    let det = dot(m0, adj0);
    let rawNorm = select((modelMat * vec4<f32>(localNorm, 0.0)).xyz,
                         adj0 * localNorm.x + adj1 * localNorm.y + adj2 * localNorm.z,
                         abs(det) > 1e-12);
    let normLen = length(rawNorm);
    out.normal = select(vec3<f32>(0.0, 1.0, 0.0), rawNorm / normLen, normLen > 1e-8);

    let worldTan = (modelMat * vec4<f32>(localTan, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    let detSign = select(1.0, -1.0, det < 0.0);
    out.tangent = vec4<f32>(normTan, in.tangent.w * detSign);
    out.uv = in.uv;
    return out;
}

fn getTBN(worldPos : vec3<f32>, geomNormal : vec3<f32>, uv : vec2<f32>, tangent : vec4<f32>, dp1 : vec3<f32>, dp2 : vec3<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> mat3x3<f32> {
    let N = normalize(geomNormal);
    let orthoT = tangent.xyz - N * dot(N, tangent.xyz);
    let orthoLenSq = dot(orthoT, orthoT);
    if (abs(tangent.w) > 0.001 && orthoLenSq > 1e-6) {
        let T = orthoT * inverseSqrt(orthoLenSq);
        let B = normalize(cross(N, T) * sign(tangent.w));
        return mat3x3<f32>(T, B, N);
    }
    let dp2perp = cross(dp2, N);
    let dp1perp = cross(N, dp1);
    let T = dp2perp * duv1.x + dp1perp * duv2.x;
    let B = dp2perp * duv1.y + dp1perp * duv2.y;
    let maxLenSq = max(dot(T, T), dot(B, B));
    if (maxLenSq > 1e-18) {
        let invmax = inverseSqrt(maxLenSq);
        return mat3x3<f32>(normalize(T * invmax), normalize(B * invmax), N);
    }
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(N.y) < 0.999);
    let fallbackT = normalize(cross(up, N));
    let fallbackB = normalize(cross(N, fallbackT));
    return mat3x3<f32>(fallbackT, fallbackB, N);
}

fn distributionGGX(NdotH : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265359 * d * d);
}

fn visibilitySmithGGX(NdotL : f32, NdotV : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let ggxL = NdotL + sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
    let ggxV = NdotV + sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
    return 1.0 / max(ggxL * ggxV, 0.0001);
}

fn fresnelSchlick(VdotH : f32, F0 : vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(cosTheta : f32, F0 : vec3<f32>, roughness : f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn computePCFShadow(worldPos : vec3<f32>, N : vec3<f32>, L : vec3<f32>) -> f32 {
    let shadowBias = sceneLighting.renderParams.y;
    let NdotL = max(dot(N, L), 0.0);
    let normalOffset = N * clamp(shadowBias * 4.0, 0.002, 0.015) * (1.0 - NdotL);
    let lightClip = drawUbo.lightViewProj * vec4<f32>(worldPos + normalOffset, 1.0);
    let projCoords = lightClip.xyz / lightClip.w;

    let uv = vec2<f32>(projCoords.x * 0.5 + 0.5, 0.5 - projCoords.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 1.0;
    }

    let slopeBias = max(shadowBias * (1.0 - NdotL), shadowBias * 0.35);
    let compareDepth = projCoords.z - slopeBias;
    let texelSize = 1.0 / 2048.0;

    // Shader-LOD: distant LOD2/LOD3 primitives use fast 1-tap compare; LOD0/LOD1 use 5-tap Rotated-Grid PCF
    let lodLevel = (drawUbo.drawFlags >> 19u) & 3u;
    var shadow = 0.0;
    if (lodLevel >= 2u) {
        shadow = textureSampleCompareLevel(shadowMap, shadowSampler, uv, compareDepth);
    } else {
        let r = texelSize * 1.15;
        let s0 = textureSampleCompareLevel(shadowMap, shadowSampler, uv, compareDepth);
        let s1 = textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2<f32>(-0.866,  0.500) * r, compareDepth);
        let s2 = textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2<f32>( 0.500,  0.866) * r, compareDepth);
        let s3 = textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2<f32>( 0.866, -0.500) * r, compareDepth);
        let s4 = textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2<f32>(-0.500, -0.866) * r, compareDepth);
        shadow = s0 * 0.28 + (s1 + s2 + s3 + s4) * 0.18;
    }

    let edgeDist = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    let borderFade = smoothstep(0.0, 0.04, edgeDist);
    return mix(1.0, shadow, borderFade);
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

fn evaluateLodHeatmapColor(lodLevel : u32) -> vec3<f32> {
    switch (lodLevel) {
        case 0u: { return vec3<f32>(0.10, 0.92, 0.38); } // LOD0: Emerald Green (100% Detail)
        case 1u: { return vec3<f32>(0.96, 0.82, 0.12); } // LOD1: Amber Yellow (~50% Detail)
        case 2u: { return vec3<f32>(0.98, 0.45, 0.10); } // LOD2: Deep Orange (~25% Detail)
        default: { return vec3<f32>(0.95, 0.16, 0.22); } // LOD3: Crimson Red (~10% Detail)
    }
}

fn evaluateDirectLobe(
    L : vec3<f32>,
    V : vec3<f32>,
    N : vec3<f32>,
    radiance : vec3<f32>,
    F0 : vec3<f32>,
    diffuseColor : vec3<f32>,
    roughness : f32,
    metallic : f32,
    transFactor : f32,
    ccFactor : f32,
    ccRoughness : f32,
    sheenColor : vec3<f32>,
    sheenRoughness : f32
) -> vec3<f32> {
    let NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) {
        return vec3<f32>(0.0);
    }
    let NdotV = max(dot(N, V), 0.001);
    let H = normalize(L + V);
    let NdotH = max(dot(N, H), 0.0);
    let VdotH = max(dot(V, H), 0.0);

    let D = distributionGGX(NdotH, roughness);
    let Vis = visibilitySmithGGX(NdotL, NdotV, roughness);
    let F = fresnelSchlick(VdotH, F0);
    let specularBRDF = D * Vis * F;

    let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
    let diffuseBRDF = kD * (diffuseColor / 3.14159265359) * (1.0 - transFactor);

    var contrib = (diffuseBRDF + specularBRDF) * radiance * NdotL;

    if (ccFactor > 0.001) {
        let Dcc = distributionGGX(NdotH, ccRoughness);
        let VisCc = visibilitySmithGGX(NdotL, NdotV, ccRoughness);
        let clearcoatF = fresnelSchlick(VdotH, vec3<f32>(0.04));
        let clearcoatLobe = vec3<f32>(Dcc * VisCc) * clearcoatF * ccFactor;
        contrib = contrib * (1.0 - clearcoatF * ccFactor) + clearcoatLobe * radiance * NdotL;
    }

    if (length(sheenColor) > 0.001) {
        let invAlpha = 1.0 / sheenRoughness;
        let sin2 = max(1.0 - NdotH * NdotH, 0.0);
        let Dcharlie = (2.0 + invAlpha) / (2.0 * 3.14159265359) * pow(sin2, invAlpha * 0.5);
        let sheenLobe = sheenColor * Dcharlie / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 0.001);
        contrib = contrib + sheenLobe * radiance * NdotL;
    }

    return contrib;
}

/**
 * @brief Samples normalized surface displacement height from occlusion map, normal alpha, or base color luma.
 * @param uv Normalized 2D texture coordinates.
 * @param duv1 Screen-space horizontal derivative of UV coordinates.
 * @param duv2 Screen-space vertical derivative of UV coordinates.
 * @return Normalized height value in [0.0, 1.0] where 1.0 represents the outer surface plane.
 */
fn getSurfaceHeight(uv : vec2<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> f32 {
    var height = 0.5;
    if ((matUbo.materialFlags & 16u) != 0u) {
        // Occlusion texture red channel represents cavity depth (0 = deep crevice, 1 = surface plane)
        let occUv = transformUv(uv, matUbo.occlusionUvTransform, matUbo.uvRotations0.w);
        let occD1 = transformUvDeriv(duv1, matUbo.occlusionUvTransform, matUbo.uvRotations0.w);
        let occD2 = transformUvDeriv(duv2, matUbo.occlusionUvTransform, matUbo.uvRotations0.w);
        height = textureSampleGrad(occlusionTexture, defaultSampler, occUv, occD1, occD2).r;
    } else {
        // Base color luminance fallback for materials without dedicated height/occlusion maps
        let baseUv = transformUv(uv, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
        let baseD1 = transformUvDeriv(duv1, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
        let baseD2 = transformUvDeriv(duv2, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
        let col = textureSampleGrad(baseColorTexture, defaultSampler, baseUv, baseD1, baseD2).rgb;
        height = dot(col, vec3<f32>(0.299, 0.587, 0.114));
    }
    return clamp(height, 0.0, 1.0);
}

/**
 * @brief Steep Parallax Occlusion Mapping (POM) with adaptive view-dependent sampling and linear secant interpolation.
 * @param initialUV Original interpolated UV coordinates.
 * @param viewDirTangent Camera view direction normalized in tangent space.
 * @param duv1 Screen-space horizontal derivative of UV coordinates.
 * @param duv2 Screen-space vertical derivative of UV coordinates.
 * @param scale Parallax displacement depth scale factor.
 * @return Displaced texture coordinates giving true 3D surface depth.
 */
fn parallaxOcclusionMapping(
    initialUV : vec2<f32>,
    viewDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> vec2<f32> {
    // Dynamic step count between 8 and 32 based on viewing angle (steep grazing angles need more samples)
    let minLayers : f32 = 8.0;
    let maxLayers : f32 = 32.0;
    let numLayers = mix(maxLayers, minLayers, clamp(abs(viewDirTangent.z), 0.0, 1.0));
    let layerDepth = 1.0 / numLayers;
    var currentLayerDepth : f32 = 0.0;

    // Tangent space ray delta per step (depth downwards: 1.0 is surface, 0.0 is base)
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

    // Parallax Occlusion interpolation between previous layer and current layer
    let afterDepth = currentLayerDepth - (1.0 - currentHeight);
    let beforeDepth = (1.0 - prevHeight) - prevLayerDepth;
    let denom = afterDepth + beforeDepth;
    let weight = select(0.5, afterDepth / max(denom, 1e-5), abs(denom) > 1e-6);

    return mix(currentUV, prevUV, clamp(weight, 0.0, 1.0));
}

/**
 * @brief Parallax self-shadowing raymarch from surface point along tangent-space light direction.
 * @param startUV Displaced surface texture coordinate.
 * @param lightDirTangent Normalized light direction in tangent space.
 * @param duv1 Screen-space horizontal derivative of UV coordinates.
 * @param duv2 Screen-space vertical derivative of UV coordinates.
 * @param scale Parallax displacement depth scale factor.
 * @return Shadow attenuation factor in [0.25, 1.0].
 */
fn computeParallaxShadow(
    startUV : vec2<f32>,
    lightDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> f32 {
    if (lightDirTangent.z <= 0.0) {
        return 0.25;
    }
    let initialHeight = getSurfaceHeight(startUV, duv1, duv2);
    let numShadowSteps = 8.0;
    let stepDepth = (1.0 - initialHeight) / numShadowSteps;
    let deltaUV = (lightDirTangent.xy / max(lightDirTangent.z, 0.15)) * (scale / numShadowSteps);

    var currentUV = startUV;
    var currentDepth = 1.0 - initialHeight;
    var shadowFactor = 1.0;

    for (var i : i32 = 0; i < 8; i = i + 1) {
        currentUV = currentUV + deltaUV;
        currentDepth = currentDepth - stepDepth;
        let h = getSurfaceHeight(currentUV, duv1, duv2);
        let sampleDepth = 1.0 - h;
        if (sampleDepth < currentDepth) {
            shadowFactor = min(shadowFactor, clamp((currentDepth - sampleDepth) * 3.5, 0.0, 1.0));
        }
    }
    return clamp(shadowFactor, 0.25, 1.0);
}

#include "common/weather.wgsl"

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    let flags = matUbo.materialFlags | drawUbo.drawFlags;
    let dp1 = dpdx(in.worldPos);
    let dp2 = dpdy(in.worldPos);
    let rawDuv1 = dpdx(in.uv);
    let rawDuv2 = dpdy(in.uv);
    let duvBase1 = transformUvDeriv(rawDuv1, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
    let duvBase2 = transformUvDeriv(rawDuv2, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
    let duvMr1 = transformUvDeriv(rawDuv1, matUbo.metallicRoughnessUvTransform, matUbo.uvRotations0.y);
    let duvMr2 = transformUvDeriv(rawDuv2, matUbo.metallicRoughnessUvTransform, matUbo.uvRotations0.y);
    let duvNorm1 = transformUvDeriv(rawDuv1, matUbo.normalUvTransform, matUbo.uvRotations0.z);
    let duvNorm2 = transformUvDeriv(rawDuv2, matUbo.normalUvTransform, matUbo.uvRotations0.z);
    let duvOcc1 = transformUvDeriv(rawDuv1, matUbo.occlusionUvTransform, matUbo.uvRotations0.w);
    let duvOcc2 = transformUvDeriv(rawDuv2, matUbo.occlusionUvTransform, matUbo.uvRotations0.w);
    let duvEm1 = transformUvDeriv(rawDuv1, matUbo.emissiveUvTransform, matUbo.uvRotations1.x);
    let duvEm2 = transformUvDeriv(rawDuv2, matUbo.emissiveUvTransform, matUbo.uvRotations1.x);

    var geomN = normalize(in.normal);
    let camToFrag = sceneLighting.cameraPos.xyz - in.worldPos;
    let viewDist = length(camToFrag);
    let viewDir = camToFrag / max(viewDist, 1e-6);
    if ((flags & 512u) != 0u && dot(geomN, viewDir) < 0.0) {
        geomN = -geomN;
    }

    let tbn = getTBN(in.worldPos, geomN, in.uv, in.tangent, dp1, dp2, duvNorm1, duvNorm2);

    // Tangent-space view vector (TBN transpose * V)
    let V_tangent = vec3<f32>(
        dot(tbn[0], viewDir),
        dot(tbn[1], viewDir),
        dot(tbn[2], viewDir)
    );

    let envRot = sceneLighting.envParams.x;
    let sunL = normalize(sceneLighting.lightDir.xyz);

    let lodLevel = (flags >> 19u) & 3u;
    let debugViewMode = (flags >> 21u) & 7u;
    let lodTransition = clamp(drawUbo.drawParams.x, 0.0, 1.0);

    // Shader-LOD + smooth distance fade-out for Parallax Occlusion Mapping (POM)
    var uv = in.uv;
    var pomShadow = 1.0;
    let pomDistFade = 1.0 - smoothstep(10.0, 22.0, viewDist);
    if (lodLevel == 0u && pomDistFade > 0.02 && (flags & 32768u) != 0u && (flags & 1024u) != 0u) {
        let isNormalMapped = (flags & 4u) != 0u;
        let baseScale = select(0.035, 0.005, isNormalMapped);
        let pomScale = max(matUbo.bumpScale * baseScale, 0.001) * pomDistFade;
        uv = parallaxOcclusionMapping(in.uv, V_tangent, rawDuv1, rawDuv2, pomScale);

        let L_tangent = vec3<f32>(
            dot(tbn[0], sunL),
            dot(tbn[1], sunL),
            dot(tbn[2], sunL)
        );
        pomShadow = computeParallaxShadow(uv, L_tangent, rawDuv1, rawDuv2, pomScale);
    }

    let uvBase = transformUv(uv, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
    let uvMr = transformUv(uv, matUbo.metallicRoughnessUvTransform, matUbo.uvRotations0.y);
    let uvNorm = transformUv(uv, matUbo.normalUvTransform, matUbo.uvRotations0.z);
    let uvOcc = transformUv(uv, matUbo.occlusionUvTransform, matUbo.uvRotations0.w);
    let uvEm = transformUv(uv, matUbo.emissiveUvTransform, matUbo.uvRotations1.x);

    let baseTexSample = textureSampleGrad(baseColorTexture, defaultSampler, uvBase, duvBase1, duvBase2);
    let mrTexSample = textureSampleGrad(metallicRoughnessTexture, defaultSampler, uvMr, duvMr1, duvMr2);
    let normTexSample = textureSampleGrad(normalTexture, defaultSampler, uvNorm, duvNorm1, duvNorm2);
    let emTexSample = textureSampleGrad(emissiveTexture, defaultSampler, uvEm, duvEm1, duvEm2);
    let occTexSample = textureSampleGrad(occlusionTexture, defaultSampler, uvOcc, duvOcc1, duvOcc2);
    let ccTexSample = textureSampleGrad(clearcoatTexture, defaultSampler, uvBase, duvBase1, duvBase2);
    let transTexSample = textureSampleGrad(transmissionTexture, defaultSampler, uvBase, duvBase1, duvBase2);
    let sheenTexSample = textureSampleGrad(sheenTexture, defaultSampler, uvBase, duvBase1, duvBase2);

    var baseColor = matUbo.baseColorFactor;
    if ((flags & 1u) != 0u) {
        baseColor = baseColor * baseTexSample;
    }

    if (matUbo.alphaMode == 1u && baseColor.a < matUbo.alphaCutoff) {
        discard;
    }

    if ((flags & 256u) != 0u) {
        return baseColor;
    }

    var N = geomN;
    if ((flags & 4u) != 0u) {
        let normTex = normTexSample.xyz * 2.0 - 1.0;
        let scaledNorm = normalize(vec3<f32>(normTex.xy * matUbo.normalScale, normTex.z));
        N = normalize(tbn * scaledNorm);
    } else if ((flags & 1024u) != 0u && ((flags & 1u) != 0u || (flags & 16u) != 0u)) {
        let texDims = vec2<f32>(textureDimensions(baseColorTexture, 0));
        let texelStep = 1.0 / clamp(texDims, vec2<f32>(64.0), vec2<f32>(2048.0));
        let lumaWeights = vec3<f32>(0.299, 0.587, 0.114);

        let hR_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uvBase + vec2<f32>(texelStep.x, 0.0), duvBase1, duvBase2).rgb;
        let hL_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uvBase - vec2<f32>(texelStep.x, 0.0), duvBase1, duvBase2).rgb;
        let hU_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uvBase + vec2<f32>(0.0, texelStep.y), duvBase1, duvBase2).rgb;
        let hD_rgb = textureSampleGrad(baseColorTexture, defaultSampler, uvBase - vec2<f32>(0.0, texelStep.y), duvBase1, duvBase2).rgb;

        var dHdU = dot(hR_rgb - hL_rgb, lumaWeights);
        var dHdV = dot(hU_rgb - hD_rgb, lumaWeights);
        if (lodLevel < 2u && (flags & 16u) != 0u) {
            let occR = textureSampleGrad(occlusionTexture, defaultSampler, uvOcc + vec2<f32>(texelStep.x, 0.0), duvOcc1, duvOcc2).r;
            let occL = textureSampleGrad(occlusionTexture, defaultSampler, uvOcc - vec2<f32>(texelStep.x, 0.0), duvOcc1, duvOcc2).r;
            let occU = textureSampleGrad(occlusionTexture, defaultSampler, uvOcc + vec2<f32>(0.0, texelStep.y), duvOcc1, duvOcc2).r;
            let occD = textureSampleGrad(occlusionTexture, defaultSampler, uvOcc - vec2<f32>(0.0, texelStep.y), duvOcc1, duvOcc2).r;
            dHdU = dHdU + (occR - occL) * 0.75;
            dHdV = dHdV + (occU - occD) * 0.75;
        }
        let bumpNorm = normalize(vec3<f32>(vec2<f32>(-dHdU, -dHdV) * (2.0 * matUbo.bumpScale), 1.0));
        N = normalize(tbn * bumpNorm);
    }

    // Weather Parameters & Surface Normal Perturbation (Rain Ripples & Snow Accumulation)
    let weatherMode = sceneLighting.weatherParams.x;
    let weatherIntensity = sceneLighting.weatherParams.y;
    let windStrength = sceneLighting.weatherParams.z;
    let weatherTime = sceneLighting.weatherParams.w;

    if (weatherMode == 1.0 && weatherIntensity > 0.001 && geomN.y > 0.15) {
        let rippleTan = calculateRainRipples(uvBase * 5.0 + in.worldPos.xz * 2.5, weatherTime);
        let rippleWeight = smoothstep(0.15, 0.75, geomN.y) * clamp(weatherIntensity * 0.55, 0.0, 0.65);
        N = normalize(mix(N, tbn * rippleTan, rippleWeight));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomN.y > 0.2) {
        let snowAccum = smoothstep(0.2, 0.75, geomN.y) * clamp(weatherIntensity, 0.0, 1.5);
        N = normalize(mix(N, vec3<f32>(0.0, 1.0, 0.0), snowAccum * 0.6));
    }

    var roughness = matUbo.roughnessFactor;
    var metallic = matUbo.metallicFactor;
    if ((flags & 2u) != 0u) {
        roughness = roughness * mrTexSample.g;
        metallic = metallic * mrTexSample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.08, clamp(weatherIntensity * 0.85, 0.0, 0.88));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomN.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            roughness = mix(roughness, 0.70, snowAccum * 0.9);
            metallic = mix(metallic, 0.0, snowAccum * 0.95);
        }
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.30, clamp(weatherIntensity * 0.35, 0.0, 0.45));
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.88, clamp(weatherIntensity * 0.4, 0.0, 0.55));
    }

    let ior = max(matUbo.ior, 1.0);
    let f0_dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    let specularColor = matUbo.specularFactor * matUbo.specularColorFactor.rgb;
    let F0 = mix(vec3<f32>(f0_dielectric) * specularColor, baseColor.rgb, metallic);
    var diffuseColor = baseColor.rgb * (1.0 - metallic);

    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        let wetWeight = clamp(weatherIntensity * 0.8, 0.0, 0.75);
        diffuseColor = mix(diffuseColor, diffuseColor * 0.65, wetWeight * (1.0 - metallic));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomN.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            let crispSnowColor = vec3<f32>(0.93, 0.95, 0.99);
            diffuseColor = mix(diffuseColor, crispSnowColor, snowAccum * 0.95);
        }
    }

    var ccFactor = matUbo.clearcoatFactor;
    var ccRoughness = matUbo.clearcoatRoughness;
    if ((flags & 32u) != 0u) {
        ccFactor = ccFactor * ccTexSample.r;
        ccRoughness = ccRoughness * ccTexSample.g;
    }
    ccRoughness = clamp(ccRoughness, 0.04, 1.0);

    var sheenColor = matUbo.sheenColorFactor.rgb;
    let sheenRoughness = clamp(matUbo.sheenColorFactor.w, 0.04, 1.0);
    if ((flags & 128u) != 0u) {
        sheenColor = sheenColor * sheenTexSample.rgb;
    }

    var transmission = matUbo.transmissionFactor;
    if ((flags & 64u) != 0u) {
        transmission = transmission * transTexSample.r;
    }
    let transFactor = clamp(transmission, 0.0, 1.0);

    // Ground-truth Ambient Occlusion (glTF occlusion map + multi-bounce)
    var ao = 1.0;
    if ((flags & 16384u) != 0u) { // Bit 14 = EnableAmbientOcclusion
        if ((flags & 16u) != 0u) {
            let occSample = occTexSample.r;
            ao = clamp(1.0 + (matUbo.occlusionStrength * sceneLighting.renderParams.z) * (occSample - 1.0), 0.0, 1.0);
        }
        ao = mix(ao, ao * (diffuseColor.r * (1.0 - ao) + 1.0), 0.2);
    }

    var emissive = matUbo.emissiveFactor.xyz * matUbo.emissiveFactor.w;
    if ((flags & 8u) != 0u) {
        emissive = emissive * emTexSample.rgb;
    }

    let V = viewDir;
    let NdotV = max(dot(N, V), 0.001);

    // 1. Primary Sun/Moon Light (Diurnal Cycle + Rotated with Environment) + Shadow Map
    let solarElev = sceneLighting.lightDir.w;
    let diurnalAmbientScale = evaluateDiurnalAmbientScale(solarElev);
    var shadowFactor = 1.0;
    if ((flags & 4096u) != 0u) {
        shadowFactor = computePCFShadow(in.worldPos, geomN, sunL);
    }

    var directLighting = vec3<f32>(0.0);
    if (sceneLighting.envParams.w > 0.5) {
        let sunRadiance = evaluateDiurnalSunRadiance(solarElev);
        directLighting = evaluateDirectLobe(
            sunL, V, N, sunRadiance, F0, diffuseColor, roughness, metallic,
            transFactor, ccFactor, ccRoughness, sheenColor, sheenRoughness
        ) * (shadowFactor * pomShadow);
    }

    if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomN.y > 0.2) {
        let snowAccum = smoothstep(0.2, 0.75, geomN.y) * clamp(weatherIntensity, 0.0, 1.5);
        let sparkleCoord = floor(in.worldPos * 75.0);
        let sparkleNoise = fract(sin(dot(sparkleCoord, vec3<f32>(27.1, 59.3, 113.7))) * 43758.5453);
        let sparkle = pow(sparkleNoise, 16.0) * 3.5 * snowAccum * max(dot(N, sunL), 0.0) * shadowFactor;
        directLighting = directLighting + vec3<f32>(sparkle);
    }

    // 2. KHR_lights_punctual Evaluated via Clustered Forward+ Frustum Culling
    let punctualCount = min(u32(sceneLighting.envParams.y), 16u);
    if (punctualCount > 0u) {
        let clusterMask = getClusterLightMask(in.clipPos, in.worldPos);
        for (var li : u32 = 0u; li < punctualCount; li = li + 1u) {
            let light = sceneLighting.lights[li];
            let lightType = u32(light.posOrDir.w + 0.5);

            var L_p = vec3<f32>(0.0, 1.0, 0.0);
            var attenuation = 1.0;

            if (lightType == 0u) {
                // Directional light (always active across all clusters)
                L_p = normalize(light.posOrDir.xyz);
            } else {
                // Point or Spot light: check Forward+ 3D cluster mask first
                if ((clusterMask & (1u << li)) == 0u) {
                    continue;
                }
                let toLight = light.posOrDir.xyz - in.worldPos;
                let distSq = dot(toLight, toLight);
                let dist = sqrt(max(distSq, 1e-6));
                L_p = toLight / dist;

                let range = light.colorInt.w;
                var rangeAtten = 1.0 / max(distSq, 0.01 * 0.01);
                if (range > 0.0) {
                    let ratio = dist / range;
                    let ratio4 = ratio * ratio * ratio * ratio;
                    let window = clamp(1.0 - ratio4, 0.0, 1.0);
                    rangeAtten = rangeAtten * (window * window);
                }
                attenuation = rangeAtten;

                if (lightType == 2u) {
                    // Spot light angular falloff (KHR_lights_punctual spec)
                    let spotDir = normalize(light.spotParams.xyz);
                    let cd = dot(spotDir, -L_p);
                    let angleScale = sceneLighting.spotAngles[li].x;
                    let angleOffset = sceneLighting.spotAngles[li].y;
                    let angularAtten = clamp(cd * angleScale + angleOffset, 0.0, 1.0);
                    attenuation = attenuation * (angularAtten * angularAtten);
                }
            }

            if (attenuation > 1e-5) {
                let lightRadiance = light.colorInt.xyz * attenuation;
                var pShadow = 1.0;
                if (lightType == 0u && sceneLighting.spotAngles[li].z > 0.5) {
                    pShadow = shadowFactor;
                }
                directLighting = directLighting + evaluateDirectLobe(
                    L_p, V, N, lightRadiance, F0, diffuseColor, roughness, metallic,
                    transFactor, ccFactor, ccRoughness, sheenColor, sheenRoughness
                ) * pShadow;
            }
        }
    }

    // 3. True HDR Image-Based Lighting (IBL):
    //    - Diffuse Irradiance via L2 Spherical Harmonics (9 coefficients)
    //    - Specular IBL via Compute GGX Prefiltered Cubemap + Split-Sum 2D BRDF LUT
    let rotN = rotateY(N, envRot);
    let R = rotateY(reflect(-V, N), envRot);

    let shIrradiance = evaluateSHIrradiance(rotN);
    let brdfSample = textureSampleLevel(brdfLutTexture, defaultSampler, vec2<f32>(NdotV, roughness), 0.0).rg;
    let F_ibl = fresnelSchlickRoughness(NdotV, F0, roughness);
    let kD_ibl = (vec3<f32>(1.0) - F_ibl) * (1.0 - metallic);

    var ambientDiffuse = kD_ibl * diffuseColor * shIrradiance * ao * (1.0 - transFactor);
    var ambientSpecular = vec3<f32>(0.0);

    if ((flags & 8192u) != 0u) {
        let maxMip = max(sceneLighting.renderParams.x - 1.0, 0.0);
        let specMip = clamp(roughness * maxMip, 0.0, maxMip);
        let prefilteredEnv = textureSampleLevel(envCubemap, defaultSampler, R, specMip).rgb;
        let specBrdf = F_ibl * brdfSample.x + vec3<f32>(brdfSample.y);
        ambientSpecular = prefilteredEnv * specBrdf * matUbo.reflectionIntensity * ao;

        if (ccFactor > 0.001) {
            let R_cc = rotateY(reflect(-V, geomN), envRot);
            let ccMip = clamp(ccRoughness * maxMip, 0.0, maxMip);
            let ccEnv = textureSampleLevel(envCubemap, defaultSampler, R_cc, ccMip).rgb;
            let ccBrdf = textureSampleLevel(brdfLutTexture, defaultSampler, vec2<f32>(NdotV, ccRoughness), 0.0).rg;
            let ccFresnel = vec3<f32>(0.04) * ccBrdf.x + vec3<f32>(ccBrdf.y);
            ambientSpecular = ambientSpecular * (1.0 - ccFresnel * ccFactor) + ccEnv * ccFresnel * ccFactor * matUbo.reflectionIntensity;
        }
    } else {
        ambientSpecular = (F_ibl * brdfSample.x + vec3<f32>(brdfSample.y)) * shIrradiance * (1.0 - roughness) * 0.25 * ao;
    }

    ambientDiffuse = ambientDiffuse * diurnalAmbientScale;
    ambientSpecular = ambientSpecular * diurnalAmbientScale;

    var finalColor = directLighting + ambientDiffuse + ambientSpecular + emissive;

    // Hoist screen-space derivatives into uniform control flow before non-uniform transFactor branch.
    let screenDim = max(sceneLighting.gridParams.xy, vec2<f32>(1.0, 1.0));
    let screenUV = in.clipPos.xy / screenDim;
    let refrDpdx = dpdx(screenUV);
    let refrDpdy = dpdy(screenUV);

    var outAlpha = baseColor.a;
    if (transFactor > 0.001) {
        let fresnelGlass = fresnelSchlick(NdotV, vec3<f32>(f0_dielectric)).r;

        // Physical Snell's law refraction in view space
        let refrIor = max(matUbo.ior, 1.0);
        let eta = 1.0 / refrIor;
        let viewPos = sceneLighting.viewMatrix * vec4<f32>(in.worldPos, 1.0);
        let viewZ = max(-viewPos.z, 0.1);
        let N_view = normalize((sceneLighting.viewMatrix * vec4<f32>(N, 0.0)).xyz);
        let thickness = max(matUbo.thicknessFactor, 0.05);

        // Chromatic dispersion & refraction UV offset
        let refrOffset = (N_view.xy * (1.0 - eta)) * (thickness / viewZ) * 0.12;
        let refrUV = clamp(screenUV + refrOffset, vec2<f32>(0.001), vec2<f32>(0.999));

        var bgSample = textureSampleGrad(opaqueColorTexture, defaultSampler, refrUV, refrDpdx, refrDpdy).rgb;

        // Beer-Lambert volume attenuation (KHR_materials_volume)
        if (matUbo.thicknessFactor > 0.0 && matUbo.attenuationDistance < 1e8) {
            let attDist = max(matUbo.attenuationDistance, 1e-4);
            let attColor = clamp(matUbo.attenuationColor.rgb, vec3<f32>(1e-4), vec3<f32>(1.0));
            let extinction = -log(attColor) / attDist;
            let opticalPath = matUbo.thicknessFactor / max(NdotV, 0.1);
            let transmittance = exp(-extinction * opticalPath);
            bgSample = bgSample * transmittance;
        }

        let transmittedColor = bgSample * baseColor.rgb;
        let transMix = (1.0 - fresnelGlass) * transFactor;
        finalColor = mix(directLighting + ambientSpecular, transmittedColor, transMix) + emissive;
        outAlpha = 1.0;
    }

    let camPosW = sceneLighting.cameraPos.xyz;
    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        finalColor = finalColor + calculateRainStreaks(in.worldPos, camPosW, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        finalColor = finalColor + calculateSnowFlakes(in.worldPos, camPosW, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        finalColor = calculateFogEffect(finalColor, in.worldPos, camPosW, sunL, solarElev, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        finalColor = calculateSmokePlumes(finalColor, in.worldPos, camPosW, weatherTime, windStrength, weatherIntensity);
    }

    // Multi-Mode Debug Visualization & Screen-Space Bayer Dithered LOD Heatmap
    if (debugViewMode == 1u) {
        let baseLodCol = evaluateLodHeatmapColor(lodLevel);
        let nextLodCol = evaluateLodHeatmapColor(min(lodLevel + 1u, 3u));
        let ditherPick = select(0.0, 1.0, lodTransition > bayerDither4x4(in.clipPos.xy));
        let heatCol = mix(baseLodCol, nextLodCol, ditherPick);
        let ndotlShade = 0.32 + 0.68 * max(dot(N, sunL), 0.0);
        finalColor = mix(heatCol * ndotlShade, finalColor, 0.18);
    } else if (debugViewMode == 2u) {
        finalColor = N * 0.5 + vec3<f32>(0.5);
    } else if (debugViewMode == 3u) {
        finalColor = vec3<f32>(ao, roughness, metallic);
    } else if (debugViewMode == 4u) {
        let grid = floor(uvBase * 16.0);
        let parity = f32((i32(grid.x) + i32(grid.y)) & 1);
        let uvTint = vec3<f32>(fract(uvBase.x), fract(uvBase.y), 0.55);
        finalColor = mix(uvTint * 0.35, uvTint * 1.15, parity);
    }

    return vec4<f32>(finalColor, outAlpha);
}
