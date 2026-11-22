struct RtCompositeUniforms {
    params : vec4<f32>, // x = fullW, y = fullH, z = traceW, w = traceH
};

@group(0) @binding(0) var<uniform> compUbo : RtCompositeUniforms;
@group(0) @binding(1) var linearSampler : sampler;
@group(0) @binding(2) var reflTex : texture_2d<f32>;
@group(0) @binding(3) var gbufferDepth : texture_depth_2d;
@group(0) @binding(4) var<storage, read> pathAccum : array<vec4<f32>>;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_rt_fullscreen(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

@fragment
fn fs_rt_composite_refl(in : VsOut) -> @location(0) vec4<f32> {
    let fullDims = vec2<i32>(i32(compUbo.params.x), i32(compUbo.params.y));
    let fullPx = clamp(vec2<i32>(in.pos.xy), vec2<i32>(0), fullDims - vec2<i32>(1));
    let depth = textureLoad(gbufferDepth, fullPx, 0);
    if (depth >= 0.99999) {
        return vec4<f32>(0.0);
    }
    let refl = textureSampleLevel(reflTex, linearSampler, in.uv, 0.0).rgb;
    return vec4<f32>(refl, 1.0);
}

@fragment
fn fs_rt_pathtrace_present(in : VsOut) -> @location(0) vec4<f32> {
    let w = max(u32(compUbo.params.x), 1u);
    let h = max(u32(compUbo.params.y), 1u);
    let px = clamp(vec2<u32>(in.pos.xy), vec2<u32>(0u), vec2<u32>(w - 1u, h - 1u));
    let idx = px.y * w + px.x;
    let accum = pathAccum[idx];
    let col = accum.rgb / max(accum.a, 1.0);
    return vec4<f32>(max(col, vec3<f32>(0.0)), 1.0);
}
