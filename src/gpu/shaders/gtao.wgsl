// Half-resolution Horizon Ground-Truth Ambient Occlusion (GTAO) from linear R32Float depth.

struct GtaoUniforms {
    invProj : mat4x4<f32>,
    params : vec4<f32>,    // x: radiusWorld, y: intensity, z: bias, w: maxRange
    texelInfo : vec4<f32>, // x: fullW, y: fullH, z: halfW, w: halfH
};

@group(0) @binding(0) var<uniform> ubo : GtaoUniforms;
@group(0) @binding(1) var linearDepthTex : texture_2d<f32>;

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_gtao(@builtin(vertex_index) vid : u32) -> VsOut {
    let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[vid];
    var out : VsOut;
    out.pos = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

fn loadViewPosAtFullCoord(fullCoord : vec2<i32>, fullDims : vec2<i32>) -> vec4<f32> {
    let clamped = clamp(fullCoord, vec2<i32>(0), fullDims - vec2<i32>(1));
    let linearZ = textureLoad(linearDepthTex, clamped, 0).r;
    let uv = (vec2<f32>(clamped) + vec2<f32>(0.5)) / vec2<f32>(fullDims);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    let farV = ubo.invProj * ndc;
    let ray = farV.xyz / max(-farV.z, 1e-4);
    return vec4<f32>(ray * linearZ, linearZ);
}

fn interleavedGradientNoise(pixelPos : vec2<f32>) -> f32 {
    let magic = vec3<f32>(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(pixelPos, magic.xy)));
}

@fragment
fn fs_gtao(in : VsOut) -> @location(0) vec4<f32> {
    let fullDims = vec2<i32>(textureDimensions(linearDepthTex));
    let centerCoord = clamp(vec2<i32>(in.uv * vec2<f32>(fullDims)), vec2<i32>(0), fullDims - vec2<i32>(1));
    let centerInfo = loadViewPosAtFullCoord(centerCoord, fullDims);
    let centerZ = centerInfo.w;

    if (centerZ > 500000.0) {
        return vec4<f32>(1.0, 1.0, 0.0, 1.0);
    }
    let centerPos = centerInfo.xyz;

    // Reconstruct view-space normal via closest finite differences (zero silhouette streaks)
    let pL = loadViewPosAtFullCoord(centerCoord + vec2<i32>(-1, 0), fullDims);
    let pR = loadViewPosAtFullCoord(centerCoord + vec2<i32>(1, 0), fullDims);
    let pU = loadViewPosAtFullCoord(centerCoord + vec2<i32>(0, -1), fullDims);
    let pD = loadViewPosAtFullCoord(centerCoord + vec2<i32>(0, 1), fullDims);

    let dLeft = centerPos - pL.xyz;
    let dRight = pR.xyz - centerPos;
    let dUp = centerPos - pU.xyz;
    let dDown = pD.xyz - centerPos;

    let ddx = select(dRight, dLeft, abs(pL.w - centerZ) < abs(pR.w - centerZ));
    let ddy = select(dDown, dUp, abs(pU.w - centerZ) < abs(pD.w - centerZ));
    let nLenSq = dot(cross(ddx, ddy), cross(ddx, ddy));
    let viewNormal = select(vec3<f32>(0.0, 0.0, 1.0), normalize(cross(ddy, ddx)), nLenSq > 1e-8);
    let viewDir = normalize(-centerPos);

    // Screen-space radius proportional to world radius & camera distance
    let worldRadius = clamp(ubo.params.x * 20.0, 0.15, 2.5);
    let projScale = abs(ubo.invProj[1][1]);
    let pixelRadius = clamp((worldRadius / max(centerZ * projScale, 0.08)) * f32(fullDims.y) * 0.5, 4.0, 42.0);

    let noise = interleavedGradientNoise(in.pos.xy);
    let angleOffset = noise * 3.14159265;
    let stepJitter = fract(noise * 7.31);

    let kDirs = 4;
    let kSteps = 3;
    var occlusionAccum : f32 = 0.0;
    let falloffRange = max(worldRadius, 0.35);

    for (var d = 0; d < kDirs; d = d + 1) {
        let phi = angleOffset + (f32(d) / f32(kDirs)) * 6.2831853;
        let dirPx = vec2<f32>(cos(phi), sin(phi));

        for (var s = 0; s < kSteps; s = s + 1) {
            let frac = (f32(s) + 0.35 + stepJitter * 0.65) / f32(kSteps);
            let tapCoord = centerCoord + vec2<i32>(round(dirPx * frac * pixelRadius));
            let tapInfo = loadViewPosAtFullCoord(tapCoord, fullDims);
            if (tapInfo.w > 500000.0) {
                continue;
            }
            let diff = tapInfo.xyz - centerPos;
            let dist = length(diff);
            if (dist < 1e-3 || dist > falloffRange * 2.0) {
                continue;
            }
            let horizonCos = dot(diff / dist, viewNormal);
            let rangeAtten = clamp(1.0 - (dist / (falloffRange * 2.0)), 0.0, 1.0);
            let bias = ubo.params.z * 8.0;
            occlusionAccum += max(0.0, horizonCos - bias) * rangeAtten * rangeAtten;
        }
    }

    let rawAo = 1.0 - (occlusionAccum / f32(kDirs * kSteps)) * ubo.params.y * 1.45;
    let ao = clamp(rawAo, 0.12, 1.0);
    return vec4<f32>(ao, ao, ao, 1.0);
}
