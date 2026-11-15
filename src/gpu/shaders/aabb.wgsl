struct AabbUniforms {
    viewProj : mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> ubo : AabbUniforms;

struct LineVertexInput {
    @location(0) pos : vec3<f32>,
    @location(1) color : vec4<f32>,
};

struct LineVertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) color : vec4<f32>,
};

@vertex
fn vs_aabb(in : LineVertexInput) -> LineVertexOutput {
    var out : LineVertexOutput;
    out.clipPos = ubo.viewProj * vec4<f32>(in.pos, 1.0);
    out.color = in.color;
    return out;
}

@fragment
fn fs_aabb(in : LineVertexOutput) -> @location(0) vec4<f32> {
    return in.color;
}
