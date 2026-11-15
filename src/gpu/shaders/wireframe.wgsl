#include "common/uniforms.wgsl"

@group(1) @binding(0) var<uniform> drawUbo : PerDrawUniforms;
@group(2) @binding(10) var<storage, read> morphDeltas : array<vec4<f32>>;
@group(3) @binding(0) var<storage, read> jointMatrices : array<mat4x4<f32>>;

@vertex
fn vs_main(in : VertexInput) -> @builtin(position) vec4<f32> {
    var localPos = in.position;
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
    return drawUbo.viewProj * modelMat * vec4<f32>(localPos, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(0.22, 0.74, 0.97, 1.0);
}
