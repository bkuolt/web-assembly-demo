// Screen-space dilated selection silhouette composite blended into HDR color before tonemapping.

struct OutlineCompUniforms {
    rimColor : vec4<f32>,      // rgb: golden HDR rim color, a: max alpha
    interiorColor : vec4<f32>, // rgb: subtle interior tint, a: interior alpha
    params : vec4<f32>,        // x: widthPx (2.5), y..w: unused
};

@group(0) @binding(0) var<uniform> ubo : OutlineCompUniforms;
@group(0) @binding(1) var maskTex : texture_2d<f32>;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_outline_comp(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

@fragment
fn fs_outline_comp(in : VsOut) -> @location(0) vec4<f32> {
    let dims = vec2<i32>(textureDimensions(maskTex));
    let coord = clamp(vec2<i32>(in.pos.xy), vec2<i32>(0), dims - vec2<i32>(1));
    let center = textureLoad(maskTex, coord, 0).r;

    if (center > 0.5) {
        return ubo.interiorColor;
    }

    let rOuter = i32(clamp(round(ubo.params.x), 1.0, 4.0));
    var minDist = 10.0;
    for (var dy = -rOuter; dy <= rOuter; dy = dy + 1) {
        for (var dx = -rOuter; dx <= rOuter; dx = dx + 1) {
            let tap = clamp(coord + vec2<i32>(dx, dy), vec2<i32>(0), dims - vec2<i32>(1));
            if (textureLoad(maskTex, tap, 0).r > 0.5) {
                minDist = min(minDist, length(vec2<f32>(f32(dx), f32(dy))));
            }
        }
    }

    let maxRadius = max(ubo.params.x, 1.5);
    if (minDist > maxRadius + 0.5) {
        return vec4<f32>(0.0);
    }
    let rimCov = 1.0 - smoothstep(maxRadius - 1.1, maxRadius + 0.4, minDist);
    return vec4<f32>(ubo.rimColor.rgb, ubo.rimColor.a * rimCov);
}
