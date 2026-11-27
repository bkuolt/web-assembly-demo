struct SkyboxUniforms {
    invViewProj : mat4x4<f32>,
    sunDir : vec4<f32>,
    cameraPos : vec4<f32>,
    params : vec4<f32>, // x = envRotation, y = blurLod, z = useEnvCubemap, w = weatherIntensity
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

fn skyHash21(p : vec2<f32>) -> f32 {
    let h = fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453123);
    return h;
}

fn skyValueNoise(p : vec2<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (vec2<f32>(3.0) - 2.0 * f);
    let a = skyHash21(i);
    let b = skyHash21(i + vec2<f32>(1.0, 0.0));
    let c = skyHash21(i + vec2<f32>(0.0, 1.0));
    let d = skyHash21(i + vec2<f32>(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

fn skyFbmClouds(p : vec2<f32>) -> f32 {
    var sum = 0.0;
    var amp = 0.5;
    var coord = p;
    let rot = mat2x2<f32>(vec2<f32>(0.80, -0.60), vec2<f32>(0.60, 0.80));
    for (var i = 0; i < 4; i = i + 1) {
        sum += amp * skyValueNoise(coord);
        coord = rot * coord * 2.04 + vec2<f32>(11.3, 7.1);
        amp *= 0.5;
    }
    return sum;
}

fn evaluateSkyDiurnalScale(solarElev : f32) -> f32 {
    let day = smoothstep(-0.15, 0.35, solarElev);
    return mix(0.06, 1.0, day);
}

fn henyeyGreenstein(cosTheta : f32, g : f32) -> f32 {
    let g2 = g * g;
    let denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-3);
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(denom, 1.5));
}

@vertex
fn vs_sky(@builtin(vertex_index) vIdx : u32) -> VertexOutput {
    let positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );
    let p = positions[vIdx];
    var out : VertexOutput;
    out.clipPos = vec4<f32>(p, 0.999999, 1.0);
    let farWorld = ubo.invViewProj * vec4<f32>(p, 1.0, 1.0);
    let safeW = select(farWorld.w, 1e-6, abs(farWorld.w) < 1e-6);
    let worldPos = farWorld.xyz / safeW;
    out.viewRay = worldPos - ubo.cameraPos.xyz;
    return out;
}

@fragment
fn fs_sky(in : VertexOutput) -> @location(0) vec4<f32> {
    let rayLenSq = dot(in.viewRay, in.viewRay);
    let rawDir = select(vec3<f32>(0.0, 0.0, -1.0), in.viewRay * inverseSqrt(rayLenSq), rayLenSq > 1e-12);
    let rotDir = rotateY(rawDir, ubo.params.x);
    let sunLenSq = dot(ubo.sunDir.xyz, ubo.sunDir.xyz);
    let sunVector = select(vec3<f32>(0.35, 1.0, 0.25), ubo.sunDir.xyz * inverseSqrt(sunLenSq), sunLenSq > 1e-12);
    let solarElev = ubo.sunDir.w;
    let weatherMode = ubo.cameraPos.w;
    let weatherIntensity = clamp(ubo.params.w, 0.0, 1.5);
    let dayFactor = smoothstep(-0.12, 0.38, solarElev);
    let twilight = exp(-abs(solarElev - 0.04) * 14.0);
    let diurnalScale = evaluateSkyDiurnalScale(solarElev);

    var skyColor = vec3<f32>(0.0);
    if (ubo.params.z > 0.5) {
        skyColor = textureSampleLevel(envCubemap, envSampler, rotDir, ubo.params.y).rgb * diurnalScale;
    } else {
        // Rayleigh + Henyey-Greenstein Mie Atmospheric Scattering
        let upAmount = clamp(rawDir.y, 0.0, 1.0);
        let cosSun = dot(rotDir, sunVector);
        let rayleighZenith = vec3<f32>(0.11, 0.32, 0.78);
        let rayleighHorizon = mix(vec3<f32>(0.52, 0.71, 0.92), vec3<f32>(1.15, 0.48, 0.22), twilight);
        var rayleighCol = mix(rayleighHorizon, rayleighZenith, pow(upAmount, 0.48)) * max(dayFactor, 0.08);

        // Mie forward scattering solar aureole + crisp solar disc
        let miePhase = henyeyGreenstein(cosSun, 0.78);
        let mieTint = mix(vec3<f32>(1.0, 0.94, 0.84), vec3<f32>(1.35, 0.56, 0.18), twilight);
        let sunDisc = smoothstep(0.99945, 0.99985, cosSun) * 16.0;
        let atmosphere = rayleighCol + mieTint * (miePhase * 1.15 + sunDisc * 0.65) * smoothstep(-0.08, 0.12, solarElev);

        // Nocturnal twilight horizon + procedural starfield
        let nightBase = mix(vec3<f32>(0.015, 0.025, 0.055), vec3<f32>(0.004, 0.008, 0.020), upAmount);
        skyColor = mix(nightBase, atmosphere, dayFactor);

        // Animated FBM volumetric cloud deck across upper hemisphere
        if (rawDir.y > 0.015) {
            let cloudProj = rawDir.xz / (rawDir.y + 0.16);
            let coverageBias = select(0.49, 0.32, weatherMode == 1.0 || weatherMode == 2.0);
            let f0 = skyFbmClouds(cloudProj * 1.35 + vec2<f32>(0.03, 0.015));
            let detail = skyValueNoise(cloudProj * 4.5 + vec2<f32>(f0 * 1.6));
            let cloudDensity = smoothstep(coverageBias, coverageBias + 0.34, f0 * 0.78 + detail * 0.22);
            let horizonCloudFade = smoothstep(0.015, 0.22, rawDir.y);

            let forwardGlow = pow(clamp(cosSun * 0.5 + 0.5, 0.0, 1.0), 6.0);
            let baseCloudLit = mix(vec3<f32>(0.42, 0.47, 0.56), vec3<f32>(0.98, 0.97, 0.96), max(dayFactor, 0.15));
            let cloudSunBoost = mix(vec3<f32>(1.0, 0.92, 0.78), vec3<f32>(1.35, 0.62, 0.28), twilight);
            var cloudColor = baseCloudLit * diurnalScale + cloudSunBoost * forwardGlow * (0.45 * dayFactor);
            if (weatherMode == 1.0 || weatherMode == 2.0) {
                cloudColor *= mix(1.0, 0.45, clamp(weatherIntensity, 0.0, 1.0));
            }
            skyColor = mix(skyColor, cloudColor, cloudDensity * horizonCloudFade * 0.88);
        }

        // Sharp twinkling starfield at night above clouds
        if (solarElev < 0.08 && rawDir.y > 0.05) {
            let starCell = floor(rotDir * 240.0);
            let starH = fract(sin(dot(starCell, vec3<f32>(12.9898, 78.233, 37.719))) * 43758.5453);
            if (starH > 0.9965) {
                let nightFade = 1.0 - smoothstep(-0.18, 0.08, solarElev);
                skyColor = skyColor + vec3<f32>(0.95, 0.97, 1.0) * nightFade * ((starH - 0.9965) * 320.0);
            }
        }
    }

    // Weather horizon atmosphere (Rain, Fog & Smoke)
    if ((weatherMode == 1.0 || weatherMode == 2.0) && weatherIntensity > 0.01) {
        let stormTint = vec3<f32>(0.18, 0.21, 0.26) * max(diurnalScale, 0.20);
        let stormBlend = clamp(weatherIntensity * 0.42, 0.0, 0.75) * (1.0 - smoothstep(-0.1, 0.65, rawDir.y) * 0.45);
        skyColor = mix(skyColor, stormTint, stormBlend);
    } else if (weatherMode == 3.0 && weatherIntensity > 0.01) {
        let fogHorizon = smoothstep(0.65, -0.15, rawDir.y);
        let fogCol = vec3<f32>(0.68, 0.73, 0.80) * max(diurnalScale, 0.14);
        skyColor = mix(skyColor, fogCol, fogHorizon * clamp(weatherIntensity * 0.75, 0.0, 0.96));
    } else if (weatherMode == 4.0 && weatherIntensity > 0.01) {
        let smokeHorizon = smoothstep(0.75, -0.1, rawDir.y);
        let smokeCol = vec3<f32>(0.24, 0.23, 0.22) * max(diurnalScale, 0.22);
        skyColor = mix(skyColor, smokeCol, smokeHorizon * clamp(weatherIntensity * 0.85, 0.0, 0.95));
    }

    return vec4<f32>(skyColor, 1.0);
}
