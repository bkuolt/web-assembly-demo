#pragma once

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
@group(0) @binding(3) var gbufferAlbedoMetallic : texture_2d<f32>;
@group(0) @binding(4) var envSampler : sampler;
@group(0) @binding(5) var envCubemap : texture_cube<f32>;
@group(0) @binding(6) var reflOutTex : texture_storage_2d<rgba16float, write>;

fn rtRotateY(v : vec3<f32>, angle : f32) -> vec3<f32> {
    let c = cos(angle);
    let s = sin(angle);
    return vec3<f32>(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

fn rtSampleGgxHalfVector(u1 : f32, u2 : f32, N : vec3<f32>, roughness : f32) -> vec3<f32> {
    let a = max(roughness * roughness, 0.002);
    let a2 = a * a;
    let phi = 6.28318530718 * u1;
    let cosTheta = sqrt(max(0.0, (1.0 - u2) / (1.0 + (a2 - 1.0) * u2)));
    let sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    let hTan = vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return normalize(rtBuildOrthonormalBasis(N) * hTan);
}

fn rtFresnelSchlickRough(cosTheta : f32, F0 : vec3<f32>, roughness : f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

@compute @workgroup_size(8, 8)
fn cs_rt_reflections(@builtin(global_invocation_id) gid : vec3<u32>) {
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
        textureStore(reflOutTex, vec2<i32>(gid.xy), vec4<f32>(0.0));
        return;
    }

    let nr = textureLoad(gbufferNormalRoughness, fullPx, 0);
    let N = normalize(nr.xyz);
    let roughness = clamp(nr.w, 0.04, 1.0);
    let rtWeight = (1.0 - smoothstep(0.40, 0.65, roughness)) * clamp(ubo.params1.z, 0.0, 2.0);
    if (rtWeight <= 0.005) {
        textureStore(reflOutTex, vec2<i32>(gid.xy), vec4<f32>(0.0));
        return;
    }

    let am = textureLoad(gbufferAlbedoMetallic, fullPx, 0);
    let baseAlbedo = am.rgb;
    let metallic = clamp(am.a, 0.0, 1.0);
    let F0 = mix(vec3<f32>(0.04), baseAlbedo, metallic);

    let uv = (vec2<f32>(fullPx) + vec2<f32>(0.5)) / vec2<f32>(fullDims);
    let worldPos = rtWorldPosFromDepth(uv, depth, ubo.invViewProj);
    let V = normalize(ubo.cameraPos.xyz - worldPos);
    let viewDist = length(ubo.cameraPos.xyz - worldPos);
    let origin = worldPos + N * clamp(0.003 + viewDist * 0.001, 0.003, 0.04);

    var rng = rtPcgHash((gid.x * 3571u) ^ (gid.y * 7919u) ^ (u32(ubo.cameraPos.w) * 104729u));
    let u1 = rtRand01(&rng);
    let u2 = rtRand01(&rng) * 0.85; // Bias against extreme grazing tail fireflies
    let H = select(N, rtSampleGgxHalfVector(u1, u2, N, roughness), roughness > 0.06);
    var reflDir = reflect(-V, H);
    if (dot(reflDir, N) <= 0.001) {
        reflDir = reflect(-V, N);
    }
    reflDir = normalize(reflDir);

    let envRot = ubo.params1.w;
    let solarElev = ubo.params3.y;
    let ambientScale = evaluateDiurnalAmbientScale(solarElev);
    let maxMip = max(ubo.params3.z - 1.0, 0.0);

    let hit = rtTraceClosest(origin, reflDir, 0.002, 500.0);
    var radiance = vec3<f32>(0.0);
    if (!hit.hit) {
        let rotR = rtRotateY(reflDir, envRot);
        let mip = clamp(roughness * maxMip, 0.0, maxMip);
        radiance = textureSampleLevel(envCubemap, envSampler, rotR, mip).rgb * ambientScale;
    } else {
        let mat = rtMaterials[hit.materialIdx];
        let hitBase = rtSampleBaseColor(mat, hit.uv);
        let hitEmissive = rtSampleEmissive(mat, hit.uv);
        let hitRough = clamp(mat.pbrParams.y, 0.04, 1.0);
        let hitMetal = clamp(mat.pbrParams.x, 0.0, 1.0);
        let hitDiff = hitBase * (1.0 - hitMetal);
        let hitF0 = mix(vec3<f32>(0.04), hitBase, hitMetal);
        let hitOrigin = hit.worldPos + hit.worldNormal * 0.004;

        var hitCol = hitEmissive;
        let sunL = normalize(ubo.sunDir.xyz);
        if (ubo.params2.w > 0.5) {
            let ndl = max(dot(hit.worldNormal, sunL), 0.0);
            if (ndl > 0.0) {
                if (!rtTraceAnyHit(hitOrigin, sunL, 0.002, 500.0)) {
                    let sunRad = evaluateDiurnalSunRadiance(solarElev);
                    let hVec = normalize(sunL - reflDir);
                    let ndh = max(dot(hit.worldNormal, hVec), 0.0);
                    let spec = pow(ndh, mix(64.0, 4.0, hitRough)) * 0.25;
                    hitCol = hitCol + (hitDiff * 0.318309886 + hitF0 * spec) * sunRad * ndl;
                }
            }
        }

        let pCount = min(u32(ubo.params3.x), 4u);
        for (var li : u32 = 0u; li < pCount; li = li + 1u) {
            let lp = ubo.lightsPos[li];
            let lType = u32(lp.w + 0.5);
            var L_p = normalize(lp.xyz);
            var atten = 1.0;
            if (lType != 0u) {
                let toL = lp.xyz - hit.worldPos;
                let distSq = dot(toL, toL);
                let dist = sqrt(max(distSq, 1e-6));
                L_p = toL / dist;
                atten = 1.0 / max(distSq, 0.01);
                let range = ubo.lightsCol[li].w;
                if (range > 0.0) {
                    let ratio = dist / range;
                    let w = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
                    atten = atten * w * w;
                }
            }
            let ndlP = max(dot(hit.worldNormal, L_p), 0.0);
            if (ndlP > 0.0 && atten > 1e-4) {
                hitCol = hitCol + hitDiff * 0.318309886 * ubo.lightsCol[li].xyz * (atten * ndlP);
            }
        }

        // Hemisphere IBL approximation at hit point
        let hitEnv = textureSampleLevel(envCubemap, envSampler, rtRotateY(hit.worldNormal, envRot), maxMip).rgb;
        hitCol = hitCol + hitDiff * hitEnv * ambientScale * 0.85;
        radiance = hitCol;
    }

    let NdotV = max(dot(N, V), 0.001);
    let F_spec = rtFresnelSchlickRough(NdotV, F0, roughness);
    let finalRefl = min(radiance * F_spec * rtWeight, vec3<f32>(16.0));
    textureStore(reflOutTex, vec2<i32>(gid.xy), vec4<f32>(finalRefl, 1.0));
}
