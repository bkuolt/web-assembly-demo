// Shared 48-byte GPU primitive AABB record for compute culling & ray picking
#pragma once

struct GpuBoundingBox {
    minBound: vec4<f32>,
    maxBound: vec4<f32>,
    nodeIndex: u32,
    primitiveIndex: u32,
    instanceCount: u32,
    pad1: u32,
};

