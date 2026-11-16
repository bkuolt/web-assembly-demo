// Edge-preserving bilateral filter smoothing half-res GTAO without leaking across depth edges.

struct GtaoUniforms {
    invProj : mat4x4<f32>,
    params : vec4<f32>,    // x: radiusWorld, y: intensity, z: bias, w: maxRange
    texelInfo : vec4<f32>, // x: fullW, y: fullH, z: halfW, w: halfH
};

@group(0) @binding(0) var<uniform> ubo : GtaoUniforms;
@group(0) @binding(1) var linearDepthTex : texture_2d<f32>;
@group(0) @binding(2) var rawGtaoTex : texture_2d<f32>;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_gtao_blur(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

@fragment
fn fs_gtao_blur(in : VsOut) -> @location(0) vec4<f32> {
    let aoDims = vec2<i32>(textureDimensions(rawGtaoTex));
    let fullDims = vec2<i32>(textureDimensions(linearDepthTex));
    let centerAoCoord = clamp(vec2<i32>(in.pos.xy), vec2<i32>(0), aoDims - vec2<i32>(1));
    let centerFullCoord = clamp(centerAoCoord * 2, vec2<i32>(0), fullDims - vec2<i32>(1));
    let centerZ = textureLoad(linearDepthTex, centerFullCoord, 0).r;

    if (centerZ > 500000.0) {
        return vec4<f32>(1.0, 1.0, 1.0, 1.0);
    }

    var sumAo : f32 = 0.0;
    var sumW : f32 = 0.0;
    let sharpScale = 1.0 / max(centerZ * 0.08, 0.04);

    for (var dy = -2; dy <= 2; dy = dy + 1) {
        for (var dx = -2; dx <= 2; dx = dx + 1) {
            let tapAoCoord = clamp(centerAoCoord + vec2<i32>(dx, dy), vec2<i32>(0), aoDims - vec2<i32>(1));
            let tapFullCoord = clamp(tapAoCoord * 2, vec2<i32>(0), fullDims - vec2<i32>(1));
            let tapAo = textureLoad(rawGtaoTex, tapAoCoord, 0).r;
            let tapZ = textureLoad(linearDepthTex, tapFullCoord, 0).r;

            let spatialW = exp(-f32(dx * dx + dy * dy) * 0.28);
            let depthDiff = abs(tapZ - centerZ) * sharpScale;
            let depthW = exp(-depthDiff * depthDiff);
            let w = spatialW * depthW;

            sumAo += tapAo * w;
            sumW += w;
        }
    }

    let finalAo = clamp(sumAo / max(sumW, 1e-4), 0.1, 1.0);
    return vec4<f32>(finalAo, finalAo, finalAo, 1.0);
}
