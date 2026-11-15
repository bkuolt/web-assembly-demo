#include "common/gpu_bounds.wgsl"

struct GpuRayPickUniforms {
    rayOrigin: vec4<f32>,
    rayDirection: vec4<f32>,
};

struct GpuRayPickResult {
    hitNodeIndex: atomic<u32>,
    hitPrimitiveIndex: atomic<u32>,
    hitDistanceBits: atomic<u32>,
    hitFound: atomic<u32>,
};

@group(0) @binding(0) var<uniform> uniforms: GpuRayPickUniforms;
@group(0) @binding(1) var<storage, read> bounds: array<GpuBoundingBox>;
@group(0) @binding(2) var<storage, read_write> result: GpuRayPickResult;

fn intersectRayAabb(ro: vec3<f32>, rd: vec3<f32>, bmin: vec3<f32>, bmax: vec3<f32>, maxT: f32) -> f32 {
    var tmin : f32 = -1e30;
    var tmax : f32 = maxT;

    for (var axis : u32 = 0u; axis < 3u; axis = axis + 1u) {
        let d = rd[axis];
        if (abs(d) < 1e-8) {
            if (ro[axis] < bmin[axis] || ro[axis] > bmax[axis]) {
                return -1.0;
            }
        } else {
            let invD = 1.0 / d;
            let t0 = (bmin[axis] - ro[axis]) * invD;
            let t1 = (bmax[axis] - ro[axis]) * invD;
            tmin = max(tmin, min(t0, t1));
            tmax = min(tmax, max(t0, t1));
        }
    }

    if (tmax < max(tmin, 0.0)) {
        return -1.0;
    }
    let hitDist = select(tmin, tmax, tmin < 0.0);
    return select(-1.0, hitDist, hitDist > 0.0 && hitDist < maxT);
}

@compute @workgroup_size(64)
fn cs_pick(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let count = u32(uniforms.rayDirection.w);
    if (index >= count) {
        return;
    }

    let b = bounds[index];
    let hitDist = intersectRayAabb(uniforms.rayOrigin.xyz, uniforms.rayDirection.xyz, b.minBound.xyz, b.maxBound.xyz,
                                   uniforms.rayOrigin.w);
    if (hitDist > 0.0) {
        atomicMin(&result.hitDistanceBits, bitcast<u32>(hitDist));
    }
}

@compute @workgroup_size(64)
fn cs_resolve(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let count = u32(uniforms.rayDirection.w);
    if (index >= count) {
        return;
    }

    let minDistBits = atomicLoad(&result.hitDistanceBits);
    if (minDistBits == 0x7F7FFFFFu) {
        return;
    }

    let b = bounds[index];
    let hitDist = intersectRayAabb(uniforms.rayOrigin.xyz, uniforms.rayDirection.xyz, b.minBound.xyz, b.maxBound.xyz,
                                   uniforms.rayOrigin.w);
    if (hitDist > 0.0 && bitcast<u32>(hitDist) == minDistBits) {
        if (atomicExchange(&result.hitFound, 1u) == 0u) {
            atomicStore(&result.hitNodeIndex, b.nodeIndex);
            atomicStore(&result.hitPrimitiveIndex, b.primitiveIndex);
        }
    }
}
