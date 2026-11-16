/**
 * @file js/renderer.js
 * @brief Hardware WebGPU PBR/POM/SSAO/Shadow/Animation renderer.
 */
(() => {
  'use strict';

  // ============================================================================
  // Native WebGPU 3D Renderer (WGSL Shaders & Hardware Acceleration)
  // ============================================================================
  /**
   * @typedef {Object} WebGpuPrimitiveRecord
   * @property {GPUBuffer} vertexBuffer - Interleaved vertex buffer [pos(3), norm(3), uv(2)]
   * @property {GPUBuffer|null} indexBuffer - 4-byte aligned index buffer (uint16/uint32)
   * @property {number} indexCount - Number of indices to draw
   * @property {'uint16'|'uint32'} indexFormat - WebGPU index buffer format
   * @property {number} vertexCount - Number of vertices (if unindexed)
   * @property {boolean} hasIndices - Whether primitive uses indexed draw call
   * @property {GPUBuffer} uniformBuffer - 256-byte aligned per-primitive UBO
   * @property {GPUBindGroup} bindGroup - Bind group containing UBO, sampler, and texture view
   * @property {number[]} baseColorFactor - RGBA base color multiplier [r, g, b, a]
   * @property {boolean} doubleSided - Whether primitive requires backface culling disabled
   * @property {boolean} hasTexture - Whether diffuse texture is bound
   * @property {GPUTexture|null} texture - Associated diffuse GPUTexture
   * @property {Float32Array} modelMatrix - 4x4 world transform matrix
   */

  /**
   * Hardware-accelerated native WebGPU 3D renderer.
   * Utilizes WGSL shaders, dynamic depth textures, zero-copy buffer uploads,
   * 4-byte index buffer alignment, and zero-allocation frame rendering.
   */
  const BglWebGpuRenderer = {
    adapter: null,
    device: null,
    context: null,
    canvas: null,
    format: null,
    pipeline: null,
    pipelineDoubleSided: null,
    depthTexture: null,
    depthTextureView: null,
    defaultWhiteTexture: null,
    defaultWhiteTextureView: null,
    defaultMrTexture: null,
    defaultMrTextureView: null,
    defaultNormalTexture: null,
    defaultNormalTextureView: null,
    defaultBlackTexture: null,
    defaultBlackTextureView: null,
    defaultSampler: null,
    bindGroupLayout: null,
    pipelineLayout: null,
    skinBindGroupLayout: null,
    skinnedPipelineLayout: null,
    skinnedPipeline: null,
    skinnedPipelineDoubleSided: null,
    isActive: false,
    primitives: [],
    nodes: [],
    rootNodes: [],
    skins: [],
    animations: [],
    activeAnimationIndex: -1,
    animationTime: 0.0,
    animationPlaying: true,
    animationSpeed: 1.0,
    textureCache: new Map(),

    // Preallocated scratch objects for zero-allocation 60/120 FPS rendering
    _scratchEye: new Float32Array(3),
    _scratchLightDir: new Float32Array(3),
    _scratchProj: null,
    _scratchView: null,
    _scratchViewProj: null,
    _scratchNormalMat: new Float32Array(16),
    _scratchInvMat: new Float32Array(16),
    _scratchJointMat: new Float32Array(16),
    _scratchSampleVal: new Float32Array(4),
    _scratchUboBuffer: new ArrayBuffer(512),
    _scratchUboF32: null,
    _scratchUboU32: null,

    camera: {
      target: [0.0, 0.0, 0.0],
      distance: 3.5,
      yaw: 0.45,
      pitch: 0.35,
      fov: (45 * Math.PI) / 180,
      near: 0.05,
      far: 500.0,
      minDistance: 0.0001,
      maxDistance: 1000.0,
    },

    isDragging: false,
    lastMouseX: 0,
    lastMouseY: 0,
    dragButton: 0,
    wireframe: false,
    turntable: false,
    turntableSpeed: 0.35,
    lastFrameTime: performance.now(),
    modelBounds: null,

    mat4: {
      create() {
        const out = new Float32Array(16);
        out[0] = 1; out[5] = 1; out[10] = 1; out[15] = 1;
        return out;
      },
      identity(out) {
        out.fill(0);
        out[0] = 1; out[5] = 1; out[10] = 1; out[15] = 1;
        return out;
      },
      perspective(out, fovy, aspect, near, far) {
        const f = 1.0 / Math.tan(fovy / 2);
        const nf = 1.0 / (near - far);
        out[0] = f / aspect; out[1] = 0; out[2] = 0; out[3] = 0;
        out[4] = 0; out[5] = f; out[6] = 0; out[7] = 0;
        out[8] = 0; out[9] = 0; out[10] = (far + near) * nf; out[11] = -1;
        out[12] = 0; out[13] = 0; out[14] = (2 * far * near) * nf; out[15] = 0;
        return out;
      },
      lookAt(out, eye, target, up) {
        let z0 = eye[0] - target[0], z1 = eye[1] - target[1], z2 = eye[2] - target[2];
        let len = 1 / Math.hypot(z0, z1, z2);
        z0 *= len; z1 *= len; z2 *= len;

        let x0 = up[1] * z2 - up[2] * z1, x1 = up[2] * z0 - up[0] * z2, x2 = up[0] * z1 - up[1] * z0;
        len = Math.hypot(x0, x1, x2);
        if (len > 0.00001) { len = 1 / len; x0 *= len; x1 *= len; x2 *= len; }

        let y0 = z1 * x2 - z2 * x1, y1 = z2 * x0 - z0 * x2, y2 = z0 * x1 - z1 * x0;

        out[0] = x0; out[1] = y0; out[2] = z0; out[3] = 0;
        out[4] = x1; out[5] = y1; out[6] = z1; out[7] = 0;
        out[8] = x2; out[9] = y2; out[10] = z2; out[11] = 0;
        out[12] = -(x0 * eye[0] + x1 * eye[1] + x2 * eye[2]);
        out[13] = -(y0 * eye[0] + y1 * eye[1] + y2 * eye[2]);
        out[14] = -(z0 * eye[0] + z1 * eye[1] + z2 * eye[2]);
        out[15] = 1;
        return out;
      },
      multiply(out, a, b) {
        const a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
        const a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
        const a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
        const a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];

        let b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];
        out[0] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[1] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[2] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[3] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;

        b0 = b[4]; b1 = b[5]; b2 = b[6]; b3 = b[7];
        out[4] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[5] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[6] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[7] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;

        b0 = b[8]; b1 = b[9]; b2 = b[10]; b3 = b[11];
        out[8] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[9] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[10] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[11] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;

        b0 = b[12]; b1 = b[13]; b2 = b[14]; b3 = b[15];
        out[12] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[13] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[14] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[15] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;
        return out;
      },
      normalMatrix4(out, m) {
        const a00 = m[0], a01 = m[1], a02 = m[2];
        const a10 = m[4], a11 = m[5], a12 = m[6];
        const a20 = m[8], a21 = m[9], a22 = m[10];

        const b01 = a22 * a11 - a12 * a21;
        const b11 = -a22 * a10 + a12 * a20;
        const b21 = a21 * a10 - a11 * a20;

        let det = a00 * b01 + a01 * b11 + a02 * b21;
        if (!det) {
          out[0] = 1; out[1] = 0; out[2] = 0; out[3] = 0;
          out[4] = 0; out[5] = 1; out[6] = 0; out[7] = 0;
          out[8] = 0; out[9] = 0; out[10] = 1; out[11] = 0;
          out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
          return out;
        }
        det = 1.0 / det;

        out[0] = b01 * det;
        out[1] = (-a22 * a01 + a02 * a21) * det;
        out[2] = (a12 * a01 - a02 * a11) * det;
        out[3] = 0;

        out[4] = b11 * det;
        out[5] = (a22 * a00 - a02 * a20) * det;
        out[6] = (-a12 * a00 + a02 * a10) * det;
        out[7] = 0;

        out[8] = b21 * det;
        out[9] = (-a21 * a00 + a01 * a20) * det;
        out[10] = (a11 * a00 - a01 * a10) * det;
        out[11] = 0;

        out[12] = 0;
        out[13] = 0;
        out[14] = 0;
        out[15] = 1;
        return out;
      },
      invert(out, a) {
        const a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
        const a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
        const a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
        const a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];

        const b00 = a00 * a11 - a01 * a10;
        const b01 = a00 * a12 - a02 * a10;
        const b02 = a00 * a13 - a03 * a10;
        const b03 = a01 * a12 - a02 * a11;
        const b04 = a01 * a13 - a03 * a11;
        const b05 = a02 * a13 - a03 * a12;
        const b06 = a20 * a31 - a21 * a30;
        const b07 = a20 * a32 - a22 * a30;
        const b08 = a20 * a33 - a23 * a30;
        const b09 = a21 * a32 - a22 * a31;
        const b10 = a21 * a33 - a23 * a31;
        const b11 = a22 * a33 - a23 * a32;

        let det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
        if (!det || Math.abs(det) < 1e-12) {
          out[0] = 1; out[1] = 0; out[2] = 0; out[3] = 0;
          out[4] = 0; out[5] = 1; out[6] = 0; out[7] = 0;
          out[8] = 0; out[9] = 0; out[10] = 1; out[11] = 0;
          out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
          return out;
        }
        det = 1.0 / det;

        out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
        out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
        out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
        out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
        out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
        out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
        out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
        out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
        out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
        out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
        out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
        out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
        out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
        out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
        out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
        out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
        return out;
      },
      fromRotationTranslationScale(out, q, v, s) {
        const x = q[0], y = q[1], z = q[2], w = q[3];
        const x2 = x + x, y2 = y + y, z2 = z + z;
        const xx = x * x2, xy = x * y2, xz = x * z2;
        const yy = y * y2, yz = y * z2, zz = z * z2;
        const wx = w * x2, wy = w * y2, wz = w * z2;
        const sx = s[0], sy = s[1], sz = s[2];

        out[0] = (1 - (yy + zz)) * sx;
        out[1] = (xy + wz) * sx;
        out[2] = (xz - wy) * sx;
        out[3] = 0;
        out[4] = (xy - wz) * sy;
        out[5] = (1 - (xx + zz)) * sy;
        out[6] = (yz + wx) * sy;
        out[7] = 0;
        out[8] = (xz + wy) * sz;
        out[9] = (yz - wx) * sz;
        out[10] = (1 - (xx + yy)) * sz;
        out[11] = 0;
        out[12] = v[0];
        out[13] = v[1];
        out[14] = v[2];
        out[15] = 1;
        return out;
      },
      extractFrustumPlanes(out, m) {
        // Left, Right, Bottom, Top, Near, Far normalized planes from column-major viewProj
        const setPlane = (idx, a, b, c, d) => {
          const len = Math.hypot(a, b, c) || 1.0;
          const inv = 1.0 / len;
          out[idx * 4 + 0] = a * inv;
          out[idx * 4 + 1] = b * inv;
          out[idx * 4 + 2] = c * inv;
          out[idx * 4 + 3] = d * inv;
        };
        setPlane(0, m[3] + m[0], m[7] + m[4], m[11] + m[8], m[15] + m[12]);
        setPlane(1, m[3] - m[0], m[7] - m[4], m[11] - m[8], m[15] - m[12]);
        setPlane(2, m[3] + m[1], m[7] + m[5], m[11] + m[9], m[15] + m[13]);
        setPlane(3, m[3] - m[1], m[7] - m[5], m[11] - m[9], m[15] - m[13]);
        setPlane(4, m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14]);
        setPlane(5, m[3] - m[2], m[7] - m[6], m[11] - m[10], m[15] - m[14]);
        return out;
      },
      isSphereInFrustum(planes, cx, cy, cz, radius) {
        for (let i = 0; i < 6; ++i) {
          const base = i * 4;
          const dist = planes[base] * cx + planes[base + 1] * cy + planes[base + 2] * cz + planes[base + 3];
          if (dist < -radius) return false;
        }
        return true;
      }
    },

    getShaderWgsl() {
      return `
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    normalMatrix : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    baseColorFactor : vec4<f32>,
    pbrFactors : vec4<f32>,      // x=metallic, y=roughness, z=normalScale, w=occlusionStrength
    emissiveFactor : vec4<f32>,  // xyz=emissive, w=alphaCutoff
    uvTransform : vec4<f32>,     // baseColor: xy=scale, zw=offset
    flags : vec4<u32>,           // x=texFlags, y=wireframe, z=doubleSided, w=alphaMode
    params : vec4<f32>,          // x=bumpScale, y=pomScale, z=isPomActive, w=isBumpActive
    weather : vec4<f32>,         // x=weatherMode (0=none, 1=rain, 2=snow), y=intensity, z=wind, w=timeSec
    lodParams : vec4<f32>,       // x=lodLevel, y=lodTransition, z=debugViewMode, w=lodBias
    mrUvTransform : vec4<f32>,   // metallicRoughness: xy=scale, zw=offset
    normalUvTransform : vec4<f32>,    // normal: xy=scale, zw=offset
    occlusionUvTransform : vec4<f32>, // occlusion: xy=scale, zw=offset
    emissiveUvTransform : vec4<f32>,  // emissive: xy=scale, zw=offset
    uvRotations0 : vec4<f32>,    // x=baseColorRot, y=mrRot, z=normalRot, w=occlusionRot
    uvRotations1 : vec4<f32>,    // x=emissiveRot, yzw=reserved
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var defaultSampler : sampler;
@group(0) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(3) var metallicRoughnessTexture : texture_2d<f32>;
@group(0) @binding(4) var normalTexture : texture_2d<f32>;
@group(0) @binding(5) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(6) var occlusionTexture : texture_2d<f32>;

@group(1) @binding(0) var<storage, read> jointMatrices : array<mat4x4<f32>>;

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
};

struct SkinnedVertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
    @location(4) joints : vec4<u32>,
    @location(5) weights : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) worldPos : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
};

struct MipVertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_mip(@builtin(vertex_index) vIdx : u32) -> MipVertexOutput {
    var out : MipVertexOutput;
    let x = f32(i32(vIdx << 1u) & 2) * 2.0 - 1.0;
    let y = f32(i32(vIdx & 2u)) * 2.0 - 1.0;
    out.clipPos = vec4<f32>(x, y, 0.0, 1.0);
    out.uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
    return out;
}

@fragment
fn fs_mip(in : MipVertexOutput) -> @location(0) vec4<f32> {
    return textureSample(baseColorTexture, defaultSampler, in.uv);
}

const PI : f32 = 3.141592653589793;

fn distributionGGX(NdotH : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

fn visibilitySmithGGX(NdotL : f32, NdotV : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let ggxL = NdotL + sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
    let ggxV = NdotV + sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
    return 1.0 / max(ggxL * ggxV, 0.0001);
}

fn fresnelSchlick(VdotH : f32, F0 : vec3<f32>) -> vec3<f32> {
    return F0 + (vec3<f32>(1.0) - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(NdotV : f32, F0 : vec3<f32>, roughness : f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
}

fn getTBN(worldPos : vec3<f32>, geomNormal : vec3<f32>, uv : vec2<f32>, tangent : vec4<f32>, dp1 : vec3<f32>, dp2 : vec3<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> mat3x3<f32> {
    let N = normalize(geomNormal);
    let orthoT = tangent.xyz - N * dot(N, tangent.xyz);
    let orthoLenSq = dot(orthoT, orthoT);
    if (abs(tangent.w) > 0.001 && orthoLenSq > 1e-6) {
        let T = orthoT * inverseSqrt(orthoLenSq);
        let B = normalize(cross(N, T) * sign(tangent.w));
        return mat3x3<f32>(T, B, N);
    }
    let dp2perp = cross(dp2, N);
    let dp1perp = cross(N, dp1);
    let T = dp2perp * duv1.x + dp1perp * duv2.x;
    let B = dp2perp * duv1.y + dp1perp * duv2.y;
    let maxLenSq = max(dot(T, T), dot(B, B));
    if (maxLenSq > 1e-18) {
        let invmax = inverseSqrt(maxLenSq);
        return mat3x3<f32>(normalize(T * invmax), normalize(B * invmax), N);
    }
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(N.y) < 0.999);
    let fallbackT = normalize(cross(up, N));
    let fallbackB = normalize(cross(N, fallbackT));
    return mat3x3<f32>(fallbackT, fallbackB, N);
}

fn getSurfaceHeight(uv : vec2<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> f32 {
    var height = 0.5;
    if ((ubo.flags.x & 16u) != 0u) {
        height = textureSampleGrad(occlusionTexture, defaultSampler, uv, duv1, duv2).r;
    } else if ((ubo.flags.x & 1u) != 0u) {
        let col = textureSampleGrad(baseColorTexture, defaultSampler, uv, duv1, duv2).rgb;
        height = dot(col, vec3<f32>(0.299, 0.587, 0.114));
    }
    return clamp(height, 0.0, 1.0);
}

fn parallaxOcclusionMapping(
    initialUV : vec2<f32>,
    viewDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> vec2<f32> {
    let minLayers : f32 = 8.0;
    let maxLayers : f32 = 32.0;
    let numLayers = mix(maxLayers, minLayers, clamp(abs(viewDirTangent.z), 0.0, 1.0));
    let layerDepth = 1.0 / numLayers;
    var currentLayerDepth : f32 = 0.0;

    let P = (viewDirTangent.xy / max(abs(viewDirTangent.z), 0.15)) * scale;
    let deltaUV = P / numLayers;

    var currentUV = initialUV;
    var currentHeight = getSurfaceHeight(currentUV, duv1, duv2);

    var prevUV = currentUV;
    var prevHeight = currentHeight;
    var prevLayerDepth = currentLayerDepth;

    for (var i : i32 = 0; i < 32; i = i + 1) {
        if (f32(i) >= numLayers || currentLayerDepth >= (1.0 - currentHeight)) {
            break;
        }
        prevUV = currentUV;
        prevHeight = currentHeight;
        prevLayerDepth = currentLayerDepth;

        currentUV = currentUV - deltaUV;
        currentHeight = getSurfaceHeight(currentUV, duv1, duv2);
        currentLayerDepth = currentLayerDepth + layerDepth;
    }

    let afterDepth = currentLayerDepth - (1.0 - currentHeight);
    let beforeDepth = (1.0 - prevHeight) - prevLayerDepth;
    let weight = afterDepth / max(afterDepth + beforeDepth, 0.0001);
    return mix(currentUV, prevUV, clamp(weight, 0.0, 1.0));
}

fn computeParallaxShadow(
    displacedUV : vec2<f32>,
    lightDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> f32 {
    let initialHeight = getSurfaceHeight(displacedUV, duv1, duv2);
    if (lightDirTangent.z <= 0.0) {
        return 0.25;
    }
    let numSamples : f32 = 8.0;
    let stepSize = (1.0 - initialHeight) / numSamples;
    let deltaUV = (lightDirTangent.xy / max(lightDirTangent.z, 0.15)) * scale / numSamples;

    var currentLayerHeight = initialHeight;
    var currentUV = displacedUV;
    var shadowFactor = 1.0;

    for (var i : i32 = 0; i < 8; i = i + 1) {
        currentUV = currentUV + deltaUV;
        currentLayerHeight = currentLayerHeight + stepSize;
        let sampledHeight = getSurfaceHeight(currentUV, duv1, duv2);
        if (sampledHeight > currentLayerHeight) {
            let diff = sampledHeight - currentLayerHeight;
            shadowFactor = min(shadowFactor, 1.0 - diff * 0.85);
        }
    }
    return clamp(shadowFactor, 0.25, 1.0);
}

fn srgbToLinear(c : vec3<f32>) -> vec3<f32> {
    return pow(c, vec3<f32>(2.2));
}

fn linearToSrgb(c : vec3<f32>) -> vec3<f32> {
    return pow(c, vec3<f32>(1.0 / 2.2));
}

fn tonemapKhronosPbrNeutral(color : vec3<f32>) -> vec3<f32> {
    let startCompression = 0.8 - 0.04;
    let desaturation = 0.15;
    let x = min(color.r, min(color.g, color.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    var c = color - offset;
    let peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) {
        return color;
    }
    let d = 1.0 - startCompression;
    let newPeak = 1.0 - d * d / (peak + d - startCompression);
    c = c * (newPeak / peak);
    let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, vec3<f32>(newPeak), g);
}

// ============================================================================
// Procedural Weather Effects (Atmospheric Rain & Snow WGSL Subroutines)
// ============================================================================

fn hash31(p : vec3<f32>) -> f32 {
    let h = fract(sin(dot(p, vec3<f32>(12.9898, 78.233, 45.164))) * 43758.5453123);
    return h;
}

// Procedural dynamic raindrops ripples on surface
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

// Volumetric atmospheric rain streaks
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

// Volumetric atmospheric drifting snow flakes
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
    let noonSun = vec3<f32>(1.0, 0.97, 0.92) * 2.6;
    let goldenSun = vec3<f32>(1.0, 0.48, 0.18) * 1.9;
    let nightMoon = vec3<f32>(0.28, 0.44, 0.78) * 0.48;
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

    let windOffset = vec3<f32>(wind * time * 0.6, -time * 0.08, wind * time * 0.3);
    let mistNoise = fbm3D(worldPos * 0.28 - windOffset);
    let heightFalloff = exp(-max(worldPos.y + 2.0, 0.0) * 0.12);
    let density = (0.045 + 0.055 * heightFalloff * (0.65 + 0.7 * mistNoise)) * intensity;
    let fogFactor = clamp(1.0 - exp(-dist * density), 0.0, 0.96);

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
    let flow = vec3<f32>(wind * time * 1.1, -time * 1.35, wind * time * 0.45);
    let p1 = worldPos * 0.42 + flow;
    let warp = vec3<f32>(
        fbm3D(p1 + vec3<f32>(5.2, 1.3, 2.8)),
        fbm3D(p1 + vec3<f32>(1.7, 9.2, 4.1)),
        fbm3D(p1 + vec3<f32>(8.3, 2.8, 6.4))
    ) * 1.35;
    let rawTurbulence = fbm3D(p1 + warp);

    let heightEnvelope = exp(-max(worldPos.y + 3.0, 0.0) * 0.08);
    let distEnvelope = 1.0 - exp(-dist * 0.085);
    let plumeShape = smoothstep(0.32, 0.78, rawTurbulence);
    let smokeOpacity = clamp((plumeShape * 0.75 + distEnvelope * 0.35) * heightEnvelope * intensity, 0.0, 0.94);

    let sootColor = vec3<f32>(0.15, 0.16, 0.18);
    let ashHighlight = vec3<f32>(0.34, 0.35, 0.38);
    let emberGlow = vec3<f32>(0.95, 0.38, 0.08) * exp(-max(worldPos.y + 1.5, 0.0) * 0.45) * pow(plumeShape, 2.5) * 0.55 * clamp(intensity, 0.0, 1.5);
    let smokeColor = mix(sootColor, ashHighlight, rawTurbulence) + emberGlow;

    return mix(sceneColor, smokeColor, smokeOpacity);
}

fn bayerDither4x4(fragCoord : vec2<f32>) -> f32 {
    let x = u32(fragCoord.x) & 3u;
    let y = u32(fragCoord.y) & 3u;
    let idx = y * 4u + x;
    let bayer = array<f32, 16>(
         0.0 / 16.0,  8.0 / 16.0,  2.0 / 16.0, 10.0 / 16.0,
        12.0 / 16.0,  4.0 / 16.0, 14.0 / 16.0,  6.0 / 16.0,
         3.0 / 16.0, 11.0 / 16.0,  1.0 / 16.0,  9.0 / 16.0,
        15.0 / 16.0,  7.0 / 16.0, 13.0 / 16.0,  5.0 / 16.0
    );
    return bayer[idx];
}

fn transformUv(uv : vec2<f32>, scaleOffset : vec4<f32>, rot : f32) -> vec2<f32> {
    let su = uv * scaleOffset.xy;
    if (abs(rot) < 1e-6) {
        return su + scaleOffset.zw;
    }
    let s = sin(rot);
    let c = cos(rot);
    return vec2<f32>(c * su.x + s * su.y, -s * su.x + c * su.y) + scaleOffset.zw;
}

fn transformUvDeriv(d : vec2<f32>, scaleOffset : vec4<f32>, rot : f32) -> vec2<f32> {
    let sd = d * scaleOffset.xy;
    if (abs(rot) < 1e-6) {
        return sd;
    }
    let s = sin(rot);
    let c = cos(rot);
    return vec2<f32>(c * sd.x + s * sd.y, -s * sd.x + c * sd.y);
}

fn getLodHeatmapColor(lod : u32) -> vec3<f32> {
    if (lod == 0u) {
        return vec3<f32>(0.15, 0.90, 0.35);
    } else if (lod == 1u) {
        return vec3<f32>(0.95, 0.85, 0.15);
    } else if (lod == 2u) {
        return vec3<f32>(0.98, 0.48, 0.12);
    }
    return vec3<f32>(0.95, 0.18, 0.22);
}

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    let worldPos4 = ubo.model * vec4<f32>(in.position, 1.0);
    out.worldPos = worldPos4.xyz;
    let n = (ubo.normalMatrix * vec4<f32>(in.normal, 0.0)).xyz;
    if (dot(n, n) > 0.0001) {
        out.normal = normalize(n);
    } else {
        out.normal = vec3<f32>(0.0, 1.0, 0.0);
    }
    let worldTan = (ubo.model * vec4<f32>(in.tangent.xyz, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    out.tangent = vec4<f32>(normTan, in.tangent.w);
    out.uv = in.uv;
    out.clipPos = ubo.viewProj * worldPos4;
    return out;
}

@vertex
fn vs_main_skinned(in : SkinnedVertexInput) -> VertexOutput {
    var out : VertexOutput;
    var localPos = in.position;
    var localNorm = in.normal;
    var localTan = in.tangent.xyz;

    let weightSum = in.weights.x + in.weights.y + in.weights.z + in.weights.w;
    if (weightSum > 0.0001) {
        let numJoints = arrayLength(&jointMatrices);
        let i0 = min(in.joints.x, numJoints - 1u);
        let i1 = min(in.joints.y, numJoints - 1u);
        let i2 = min(in.joints.z, numJoints - 1u);
        let i3 = min(in.joints.w, numJoints - 1u);
        let skinMat = in.weights.x * jointMatrices[i0] +
                      in.weights.y * jointMatrices[i1] +
                      in.weights.z * jointMatrices[i2] +
                      in.weights.w * jointMatrices[i3];
        localPos = (skinMat * vec4<f32>(in.position, 1.0)).xyz;
        localNorm = (skinMat * vec4<f32>(in.normal, 0.0)).xyz;
        localTan = (skinMat * vec4<f32>(in.tangent.xyz, 0.0)).xyz;
    }

    let worldPos4 = ubo.model * vec4<f32>(localPos, 1.0);
    out.worldPos = worldPos4.xyz;
    let n = (ubo.normalMatrix * vec4<f32>(localNorm, 0.0)).xyz;
    if (dot(n, n) > 0.0001) {
        out.normal = normalize(n);
    } else {
        out.normal = vec3<f32>(0.0, 1.0, 0.0);
    }
    let worldTan = (ubo.model * vec4<f32>(localTan, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    out.tangent = vec4<f32>(normTan, in.tangent.w);
    out.uv = in.uv;
    out.clipPos = ubo.viewProj * worldPos4;
    return out;
}

@fragment
fn fs_main(in : VertexOutput, @builtin(front_facing) frontFacing : bool) -> @location(0) vec4<f32> {
    let dp1 = dpdx(in.worldPos);
    let dp2 = dpdy(in.worldPos);
    let rawDuv1 = dpdx(in.uv);
    let rawDuv2 = dpdy(in.uv);
    let normDuv1 = transformUvDeriv(rawDuv1, ubo.normalUvTransform, ubo.uvRotations0.z);
    let normDuv2 = transformUvDeriv(rawDuv2, ubo.normalUvTransform, ubo.uvRotations0.z);

    // Wireframe debug override
    if (ubo.flags.y != 0u) {
        return vec4<f32>(0.22, 0.74, 0.97, 1.0);
    }

    // 1. Initial Geometry Normal & Orthonormal TBN Basis
    var geomNormal = in.normal;
    if (dot(geomNormal, geomNormal) > 0.0001) {
        geomNormal = normalize(geomNormal);
    } else {
        geomNormal = vec3<f32>(0.0, 1.0, 0.0);
    }
    let V = normalize(ubo.cameraPos.xyz - in.worldPos);
    if (!frontFacing || (ubo.flags.z != 0u && dot(geomNormal, V) < 0.0)) {
        geomNormal = -geomNormal;
    }

    let TBN = getTBN(in.worldPos, geomNormal, in.uv, in.tangent, dp1, dp2, rawDuv1, rawDuv2);
    let invTBN = transpose(TBN);
    let V_tangent = invTBN * V;

    // 2. Parallax Occlusion Mapping (POM) & Surface Displacement
    var baseRawUv = in.uv;
    var pomShadow = 1.0;
    let bumpScale = ubo.params.x;
    let pomScale = ubo.params.y;
    let activeLodLevel = u32(clamp(ubo.lodParams.x, 0.0, 3.0));
    let lodTransition = clamp(ubo.lodParams.y, 0.0, 1.0);
    let debugViewMode = u32(clamp(ubo.lodParams.z, 0.0, 4.0));
    let isPomActive = (ubo.params.z > 0.5) && (activeLodLevel == 0u);
    let isBumpActive = (ubo.params.w > 0.5) && (activeLodLevel < 2u);

    if (isPomActive && isBumpActive) {
        let isNormalMapped = (ubo.flags.x & 4u) != 0u;
        let effPomScale = select(pomScale, pomScale * 0.14, isNormalMapped);
        baseRawUv = parallaxOcclusionMapping(in.uv, V_tangent, rawDuv1, rawDuv2, effPomScale);
        let L0_tangent = invTBN * normalize(ubo.lightDir.xyz);
        pomShadow = computeParallaxShadow(baseRawUv, L0_tangent, rawDuv1, rawDuv2, effPomScale);
    }

    // Apply independent per-channel KHR_texture_transform coordinates & derivatives
    let baseUv = transformUv(baseRawUv, ubo.uvTransform, ubo.uvRotations0.x);
    let mrUv = transformUv(baseRawUv, ubo.mrUvTransform, ubo.uvRotations0.y);
    let normUv = transformUv(baseRawUv, ubo.normalUvTransform, ubo.uvRotations0.z);
    let occUv = transformUv(baseRawUv, ubo.occlusionUvTransform, ubo.uvRotations0.w);
    let emUv = transformUv(baseRawUv, ubo.emissiveUvTransform, ubo.uvRotations1.x);

    let baseDuv1 = transformUvDeriv(rawDuv1, ubo.uvTransform, ubo.uvRotations0.x);
    let baseDuv2 = transformUvDeriv(rawDuv2, ubo.uvTransform, ubo.uvRotations0.x);
    let mrDuv1 = transformUvDeriv(rawDuv1, ubo.mrUvTransform, ubo.uvRotations0.y);
    let mrDuv2 = transformUvDeriv(rawDuv2, ubo.mrUvTransform, ubo.uvRotations0.y);
    let occDuv1 = transformUvDeriv(rawDuv1, ubo.occlusionUvTransform, ubo.uvRotations0.w);
    let occDuv2 = transformUvDeriv(rawDuv2, ubo.occlusionUvTransform, ubo.uvRotations0.w);
    let emDuv1 = transformUvDeriv(rawDuv1, ubo.emissiveUvTransform, ubo.uvRotations1.x);
    let emDuv2 = transformUvDeriv(rawDuv2, ubo.emissiveUvTransform, ubo.uvRotations1.x);

    // Sample all PBR material maps using explicit screen-space gradients (uniformity-safe & anisotropic-correct)
    let baseTexSample = textureSampleGrad(baseColorTexture, defaultSampler, baseUv, baseDuv1, baseDuv2);
    let mrTexSample = textureSampleGrad(metallicRoughnessTexture, defaultSampler, mrUv, mrDuv1, mrDuv2);
    let normTexSample = textureSampleGrad(normalTexture, defaultSampler, normUv, normDuv1, normDuv2);
    let duv1 = rawDuv1;
    let duv2 = rawDuv2;
    let emTexSample = textureSampleGrad(emissiveTexture, defaultSampler, emUv, emDuv1, emDuv2);
    let occTexSample = textureSampleGrad(occlusionTexture, defaultSampler, occUv, occDuv1, occDuv2);

    // 3. Base Color & Alpha (glTF baseColorFactor is already linear-space)
    var baseColor = ubo.baseColorFactor;
    if ((ubo.flags.x & 1u) != 0u) {
        baseColor = baseColor * vec4<f32>(srgbToLinear(baseTexSample.rgb), baseTexSample.a);
    }

    // Alpha Cutoff test (glTF 2.0 alphaMode: 0 = OPAQUE, 1 = MASK, 2 = BLEND)
    let alphaMode = ubo.flags.w;
    let alphaCutoff = ubo.emissiveFactor.w;
    if (alphaMode == 1u && baseColor.a < alphaCutoff) {
        discard;
    }

    // 4. High-Detail Sculptural Bump & Normal Mapping
    var N = geomNormal;
    if (isBumpActive) {
        if ((ubo.flags.x & 4u) != 0u) { // HasNormalTex
            let normalSample = normTexSample.xyz * 2.0 - 1.0;
            let totalScale = ubo.pbrFactors.z * bumpScale;
            let tangentNormal = normalize(vec3<f32>(normalSample.xy * totalScale, normalSample.z));
            N = normalize(TBN * tangentNormal);
        } else if ((ubo.flags.x & 1u) != 0u || (ubo.flags.x & 16u) != 0u) {
            // Surface Gradient Bump Mapping from BaseColor Luminance and Occlusion Cavity
            let texDims = vec2<f32>(textureDimensions(baseColorTexture, 0));
            let texelStep = 1.0 / clamp(texDims, vec2<f32>(64.0), vec2<f32>(2048.0));
            let lumaWeights = vec3<f32>(0.299, 0.587, 0.114);

            let hR = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv + vec2<f32>(texelStep.x, 0.0), baseDuv1, baseDuv2).rgb, lumaWeights);
            let hL = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv - vec2<f32>(texelStep.x, 0.0), baseDuv1, baseDuv2).rgb, lumaWeights);
            let hU = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv + vec2<f32>(0.0, texelStep.y), baseDuv1, baseDuv2).rgb, lumaWeights);
            let hD = dot(textureSampleGrad(baseColorTexture, defaultSampler, baseUv - vec2<f32>(0.0, texelStep.y), baseDuv1, baseDuv2).rgb, lumaWeights);

            var dHdU = (hR - hL);
            var dHdV = (hU - hD);

            if ((ubo.flags.x & 16u) != 0u) {
                let occR = textureSampleGrad(occlusionTexture, defaultSampler, occUv + vec2<f32>(texelStep.x, 0.0), occDuv1, occDuv2).r;
                let occL = textureSampleGrad(occlusionTexture, defaultSampler, occUv - vec2<f32>(texelStep.x, 0.0), occDuv1, occDuv2).r;
                let occU = textureSampleGrad(occlusionTexture, defaultSampler, occUv + vec2<f32>(0.0, texelStep.y), occDuv1, occDuv2).r;
                let occD = textureSampleGrad(occlusionTexture, defaultSampler, occUv - vec2<f32>(0.0, texelStep.y), occDuv1, occDuv2).r;
                dHdU = dHdU + (occR - occL) * 0.75;
                dHdV = dHdV + (occU - occD) * 0.75;
            }

            let bumpStrength = 2.0 * bumpScale;
            let tangentBump = normalize(vec3<f32>(-dHdU * bumpStrength, -dHdV * bumpStrength, 1.0));
            N = normalize(TBN * tangentBump);
        }
    }

    // 5. Micro-Cavity Factor & Horizon Occlusion
    let cavityFactor = clamp(dot(geomNormal, N), 0.0, 1.0);

    // Weather Parameters
    let weatherMode = ubo.weather.x;
    let weatherIntensity = ubo.weather.y;
    let windStrength = ubo.weather.z;
    let weatherTime = ubo.weather.w;

    if (weatherMode == 1.0 && weatherIntensity > 0.001 && geomNormal.y > 0.15) {
        // Rain droplet ripples on upward-facing surfaces
        let rippleTan = calculateRainRipples(in.uv * 5.0 + in.worldPos.xz * 2.5, weatherTime);
        let rippleWeight = smoothstep(0.15, 0.75, geomNormal.y) * clamp(weatherIntensity * 0.55, 0.0, 0.65);
        N = normalize(mix(N, TBN * rippleTan, rippleWeight));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomNormal.y > 0.2) {
        // Snow settles and flattens crevices upward
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        N = normalize(mix(N, vec3<f32>(0.0, 1.0, 0.0), snowAccum * 0.6));
    }

    // 6. Metallic & Roughness with Micro-Roughness Relief
    var metallic = ubo.pbrFactors.x;
    var roughness = ubo.pbrFactors.y;
    if ((ubo.flags.x & 2u) != 0u) { // HasMrTex
        roughness = roughness * mrTexSample.g;
        metallic = metallic * mrTexSample.b;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    // Ridge polish & crevice matte modulation for heightened tactile plasticity
    roughness = clamp(roughness * mix(1.15, 0.85, cavityFactor), 0.04, 1.0);

    // Weather surface material modulation (wet absorption glaze, crystalline snow, fog condensation, smoke soot)
    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.08, clamp(weatherIntensity * 0.85, 0.0, 0.88));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            roughness = mix(roughness, 0.70, snowAccum * 0.9);
            metallic = mix(metallic, 0.0, snowAccum * 0.95);
        }
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.30, clamp(weatherIntensity * 0.35, 0.0, 0.45));
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.88, clamp(weatherIntensity * 0.4, 0.0, 0.55));
    }

    // 7. Ambient Occlusion with Micro-Cavity Crevice Deepening
    var ao = 1.0;
    if ((ubo.flags.x & 16u) != 0u) { // HasOcclusionTex
        ao = mix(1.0, occTexSample.r, ubo.pbrFactors.w);
    }
    ao = ao * mix(0.5, 1.0, cavityFactor * cavityFactor);

    // 8. Emissive
    var emissive = ubo.emissiveFactor.rgb;
    if ((ubo.flags.x & 8u) != 0u) { // HasEmissiveTex
        emissive = emissive * srgbToLinear(emTexSample.rgb);
    }

    // 9. PBR Vectors & Dielectric/Metallic Reflectance Setup
    let NdotV = max(dot(N, V), 0.001);
    let F0 = mix(vec3<f32>(0.04), baseColor.rgb, metallic);
    var diffuseColor = baseColor.rgb * (1.0 - metallic);

    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        let wetWeight = clamp(weatherIntensity * 0.8, 0.0, 0.75);
        diffuseColor = mix(diffuseColor, diffuseColor * 0.65, wetWeight * (1.0 - metallic * 0.5));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            let crispSnowColor = vec3<f32>(0.93, 0.95, 0.99);
            diffuseColor = mix(diffuseColor, crispSnowColor, snowAccum * 0.95);
        }
    }

    // 10. Direct Lighting Evaluation with Diurnal Sun/Moon Cycle & Horizon Micro-Shadowing
    var directRadiance = vec3<f32>(0.0);
    let solarElev = ubo.lightDir.w;
    let diurnalAmbientScale = evaluateDiurnalAmbientScale(solarElev);

    // Light 0: Sun / Moon Key Light
    let L0 = normalize(ubo.lightDir.xyz);
    let NdotL0 = max(dot(N, L0), 0.0);
    let geomNdotL0 = max(dot(geomNormal, L0), 0.0);
    let horizon0 = clamp(geomNdotL0 * 3.0, 0.0, 1.0);
    let light0Shadow = pomShadow * horizon0;

    if (NdotL0 > 0.0 && light0Shadow > 0.001) {
        let H0 = normalize(L0 + V);
        let NdotH0 = max(dot(N, H0), 0.0);
        let VdotH0 = max(dot(V, H0), 0.0);

        let D = distributionGGX(NdotH0, roughness);
        let Vis = visibilitySmithGGX(NdotL0, NdotV, roughness);
        let F = fresnelSchlick(VdotH0, F0);
        let specularBRDF = D * Vis * F;

        let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
        let diffuseBRDF = kD * (diffuseColor / PI);

        let sunRadiance = evaluateDiurnalSunRadiance(solarElev);
        directRadiance = directRadiance + (diffuseBRDF + specularBRDF) * sunRadiance * (NdotL0 * light0Shadow);
    }

    // Light 1: Cool Sky Fill Light
    let L1 = normalize(vec3<f32>(-L0.x * 0.7, 0.5, -L0.z * 0.7));
    let NdotL1 = max(dot(N, L1), 0.0);
    let geomNdotL1 = max(dot(geomNormal, L1), 0.0);
    let horizon1 = clamp(geomNdotL1 * 3.0, 0.0, 1.0);

    if (NdotL1 > 0.0 && horizon1 > 0.001) {
        let H1 = normalize(L1 + V);
        let NdotH1 = max(dot(N, H1), 0.0);
        let VdotH1 = max(dot(V, H1), 0.0);

        let D = distributionGGX(NdotH1, roughness);
        let Vis = visibilitySmithGGX(NdotL1, NdotV, roughness);
        let F = fresnelSchlick(VdotH1, F0);
        let specularBRDF = D * Vis * F;

        let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
        let diffuseBRDF = kD * (diffuseColor / PI);

        let fillRadiance = vec3<f32>(0.35, 0.45, 0.60) * 0.8 * diurnalAmbientScale;
        directRadiance = directRadiance + (diffuseBRDF + specularBRDF) * fillRadiance * (NdotL1 * horizon1);
    }

    // Snow crystalline micro-sparkle under direct key illumination
    if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomNormal.y > 0.2) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        let sparkleCoord = floor(in.worldPos * 75.0);
        let sparkleNoise = fract(sin(dot(sparkleCoord, vec3<f32>(27.1, 59.3, 113.7))) * 43758.5453);
        let sparkle = pow(sparkleNoise, 16.0) * 3.5 * snowAccum * max(dot(N, L0), 0.0);
        directRadiance = directRadiance + vec3<f32>(sparkle);
    }

    // 11. Ambient Lighting (Hemispheric Diffuse Irradiance + GGX Environment Specular Reflection)
    let hemiWeight = N.y * 0.5 + 0.5;
    let groundAmbient = vec3<f32>(0.12, 0.11, 0.10) * diurnalAmbientScale;
    let skyAmbient = vec3<f32>(0.35, 0.45, 0.60) * diurnalAmbientScale;
    let irradiance = mix(groundAmbient, skyAmbient, hemiWeight) * 0.9;

    let F_env = fresnelSchlickRoughness(NdotV, F0, roughness);
    let kD_env = (vec3<f32>(1.0) - F_env) * (1.0 - metallic);
    let ambientDiffuse = kD_env * diffuseColor * irradiance;

    // Specular Reflection (Roughness-dependent environmental reflection modulated by cavity factor)
    let R = reflect(-V, N);
    let refHemi = R.y * 0.5 + 0.5;
    let groundReflect = vec3<f32>(0.10, 0.09, 0.08) * diurnalAmbientScale;
    let skyReflect = vec3<f32>(0.55, 0.70, 0.95) * diurnalAmbientScale;
    let envReflectCol = mix(groundReflect, skyReflect, refHemi);
    let roughnessAttenuation = 1.0 - roughness * 0.6;
    let ambientSpecular = envReflectCol * F_env * roughnessAttenuation * cavityFactor;

    let ambientColor = (ambientDiffuse + ambientSpecular) * ao;

    // 12. Total Radiance in Linear HDR Space
    var totalColor = directRadiance + ambientColor + emissive;

    // Atmospheric Weather Effects (Rain, Snow, Fog, Smoke)
    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        totalColor = totalColor + calculateRainStreaks(in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        totalColor = totalColor + calculateSnowFlakes(in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        totalColor = calculateFogEffect(totalColor, in.worldPos, ubo.cameraPos.xyz, L0, solarElev, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        totalColor = calculateSmokePlumes(totalColor, in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    }

    // 13. Diagnostic Render Views & HDR Tonemapping (Khronos PBR Neutral)
    if (debugViewMode == 1u) {
        var heatCol = getLodHeatmapColor(activeLodLevel);
        if (lodTransition > 0.01 && activeLodLevel < 3u) {
            let dither = bayerDither4x4(in.clipPos.xy);
            if (lodTransition > dither) {
                heatCol = getLodHeatmapColor(activeLodLevel + 1u);
            }
        }
        let litLuma = dot(linearToSrgb(tonemapKhronosPbrNeutral(totalColor)), vec3<f32>(0.299, 0.587, 0.114));
        return vec4<f32>(heatCol * (0.35 + 0.65 * litLuma), 1.0);
    } else if (debugViewMode == 2u) {
        return vec4<f32>(N * 0.5 + 0.5, 1.0);
    } else if (debugViewMode == 3u) {
        return vec4<f32>(ao, roughness, metallic, 1.0);
    } else if (debugViewMode == 4u) {
        let grid = floor(baseUv * 16.0);
        let checker = abs((grid.x + grid.y) % 2.0);
        let checkerCol = mix(vec3<f32>(0.16, 0.18, 0.22), vec3<f32>(0.90, 0.92, 0.96), checker);
        return vec4<f32>(checkerCol * (0.4 + 0.6 * max(dot(N, L0), 0.2)), 1.0);
    }

    totalColor = tonemapKhronosPbrNeutral(totalColor);
    let finalColor = linearToSrgb(totalColor);

    return vec4<f32>(finalColor, baseColor.a);
}
`;
    },

    async init() {
      if (this.isActive && this.device) return true;
      this.canvas = document.getElementById('canvas');
      if (!this.canvas) return false;

      if (!navigator.gpu) {
        BglLogger.warn('WebGPU', 'navigator.gpu is unavailable in this environment.');
        return false;
      }

      try {
        this.adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
        if (!this.adapter) {
          BglLogger.warn('WebGPU', 'navigator.gpu.requestAdapter() returned null.');
          return false;
        }

        this.device = await this.adapter.requestDevice();
        if (!this.device) {
          BglLogger.warn('WebGPU', 'adapter.requestDevice() returned null.');
          return false;
        }

        if (this.device.lost) {
          this.device.lost.then((info) => {
            BglLogger.error('WebGPU', `Device lost: ${info.reason || 'Unknown'} - ${info.message || ''}`);
            this.isActive = false;
          });
        }

        if (this.adapter.info) {
          window.cachedGpuInfo = this.adapter.info;
          BglLogger.log('WebGPU', `Acquired GPU Adapter: ${this.adapter.info.vendor || 'Unknown'} - ${this.adapter.info.architecture || ''} (${this.adapter.info.description || ''})`);
        } else {
          BglLogger.log('WebGPU', 'Acquired compatible WebGPU hardware adapter.');
        }

        this.context = this.canvas.getContext('webgpu');
        if (!this.context) {
          BglLogger.warn('WebGPU', 'canvas.getContext("webgpu") returned null.');
          return false;
        }

        this.format = navigator.gpu.getPreferredCanvasFormat();
        this.context.configure({
          device: this.device,
          format: this.format,
          alphaMode: 'opaque'
        });

        // 1x1 default white texture [255, 255, 255, 255]
        this.defaultWhiteTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultWhiteTexture },
          new Uint8Array([255, 255, 255, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultWhiteTextureView = this.defaultWhiteTexture.createView();

        // 1x1 default metallic-roughness texture [255, 255, 255, 255] (R=1, G=Roughness=1, B=Metallic=1)
        this.defaultMrTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultMrTexture },
          new Uint8Array([255, 255, 255, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultMrTextureView = this.defaultMrTexture.createView();

        // 1x1 default normal texture [128, 128, 255, 255] (tangent normal [0, 0, 1])
        this.defaultNormalTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultNormalTexture },
          new Uint8Array([128, 128, 255, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultNormalTextureView = this.defaultNormalTexture.createView();

        // 1x1 default black texture [0, 0, 0, 255] (emissive default)
        this.defaultBlackTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultBlackTexture },
          new Uint8Array([0, 0, 0, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultBlackTextureView = this.defaultBlackTexture.createView();

        // Default 16x anisotropic trilinear repeat sampler (glTF 2.0 default wrapS/wrapT = REPEAT 10497)
        this.defaultSampler = this.device.createSampler({
          addressModeU: 'repeat',
          addressModeV: 'repeat',
          addressModeW: 'repeat',
          magFilter: 'linear',
          minFilter: 'linear',
          mipmapFilter: 'linear',
          maxAnisotropy: 16
        });

        this.mipmapSampler = this.device.createSampler({
          addressModeU: 'clamp-to-edge',
          addressModeV: 'clamp-to-edge',
          magFilter: 'linear',
          minFilter: 'linear'
        });

        // 7-Entry PBR Bind Group Layout
        this.bindGroupLayout = this.device.createBindGroupLayout({
          entries: [
            {
              binding: 0,
              visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
              buffer: { type: 'uniform' }
            },
            {
              binding: 1,
              visibility: GPUShaderStage.FRAGMENT,
              sampler: { type: 'filtering' }
            },
            {
              binding: 2,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 3,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 4,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 5,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 6,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            }
          ]
        });

        this.pipelineLayout = this.device.createPipelineLayout({
          bindGroupLayouts: [this.bindGroupLayout]
        });

        const shaderModule = this.device.createShaderModule({
          code: this.getShaderWgsl()
        });

        const vertexBufferLayout = {
          arrayStride: 48,
          attributes: [
            { shaderLocation: 0, offset: 0, format: 'float32x3' },  // position
            { shaderLocation: 1, offset: 12, format: 'float32x3' }, // normal
            { shaderLocation: 2, offset: 24, format: 'float32x2' }, // uv
            { shaderLocation: 3, offset: 32, format: 'float32x4' }  // tangent
          ]
        };

        this.pipeline = this.device.createRenderPipeline({
          layout: this.pipelineLayout,
          vertex: {
            module: shaderModule,
            entryPoint: 'vs_main',
            buffers: [vertexBufferLayout]
          },
          fragment: {
            module: shaderModule,
            entryPoint: 'fs_main',
            targets: [{ format: this.format }]
          },
          primitive: {
            topology: 'triangle-list',
            cullMode: 'back'
          },
          depthStencil: {
            format: 'depth24plus',
            depthWriteEnabled: true,
            depthCompare: 'less'
          }
        });

        this.pipelineDoubleSided = this.device.createRenderPipeline({
          layout: this.pipelineLayout,
          vertex: {
            module: shaderModule,
            entryPoint: 'vs_main',
            buffers: [vertexBufferLayout]
          },
          fragment: {
            module: shaderModule,
            entryPoint: 'fs_main',
            targets: [{ format: this.format }]
          },
          primitive: {
            topology: 'triangle-list',
            cullMode: 'none'
          },
          depthStencil: {
            format: 'depth24plus',
            depthWriteEnabled: true,
            depthCompare: 'less'
          }
        });

        // Skeletal Skinning Bind Group Layout (@group(1)) & Pipelines
        this.skinBindGroupLayout = this.device.createBindGroupLayout({
          entries: [
            {
              binding: 0,
              visibility: GPUShaderStage.VERTEX,
              buffer: { type: 'read-only-storage' }
            }
          ]
        });

        this.skinnedPipelineLayout = this.device.createPipelineLayout({
          bindGroupLayouts: [this.bindGroupLayout, this.skinBindGroupLayout]
        });

        const skinVertexBufferLayout = {
          arrayStride: 32,
          attributes: [
            { shaderLocation: 4, offset: 0, format: 'uint32x4' },
            { shaderLocation: 5, offset: 16, format: 'float32x4' }
          ]
        };

        this.skinnedPipeline = this.device.createRenderPipeline({
          layout: this.skinnedPipelineLayout,
          vertex: {
            module: shaderModule,
            entryPoint: 'vs_main_skinned',
            buffers: [vertexBufferLayout, skinVertexBufferLayout]
          },
          fragment: {
            module: shaderModule,
            entryPoint: 'fs_main',
            targets: [{ format: this.format }]
          },
          primitive: {
            topology: 'triangle-list',
            cullMode: 'back'
          },
          depthStencil: {
            format: 'depth24plus',
            depthWriteEnabled: true,
            depthCompare: 'less'
          }
        });

        this.skinnedPipelineDoubleSided = this.device.createRenderPipeline({
          layout: this.skinnedPipelineLayout,
          vertex: {
            module: shaderModule,
            entryPoint: 'vs_main_skinned',
            buffers: [vertexBufferLayout, skinVertexBufferLayout]
          },
          fragment: {
            module: shaderModule,
            entryPoint: 'fs_main',
            targets: [{ format: this.format }]
          },
          primitive: {
            topology: 'triangle-list',
            cullMode: 'none'
          },
          depthStencil: {
            format: 'depth24plus',
            depthWriteEnabled: true,
            depthCompare: 'less'
          }
        });

        this.mipmapBindGroupLayout = this.device.createBindGroupLayout({
          entries: [
            { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
            { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d' } }
          ]
        });
        this.mipmapPipelineLayout = this.device.createPipelineLayout({
          bindGroupLayouts: [this.mipmapBindGroupLayout]
        });
        this.mipmapPipeline = this.device.createRenderPipeline({
          layout: this.mipmapPipelineLayout,
          vertex: { module: shaderModule, entryPoint: 'vs_mip' },
          fragment: { module: shaderModule, entryPoint: 'fs_mip', targets: [{ format: 'rgba8unorm' }] },
          primitive: { topology: 'triangle-list' }
        });

        // Initialize reusable scratch buffers for zero-allocation rendering
        this._scratchProj = this.mat4.create();
        this._scratchView = this.mat4.create();
        this._scratchViewProj = this.mat4.create();
        this._scratchUboF32 = new Float32Array(this._scratchUboBuffer, 0, 128);
        this._scratchUboU32 = new Uint32Array(this._scratchUboBuffer, 288, 4);
        this._scratchFrustum = new Float32Array(24);
        this._pendingMipmaps = [];
        this._mipBatchScheduled = false;
        this._hierarchyVersion = 0;
        this._lastDomTelemetryMs = 0;
        this._sortedPrimitives = [];

        this.isActive = true;
        this.initEvents();
        this.startLoop();
        BglLogger.log('WebGPU', 'Native WebGPU rendering pipeline initialized successfully.');

        if (BglState.currentGltf && BglState.currentModelBuffer) {
          BglLogger.log('WebGPU', `Immediately loading pre-parsed model '${BglState.currentModelName}' into WebGPU.`);
          this.loadModel(BglState.currentGltf, BglState.currentModelBuffer, BglState.currentModelName);
        }

        return true;
      } catch (err) {
        BglLogger.warn('WebGPU', 'Failed to initialize WebGPU renderer:', err);
        this.isActive = false;
        return false;
      }
    },

    initEvents() {
      const canvas = this.canvas;
      canvas.addEventListener('contextmenu', (e) => e.preventDefault());

      canvas.addEventListener('mousedown', (e) => {
        if (!this.isActive) return;
        this.isDragging = true;
        this.lastMouseX = e.clientX;
        this.lastMouseY = e.clientY;
        this.dragButton = e.button;
      });

      window.addEventListener('mousemove', (e) => {
        if (!this.isActive || !this.isDragging) return;
        const dx = e.clientX - this.lastMouseX;
        const dy = e.clientY - this.lastMouseY;
        this.lastMouseX = e.clientX;
        this.lastMouseY = e.clientY;

        if (this.dragButton === 0) {
          this.camera.yaw += dx * 0.006;
          this.camera.pitch = Math.max(-1.45, Math.min(1.45, this.camera.pitch - dy * 0.006));
        } else if (this.dragButton === 2) {
          const factor = this.camera.distance * 0.0015;
          const cosY = Math.cos(this.camera.yaw);
          const sinY = Math.sin(this.camera.yaw);
          this.camera.target[0] -= (cosY * dx) * factor;
          this.camera.target[1] += dy * factor;
          this.camera.target[2] -= (sinY * dx) * factor;
        }
      });

      window.addEventListener('mouseup', () => {
        if (this.isActive) this.isDragging = false;
      });

      canvas.addEventListener('wheel', (e) => {
        if (!this.isActive) return;
        e.preventDefault();
        const factor = e.deltaY > 0 ? 1.1 : 0.9;
        this.camera.distance = Math.max(this.camera.minDistance, Math.min(this.camera.maxDistance, this.camera.distance * factor));
      }, { passive: false });

      // Mobile touch controls (pinch-zoom, multi-touch pan, 1-finger orbit)
      let touchLastPos = { x: 0, y: 0 };
      let touchStartDist = 0;
      let touchLastMid = { x: 0, y: 0 };

      canvas.addEventListener('touchstart', (e) => {
        if (!this.isActive) return;
        if (e.touches.length === 1) {
          touchLastPos = { x: e.touches[0].clientX, y: e.touches[0].clientY };
        } else if (e.touches.length === 2) {
          touchStartDist = Math.hypot(
            e.touches[0].clientX - e.touches[1].clientX,
            e.touches[0].clientY - e.touches[1].clientY
          );
          touchLastMid = {
            x: (e.touches[0].clientX + e.touches[1].clientX) * 0.5,
            y: (e.touches[0].clientY + e.touches[1].clientY) * 0.5
          };
        }
      }, { passive: false });

      canvas.addEventListener('touchmove', (e) => {
        if (!this.isActive) return;
        e.preventDefault();
        if (e.touches.length === 1) {
          const dx = e.touches[0].clientX - touchLastPos.x;
          const dy = e.touches[0].clientY - touchLastPos.y;
          touchLastPos = { x: e.touches[0].clientX, y: e.touches[0].clientY };
          this.camera.yaw += dx * 0.006;
          this.camera.pitch = Math.max(-1.45, Math.min(1.45, this.camera.pitch - dy * 0.006));
        } else if (e.touches.length === 2) {
          const curDist = Math.hypot(
            e.touches[0].clientX - e.touches[1].clientX,
            e.touches[0].clientY - e.touches[1].clientY
          );
          const deltaDist = curDist - touchStartDist;
          touchStartDist = curDist;
          if (Math.abs(deltaDist) > 0.5) {
            const factor = deltaDist > 0 ? 0.96 : 1.04;
            this.camera.distance = Math.max(this.camera.minDistance, Math.min(this.camera.maxDistance, this.camera.distance * factor));
          }

          const curMid = {
            x: (e.touches[0].clientX + e.touches[1].clientX) * 0.5,
            y: (e.touches[0].clientY + e.touches[1].clientY) * 0.5
          };
          const dMidX = curMid.x - touchLastMid.x;
          const dMidY = curMid.y - touchLastMid.y;
          touchLastMid = curMid;

          const factor = this.camera.distance * 0.0015;
          const cosY = Math.cos(this.camera.yaw);
          const sinY = Math.sin(this.camera.yaw);
          this.camera.target[0] -= (cosY * dMidX) * factor;
          this.camera.target[1] += dMidY * factor;
          this.camera.target[2] -= (sinY * dMidX) * factor;
        }
      }, { passive: false });
    },

    generateLodData(posData, normData, indexData, vertexCount) {
      let baseIndices = indexData;
      let indexFormat = 'uint16';
      if (indexData instanceof Uint8Array) {
        baseIndices = new Uint16Array(indexData);
        indexFormat = 'uint16';
      } else if (indexData instanceof Uint32Array) {
        indexFormat = 'uint32';
      } else {
        indexFormat = 'uint16';
      }
      const baseCount = baseIndices.length;
      const triCount = Math.floor(baseCount / 3);

      // Compute local AABB center and bounding radius for LOD distance evaluation
      let minX = Infinity, minY = Infinity, minZ = Infinity;
      let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
      for (let i = 0; i < posData.length; i += 3) {
        const x = posData[i], y = posData[i + 1], z = posData[i + 2];
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
        if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
      }
      const localCenter = [
        Number.isFinite(minX) ? (minX + maxX) * 0.5 : 0.0,
        Number.isFinite(minY) ? (minY + maxY) * 0.5 : 0.0,
        Number.isFinite(minZ) ? (minZ + maxZ) * 0.5 : 0.0
      ];
      const extX = Number.isFinite(maxX - minX) ? (maxX - minX) : 1.0;
      const extY = Number.isFinite(maxY - minY) ? (maxY - minY) : 1.0;
      const extZ = Number.isFinite(maxZ - minZ) ? (maxZ - minZ) : 1.0;
      const localRadius = Math.max(Math.hypot(extX, extY, extZ) * 0.5, 0.05);

      // Keep small meshes (< 24 triangles) at 100% LOD0 across all levels
      if (triCount < 24 || vertexCount < 12) {
        const slice0 = { firstIndex: 0, indexCount: baseCount };
        return {
          gpuIndexData: baseIndices,
          indexFormat,
          indexCount: baseCount,
          lodSlices: [slice0, slice0, slice0, slice0],
          localCenter,
          localRadius
        };
      }

      const invExtX = extX > 1e-6 ? 1.0 / extX : 0.0;
      const invExtY = extY > 1e-6 ? 1.0 / extY : 0.0;
      const invExtZ = extZ > 1e-6 ? 1.0 / extZ : 0.0;

      const buildLodLevel = (gridRes, targetRatio) => {
        const targetTris = Math.max(8, Math.floor(triCount * targetRatio));
        const clusterMap = new Map();
        const remap = new Uint32Array(vertexCount);
        for (let v = 0; v < vertexCount; ++v) {
          const pIdx = v * 3;
          const nx = Math.max(0.0, Math.min(1.0, (posData[pIdx] - minX) * invExtX));
          const ny = Math.max(0.0, Math.min(1.0, (posData[pIdx + 1] - minY) * invExtY));
          const nz = Math.max(0.0, Math.min(1.0, (posData[pIdx + 2] - minZ) * invExtZ));
          const gx = Math.min(gridRes - 1, Math.floor(nx * gridRes));
          const gy = Math.min(gridRes - 1, Math.floor(ny * gridRes));
          const gz = Math.min(gridRes - 1, Math.floor(nz * gridRes));
          let oct = 0;
          if (normData && pIdx + 2 < normData.length) {
            oct = (normData[pIdx] >= 0 ? 1 : 0) | (normData[pIdx + 1] >= 0 ? 2 : 0) | (normData[pIdx + 2] >= 0 ? 4 : 0);
          }
          const key = ((gx * gridRes + gy) * gridRes + gz) * 8 + oct;
          let rep = clusterMap.get(key);
          if (rep === undefined) {
            rep = v;
            clusterMap.set(key, v);
          }
          remap[v] = rep;
        }

        const out = [];
        for (let t = 0; t < triCount; ++t) {
          const i0 = remap[baseIndices[t * 3]];
          const i1 = remap[baseIndices[t * 3 + 1]];
          const i2 = remap[baseIndices[t * 3 + 2]];
          if (i0 !== i1 && i1 !== i2 && i2 !== i0) {
            out.push(i0, i1, i2);
          }
        }

        const clusteredTris = Math.floor(out.length / 3);
        if (clusteredTris >= 6 && clusteredTris <= Math.floor(targetTris * 1.35)) {
          return out;
        }

        // Geometric stride-decimation fallback when uniform grid clustering retains too many/few triangles
        const stepOut = [];
        const step = Math.max(1.0, triCount / targetTris);
        for (let f = 0.0; Math.floor(f) < triCount && stepOut.length / 3 < targetTris; f += step) {
          const t = Math.floor(f);
          stepOut.push(baseIndices[t * 3], baseIndices[t * 3 + 1], baseIndices[t * 3 + 2]);
        }
        return stepOut.length >= 9 ? stepOut : Array.from(baseIndices);
      };

      const lod1Arr = buildLodLevel(28, 0.55);
      const lod2Arr = buildLodLevel(16, 0.30);
      const lod3Arr = buildLodLevel(9, 0.15);

      const totalLen = baseCount + lod1Arr.length + lod2Arr.length + lod3Arr.length;
      const TypedArr = indexFormat === 'uint32' ? Uint32Array : Uint16Array;
      const gpuIndexData = new TypedArr(totalLen);
      gpuIndexData.set(baseIndices, 0);
      let offset = baseCount;
      const lodSlices = [
        { firstIndex: 0, indexCount: baseCount }
      ];
      for (const arr of [lod1Arr, lod2Arr, lod3Arr]) {
        gpuIndexData.set(arr, offset);
        lodSlices.push({ firstIndex: offset, indexCount: arr.length });
        offset += arr.length;
      }

      return {
        gpuIndexData,
        indexFormat,
        indexCount: baseCount,
        lodSlices,
        localCenter,
        localRadius
      };
    },

    evaluatePrimitiveLod(dist, radius, lodBias = 1.0, isLodActive = true) {
      if (!isLodActive) {
        return { lodLevel: 0, lodTransition: 0.0 };
      }
      const safeRadius = Math.max(radius || 1.0, 0.05);
      const clampedBias = Math.max(0.25, Math.min(4.0, lodBias || 1.0));
      const metric = (Math.max(0.0, dist) / safeRadius) * clampedBias;

      const thresholds = [8.0, 25.0, 85.0];
      let prev = 0.0;
      for (let lvl = 0; lvl < thresholds.length; ++lvl) {
        const th = thresholds[lvl];
        if (metric < th) {
          const band = (th - prev) * 0.25;
          const tr = metric > (th - band) ? (metric - (th - band)) / band : 0.0;
          return { lodLevel: lvl, lodTransition: Math.max(0.0, Math.min(1.0, tr)) };
        }
        prev = th;
      }
      return { lodLevel: 3, lodTransition: 0.0 };
    },

    loadModel(gltf, arrayBuffer, modelName) {
      if (!this.isActive || !this.device) return;

      // Release previous GPU resources to prevent VRAM memory leak
      for (const prim of this.primitives) {
        if (prim.vertexBuffer && prim.vertexBuffer.destroy) prim.vertexBuffer.destroy();
        if (prim.skinVertexBuffer && prim.skinVertexBuffer.destroy) prim.skinVertexBuffer.destroy();
        if (prim.indexBuffer && prim.indexBuffer.destroy) prim.indexBuffer.destroy();
        if (prim.uniformBuffer && prim.uniformBuffer.destroy) prim.uniformBuffer.destroy();
      }
      for (const skin of this.skins) {
        if (skin.jointBuffer && skin.jointBuffer.destroy) skin.jointBuffer.destroy();
      }
      for (const entry of this.textureCache.values()) {
        if (entry.texture && entry.texture.destroy) entry.texture.destroy();
      }
      this.textureCache.clear();
      this.primitives = [];
      this._sortedPrimitives = [];
      this._pendingMipmaps = [];
      this._mipBatchScheduled = false;
      this.nodes = [];
      this.rootNodes = [];
      this.skins = [];
      this.animations = [];
      this.activeAnimationIndex = -1;
      this.animationTime = 0.0;
      this._hierarchyVersion = 0;

      let binBuffer = null;
      if (arrayBuffer && arrayBuffer.byteLength >= 12) {
        const dv = new DataView(arrayBuffer, 0, 12);
        const magic = dv.getUint32(0, true);
        if (magic === 0x46546C67) {
          let byteOffset = 12;
          while (byteOffset + 8 <= arrayBuffer.byteLength) {
            const chunkDv = new DataView(arrayBuffer, byteOffset, 8);
            const chunkLen = chunkDv.getUint32(0, true);
            const chunkType = chunkDv.getUint32(4, true);
            if (chunkType === 0x004E4942) {
              binBuffer = arrayBuffer.slice(byteOffset + 8, byteOffset + 8 + chunkLen);
              break;
            }
            byteOffset += 8 + chunkLen;
          }
        }
      }

      const predecoded = (gltf && Array.isArray(gltf._bglEmbeddedBuffers)) ? gltf._bglEmbeddedBuffers : null;
      const embeddedBuffers = predecoded ? predecoded.slice() : [];
      if (gltf.buffers && !predecoded) {
        for (let i = 0; i < gltf.buffers.length; ++i) {
          const buf = gltf.buffers[i];
          if (buf.uri && buf.uri.startsWith('data:')) {
            const commaIdx = buf.uri.indexOf(',');
            if (commaIdx !== -1) {
              const base64Str = buf.uri.slice(commaIdx + 1);
              try {
                const binStr = atob(base64Str);
                const bytes = new Uint8Array(binStr.length);
                for (let b = 0; b < binStr.length; ++b) {
                  bytes[b] = binStr.charCodeAt(b);
                }
                embeddedBuffers[i] = bytes.buffer;
              } catch (e) {
                BglLogger.warn('glTF', 'Failed to decode base64 buffer URI:', e);
              }
            }
          }
        }
      }

      const getBufferViewData = (viewIdx) => {
        if (!gltf.bufferViews || !gltf.bufferViews[viewIdx]) return null;
        const bv = gltf.bufferViews[viewIdx];
        const offset = bv.byteOffset || 0;
        const length = bv.byteLength || 0;
        const bufferIdx = bv.buffer || 0;
        const src = (binBuffer && bufferIdx === 0) ? binBuffer : (embeddedBuffers[bufferIdx] || binBuffer || arrayBuffer);
        if (!src || offset + length > src.byteLength) {
          BglLogger.warn('glTF', `BufferView ${viewIdx} out of bounds: offset ${offset} + length ${length} > ${src ? src.byteLength : 0}`);
          return null;
        }
        return src.slice(offset, offset + length);
      };

      const getAccessorData = (accIdx) => {
        if (!gltf.accessors || !gltf.accessors[accIdx]) return null;
        const acc = gltf.accessors[accIdx];
        if (!acc.count) return null;
        if (acc.bufferView === undefined) return null;
        const bv = gltf.bufferViews ? gltf.bufferViews[acc.bufferView] : null;
        if (!bv) return null;
        const bvData = getBufferViewData(acc.bufferView);
        if (!bvData) return null;
        const byteOffset = acc.byteOffset || 0;

        let TypedArray = Float32Array;
        if (acc.componentType === 5126) TypedArray = Float32Array;
        else if (acc.componentType === 5123) TypedArray = Uint16Array;
        else if (acc.componentType === 5125) TypedArray = Uint32Array;
        else if (acc.componentType === 5121) TypedArray = Uint8Array;
        else if (acc.componentType === 5122) TypedArray = Int16Array;
        else if (acc.componentType === 5120) TypedArray = Int8Array;

        const numComp = (acc.type === 'SCALAR' ? 1 : acc.type === 'VEC2' ? 2 : acc.type === 'VEC3' ? 3 : acc.type === 'MAT4' ? 16 : 4);
        const elemBytes = TypedArray.BYTES_PER_ELEMENT;
        const stride = bv.byteStride || (numComp * elemBytes);

        if (byteOffset + (acc.count - 1) * stride + (numComp * elemBytes) > bvData.byteLength) {
          BglLogger.warn('glTF', `Accessor ${accIdx} exceeds buffer view byte length.`);
          return null;
        }

        if (stride === numComp * elemBytes && (byteOffset % elemBytes === 0)) {
          return new TypedArray(bvData, byteOffset, acc.count * numComp);
        }

        const totalElems = acc.count * numComp;
        const out = new TypedArray(totalElems);
        const dView = new DataView(bvData);
        for (let i = 0; i < acc.count; ++i) {
          const itemOffset = byteOffset + i * stride;
          for (let c = 0; c < numComp; ++c) {
            const valOffset = itemOffset + c * elemBytes;
            if (valOffset + elemBytes <= dView.byteLength) {
              if (acc.componentType === 5126) out[i * numComp + c] = dView.getFloat32(valOffset, true);
              else if (acc.componentType === 5123) out[i * numComp + c] = dView.getUint16(valOffset, true);
              else if (acc.componentType === 5125) out[i * numComp + c] = dView.getUint32(valOffset, true);
              else if (acc.componentType === 5121) out[i * numComp + c] = dView.getUint8(valOffset);
              else if (acc.componentType === 5122) out[i * numComp + c] = dView.getInt16(valOffset, true);
              else if (acc.componentType === 5120) out[i * numComp + c] = dView.getInt8(valOffset);
            }
          }
        }
        return out;
      };

      const getAccessorAsFloat32 = (accIdx) => {
        const raw = getAccessorData(accIdx);
        if (!raw) return null;
        if (raw instanceof Float32Array) return raw;
        const acc = gltf.accessors[accIdx];
        const out = new Float32Array(raw.length);
        if (acc.componentType === 5121) { // UNSIGNED_BYTE
          for (let i = 0; i < raw.length; ++i) out[i] = raw[i] / 255.0;
        } else if (acc.componentType === 5123) { // UNSIGNED_SHORT
          for (let i = 0; i < raw.length; ++i) out[i] = raw[i] / 65535.0;
        } else if (acc.componentType === 5120) { // BYTE
          for (let i = 0; i < raw.length; ++i) out[i] = Math.max(raw[i] / 127.0, -1.0);
        } else if (acc.componentType === 5122) { // SHORT
          for (let i = 0; i < raw.length; ++i) out[i] = Math.max(raw[i] / 32767.0, -1.0);
        } else {
          for (let i = 0; i < raw.length; ++i) out[i] = raw[i];
        }
        return out;
      };

      const meshNodeIndices = new Map();
      if (gltf.nodes) {
        for (let i = 0; i < gltf.nodes.length; ++i) {
          const gn = gltf.nodes[i];
          const nodeObj = {
            name: gn.name || `Node_${i}`,
            translation: gn.translation ? [gn.translation[0], gn.translation[1], gn.translation[2]] : [0, 0, 0],
            rotation: gn.rotation ? [gn.rotation[0], gn.rotation[1], gn.rotation[2], gn.rotation[3]] : [0, 0, 0, 1],
            scale: gn.scale ? [gn.scale[0], gn.scale[1], gn.scale[2]] : [1, 1, 1],
            matrix: gn.matrix ? new Float32Array(gn.matrix) : null,
            children: gn.children ? gn.children.slice() : [],
            mesh: gn.mesh,
            skin: gn.skin,
            localMatrix: this.mat4.create(),
            worldMatrix: this.mat4.create()
          };
          this.nodes.push(nodeObj);
          if (gn.mesh !== undefined) {
            if (!meshNodeIndices.has(gn.mesh)) {
              meshNodeIndices.set(gn.mesh, []);
            }
            meshNodeIndices.get(gn.mesh).push(i);
          }
        }

        const sceneIdx = gltf.scene || 0;
        this.rootNodes = (gltf.scenes && gltf.scenes[sceneIdx] && gltf.scenes[sceneIdx].nodes)
          ? gltf.scenes[sceneIdx].nodes.slice()
          : this.nodes.map((_, i) => i);

        this.updateNodeHierarchy();
      }

      // Parse glTF 2.0 Skins (Bones & Inverse Bind Matrices)
      if (gltf.skins && this.skinBindGroupLayout) {
        for (let s = 0; s < gltf.skins.length; ++s) {
          const gs = gltf.skins[s];
          const joints = gs.joints ? gs.joints.slice() : [];
          const jointCount = Math.max(1, joints.length);
          let ibm = null;
          if (gs.inverseBindMatrices !== undefined) {
            ibm = getAccessorData(gs.inverseBindMatrices);
          }
          const inverseBindMatrices = new Float32Array(jointCount * 16);
          for (let j = 0; j < jointCount; ++j) {
            if (ibm && j * 16 + 16 <= ibm.length) {
              for (let k = 0; k < 16; ++k) inverseBindMatrices[j * 16 + k] = ibm[j * 16 + k];
            } else {
              inverseBindMatrices[j * 16 + 0] = 1;
              inverseBindMatrices[j * 16 + 5] = 1;
              inverseBindMatrices[j * 16 + 10] = 1;
              inverseBindMatrices[j * 16 + 15] = 1;
            }
          }

          const jointMatricesData = new Float32Array(jointCount * 16);
          for (let j = 0; j < jointCount; ++j) {
            jointMatricesData[j * 16 + 0] = 1;
            jointMatricesData[j * 16 + 5] = 1;
            jointMatricesData[j * 16 + 10] = 1;
            jointMatricesData[j * 16 + 15] = 1;
          }

          const jointBuffer = this.device.createBuffer({
            size: Math.max(64, jointCount * 64),
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST
          });
          this.device.queue.writeBuffer(jointBuffer, 0, jointMatricesData);

          const bindGroup = this.device.createBindGroup({
            layout: this.skinBindGroupLayout,
            entries: [{ binding: 0, resource: { buffer: jointBuffer } }]
          });

          this.skins.push({
            name: gs.name || `Skin_${s}`,
            joints,
            inverseBindMatrices,
            jointMatricesData,
            jointBuffer,
            bindGroup,
            meshNodeIdx: -1
          });
        }
      }

      // Parse glTF 2.0 Animations (Samplers & Channels)
      if (gltf.animations) {
        for (let a = 0; a < gltf.animations.length; ++a) {
          const ga = gltf.animations[a];
          const samplers = [];
          let duration = 0.0;
          if (ga.samplers) {
            for (const gs of ga.samplers) {
              const input = getAccessorAsFloat32(gs.input) || new Float32Array(0);
              const output = getAccessorAsFloat32(gs.output) || new Float32Array(0);
              const accOut = (gs.output !== undefined && gltf.accessors) ? gltf.accessors[gs.output] : null;
              const stride = accOut
                ? (accOut.type === 'SCALAR' ? 1 : accOut.type === 'VEC2' ? 2 : accOut.type === 'VEC3' ? 3 : 4)
                : 3;
              if (input.length > 0) {
                duration = Math.max(duration, input[input.length - 1]);
              }
              samplers.push({
                input,
                output,
                interpolation: gs.interpolation || 'LINEAR',
                stride
              });
            }
          }
          const channels = [];
          if (ga.channels) {
            for (const gc of ga.channels) {
              if (gc.target && gc.target.node !== undefined && gc.target.path) {
                channels.push({
                  sampler: gc.sampler,
                  targetNode: gc.target.node,
                  path: gc.target.path
                });
              }
            }
          }
          this.animations.push({
            name: ga.name || `Animation ${a}`,
            samplers,
            channels,
            duration
          });
        }
        if (this.animations.length > 0) {
          this.activeAnimationIndex = 0;
          this.animationTime = 0.0;
          BglLogger.log('Animation', `Loaded ${this.animations.length} glTF animation(s), ${this.skins.length} skin(s). Active: '${this.animations[0].name}' (${this.animations[0].duration.toFixed(2)}s)`);
        }
      }

      let minBound = [Infinity, Infinity, Infinity];
      let maxBound = [-Infinity, -Infinity, -Infinity];

      if (gltf.meshes) {
        gltf.meshes.forEach((mesh, meshIdx) => {
          if (!mesh.primitives) return;
          const nodeIdxList = meshNodeIndices.get(meshIdx) || [-1];

          for (const prim of mesh.primitives) {
            if (!prim.attributes || prim.attributes.POSITION === undefined) continue;

            const posData = getAccessorData(prim.attributes.POSITION);
            if (!posData) continue;

            const normData = prim.attributes.NORMAL !== undefined ? getAccessorData(prim.attributes.NORMAL) : null;
            const uvData = prim.attributes.TEXCOORD_0 !== undefined ? getAccessorData(prim.attributes.TEXCOORD_0) : null;
            const tanData = prim.attributes.TANGENT !== undefined ? getAccessorData(prim.attributes.TANGENT) : null;
            const jointsData = prim.attributes.JOINTS_0 !== undefined ? getAccessorData(prim.attributes.JOINTS_0) : null;
            const weightsData = prim.attributes.WEIGHTS_0 !== undefined ? getAccessorAsFloat32(prim.attributes.WEIGHTS_0) : null;
            const indexData = prim.indices !== undefined ? getAccessorData(prim.indices) : null;

            for (const nIdx of nodeIdxList) {
              const worldMatrix = (nIdx >= 0 && this.nodes[nIdx]) ? this.nodes[nIdx].worldMatrix : this.mat4.create();
              for (let i = 0; i < posData.length; i += 3) {
                const lx = posData[i], ly = posData[i + 1], lz = posData[i + 2];
                const wx = worldMatrix[0] * lx + worldMatrix[4] * ly + worldMatrix[8] * lz + worldMatrix[12];
                const wy = worldMatrix[1] * lx + worldMatrix[5] * ly + worldMatrix[9] * lz + worldMatrix[13];
                const wz = worldMatrix[2] * lx + worldMatrix[6] * ly + worldMatrix[10] * lz + worldMatrix[14];
                minBound[0] = Math.min(minBound[0], wx); maxBound[0] = Math.max(maxBound[0], wx);
                minBound[1] = Math.min(minBound[1], wy); maxBound[1] = Math.max(maxBound[1], wy);
                minBound[2] = Math.min(minBound[2], wz); maxBound[2] = Math.max(maxBound[2], wz);
              }
            }

            const vertexCount = posData.length / 3;
            const interleaved = new Float32Array(vertexCount * 12);
            for (let i = 0; i < vertexCount; ++i) {
              const base = i * 12;
              const pIdx = i * 3;
              interleaved[base + 0] = posData[pIdx + 0];
              interleaved[base + 1] = posData[pIdx + 1];
              interleaved[base + 2] = posData[pIdx + 2];

              if (normData) {
                interleaved[base + 3] = normData[pIdx + 0];
                interleaved[base + 4] = normData[pIdx + 1];
                interleaved[base + 5] = normData[pIdx + 2];
              } else {
                interleaved[base + 3] = 0.0;
                interleaved[base + 4] = 1.0;
                interleaved[base + 5] = 0.0;
              }

              if (uvData) {
                const uIdx = i * 2;
                interleaved[base + 6] = uvData[uIdx + 0];
                interleaved[base + 7] = uvData[uIdx + 1];
              } else {
                interleaved[base + 6] = 0.0;
                interleaved[base + 7] = 0.0;
              }

              if (tanData) {
                const tIdx = i * 4;
                interleaved[base + 8] = tanData[tIdx + 0];
                interleaved[base + 9] = tanData[tIdx + 1];
                interleaved[base + 10] = tanData[tIdx + 2];
                interleaved[base + 11] = tanData[tIdx + 3];
              } else {
                interleaved[base + 8] = 0.0;
                interleaved[base + 9] = 0.0;
                interleaved[base + 10] = 0.0;
                interleaved[base + 11] = 0.0;
              }
            }

            const vertexBuffer = this.device.createBuffer({
              size: Math.max(48, interleaved.byteLength),
              usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST
            });
            this.device.queue.writeBuffer(vertexBuffer, 0, interleaved);

            let skinVertexBuffer = null;
            if (jointsData && weightsData && this.skinnedPipeline) {
              const skinBytes = new ArrayBuffer(Math.max(32, vertexCount * 32));
              const skinU32 = new Uint32Array(skinBytes);
              const skinF32 = new Float32Array(skinBytes);
              for (let i = 0; i < vertexCount; ++i) {
                const jIdx = i * 4;
                const dBase = i * 8;
                skinU32[dBase + 0] = jointsData[jIdx + 0] || 0;
                skinU32[dBase + 1] = jointsData[jIdx + 1] || 0;
                skinU32[dBase + 2] = jointsData[jIdx + 2] || 0;
                skinU32[dBase + 3] = jointsData[jIdx + 3] || 0;

                let w0 = weightsData[jIdx + 0] || 0;
                let w1 = weightsData[jIdx + 1] || 0;
                let w2 = weightsData[jIdx + 2] || 0;
                let w3 = weightsData[jIdx + 3] || 0;
                const wSum = w0 + w1 + w2 + w3;
                if (wSum > 1e-6) {
                  const invW = 1.0 / wSum;
                  w0 *= invW; w1 *= invW; w2 *= invW; w3 *= invW;
                } else {
                  w0 = 1.0; w1 = 0.0; w2 = 0.0; w3 = 0.0;
                }
                skinF32[dBase + 4] = w0;
                skinF32[dBase + 5] = w1;
                skinF32[dBase + 6] = w2;
                skinF32[dBase + 7] = w3;
              }
              skinVertexBuffer = this.device.createBuffer({
                size: skinBytes.byteLength,
                usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST
              });
              this.device.queue.writeBuffer(skinVertexBuffer, 0, skinBytes);
            }

            let indexBuffer = null;
            let indexFormat = 'uint16';
            let indexCount = 0;
            let lodSlices = null;
            let localCenter = [0, 0, 0];
            let localRadius = 1.0;
            if (indexData) {
              const lodInfo = this.generateLodData(posData, normData, indexData, vertexCount);
              const gpuIndexData = lodInfo.gpuIndexData;
              indexFormat = lodInfo.indexFormat;
              indexCount = lodInfo.indexCount;
              lodSlices = lodInfo.lodSlices;
              localCenter = lodInfo.localCenter;
              localRadius = lodInfo.localRadius;

              // Ensure buffer size and write size are strictly multiples of 4 (W3C WebGPU specification requirement)
              const rawByteLength = gpuIndexData.byteLength;
              const alignedByteLength = Math.max(4, Math.ceil(rawByteLength / 4) * 4);
              indexBuffer = this.device.createBuffer({
                size: alignedByteLength,
                usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST
              });

              if (rawByteLength % 4 === 0) {
                this.device.queue.writeBuffer(indexBuffer, 0, gpuIndexData);
              } else {
                const paddedBytes = new Uint8Array(alignedByteLength);
                paddedBytes.set(new Uint8Array(gpuIndexData.buffer, gpuIndexData.byteOffset, rawByteLength));
                this.device.queue.writeBuffer(indexBuffer, 0, paddedBytes);
              }
            }

            // Material parsing (PBR Metallic-Roughness, Normal, Occlusion, Emissive, Alpha, Transform)
            let baseColorFactor = [1.0, 1.0, 1.0, 1.0];
            let metallicFactor = 1.0;
            let roughnessFactor = 1.0;
            let normalScale = 1.0;
            let occlusionStrength = 1.0;
            let emissiveFactor = [0.0, 0.0, 0.0];
            let alphaCutoff = 0.5;
            let alphaMode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND
            let doubleSided = false;
            const extractUvTransform = (texInfo) => {
              const tt = texInfo && texInfo.extensions && texInfo.extensions.KHR_texture_transform;
              return {
                scale: (tt && tt.scale) ? [tt.scale[0], tt.scale[1]] : [1.0, 1.0],
                offset: (tt && tt.offset) ? [tt.offset[0], tt.offset[1]] : [0.0, 0.0],
                rotation: (tt && tt.rotation !== undefined) ? tt.rotation : 0.0
              };
            };
            let baseUvTx = extractUvTransform(null);
            let mrUvTx = extractUvTransform(null);
            let normalUvTx = extractUvTransform(null);
            let occlusionUvTx = extractUvTransform(null);
            let emissiveUvTx = extractUvTransform(null);

            if (prim.material !== undefined && gltf.materials && gltf.materials[prim.material]) {
              const mat = gltf.materials[prim.material];
              if (mat.doubleSided) doubleSided = true;
              if (mat.alphaMode === 'MASK') alphaMode = 1;
              else if (mat.alphaMode === 'BLEND') alphaMode = 2;
              if (mat.alphaCutoff !== undefined) alphaCutoff = mat.alphaCutoff;
              if (mat.emissiveFactor) emissiveFactor = mat.emissiveFactor;

              if (mat.pbrMetallicRoughness) {
                const pbr = mat.pbrMetallicRoughness;
                if (pbr.baseColorFactor) baseColorFactor = pbr.baseColorFactor;
                if (pbr.metallicFactor !== undefined) metallicFactor = pbr.metallicFactor;
                if (pbr.roughnessFactor !== undefined) roughnessFactor = pbr.roughnessFactor;
                if (pbr.baseColorTexture) baseUvTx = extractUvTransform(pbr.baseColorTexture);
                if (pbr.metallicRoughnessTexture) mrUvTx = extractUvTransform(pbr.metallicRoughnessTexture);
              }

              if (mat.normalTexture) {
                if (mat.normalTexture.scale !== undefined) normalScale = mat.normalTexture.scale;
                normalUvTx = extractUvTransform(mat.normalTexture);
              }

              if (mat.occlusionTexture) {
                if (mat.occlusionTexture.strength !== undefined) {
                  occlusionStrength = mat.occlusionTexture.strength;
                }
                occlusionUvTx = extractUvTransform(mat.occlusionTexture);
              }

              if (mat.emissiveTexture) {
                emissiveUvTx = extractUvTransform(mat.emissiveTexture);
              }
            }
            const uvScale = baseUvTx.scale;
            const uvOffset = baseUvTx.offset;

            const currentPrimRecords = [];
            for (const nIdx of nodeIdxList) {
              const nodeObj = (nIdx >= 0 && this.nodes[nIdx]) ? this.nodes[nIdx] : null;
              const worldMatrix = nodeObj ? nodeObj.worldMatrix : this.mat4.create();
              const skinIdx = (nodeObj && nodeObj.skin !== undefined && this.skins[nodeObj.skin]) ? nodeObj.skin : -1;
              const isSkinned = (skinIdx >= 0 && !!skinVertexBuffer);
              if (isSkinned && this.skins[skinIdx].meshNodeIdx < 0) {
                this.skins[skinIdx].meshNodeIdx = nIdx;
              }

              const uniformBuffer = this.device.createBuffer({
                size: 512,
                usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
              });

              const primRecord = {
                vertexBuffer,
                skinVertexBuffer: isSkinned ? skinVertexBuffer : null,
                indexBuffer,
                indexCount,
                indexFormat,
                lodSlices,
                localCenter,
                localRadius,
                worldCenter: [0, 0, 0],
                worldRadius: localRadius,
                normalMatrix: this.mat4.normalMatrix4(this.mat4.create(), worldMatrix),
                worldMatrixVersion: -1,
                vertexCount,
                hasIndices: !!indexData,
                uniformBuffer,
                bindGroup: null,
                baseColorFactor,
                metallicFactor,
                roughnessFactor,
                normalScale,
                occlusionStrength,
                emissiveFactor,
                alphaCutoff,
                alphaMode,
                doubleSided,
                uvScale,
                uvOffset,
                uvTransforms: {
                  baseColor: baseUvTx,
                  mr: mrUvTx,
                  normal: normalUvTx,
                  occlusion: occlusionUvTx,
                  emissive: emissiveUvTx
                },
                materialIdx: prim.material !== undefined ? prim.material : -1,
                flags: 0,
                baseColorView: null,
                mrView: null,
                normalView: null,
                emissiveView: null,
                occlusionView: null,
                modelMatrix: worldMatrix,
                nodeIdx: nIdx,
                skinIdx,
                isSkinned,
                updateBindGroup: null
              };

              primRecord.updateBindGroup = () => {
                primRecord.bindGroup = this.device.createBindGroup({
                  layout: this.bindGroupLayout,
                  entries: [
                    { binding: 0, resource: { buffer: primRecord.uniformBuffer } },
                    { binding: 1, resource: this.defaultSampler },
                    { binding: 2, resource: primRecord.baseColorView || this.defaultWhiteTextureView },
                    { binding: 3, resource: primRecord.mrView || this.defaultMrTextureView },
                    { binding: 4, resource: primRecord.normalView || this.defaultNormalTextureView },
                    { binding: 5, resource: primRecord.emissiveView || this.defaultBlackTextureView },
                    { binding: 6, resource: primRecord.occlusionView || this.defaultWhiteTextureView }
                  ]
                });
              };

              primRecord.updateBindGroup();
              currentPrimRecords.push(primRecord);
              this.primitives.push(primRecord);
            }

            // Asynchronously load textures and update bind groups
            if (prim.material !== undefined && gltf.materials && gltf.materials[prim.material]) {
              const mat = gltf.materials[prim.material];

              const loadTextureImage = (texInfo, viewSetter, flagBit) => {
                if (!texInfo || texInfo.index === undefined || !gltf.textures || !gltf.textures[texInfo.index]) return;
                const texObj = gltf.textures[texInfo.index];
                if (texObj.source === undefined || !gltf.images || !gltf.images[texObj.source]) return;
                const imgIdx = texObj.source;
                const imgObj = gltf.images[imgIdx];

                const applyView = (view) => {
                  for (const pr of currentPrimRecords) {
                    viewSetter(pr, view);
                    pr.flags |= flagBit;
                    pr.updateBindGroup();
                  }
                };

                if (this.textureCache.has(imgIdx)) {
                  const cached = this.textureCache.get(imgIdx);
                  if (cached.view) {
                    applyView(cached.view);
                  } else if (cached.waiters) {
                    cached.waiters.push(applyView);
                  }
                  return;
                }

                // Register pending entry immediately to deduplicate concurrent async decodes across primitives
                this.textureCache.set(imgIdx, { texture: null, view: null, waiters: [applyView] });

                const onImageReady = (bmp) => {
                  try {
                    if (!this.device) return;
                    const mipLevelCount = Math.max(1, Math.floor(Math.log2(Math.max(bmp.width || 1, bmp.height || 1))) + 1);
                    const gpuTex = this.device.createTexture({
                      size: [bmp.width, bmp.height, 1],
                      mipLevelCount,
                      format: 'rgba8unorm',
                      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT
                    });
                    this.device.queue.copyExternalImageToTexture(
                      { source: bmp },
                      { texture: gpuTex, mipLevel: 0 },
                      [bmp.width, bmp.height]
                    );
                    if (mipLevelCount > 1) {
                      this.queueMipmapGeneration(gpuTex, bmp.width, bmp.height, mipLevelCount);
                    }
                    if (typeof bmp.close === 'function') {
                      bmp.close();
                    }
                    const gpuView = gpuTex.createView();
                    const prevEntry = this.textureCache.get(imgIdx);
                    const waiters = (prevEntry && prevEntry.waiters) ? prevEntry.waiters : [applyView];
                    this.textureCache.set(imgIdx, { texture: gpuTex, view: gpuView, waiters: null });
                    for (const fn of waiters) {
                      fn(gpuView);
                    }
                  } catch (texErr) {
                    BglLogger.warn('WebGPU', `Failed to upload texture image #${imgIdx}:`, texErr);
                  }
                };

                const decodeBitmap = (srcObj) => {
                  const opts = { colorSpaceConversion: 'none' };
                  createImageBitmap(srcObj, opts).then(onImageReady).catch(() => {
                    createImageBitmap(srcObj).then(onImageReady).catch((err) => {
                      BglLogger.warn('WebGPU', `createImageBitmap failed for image #${imgIdx}:`, err);
                    });
                  });
                };

                if (imgObj.bufferView !== undefined) {
                  const imgBytes = getBufferViewData(imgObj.bufferView);
                  if (imgBytes) {
                    const blob = new Blob([imgBytes], { type: imgObj.mimeType || 'image/png' });
                    decodeBitmap(blob);
                  }
                } else if (imgObj.uri) {
                  const img = new Image();
                  img.crossOrigin = 'anonymous';
                  img.onload = () => {
                    decodeBitmap(img);
                  };
                  img.src = imgObj.uri;
                }
              };

              if (mat.pbrMetallicRoughness) {
                if (mat.pbrMetallicRoughness.baseColorTexture) {
                  loadTextureImage(mat.pbrMetallicRoughness.baseColorTexture, (pr, v) => { pr.baseColorView = v; }, 1);
                }
                if (mat.pbrMetallicRoughness.metallicRoughnessTexture) {
                  loadTextureImage(mat.pbrMetallicRoughness.metallicRoughnessTexture, (pr, v) => { pr.mrView = v; }, 2);
                }
              }

              if (mat.normalTexture) {
                loadTextureImage(mat.normalTexture, (pr, v) => { pr.normalView = v; }, 4);
              }
              if (mat.emissiveTexture) {
                loadTextureImage(mat.emissiveTexture, (pr, v) => { pr.emissiveView = v; }, 8);
              }
              if (mat.occlusionTexture) {
                loadTextureImage(mat.occlusionTexture, (pr, v) => { pr.occlusionView = v; }, 16);
              }
            }
          }
        });
      }

      if (minBound[0] <= maxBound[0]) {
        const cx = (minBound[0] + maxBound[0]) * 0.5;
        const cy = (minBound[1] + maxBound[1]) * 0.5;
        const cz = (minBound[2] + maxBound[2]) * 0.5;
        const dx = maxBound[0] - minBound[0];
        const dy = maxBound[1] - minBound[1];
        const dz = maxBound[2] - minBound[2];
        const rawRadius = Math.hypot(dx, dy, dz) * 0.5;
        const radius = Math.max(rawRadius, 0.0001);

        const isSponza = /sponza/i.test(modelName || '') || (dx > 25.0 && dx < 32.0 && dy > 10.0 && dy < 15.0 && dz > 15.0 && dz < 22.0);
        if (isSponza) {
          this.camera.target = [cx, minBound[1] + dy * 0.25, cz];
          this.camera.distance = 7.2;
          this.camera.yaw = 1.5708;
          this.camera.pitch = 0.06;
        } else {
          this.camera.target = [cx, cy, cz];
          this.camera.distance = radius * 2.2;
        }
        this.camera.minDistance = Math.max(0.0001, radius * 0.02);
        this.camera.maxDistance = Math.max(10.0, radius * 35.0);
        this.camera.near = Math.max(0.0001, radius * 0.008);
        this.camera.far = Math.max(100.0, radius * 50.0);
        this.modelBounds = { center: [cx, cy, cz], radius, isSponza, minBound, maxBound };
      }

      if (this.animations.length > 0 && this.activeAnimationIndex >= 0) {
        this.evaluateAnimation(this.activeAnimationIndex, 0.0);
      } else {
        this.updateNodeHierarchy();
      }
      this.updateSkinMatrices();
      this.rebuildSortedPrimitives();

      if (typeof updateAnimationDropdown === 'function') {
        updateAnimationDropdown();
      }

      BglLogger.log('WebGPU', `Loaded '${modelName}' into WebGPU rendering pipeline (${this.primitives.length} primitives, ${this.skins.length} skins, ${this.animations.length} animations).`);
    },

    rebuildSortedPrimitives() {
      this._sortedPrimitives = this.primitives.slice().sort((a, b) => {
        const transA = a.alphaMode === 2 ? 1 : 0;
        const transB = b.alphaMode === 2 ? 1 : 0;
        if (transA !== transB) return transA - transB;
        const skinA = a.isSkinned ? 1 : 0;
        const skinB = b.isSkinned ? 1 : 0;
        if (skinA !== skinB) return skinA - skinB;
        const dsA = a.doubleSided ? 1 : 0;
        const dsB = b.doubleSided ? 1 : 0;
        if (dsA !== dsB) return dsA - dsB;
        return (a.materialIdx || 0) - (b.materialIdx || 0);
      });
    },

    updateNodeHierarchy() {
      this._hierarchyVersion = (this._hierarchyVersion || 0) + 1;
      const updateNode = (nodeIdx, parentWorld) => {
        const node = this.nodes[nodeIdx];
        if (!node) return;
        if (node.matrix) {
          node.localMatrix.set(node.matrix);
        } else {
          this.mat4.fromRotationTranslationScale(node.localMatrix, node.rotation, node.translation, node.scale);
        }
        if (parentWorld) {
          this.mat4.multiply(node.worldMatrix, parentWorld, node.localMatrix);
        } else {
          node.worldMatrix.set(node.localMatrix);
        }
        for (let i = 0; i < node.children.length; ++i) {
          updateNode(node.children[i], node.worldMatrix);
        }
      };
      for (let i = 0; i < this.rootNodes.length; ++i) {
        updateNode(this.rootNodes[i], null);
      }
    },

    sampleAnimationSampler(sampler, t, path, out) {
      const timestamps = sampler.input;
      const values = sampler.output;
      const stride = sampler.stride;
      const count = timestamps.length;
      if (count === 0 || values.length === 0 || stride === 0) return;

      const copyKeyframe = (kfIdx) => {
        const off = (sampler.interpolation === 'CUBICSPLINE')
          ? (kfIdx * 3 + 1) * stride
          : kfIdx * stride;
        if (off + stride <= values.length) {
          for (let i = 0; i < stride; ++i) out[i] = values[off + i];
        }
      };

      const normalizeQuat = () => {
        if (path === 'rotation' && stride >= 4) {
          const len = Math.hypot(out[0], out[1], out[2], out[3]);
          if (len > 1e-8) {
            const inv = 1.0 / len;
            out[0] *= inv; out[1] *= inv; out[2] *= inv; out[3] *= inv;
          } else {
            out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
          }
        }
      };

      if (t <= timestamps[0]) {
        copyKeyframe(0);
        normalizeQuat();
        return;
      }
      if (t >= timestamps[count - 1]) {
        copyKeyframe(count - 1);
        normalizeQuat();
        return;
      }

      // Binary search for surrounding keyframes k and k+1
      let lo = 0, hi = count - 1;
      while (lo + 1 < hi) {
        const mid = (lo + hi) >> 1;
        if (timestamps[mid] <= t) lo = mid;
        else hi = mid;
      }
      const k = lo;
      const k1 = Math.min(k + 1, count - 1);

      if (k === k1 || sampler.interpolation === 'STEP') {
        copyKeyframe(k);
        normalizeQuat();
        return;
      }

      const t0 = timestamps[k];
      const t1 = timestamps[k1];
      const dt = t1 - t0;
      const alpha = dt > 1e-7 ? Math.max(0.0, Math.min(1.0, (t - t0) / dt)) : 0.0;

      if (sampler.interpolation === 'LINEAR') {
        const off0 = k * stride;
        const off1 = k1 * stride;
        if (off1 + stride > values.length) return;

        if (path === 'rotation' && stride >= 4) {
          let ax = values[off0 + 0], ay = values[off0 + 1], az = values[off0 + 2], aw = values[off0 + 3];
          let bx = values[off1 + 0], by = values[off1 + 1], bz = values[off1 + 2], bw = values[off1 + 3];
          let dot = ax * bx + ay * by + az * bz + aw * bw;
          if (dot < 0.0) {
            dot = -dot;
            bx = -bx; by = -by; bz = -bz; bw = -bw;
          }
          let s0, s1;
          if (dot > 0.9995) {
            s0 = 1.0 - alpha;
            s1 = alpha;
          } else {
            const omega = Math.acos(Math.min(1.0, dot));
            const sinOmega = Math.sin(omega);
            s0 = Math.sin((1.0 - alpha) * omega) / sinOmega;
            s1 = Math.sin(alpha * omega) / sinOmega;
          }
          out[0] = s0 * ax + s1 * bx;
          out[1] = s0 * ay + s1 * by;
          out[2] = s0 * az + s1 * bz;
          out[3] = s0 * aw + s1 * bw;
          normalizeQuat();
        } else {
          for (let i = 0; i < stride; ++i) {
            out[i] = values[off0 + i] + (values[off1 + i] - values[off0 + i]) * alpha;
          }
        }
        return;
      }

      if (sampler.interpolation === 'CUBICSPLINE') {
        const kStride3 = k * 3 * stride;
        const k1Stride3 = k1 * 3 * stride;
        if (k1Stride3 + 3 * stride > values.length) return;

        const v0Off = kStride3 + stride;
        const b0Off = kStride3 + 2 * stride;
        const a1Off = k1Stride3;
        const v1Off = k1Stride3 + stride;

        const s = alpha, s2 = s * s, s3 = s2 * s;
        const h00 = 2 * s3 - 3 * s2 + 1;
        const h10 = s3 - 2 * s2 + s;
        const h01 = -2 * s3 + 3 * s2;
        const h11 = s3 - s2;

        for (let i = 0; i < stride; ++i) {
          out[i] = h00 * values[v0Off + i] + h10 * dt * values[b0Off + i] + h01 * values[v1Off + i] + h11 * dt * values[a1Off + i];
        }
        normalizeQuat();
      }
    },

    evaluateAnimation(animIdx, time) {
      if (animIdx < 0 || animIdx >= this.animations.length) return;
      const anim = this.animations[animIdx];
      const val = this._scratchSampleVal;
      for (let i = 0; i < anim.channels.length; ++i) {
        const ch = anim.channels[i];
        const sampler = anim.samplers[ch.sampler];
        const node = this.nodes[ch.targetNode];
        if (!sampler || !node) continue;

        this.sampleAnimationSampler(sampler, time, ch.path, val);
        if (ch.path === 'translation') {
          node.translation[0] = val[0];
          node.translation[1] = val[1];
          node.translation[2] = val[2];
          node.matrix = null;
        } else if (ch.path === 'rotation') {
          node.rotation[0] = val[0];
          node.rotation[1] = val[1];
          node.rotation[2] = val[2];
          node.rotation[3] = val[3];
          node.matrix = null;
        } else if (ch.path === 'scale') {
          node.scale[0] = val[0];
          node.scale[1] = val[1];
          node.scale[2] = val[2];
          node.matrix = null;
        }
      }
      this.updateNodeHierarchy();
    },

    updateSkinMatrices() {
      if (!this.device || this.skins.length === 0) return;
      const invMeshWorld = this._scratchInvMat;
      const jointMat = this._scratchJointMat;
      for (let s = 0; s < this.skins.length; ++s) {
        const skin = this.skins[s];
        const meshNode = (skin.meshNodeIdx >= 0 && this.nodes[skin.meshNodeIdx]) ? this.nodes[skin.meshNodeIdx] : null;
        if (meshNode) {
          this.mat4.invert(invMeshWorld, meshNode.worldMatrix);
        } else {
          this.mat4.identity(invMeshWorld);
        }
        const out = skin.jointMatricesData;
        for (let j = 0; j < skin.joints.length; ++j) {
          const jNode = this.nodes[skin.joints[j]];
          if (jNode) {
            const ibm = skin.inverseBindMatrices.subarray(j * 16, j * 16 + 16);
            this.mat4.multiply(jointMat, jNode.worldMatrix, ibm);
            this.mat4.multiply(out.subarray(j * 16, j * 16 + 16), invMeshWorld, jointMat);
          }
        }
        this.device.queue.writeBuffer(skin.jointBuffer, 0, out);
      }
    },

    setAnimation(index) {
      if (index >= 0 && index < this.animations.length) {
        this.activeAnimationIndex = index;
        this.animationTime = 0.0;
        this.animationPlaying = true;
        this.evaluateAnimation(index, 0.0);
        this.updateSkinMatrices();
      } else {
        this.activeAnimationIndex = -1;
      }
    },

    setAnimationPlaying(playing) {
      this.animationPlaying = !!playing;
    },

    encodeMipmapPasses(encoder, gpuTex, mipLevelCount) {
      for (let mip = 1; mip < mipLevelCount; ++mip) {
        const srcView = gpuTex.createView({ baseMipLevel: mip - 1, mipLevelCount: 1 });
        const dstView = gpuTex.createView({ baseMipLevel: mip, mipLevelCount: 1 });
        const bindGroup = this.device.createBindGroup({
          layout: this.mipmapBindGroupLayout,
          entries: [
            { binding: 1, resource: this.mipmapSampler || this.defaultSampler },
            { binding: 2, resource: srcView }
          ]
        });
        const pass = encoder.beginRenderPass({
          colorAttachments: [{
            view: dstView,
            clearValue: { r: 0, g: 0, b: 0, a: 1 },
            loadOp: 'clear',
            storeOp: 'store'
          }]
        });
        pass.setPipeline(this.mipmapPipeline);
        pass.setBindGroup(0, bindGroup);
        pass.draw(3);
        pass.end();
      }
    },

    queueMipmapGeneration(gpuTex, width, height, mipLevelCount) {
      if (!this.device || !this.mipmapPipeline || mipLevelCount <= 1 || typeof this.device.createCommandEncoder !== 'function') return;
      if (!this._pendingMipmaps) this._pendingMipmaps = [];
      this._pendingMipmaps.push({ gpuTex, mipLevelCount });
      if (!this._mipBatchScheduled) {
        this._mipBatchScheduled = true;
        const schedule = typeof queueMicrotask === 'function' ? queueMicrotask : (fn) => Promise.resolve().then(fn);
        schedule(() => this.flushPendingMipmaps());
      }
    },

    flushPendingMipmaps() {
      this._mipBatchScheduled = false;
      if (!this._pendingMipmaps || this._pendingMipmaps.length === 0) return;
      if (!this.device || !this.mipmapPipeline || typeof this.device.createCommandEncoder !== 'function') {
        this._pendingMipmaps.length = 0;
        return;
      }
      const batch = this._pendingMipmaps.splice(0, this._pendingMipmaps.length);
      try {
        const encoder = this.device.createCommandEncoder();
        for (let i = 0; i < batch.length; ++i) {
          this.encodeMipmapPasses(encoder, batch[i].gpuTex, batch[i].mipLevelCount);
        }
        this.device.queue.submit([encoder.finish()]);
      } catch (mipErr) {
        BglLogger.warn('WebGPU', 'Batched mipmap generation notice:', mipErr);
      }
    },

    generateMipmaps(gpuTex, width, height, mipLevelCount) {
      if (!this.device || !this.mipmapPipeline || mipLevelCount <= 1 || typeof this.device.createCommandEncoder !== 'function') return;
      try {
        const encoder = this.device.createCommandEncoder();
        this.encodeMipmapPasses(encoder, gpuTex, mipLevelCount);
        this.device.queue.submit([encoder.finish()]);
      } catch (mipErr) {
        BglLogger.warn('WebGPU', 'Mipmap generation notice:', mipErr);
      }
    },

    stepFrame(time) {
      if (!this.isActive || !this.device || !this.context) return;
      const dt = (time - this.lastFrameTime) * 0.001;
      this.lastFrameTime = time;
      if (this.turntable) {
        this.camera.yaw += this.turntableSpeed * dt;
      }
      this.draw();
    },

    startLoop() {
      if (typeof BglFrameScheduler !== 'undefined' && BglFrameScheduler.registerRenderer) {
        BglFrameScheduler.registerRenderer((time) => this.stepFrame(time));
        return;
      }
      const render = (time) => {
        if (!this.isActive || !this.device || !this.context) return;
        this.stepFrame(time);
        requestAnimationFrame(render);
      };
      requestAnimationFrame(render);
    },

    draw() {
      if (!this.isActive || !this.device || !this.context) return;

      this.flushPendingMipmaps();

      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = Math.max(1, Math.round((this.canvas.clientWidth || window.innerWidth) * dpr));
      const h = Math.max(1, Math.round((this.canvas.clientHeight || window.innerHeight) * dpr));

      if (this.canvas.width !== w || this.canvas.height !== h || !this.depthTexture) {
        this.canvas.width = w;
        this.canvas.height = h;

        if (this.depthTexture) {
          this.depthTexture.destroy();
        }
        this.depthTexture = this.device.createTexture({
          size: [w, h, 1],
          format: 'depth24plus',
          usage: GPUTextureUsage.RENDER_ATTACHMENT
        });
        this.depthTextureView = this.depthTexture.createView();

        this.context.configure({
          device: this.device,
          format: this.format,
          alphaMode: 'opaque'
        });
      }

      let currentTexture;
      try {
        currentTexture = this.context.getCurrentTexture();
      } catch (texErr) {
        return;
      }

      const nowMs = performance.now();
      const dtSec = this._lastDrawMs ? Math.max(0.0, Math.min((nowMs - this._lastDrawMs) * 0.001, 0.1)) : (1.0 / 60.0);
      this._lastDrawMs = nowMs;
      const shouldUpdateDom = !this._lastDomTelemetryMs || (nowMs - this._lastDomTelemetryMs) >= 200;
      if (shouldUpdateDom) {
        this._lastDomTelemetryMs = nowMs;
      }
      const uiCache = (typeof BglUI !== 'undefined') ? BglUI : null;

      if (this.animations.length > 0 && this.activeAnimationIndex >= 0 && this.animationPlaying) {
        const anim = this.animations[this.activeAnimationIndex];
        if (anim && anim.duration > 0.0) {
          this.animationTime = (this.animationTime + dtSec * this.animationSpeed) % anim.duration;
          this.evaluateAnimation(this.activeAnimationIndex, this.animationTime);
          this.updateSkinMatrices();
        }
      }

      if (typeof BglState !== 'undefined' && BglState.isDayNightCycleActive) {
        const speed = (BglState.dayNightSpeed !== undefined) ? BglState.dayNightSpeed : 1.0;
        let nextTod = ((BglState.timeOfDay !== undefined ? BglState.timeOfDay : 14.0) + speed * dtSec) % 24.0;
        if (nextTod < 0.0) nextTod += 24.0;
        BglState.timeOfDay = nextTod;
        if (shouldUpdateDom) {
          const sliderTod = uiCache ? uiCache.sliderTimeOfDay : document.getElementById('slider-time-of-day');
          if (sliderTod) sliderTod.value = nextTod.toFixed(2);
          const labelTod = uiCache ? uiCache.valTimeOfDay : document.getElementById('val-time-of-day');
          if (labelTod) {
            const hrs = Math.floor(nextTod) % 24;
            const mins = Math.floor((nextTod - Math.floor(nextTod)) * 60) % 60;
            const icon = (nextTod >= 7.0 && nextTod < 17.5) ? '☀️' : ((nextTod >= 5.5 && nextTod < 19.5) ? '🌅' : '🌙');
            labelTod.textContent = `${String(hrs).padStart(2, '0')}:${String(mins).padStart(2, '0')} ${icon}`;
          }
        }
      }

      const tod = (typeof BglState !== 'undefined' && BglState.timeOfDay !== undefined) ? BglState.timeOfDay : 14.0;
      const solarElev = -Math.cos((tod / 24.0) * 2.0 * Math.PI);
      const dayBlend = Math.max(0.0, Math.min(1.0, (solarElev + 0.15) / 0.85));
      let clearR = 0.015 + 0.045 * dayBlend;
      let clearG = 0.022 + 0.068 * dayBlend;
      let clearB = 0.045 + 0.120 * dayBlend;

      const wModeStr = (typeof BglState !== 'undefined' && BglState.weatherMode) ? BglState.weatherMode : 'none';
      const weatherIntensity = (typeof BglState !== 'undefined' && BglState.weatherIntensity !== undefined) ? BglState.weatherIntensity : 1.0;
      if (wModeStr === 'fog' && weatherIntensity > 0.001) {
        const fogMix = Math.min(weatherIntensity * 0.65, 0.85);
        clearR = clearR * (1.0 - fogMix) + (0.42 * dayBlend + 0.06) * fogMix;
        clearG = clearG * (1.0 - fogMix) + (0.48 * dayBlend + 0.08) * fogMix;
        clearB = clearB * (1.0 - fogMix) + (0.55 * dayBlend + 0.12) * fogMix;
      } else if (wModeStr === 'smoke' && weatherIntensity > 0.001) {
        const smokeMix = Math.min(weatherIntensity * 0.70, 0.88);
        clearR = clearR * (1.0 - smokeMix) + 0.12 * smokeMix;
        clearG = clearG * (1.0 - smokeMix) + 0.12 * smokeMix;
        clearB = clearB * (1.0 - smokeMix) + 0.14 * smokeMix;
      }

      const commandEncoder = this.device.createCommandEncoder();
      const renderPassDesc = {
        colorAttachments: [
          {
            view: currentTexture.createView(),
            clearValue: { r: clearR, g: clearG, b: clearB, a: 1.0 },
            loadOp: 'clear',
            storeOp: 'store'
          }
        ],
        depthStencilAttachment: {
          view: this.depthTextureView,
          depthClearValue: 1.0,
          depthLoadOp: 'clear',
          depthStoreOp: 'store'
        }
      };

      const pass = commandEncoder.beginRenderPass(renderPassDesc);

      const cy = Math.cos(this.camera.yaw);
      const sy = Math.sin(this.camera.yaw);
      const cp = Math.cos(this.camera.pitch);
      const sp = Math.sin(this.camera.pitch);

      const eye = this._scratchEye;
      eye[0] = this.camera.target[0] + this.camera.distance * cp * sy;
      eye[1] = this.camera.target[1] + this.camera.distance * sp;
      eye[2] = this.camera.target[2] + this.camera.distance * cp * cy;

      const proj = this._scratchProj;
      this.mat4.perspective(proj, this.camera.fov, w / Math.max(1, h), this.camera.near, this.camera.far);

      const view = this._scratchView;
      this.mat4.lookAt(view, eye, this.camera.target, [0, 1, 0]);

      const viewProj = this._scratchViewProj;
      this.mat4.multiply(viewProj, proj, view);

      const frustum = this._scratchFrustum || (this._scratchFrustum = new Float32Array(24));
      this.mat4.extractFrustumPlanes(frustum, viewProj);

      const azimuth = (tod / 24.0) * 2.0 * Math.PI;
      const activeElevSin = (solarElev >= 0.0) ? Math.max(solarElev, 0.08) : Math.max(-solarElev * 0.75, 0.15);
      const activeElevCos = Math.sqrt(Math.max(0.0, 1.0 - activeElevSin * activeElevSin));
      const lightDir = this._scratchLightDir;
      lightDir[0] = activeElevCos * Math.sin(azimuth);
      lightDir[1] = activeElevSin;
      lightDir[2] = activeElevCos * Math.cos(azimuth);

      const uboBuffer = this._scratchUboBuffer;
      const uboF32 = this._scratchUboF32;
      const uboU32 = this._scratchUboU32;

      // Adaptive Auto-LOD 60 FPS Feedback Loop
      if (typeof BglState !== 'undefined' && BglState.isAutoLodActive && BglState.isLodActive) {
        if (dtSec > 0.0185) {
          BglState.lodBias = Math.min(4.0, (BglState.lodBias || 1.0) + dtSec * 0.45);
        } else if (dtSec < 0.0155) {
          BglState.lodBias = Math.max(0.5, (BglState.lodBias || 1.0) - dtSec * 0.25);
        }
        if (shouldUpdateDom) {
          const sliderLod = uiCache ? uiCache.sliderLodBias : document.getElementById('slider-lod-bias');
          if (sliderLod) sliderLod.value = BglState.lodBias.toFixed(2);
          const labelLod = uiCache ? uiCache.valLodBias : document.getElementById('val-lod-bias');
          if (labelLod) labelLod.textContent = `${BglState.lodBias.toFixed(2)}×`;
        }
      }

      const isLodActive = (typeof BglState !== 'undefined' && BglState.isLodActive !== undefined) ? BglState.isLodActive : true;
      const lodBias = (typeof BglState !== 'undefined' && BglState.lodBias !== undefined) ? BglState.lodBias : 1.0;
      const debugViewMode = (typeof BglState !== 'undefined' && BglState.debugViewMode !== undefined) ? BglState.debugViewMode : 0;
      const bumpScaleVal = (typeof BglState !== 'undefined' && BglState.bumpScale !== undefined) ? BglState.bumpScale : 1.5;
      const pomScaleVal = (typeof BglState !== 'undefined' && BglState.pomScale !== undefined) ? BglState.pomScale : 0.035;
      const isPomVal = (typeof BglState !== 'undefined' && BglState.isPomActive) ? 1.0 : 0.0;
      const isBumpVal = (typeof BglState !== 'undefined' && BglState.isBumpActive !== undefined) ? (BglState.isBumpActive ? 1.0 : 0.0) : 1.0;
      const weatherMode = wModeStr === 'rain' ? 1.0 : (wModeStr === 'snow' ? 2.0 : (wModeStr === 'fog' ? 3.0 : (wModeStr === 'smoke' ? 4.0 : 0.0)));
      const windStrength = (typeof BglState !== 'undefined' && BglState.windStrength !== undefined) ? BglState.windStrength : 0.3;
      const timeSec = nowMs * 0.001;

      // Populate frame-invariant UBO fields once outside the per-primitive loop
      uboF32.set(viewProj, 0);
      uboF32[48] = eye[0]; uboF32[49] = eye[1]; uboF32[50] = eye[2]; uboF32[51] = 1.0;
      uboF32[52] = lightDir[0]; uboF32[53] = lightDir[1]; uboF32[54] = lightDir[2]; uboF32[55] = solarElev;
      uboF32[76] = bumpScaleVal;
      uboF32[77] = pomScaleVal;
      uboF32[78] = isPomVal;
      uboF32[79] = isBumpVal;
      uboF32[80] = weatherMode;
      uboF32[81] = weatherIntensity;
      uboF32[82] = windStrength;
      uboF32[83] = timeSec;

      let frameRenderedTris = 0;
      let frameTotalTris = 0;
      const frameLodCounts = [0, 0, 0, 0];
      const hierarchyVer = this._hierarchyVersion || 0;
      const drawList = (this._sortedPrimitives && this._sortedPrimitives.length === this.primitives.length)
        ? this._sortedPrimitives
        : this.primitives;

      let lastPipeline = null;
      let lastBg0 = null;
      let lastBg1 = null;
      let lastVb0 = null;
      let lastVb1 = null;
      let lastIb = null;
      let lastIbFmt = null;

      for (let i = 0; i < drawList.length; ++i) {
        const prim = drawList[i];
        const m = prim.modelMatrix;

        if (prim.worldMatrixVersion !== hierarchyVer || !prim.normalMatrix || !prim.worldCenter) {
          if (!prim.normalMatrix) prim.normalMatrix = this.mat4.create();
          if (!prim.worldCenter) prim.worldCenter = [0, 0, 0];
          this.mat4.normalMatrix4(prim.normalMatrix, m);
          const lc = prim.localCenter || [0, 0, 0];
          prim.worldCenter[0] = m[0] * lc[0] + m[4] * lc[1] + m[8] * lc[2] + m[12];
          prim.worldCenter[1] = m[1] * lc[0] + m[5] * lc[1] + m[9] * lc[2] + m[13];
          prim.worldCenter[2] = m[2] * lc[0] + m[6] * lc[1] + m[10] * lc[2] + m[14];
          const sx = Math.hypot(m[0], m[1], m[2]);
          const sy = Math.hypot(m[4], m[5], m[6]);
          const sz = Math.hypot(m[8], m[9], m[10]);
          prim.worldRadius = (prim.localRadius || 1.0) * Math.max(sx, sy, sz, 0.001);
          prim.worldMatrixVersion = hierarchyVer;
        }

        const wc = prim.worldCenter;
        const worldRadius = prim.worldRadius;
        if (prim.hasIndices) {
          frameTotalTris += Math.floor(prim.indexCount / 3);
        } else {
          frameTotalTris += Math.floor(prim.vertexCount / 3);
        }

        // View frustum culling for rigid primitives (skip skinned to preserve extreme bone poses)
        if (!prim.isSkinned && !this.mat4.isSphereInFrustum(frustum, wc[0], wc[1], wc[2], worldRadius)) {
          continue;
        }

        const distToCam = Math.hypot(eye[0] - wc[0], eye[1] - wc[1], eye[2] - wc[2]);
        const lodEval = this.evaluatePrimitiveLod(distToCam, worldRadius, lodBias, isLodActive);
        const activeLod = prim.isSkinned ? 0 : lodEval.lodLevel;
        frameLodCounts[activeLod] = (frameLodCounts[activeLod] || 0) + 1;

        uboF32.set(m, 16);
        uboF32.set(prim.normalMatrix, 32);
        uboF32[56] = prim.baseColorFactor[0];
        uboF32[57] = prim.baseColorFactor[1];
        uboF32[58] = prim.baseColorFactor[2];
        uboF32[59] = prim.baseColorFactor[3];

        uboF32[60] = prim.metallicFactor;
        uboF32[61] = prim.roughnessFactor;
        uboF32[62] = prim.normalScale;
        uboF32[63] = prim.occlusionStrength;

        uboF32[64] = prim.emissiveFactor[0];
        uboF32[65] = prim.emissiveFactor[1];
        uboF32[66] = prim.emissiveFactor[2];
        uboF32[67] = prim.alphaCutoff;

        const uvTxs = prim.uvTransforms;
        const baseScale = uvTxs ? uvTxs.baseColor.scale : (prim.uvScale || [1, 1]);
        const baseOff = uvTxs ? uvTxs.baseColor.offset : (prim.uvOffset || [0, 0]);
        uboF32[68] = baseScale[0];
        uboF32[69] = baseScale[1];
        uboF32[70] = baseOff[0];
        uboF32[71] = baseOff[1];

        uboU32[0] = prim.flags;
        uboU32[1] = this.wireframe ? 1 : 0;
        uboU32[2] = prim.doubleSided ? 1 : 0;
        uboU32[3] = prim.alphaMode;

        if (uboF32.length >= 112) {
          uboF32[84] = activeLod;
          uboF32[85] = lodEval.lodTransition;
          uboF32[86] = debugViewMode;
          uboF32[87] = lodBias;

          const mr = uvTxs ? uvTxs.mr : null;
          const nm = uvTxs ? uvTxs.normal : null;
          const oc = uvTxs ? uvTxs.occlusion : null;
          const em = uvTxs ? uvTxs.emissive : null;

          uboF32[88] = mr ? mr.scale[0] : 1.0;   uboF32[89] = mr ? mr.scale[1] : 1.0;
          uboF32[90] = mr ? mr.offset[0] : 0.0;  uboF32[91] = mr ? mr.offset[1] : 0.0;

          uboF32[92] = nm ? nm.scale[0] : 1.0;   uboF32[93] = nm ? nm.scale[1] : 1.0;
          uboF32[94] = nm ? nm.offset[0] : 0.0;  uboF32[95] = nm ? nm.offset[1] : 0.0;

          uboF32[96] = oc ? oc.scale[0] : 1.0;   uboF32[97] = oc ? oc.scale[1] : 1.0;
          uboF32[98] = oc ? oc.offset[0] : 0.0;  uboF32[99] = oc ? oc.offset[1] : 0.0;

          uboF32[100] = em ? em.scale[0] : 1.0;  uboF32[101] = em ? em.scale[1] : 1.0;
          uboF32[102] = em ? em.offset[0] : 0.0; uboF32[103] = em ? em.offset[1] : 0.0;

          uboF32[104] = uvTxs ? uvTxs.baseColor.rotation : 0.0;
          uboF32[105] = mr ? mr.rotation : 0.0;
          uboF32[106] = nm ? nm.rotation : 0.0;
          uboF32[107] = oc ? oc.rotation : 0.0;
          uboF32[108] = em ? em.rotation : 0.0;
        }

        this.device.queue.writeBuffer(prim.uniformBuffer, 0, uboBuffer);

        const isSkinnedDraw = !!(prim.isSkinned && prim.skinIdx >= 0 && this.skins[prim.skinIdx] && this.skinnedPipeline);
        const pipeline = isSkinnedDraw
          ? (prim.doubleSided ? this.skinnedPipelineDoubleSided : this.skinnedPipeline)
          : (prim.doubleSided ? this.pipelineDoubleSided : this.pipeline);

        if (pipeline !== lastPipeline) {
          pass.setPipeline(pipeline);
          lastPipeline = pipeline;
        }
        if (prim.bindGroup !== lastBg0) {
          pass.setBindGroup(0, prim.bindGroup);
          lastBg0 = prim.bindGroup;
        }
        if (isSkinnedDraw) {
          const skinBg = this.skins[prim.skinIdx].bindGroup;
          if (skinBg !== lastBg1) {
            pass.setBindGroup(1, skinBg);
            lastBg1 = skinBg;
          }
        }
        if (prim.vertexBuffer !== lastVb0) {
          pass.setVertexBuffer(0, prim.vertexBuffer);
          lastVb0 = prim.vertexBuffer;
        }
        if (isSkinnedDraw && prim.skinVertexBuffer !== lastVb1) {
          pass.setVertexBuffer(1, prim.skinVertexBuffer);
          lastVb1 = prim.skinVertexBuffer;
        }

        if (prim.hasIndices) {
          if (prim.indexBuffer !== lastIb || prim.indexFormat !== lastIbFmt) {
            pass.setIndexBuffer(prim.indexBuffer, prim.indexFormat);
            lastIb = prim.indexBuffer;
            lastIbFmt = prim.indexFormat;
          }
          const slice = (prim.lodSlices && prim.lodSlices[activeLod])
            ? prim.lodSlices[activeLod]
            : { firstIndex: 0, indexCount: prim.indexCount };
          frameRenderedTris += Math.floor(slice.indexCount / 3);
          if (slice.firstIndex > 0) {
            pass.drawIndexed(slice.indexCount, 1, slice.firstIndex, 0, 0);
          } else {
            pass.drawIndexed(slice.indexCount);
          }
        } else {
          const tris = Math.floor(prim.vertexCount / 3);
          frameRenderedTris += tris;
          pass.draw(prim.vertexCount);
        }
      }

      if (typeof BglState !== 'undefined') {
        BglState.renderedTriangles = frameRenderedTris;
        BglState.totalTriangles = frameTotalTris;
        BglState.lodCounts = frameLodCounts;
        if (shouldUpdateDom && frameTotalTris > 0) {
          const elLod = (uiCache && uiCache.elLodTelemetry) || document.getElementById('lod-telemetry');
          if (elLod) {
            const savedPct = Math.max(0, Math.round(((frameTotalTris - frameRenderedTris) / frameTotalTris) * 100));
            elLod.textContent = `${frameRenderedTris.toLocaleString()} / ${frameTotalTris.toLocaleString()} Tris (-${savedPct}%) [L0:${frameLodCounts[0]} L1:${frameLodCounts[1]} L2:${frameLodCounts[2]} L3:${frameLodCounts[3]}]`;
          }
        }
      }

      pass.end();
      this.device.queue.submit([commandEncoder.finish()]);
    }
  };


  globalThis.BglWebGpuRenderer = BglWebGpuRenderer;
})();
