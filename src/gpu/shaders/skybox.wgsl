struct SkyboxUniforms {
    invViewProj : mat4x4<f32>,
    sunDir : vec4<f32>,
    cameraPos : vec4<f32>,
    params : vec4<f32>, // x = envRotation, y = blurLod, z = useEnvCubemap, w = mode
};

@group(0) @binding(0) var<uniform> ubo : SkyboxUniforms;
@group(0) @binding(1) var envSampler : sampler;
@group(0) @binding(2) var envCubemap : texture_cube<f32>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) viewRay : vec3<f32>,
};

fn rotateY(v : vec3<f32>, angle : f32) -> vec3<f32> {
    let c = cos(angle);
    let s = sin(angle);
    return vec3<f32>(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

@vertex
fn vs_sky(@builtin(vertex_index) vIdx : u32) -> VertexOutput {
    var out : VertexOutput;
    // Generate fullscreen triangle covering NDC [-1, 1]
    let x = f32(i32(vIdx << 1u) & 2) * 2.0 - 1.0;
    let y = f32(i32(vIdx & 2u)) * 2.0 - 1.0;
    // Z = 1.0 places the triangle on the far plane (WebGPU 0..1 depth)
    out.clipPos = vec4<f32>(x, y, 1.0, 1.0);

    let unprojected = ubo.invViewProj * vec4<f32>(x, y, 1.0, 1.0);
    out.viewRay = unprojected.xyz / unprojected.w - ubo.cameraPos.xyz;
    return out;
}

@fragment
fn fs_sky(in : VertexOutput) -> @location(0) vec4<f32> {
    let dir = rotateY(normalize(in.viewRay), ubo.params.x);
    let lod = ubo.params.y;
    let solarElev = ubo.sunDir.w;
    let weatherMode = ubo.cameraPos.w;
    let weatherIntensity = ubo.params.w;

    // Diurnal sky tint & brightness scale (Day -> Sunset Golden Hour -> Starry Night)
    let dayScale = vec3<f32>(1.0, 1.0, 1.0);
    let duskScale = vec3<f32>(0.90, 0.58, 0.45);
    let nightScale = vec3<f32>(0.08, 0.12, 0.24);
    var diurnalScale = dayScale;
    if (solarElev >= 0.25) {
        diurnalScale = mix(duskScale, dayScale, clamp((solarElev - 0.25) / 0.75, 0.0, 1.0));
    } else if (solarElev >= -0.15) {
        diurnalScale = mix(nightScale, duskScale, clamp((solarElev + 0.15) / 0.40, 0.0, 1.0));
    } else {
        diurnalScale = nightScale;
    }

    var skyColor = vec3<f32>(0.0);
    if (ubo.params.z > 0.5) {
        skyColor = textureSampleLevel(envCubemap, envSampler, dir, lod).rgb * diurnalScale;
    } else {
        let sun = rotateY(normalize(ubo.sunDir.xyz), ubo.params.x);

        // Physically plausible HDR atmospheric sky gradient fallback
        let zenithColor = vec3<f32>(0.14, 0.36, 0.74) * diurnalScale;
        let horizonColor = vec3<f32>(0.66, 0.80, 0.94) * diurnalScale;
        let groundColor = vec3<f32>(0.24, 0.20, 0.18) * diurnalScale;

        let elevation = dir.y;
        var sky = vec3<f32>(0.0);
        if (elevation >= 0.0) {
            let t = pow(clamp(1.0 - elevation, 0.0, 1.0), 2.2);
            sky = mix(zenithColor, horizonColor, t);
        } else {
            let t = clamp(-elevation * 4.0, 0.0, 1.0);
            sky = mix(horizonColor, groundColor, t);
        }

        let sunDot = max(dot(dir, sun), 0.0);
        let sunDisk = pow(sunDot, 1500.0) * 10.0;
        let sunCorona = pow(sunDot, 24.0) * 0.55;
        let orbColor = select(vec3<f32>(0.65, 0.80, 1.0) * 0.25, vec3<f32>(1.0, 0.96, 0.88), solarElev >= 0.0);
        let sunGlow = orbColor * (sunDisk + sunCorona);
        skyColor = sky + sunGlow;
    }

    // Nocturnal starfield when sun is below horizon
    if (solarElev < 0.05 && dir.y > 0.05) {
        let nightFade = clamp((0.05 - solarElev) * 4.0, 0.0, 1.0) * smoothstep(0.05, 0.35, dir.y);
        let starCell = floor(dir * 220.0);
        let starHash = fract(sin(dot(starCell, vec3<f32>(127.1, 311.7, 74.7))) * 43758.5453);
        let star = pow(max(starHash - 0.985, 0.0) / 0.015, 2.0) * 2.5 * nightFade;
        skyColor = skyColor + vec3<f32>(0.88, 0.94, 1.0) * star;
    }

    // Atmospheric Fog & Smoke horizon veil
    if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        let fogHorizon = smoothstep(0.65, -0.1, dir.y) * clamp(weatherIntensity * 0.78, 0.0, 0.92);
        let fogCol = vec3<f32>(0.74, 0.81, 0.89) * diurnalScale;
        skyColor = mix(skyColor, fogCol, fogHorizon);
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        let smokeHorizon = smoothstep(0.75, -0.15, dir.y) * clamp(weatherIntensity * 0.82, 0.0, 0.94);
        let smokeCol = vec3<f32>(0.18, 0.19, 0.21);
        skyColor = mix(skyColor, smokeCol, smokeHorizon);
    }

    return vec4<f32>(skyColor, 1.0);
}
