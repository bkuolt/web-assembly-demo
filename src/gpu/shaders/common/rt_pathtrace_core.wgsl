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
@group(0) @binding(1) var envSampler : sampler;
@group(0) @binding(2) var envCubemap : texture_cube<f32>;
@group(0) @binding(3) var<storage, read_write> pathAccum : array<vec4<f32>>;

fn ptRotateY(v : vec3<f32>, angle : f32) -> vec3<f32> {
    let c = cos(angle);
    let s = sin(angle);
    return vec3<f32>(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

fn ptFresnelSchlick(cosTheta : f32, F0 : vec3<f32>) -> vec3<f32> {
    return F0 + (vec3<f32>(1.0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn ptSampleGgxHalf(u1 : f32, u2 : f32, N : vec3<f32>, roughness : f32) -> vec3<f32> {
    let a = max(roughness * roughness, 0.002);
    let a2 = a * a;
    let phi = 6.28318530718 * u1;
    let cosTheta = sqrt(max(0.0, (1.0 - u2) / (1.0 + (a2 - 1.0) * u2)));
    let sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    let hTan = vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return rtSafeNormalize(rtBuildOrthonormalBasis(N) * hTan, N);
}

fn ptEvalCookTorrance(N : vec3<f32>, V : vec3<f32>, L : vec3<f32>, F0 : vec3<f32>,
                      diffCol : vec3<f32>, roughness : f32) -> vec3<f32> {
    let NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) {
        return vec3<f32>(0.0);
    }
    let NdotV = max(dot(N, V), 0.001);
    let H = rtSafeNormalize(V + L, N);
    let NdotH = max(dot(N, H), 0.0);
    let VdotH = max(dot(V, H), 0.0);

    let a = max(roughness * roughness, 0.002);
    let a2 = a * a;
    let dDen = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    let D = a2 / (3.14159265359 * max(dDen * dDen, 1e-6));

    let ggxL = NdotL + sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
    let ggxV = NdotV + sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
    let Vis = 1.0 / max(ggxL * ggxV, 0.0001);

    let F = ptFresnelSchlick(VdotH, F0);
    let kD = (vec3<f32>(1.0) - F);
    let spec = F * (D * Vis);
    let diff = kD * diffCol * 0.318309886;
    return (diff + spec) * NdotL;
}

@compute @workgroup_size(8, 8)
fn cs_pathtrace(@builtin(global_invocation_id) gid : vec3<u32>) {
    let w = u32(ubo.params0.x);
    let h = u32(ubo.params0.y);
    if (gid.x >= w || gid.y >= h) {
        return;
    }

    let sampleIdx = u32(ubo.params2.y);
    var rng = rtPcgHash((gid.x * 1973u) ^ (gid.y * 9277u) ^ ((sampleIdx + 1u) * 26699u) ^ 0x9E3779B9u);

    let jitter = vec2<f32>(rtRand01(&rng), rtRand01(&rng));
    let uv = (vec2<f32>(gid.xy) + jitter) / vec2<f32>(f32(w), f32(h));
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;
    let nearP4 = ubo.invViewProj * vec4<f32>(ndcX, ndcY, 0.0, 1.0);
    let farP4 = ubo.invViewProj * vec4<f32>(ndcX, ndcY, 1.0, 1.0);
    let nearW = select(nearP4.w, 1e-6, abs(nearP4.w) < 1e-6);
    let farW = select(farP4.w, 1e-6, abs(farP4.w) < 1e-6);
    let nearP = nearP4.xyz / nearW;
    let farP = farP4.xyz / farW;

    var ro = ubo.cameraPos.xyz;
    var rd = rtSafeNormalize(farP - nearP, vec3<f32>(0.0, 0.0, -1.0));

    let maxBounces = clamp(u32(ubo.params2.x), 1u, 6u);
    let envRot = ubo.params1.w;
    let solarElev = ubo.params3.y;
    let ambientScale = evaluateDiurnalAmbientScale(solarElev);
    let sunL = rtSafeNormalize(ubo.sunDir.xyz, vec3<f32>(0.35, 1.0, 0.25));
    let softness = clamp(ubo.sunDir.w, 0.0, 0.15);
    let sunRad = evaluateDiurnalSunRadiance(solarElev);
    let maxMip = max(ubo.params3.z - 1.0, 0.0);

    var radiance = vec3<f32>(0.0);
    var throughput = vec3<f32>(1.0);

    for (var bounce : u32 = 0u; bounce <= maxBounces; bounce = bounce + 1u) {
        let hit = rtTraceClosest(ro, rd, 0.001, 500.0);
        if (!hit.hit) {
            let skyMip = select(1.0, 0.0, bounce == 0u);
            let skyCol = textureSampleLevel(envCubemap, envSampler, ptRotateY(rd, envRot), min(skyMip, maxMip)).rgb;
            radiance = radiance + throughput * skyCol * ambientScale;
            break;
        }

        let mat = rtMaterials[hit.materialIdx];
        let baseCol = rtSampleBaseColor(mat, hit.uv);
        let emissive = rtSampleEmissive(mat, hit.uv);
        let metallic = clamp(mat.pbrParams.x, 0.0, 1.0);
        let roughness = clamp(mat.pbrParams.y, 0.04, 1.0);
        let isTransmissive = (mat.flagsAlpha.w != 0u);

        radiance = radiance + throughput * emissive;

        let N = hit.worldNormal;
        let V = -rd;
        let F0 = mix(vec3<f32>(0.04), baseCol, metallic);
        let diffCol = baseCol * (1.0 - metallic);
        let hitOrigin = hit.worldPos + N * clamp(0.0002 + hit.t * 0.0004, 0.0002, 0.015);

        // Next Event Estimation (Primary Sun + Punctual Lights)
        if (!isTransmissive) {
            if (ubo.params2.w > 0.5) {
                let bSun = rtBuildOrthonormalBasis(sunL);
                let su1 = rtRand01(&rng);
                let su2 = rtRand01(&rng);
                let sr = sqrt(su1) * softness;
                let sa = su2 * 6.2831853;
                let shadowDir = rtSafeNormalize(sunL + bSun[0] * (sr * cos(sa)) + bSun[1] * (sr * sin(sa)), sunL);
                if (dot(N, shadowDir) > 0.0 && !rtTraceAnyHit(hitOrigin, shadowDir, 0.001, 500.0)) {
                    radiance = radiance + throughput * ptEvalCookTorrance(N, V, shadowDir, F0, diffCol, roughness) * sunRad;
                }
            }

            let pCount = min(u32(ubo.params3.x), 4u);
            for (var li : u32 = 0u; li < pCount; li = li + 1u) {
                let lp = ubo.lightsPos[li];
                let lType = u32(lp.w + 0.5);
                var L_p = rtSafeNormalize(lp.xyz, vec3<f32>(0.0, 1.0, 0.0));
                var maxD = 500.0;
                var atten = 1.0;
                if (lType != 0u) {
                    let delta = lp.xyz - hitOrigin;
                    let distSq = dot(delta, delta);
                    maxD = sqrt(max(distSq, 1e-6));
                    L_p = delta / maxD;
                    atten = 1.0 / max(distSq, 0.01);
                    let range = ubo.lightsCol[li].w;
                    if (range > 0.0) {
                        let ratio = maxD / range;
                        let w = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
                        atten = atten * w * w;
                    }
                }
                if (atten > 1e-4 && dot(N, L_p) > 0.0) {
                    if (!rtTraceAnyHit(hitOrigin, L_p, 0.001, max(maxD - 0.01, 0.002))) {
                        radiance = radiance + throughput * ptEvalCookTorrance(N, V, L_p, F0, diffCol, roughness) *
                                                  (ubo.lightsCol[li].xyz * atten);
                    }
                }
            }
        }

        if (bounce == maxBounces) {
            break;
        }

        // Sample next path segment
        let NdotV = max(dot(N, V), 0.001);
        let F_est = ptFresnelSchlick(NdotV, F0);
        if (isTransmissive) {
            let fDielectric = F_est.r;
            if (rtRand01(&rng) < fDielectric) {
                ro = hitOrigin;
                rd = normalize(reflect(-V, N));
                throughput = throughput * baseCol;
            } else {
                let eta = 1.0 / max(mat.pbrParams.w, 1.0);
                let refr = refract(-V, N, eta);
                ro = hit.worldPos - N * 0.003;
                rd = select(reflect(-V, N), normalize(refr), length(refr) > 1e-4);
                throughput = throughput * baseCol;
            }
        } else {
            let fMax = max(F_est.r, max(F_est.g, F_est.b));
            let specProb = clamp(mix(fMax, 1.0, metallic), 0.15, 0.92);
            if (rtRand01(&rng) < specProb) {
                let H = ptSampleGgxHalf(rtRand01(&rng), rtRand01(&rng), N, roughness);
                let nextRd = normalize(reflect(-V, H));
                if (dot(nextRd, N) <= 0.0) {
                    break;
                }
                let VdotH = max(dot(V, H), 0.001);
                let NdotH = max(dot(N, H), 0.001);
                let NdotL = max(dot(N, nextRd), 0.001);
                let F = ptFresnelSchlick(VdotH, F0);
                let a = max(roughness * roughness, 0.002);
                let a2 = a * a;
                let ggxL = NdotL + sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
                let ggxV = NdotV + sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
                let G_vis = (4.0 * NdotL * NdotV) / max(ggxL * ggxV, 0.001);
                let specWeight = (F * G_vis * VdotH) / max(NdotH * NdotV, 0.001);
                throughput = throughput * min(specWeight / specProb, vec3<f32>(4.0));
                rd = nextRd;
            } else {
                let nextRd = rtSampleCosineHemisphere(rtRand01(&rng), rtRand01(&rng), N);
                throughput = throughput * (diffCol / (1.0 - specProb));
                rd = nextRd;
            }
            ro = hitOrigin;
        }

        // Clamp fireflies & Russian Roulette after bounce 1
        throughput = min(throughput, vec3<f32>(12.0));
        if (bounce >= 1u) {
            let surv = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (rtRand01(&rng) > surv) {
                break;
            }
            throughput = throughput / surv;
        }
    }

    let safeRad = select(vec3<f32>(0.0), min(radiance, vec3<f32>(24.0)),
                         all(radiance >= vec3<f32>(0.0)) && all(radiance < vec3<f32>(1e5)));
    let pxIdx = gid.y * w + gid.x;
    if (pxIdx < arrayLength(&pathAccum)) {
        if (sampleIdx == 0u) {
            pathAccum[pxIdx] = vec4<f32>(safeRad, 1.0);
        } else {
            pathAccum[pxIdx] = pathAccum[pxIdx] + vec4<f32>(safeRad, 1.0);
        }
    }
}
