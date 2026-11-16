// Resolves single-sampled Depth24Plus into linear positive view-space depth (R32Float).

struct DepthLinearizeUniforms {
    invProj : mat4x4<f32>,
    params : vec4<f32>, // x: nearZ, y: farZ, z: width, w: height
};

@group(0) @binding(0) var<uniform> ubo : DepthLinearizeUniforms;
@group(0) @binding(1) var depthTex : texture_depth_2d;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_linearize(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

@fragment
fn fs_linearize(in : VsOut) -> @location(0) vec4<f32> {
    let dims = textureDimensions(depthTex);
    let maxCoord = vec2<i32>(i32(dims.x) - 1, i32(dims.y) - 1);
    let coords = clamp(vec2<i32>(in.pos.xy), vec2<i32>(0), maxCoord);
    let rawDepth = textureLoad(depthTex, coords, 0);

    if (rawDepth >= 0.99995) {
        return vec4<f32>(1000000.0, 1.0, 0.0, 1.0);
    }

    let ndc = vec4<f32>(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, rawDepth, 1.0);
    let viewH = ubo.invProj * ndc;
    let linearZ = clamp(-viewH.z / max(viewH.w, 1e-6), max(ubo.params.x, 0.001), max(ubo.params.y, 100.0));
    return vec4<f32>(linearZ, rawDepth, 0.0, 1.0);
}
