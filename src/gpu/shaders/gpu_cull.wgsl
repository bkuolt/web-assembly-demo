#include "common/gpu_bounds.wgsl"

struct GpuCullUniforms {
    frustumPlanes: array<vec4<f32>, 6>,
    primitiveCount: u32,
    pad0: u32,
    pad1: u32,
    pad2: u32,
};

@group(0) @binding(0) var<uniform> uniforms: GpuCullUniforms;
@group(0) @binding(1) var<storage, read> bounds: array<GpuBoundingBox>;
@group(0) @binding(2) var<storage, read_write> visibility: array<u32>;
@group(0) @binding(3) var<storage, read_write> visibleCount: atomic<u32>;
@group(0) @binding(4) var<storage, read_write> indirectArgs: array<u32>;

@compute @workgroup_size(64)
fn cs_cull(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    if (index >= uniforms.primitiveCount) {
        return;
    }

    let b = bounds[index];
    let minB = b.minBound.xyz;
    let maxB = b.maxBound.xyz;

    var isVisible: u32 = 1u;
    for (var p: i32 = 0; p < 6; p++) {
        let plane = uniforms.frustumPlanes[p];
        let normal = plane.xyz;
        let d = plane.w;

        let pVertex = vec3<f32>(
            select(minB.x, maxB.x, normal.x >= 0.0),
            select(minB.y, maxB.y, normal.y >= 0.0),
            select(minB.z, maxB.z, normal.z >= 0.0)
        );

        if (dot(normal, pVertex) + d < 0.0) {
            isVisible = 0u;
            break;
        }
    }

    visibility[index] = isVisible;
    let baseInstances = max(bounds[index].instanceCount, 1u);
    indirectArgs[index * 5u + 1u] = select(0u, baseInstances, isVisible == 1u);
    if (isVisible == 1u) {
        atomicAdd(&visibleCount, 1u);
    }
}
