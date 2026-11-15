struct UiUniforms {
    proj : mat4x4<f32>,
    sdfParams : vec4<f32>, // x = smoothing scale, y = threshold (0.5), z = unused, w = unused
};

@group(0) @binding(0) var<uniform> ubo : UiUniforms;
@group(0) @binding(1) var atlasSampler : sampler;
@group(0) @binding(2) var atlasTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) pos : vec2<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) color : vec4<f32>,
    @location(3) isSdf : f32,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
    @location(1) color : vec4<f32>,
    @location(2) isSdf : f32,
};

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    out.clipPos = ubo.proj * vec4<f32>(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    out.isSdf = in.isSdf;
    return out;
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4<f32> {
    let dist = textureSample(atlasTexture, atlasSampler, in.uv).a;
    // Uniform-control-flow derivative for GPU-side SDF antialiasing regardless of scale
    let fw = max(fwidth(dist), 0.002) * max(ubo.sdfParams.x, 1.0);
    let edge = ubo.sdfParams.y;
    let sdfAlpha = smoothstep(edge - fw, edge + fw, dist);
    let alpha = select(in.color.a * dist, in.color.a * sdfAlpha, in.isSdf > 0.5);
    return vec4<f32>(in.color.rgb, alpha);
}
