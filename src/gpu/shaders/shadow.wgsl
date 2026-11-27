#include "common/uniforms.wgsl"

@group(0) @binding(0) var<uniform> drawUbo : PerDrawUniforms;
@group(1) @binding(0) var<uniform> matUbo : MaterialUniforms;
@group(1) @binding(1) var defaultSampler : sampler;
@group(1) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(1) @binding(10) var<storage, read> morphDeltas : array<vec4<f32>>;
@group(2) @binding(0) var<storage, read> jointMatrices : array<mat4x4<f32>>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_shadow(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    var localPos = in.position;

    let morphCount = min(drawUbo.morphTargetCount, 8u);
    if (morphCount > 0u && in.vertexIndex < drawUbo.vertexCount && (drawUbo.drawFlags & 262144u) != 0u) {
        for (var t : u32 = 0u; t < morphCount; t = t + 1u) {
            let wVec = select(drawUbo.morphWeights0, drawUbo.morphWeights1, t >= 4u);
            let wComp = t & 3u;
            var w = wVec.x;
            if (wComp == 1u) { w = wVec.y; }
            else if (wComp == 2u) { w = wVec.z; }
            else if (wComp == 3u) { w = wVec.w; }
            if (abs(w) > 1e-6) {
                let baseIdx = (t * drawUbo.vertexCount + in.vertexIndex) * 3u;
                localPos = localPos + w * morphDeltas[baseIdx].xyz;
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
    }

    let modelMat = resolveWorldModel(in, drawUbo.model);
    let worldPos = modelMat * vec4<f32>(localPos, 1.0);
    out.clipPos = drawUbo.lightViewProj * worldPos;
    out.uv = transformUv(in.uv, matUbo.baseColorUvTransform, matUbo.uvRotations0.x);
    return out;
}

@fragment
fn fs_shadow(in : VertexOutput) {
    var alpha = matUbo.baseColorFactor.a;
    if ((matUbo.materialFlags & 1u) != 0u) { // MaterialFlags::HasBaseColorTex
        let texColor = textureSample(baseColorTexture, defaultSampler, in.uv);
        alpha = alpha * texColor.a;
    }

    // alphaMode: 0 = Opaque, 1 = Mask, 2 = Blend
    if (matUbo.alphaMode == 1u) {
        if (alpha < matUbo.alphaCutoff) {
            discard;
        }
    } else if (matUbo.alphaMode == 2u) {
        if (alpha < max(matUbo.alphaCutoff, 0.5)) {
            discard;
        }
    }
}
