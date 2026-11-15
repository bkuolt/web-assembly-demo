@group(0) @binding(0) var srcMip : texture_2d<f32>;

@vertex
fn vs_mip(@builtin(vertex_index) vid : u32) -> @builtin(position) vec4<f32> {
    let x = f32((vid & 1u) << 2u) - 1.0;
    let y = f32((vid & 2u) << 1u) - 1.0;
    return vec4<f32>(x, y, 0.0, 1.0);
}

@fragment
fn fs_mip(@builtin(position) fragPos : vec4<f32>) -> @location(0) vec4<f32> {
    let maxCoord = max(vec2<i32>(textureDimensions(srcMip, 0)) - vec2<i32>(1, 1), vec2<i32>(0, 0));
    let base = vec2<i32>(fragPos.xy) * 2;
    let p0 = textureLoad(srcMip, clamp(base + vec2<i32>(0, 0), vec2<i32>(0), maxCoord), 0);
    let p1 = textureLoad(srcMip, clamp(base + vec2<i32>(1, 0), vec2<i32>(0), maxCoord), 0);
    let p2 = textureLoad(srcMip, clamp(base + vec2<i32>(0, 1), vec2<i32>(0), maxCoord), 0);
    let p3 = textureLoad(srcMip, clamp(base + vec2<i32>(1, 1), vec2<i32>(0), maxCoord), 0);
    return (p0 + p1 + p2 + p3) * 0.25;
}

@fragment
fn fs_mip_normal(@builtin(position) fragPos : vec4<f32>) -> @location(0) vec4<f32> {
    let maxCoord = max(vec2<i32>(textureDimensions(srcMip, 0)) - vec2<i32>(1, 1), vec2<i32>(0, 0));
    let base = vec2<i32>(fragPos.xy) * 2;
    let p0 = textureLoad(srcMip, clamp(base + vec2<i32>(0, 0), vec2<i32>(0), maxCoord), 0);
    let p1 = textureLoad(srcMip, clamp(base + vec2<i32>(1, 0), vec2<i32>(0), maxCoord), 0);
    let p2 = textureLoad(srcMip, clamp(base + vec2<i32>(0, 1), vec2<i32>(0), maxCoord), 0);
    let p3 = textureLoad(srcMip, clamp(base + vec2<i32>(1, 1), vec2<i32>(0), maxCoord), 0);
    let avg = (p0 + p1 + p2 + p3) * 0.25;
    let n = avg.xyz * 2.0 - vec3<f32>(1.0);
    let len = length(n);
    let nn = select(vec3<f32>(0.0, 0.0, 1.0), n / len, len > 1e-4);
    return vec4<f32>(nn * 0.5 + vec3<f32>(0.5), avg.w);
}
