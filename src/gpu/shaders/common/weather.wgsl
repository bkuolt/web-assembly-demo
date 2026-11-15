#pragma once

// ============================================================================
// Procedural Weather Effects (Atmospheric Rain & Snow WGSL Subroutines)
// ============================================================================

fn hash31(p : vec3<f32>) -> f32 {
    let h = fract(sin(dot(p, vec3<f32>(12.9898, 78.233, 45.164))) * 43758.5453123);
    return h;
}

fn calculateRainRipples(pos : vec2<f32>, time : f32) -> vec3<f32> {
    let scale = 14.0;
    let uvCoord = pos * scale;
    let iuv = floor(uvCoord);
    let fuv = fract(uvCoord);
    var rippleVec = vec2<f32>(0.0);
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let g = vec2<f32>(f32(x), f32(y));
            let h = fract(sin(dot(iuv + g, vec2<f32>(127.1, 311.7))) * 43758.5453);
            let t = fract(time * 2.2 + h);
            let center = g + vec2<f32>(0.5) + vec2<f32>(sin(h * 6.28), cos(h * 6.28)) * 0.25;
            let d = length(fuv - center);
            let wave = sin(d * 24.0 - t * 14.0) * exp(-d * 4.5) * (1.0 - t);
            let dir = normalize(fuv - center + 1e-4);
            rippleVec = rippleVec + dir * wave * 0.5;
        }
    }
    return normalize(vec3<f32>(rippleVec, 1.0));
}

fn calculateRainStreaks(worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let rayDist = length(worldPos - cameraPos);
    var streakAccum : f32 = 0.0;
    for (var i = 0; i < 2; i = i + 1) {
        let scale = select(4.0, 8.0, i == 1);
        let speed = select(22.0, 32.0, i == 1);
        let windVec = vec3<f32>(wind * 3.5, -speed, wind * 1.5);
        let samplePos = (worldPos - windVec * time) * scale;
        let cell = floor(vec3<f32>(samplePos.x, samplePos.y * 0.08, samplePos.z));
        let f = fract(vec3<f32>(samplePos.x, samplePos.y * 0.08, samplePos.z));
        let h = hash31(cell);
        let dX = abs(f.x - 0.5);
        let dZ = abs(f.z - 0.5);
        let dY = abs(f.y - 0.5);
        let streak = smoothstep(0.08, 0.0, dX) * smoothstep(0.08, 0.0, dZ) * smoothstep(0.45, 0.0, dY);
        let activeDrop = step(0.70, h);
        streakAccum = streakAccum + streak * activeDrop;
    }
    let mist = (1.0 - exp(-rayDist * 0.08)) * 0.15 * intensity;
    let mistColor = vec3<f32>(0.65, 0.72, 0.82) * mist;
    let streakColor = vec3<f32>(0.85, 0.92, 1.0) * (streakAccum * intensity * 0.75);
    return streakColor + mistColor;
}

fn calculateSnowFlakes(worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let rayDist = length(worldPos - cameraPos);
    var flakeAccum : f32 = 0.0;
    for (var i = 0; i < 2; i = i + 1) {
        let scale = select(3.5, 6.5, i == 1);
        let fallSpeed = select(2.4, 3.8, i == 1);
        let freq = select(2.0, 3.2, i == 1);
        let wobbleX = sin(time * freq + worldPos.y * 2.5 + f32(i)) * 0.35;
        let wobbleZ = cos(time * (freq * 0.85) + worldPos.x * 2.0) * 0.3;
        let driftVec = vec3<f32>(wind * 1.8 * time + wobbleX, -fallSpeed * time, wind * 0.8 * time + wobbleZ);
        let samplePos = (worldPos - driftVec) * scale;
        let cell = floor(samplePos);
        let f = fract(samplePos) - 0.5;
        let h = hash31(cell + vec3<f32>(17.3, 41.7, 83.1));
        let dist = length(f);
        let flake = smoothstep(0.20, 0.02, dist) * step(0.66, h);
        flakeAccum = flakeAccum + flake;
    }
    let winterFog = (1.0 - exp(-rayDist * 0.06)) * 0.18 * intensity;
    let winterFogColor = vec3<f32>(0.82, 0.88, 0.96) * winterFog;
    let flakeColor = vec3<f32>(0.96, 0.98, 1.0) * (flakeAccum * intensity * 0.85);
    return flakeColor + winterFogColor;
}

fn noise3D(p : vec3<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let n000 = hash31(i + vec3<f32>(0.0, 0.0, 0.0));
    let n100 = hash31(i + vec3<f32>(1.0, 0.0, 0.0));
    let n010 = hash31(i + vec3<f32>(0.0, 1.0, 0.0));
    let n110 = hash31(i + vec3<f32>(1.0, 1.0, 0.0));
    let n001 = hash31(i + vec3<f32>(0.0, 0.0, 1.0));
    let n101 = hash31(i + vec3<f32>(1.0, 0.0, 1.0));
    let n011 = hash31(i + vec3<f32>(0.0, 1.0, 1.0));
    let n111 = hash31(i + vec3<f32>(1.0, 1.0, 1.0));
    let nx00 = mix(n000, n100, u.x);
    let nx10 = mix(n010, n110, u.x);
    let nx01 = mix(n001, n101, u.x);
    let nx11 = mix(n011, n111, u.x);
    let nxy0 = mix(nx00, nx10, u.y);
    let nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

fn fbm3D(p : vec3<f32>) -> f32 {
    var val = 0.0;
    var amp = 0.5;
    var pos = p;
    for (var i = 0; i < 4; i = i + 1) {
        val = val + amp * noise3D(pos);
        pos = pos * 2.03 + vec3<f32>(13.7, 29.3, 7.1);
        amp = amp * 0.5;
    }
    return val;
}

fn evaluateDiurnalSunRadiance(solarElev : f32) -> vec3<f32> {
    let noonSun = vec3<f32>(1.0, 0.98, 0.95) * 3.14159265359;
    let goldenSun = vec3<f32>(1.0, 0.48, 0.18) * 2.2;
    let nightMoon = vec3<f32>(0.28, 0.44, 0.78) * 0.55;
    if (solarElev >= 0.35) {
        let t = clamp((solarElev - 0.35) / 0.65, 0.0, 1.0);
        return mix(goldenSun, noonSun, t);
    } else if (solarElev >= -0.10) {
        let t = clamp((solarElev + 0.10) / 0.45, 0.0, 1.0);
        return mix(nightMoon, goldenSun, t);
    }
    return nightMoon;
}

fn evaluateDiurnalAmbientScale(solarElev : f32) -> vec3<f32> {
    let dayScale = vec3<f32>(1.0, 1.0, 1.0);
    let duskScale = vec3<f32>(0.82, 0.60, 0.52);
    let nightScale = vec3<f32>(0.16, 0.22, 0.38);
    if (solarElev >= 0.25) {
        let t = clamp((solarElev - 0.25) / 0.75, 0.0, 1.0);
        return mix(duskScale, dayScale, t);
    } else if (solarElev >= -0.15) {
        let t = clamp((solarElev + 0.15) / 0.40, 0.0, 1.0);
        return mix(nightScale, duskScale, t);
    }
    return nightScale;
}

fn calculateFogEffect(sceneColor : vec3<f32>, worldPos : vec3<f32>, cameraPos : vec3<f32>, sunDir : vec3<f32>, solarElev : f32, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let toFrag = worldPos - cameraPos;
    let dist = length(toFrag);
    let viewRay = toFrag / max(dist, 1e-4);

    // Volumetric wind-drifting mist banks + barometric height falloff
    let windOffset = vec3<f32>(wind * time * 0.6, -time * 0.08, wind * time * 0.3);
    let mistNoise = fbm3D(worldPos * 0.28 - windOffset);
    let heightFalloff = exp(-max(worldPos.y + 2.0, 0.0) * 0.12);
    let density = (0.045 + 0.055 * heightFalloff * (0.65 + 0.7 * mistNoise)) * intensity;
    let fogFactor = clamp(1.0 - exp(-dist * density), 0.0, 0.96);

    // Anisotropic Mie forward scattering (Tyndall sun/moon glow in fog)
    let cosTheta = max(dot(viewRay, sunDir), 0.0);
    let mieScatter = pow(cosTheta, 6.0) * 0.45;

    let dayFogColor = vec3<f32>(0.74, 0.81, 0.89);
    let duskFogColor = vec3<f32>(0.82, 0.58, 0.46);
    let nightFogColor = vec3<f32>(0.12, 0.17, 0.28);
    var baseFogColor = dayFogColor;
    if (solarElev < 0.25 && solarElev >= -0.15) {
        baseFogColor = mix(nightFogColor, duskFogColor, clamp((solarElev + 0.15) / 0.40, 0.0, 1.0));
    } else if (solarElev < -0.15) {
        baseFogColor = nightFogColor;
    } else {
        baseFogColor = mix(duskFogColor, dayFogColor, clamp((solarElev - 0.25) / 0.75, 0.0, 1.0));
    }

    let scatterTint = select(vec3<f32>(0.35, 0.50, 0.80), vec3<f32>(1.0, 0.82, 0.55), solarElev >= 0.0);
    let finalFogColor = baseFogColor + scatterTint * mieScatter;
    return mix(sceneColor, finalFogColor, fogFactor);
}

fn calculateSmokePlumes(sceneColor : vec3<f32>, worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let dist = length(worldPos - cameraPos);
    // Rising turbulent thermal updraft + horizontal wind shear
    let flow = vec3<f32>(wind * time * 1.1, -time * 1.35, wind * time * 0.45);
    let p1 = worldPos * 0.42 + flow;
    let warp = vec3<f32>(
        fbm3D(p1 + vec3<f32>(5.2, 1.3, 2.8)),
        fbm3D(p1 + vec3<f32>(1.7, 9.2, 4.1)),
        fbm3D(p1 + vec3<f32>(8.3, 2.8, 6.4))
    ) * 1.35;
    let rawTurbulence = fbm3D(p1 + warp);

    // Billowing smoke plume envelope (denser near ground/mid-height, curling upward)
    let heightEnvelope = exp(-max(worldPos.y + 3.0, 0.0) * 0.08);
    let distEnvelope = 1.0 - exp(-dist * 0.085);
    let plumeShape = smoothstep(0.32, 0.78, rawTurbulence);
    let smokeOpacity = clamp((plumeShape * 0.75 + distEnvelope * 0.35) * heightEnvelope * intensity, 0.0, 0.94);

    // Charcoal soot color with warm subsurface ember glow at low altitude
    let sootColor = vec3<f32>(0.15, 0.16, 0.18);
    let ashHighlight = vec3<f32>(0.34, 0.35, 0.38);
    let emberGlow = vec3<f32>(0.95, 0.38, 0.08) * exp(-max(worldPos.y + 1.5, 0.0) * 0.45) * pow(plumeShape, 2.5) * 0.55 * clamp(intensity, 0.0, 1.5);
    let smokeColor = mix(sootColor, ashHighlight, rawTurbulence) + emberGlow;

    return mix(sceneColor, smokeColor, smokeOpacity);
}

