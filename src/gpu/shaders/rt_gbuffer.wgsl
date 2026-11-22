#include "common/uniforms.wgsl"

struct SceneLightingMinUniforms {
    shIrradiance : array<vec4<f32>, 9>,
    lights : array<vec4<f32>, 48>,
    spotAngles : array<vec4<f32>, 16>,
    clusterMasks : array<vec4<u32>, 256>,
    gridParams : vec4<f32>,
    envParams : vec4<f32>,
    viewMatrix : mat4x4<f32>,
    weatherParams : vec4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    renderParams : vec4<f32>,
};

@group(0) @binding(0) var<uniform> sceneLighting : SceneLightingMinUniforms;
@group(1) @binding(0) var<uniform> drawUbo : PerDrawUniforms;
@group(2) @binding(0) var<uniform> matUbo : MaterialUniforms;
@group(2) @binding(1) var defaultSampler : sampler;
@group(2) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(2) @binding(3) var metallicRoughnessTexture : texture_2d<f32>;
@group(2) @binding(4) var normalTexture : texture_2d<f32>;
@group(2) @binding(10) var<storage, read> morphDeltas : array<vec4<f32>>;
@group(3) @binding(0) var<storage, read> jointMatrices : array<mat4x4<f32>>;

struct GBufferVsOut {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) worldPos : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) tangent : vec4<f32>,
    @location(3) uv : vec2<f32>,
};

struct GBufferFsOut {
    @location(0) normalRoughness : vec4<f32>,
    @location(1) albedoMetallic : vec4<f32>,
};

@vertex
fn vs_gbuffer(in : VertexInput) -> GBufferVsOut {
    var out : GBufferVsOut;
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

@fragment
fn fs_gbuffer(in : GBufferVsOut) -> GBufferFsOut {
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

    let uvBase = transformUv(in.uv, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
    let uvMr = transformUv(in.uv, matUbo.metallicRoughnessUvTransform, matUbo.uvRotations0.y);
    let uvNorm = transformUv(in.uv, matUbo.normalUvTransform, matUbo.uvRotations0.z);

    let baseSample = textureSampleGrad(baseColorTexture, defaultSampler, uvBase, duvBase1, duvBase2);
    let mrSample = textureSampleGrad(metallicRoughnessTexture, defaultSampler, uvMr, duvMr1, duvMr2);
    let normSample = textureSampleGrad(normalTexture, defaultSampler, uvNorm, duvNorm1, duvNorm2);

    var baseColor = matUbo.baseColorFactor;
    if ((flags & 1u) != 0u) {
        baseColor = baseColor * baseSample;
    }
    if (matUbo.alphaMode == 1u && baseColor.a < matUbo.alphaCutoff) {
        discard;
    }

    var geomN = normalize(in.normal);
    let viewDir = normalize(sceneLighting.cameraPos.xyz - in.worldPos);
    if ((flags & 512u) != 0u && dot(geomN, viewDir) < 0.0) {
        geomN = -geomN;
    }

    var N = geomN;
    if ((flags & 4u) != 0u && (flags & 1024u) != 0u) {
        let orthoT = in.tangent.xyz - geomN * dot(geomN, in.tangent.xyz);
        let orthoLenSq = dot(orthoT, orthoT);
        var T = vec3<f32>(1.0, 0.0, 0.0);
        var B = vec3<f32>(0.0, 1.0, 0.0);
        if (abs(in.tangent.w) > 0.001 && orthoLenSq > 1e-6) {
            T = orthoT * inverseSqrt(orthoLenSq);
            B = normalize(cross(geomN, T) * sign(in.tangent.w));
        } else {
            let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(geomN.y) < 0.999);
            T = normalize(cross(up, geomN));
            B = normalize(cross(geomN, T));
        }
        let tbn = mat3x3<f32>(T, B, geomN);
        let nTex = normSample.xyz * 2.0 - 1.0;
        let scaledN = normalize(vec3<f32>(nTex.xy * matUbo.normalScale, nTex.z));
        N = normalize(tbn * scaledN);
    }

    var roughness = matUbo.roughnessFactor;
    var metallic = matUbo.metallicFactor;
    if ((flags & 2u) != 0u) {
        roughness = roughness * mrSample.g;
        metallic = metallic * mrSample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    var out : GBufferFsOut;
    out.normalRoughness = vec4<f32>(N, roughness);
    out.albedoMetallic = vec4<f32>(clamp(baseColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), metallic);
    return out;
}
