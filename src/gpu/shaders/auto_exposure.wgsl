// GPU-side log-average luminance reduction and temporal Auto-Exposure (Eye Adaptation).

struct AutoExposureUniforms {
    params : vec4<f32>,    // x: deltaTime, y: adaptSpeed, z: minExposure, w: maxExposure
    keyParams : vec4<f32>, // x: keyValue (0.18..0.35), y: enabled (>0.5), z: baseExposure, w: unused
};

@group(0) @binding(0) var<uniform> ubo : AutoExposureUniforms;
@group(0) @binding(1) var hdrSrcTex : texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> exposureState : vec4<f32>; // x: adaptedExposure, y: avgLum, z: init, w: frame

var<workgroup> sharedLogLum : array<f32, 64>;

@compute @workgroup_size(8, 8, 1)
fn cs_auto_exposure(@builtin(local_invocation_id) lid : vec3<u32>,
                    @builtin(local_invocation_index) lidx : u32) {
    let dims = textureDimensions(hdrSrcTex);
    let stepPos = vec2<f32>(vec2<u32>(lid.x, lid.y)) + vec2<f32>(0.5);
    let sampleCoord = clamp(vec2<i32>((stepPos / 8.0) * vec2<f32>(dims)), vec2<i32>(0), vec2<i32>(dims) - vec2<i32>(1));
    let col = max(textureLoad(hdrSrcTex, sampleCoord, 0).rgb, vec3<f32>(0.0));
    let lum = dot(col, vec3<f32>(0.2126, 0.7152, 0.0722));
    sharedLogLum[lidx] = log2(clamp(lum, 1e-3, 64.0));

    workgroupBarrier();

    for (var stride : u32 = 32u; stride > 0u; stride = stride >> 1u) {
        if (lidx < stride) {
            sharedLogLum[lidx] += sharedLogLum[lidx + stride];
        }
        workgroupBarrier();
    }

    if (lidx == 0u) {
        let avgLog = sharedLogLum[0] / 64.0;
        let avgLum = exp2(avgLog);
        let key = max(ubo.keyParams.x, 0.05);
        let targetExp = clamp(key / max(avgLum, 0.02), max(ubo.params.z, 0.1), max(ubo.params.w, 0.2));

        let initialized = exposureState.z > 0.5;
        let prevExp = select(ubo.keyParams.z, exposureState.x, initialized);
        let rate = clamp(1.0 - exp(-max(ubo.params.x, 0.001) * max(ubo.params.y, 0.5)), 0.01, 1.0);
        let nextExp = select(ubo.keyParams.z, mix(prevExp, targetExp, rate), ubo.keyParams.y > 0.5);

        exposureState = vec4<f32>(nextExp, avgLum, 1.0, exposureState.w + 1.0);
    }
}
