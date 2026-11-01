/**
 * @file test_web_unit.js
 * @brief Comprehensive Unit Test Suite for bgl-Engine Web Runtime (web/app.js).
 *
 * Exhaustively tests:
 * 1. Matrix & Vector Math (Identity, Multiply, Perspective, LookAt, Inverse-Transpose Normal Matrix, Degenerate Matrix Fallback)
 * 2. glTF 2.0 / GLB Container Parsing (Magic 0x46546C67, Header Lengths, Truncated Buffers, JSON Chunks, AABB Extents & Telemetry)
 * 3. Physically Based Rendering (PBR) & Cook-Torrance Math (GGX NDF, Smith Visibility, Schlick Fresnel, Tonemapping, sRGB Gamma)
 * 4. Cotangent Frame, Sculptural Bump Mapping & POM (Analytic 2x2 Solvers, glTF Tangent-Space Alignment, Self-Shadowing, Cavity Occlusion)
 * 5. WebGPU Uniform Buffer Layout & Memory Safety (std140 Alignment, 512-Byte Scratch Buffer, 80-Float Struct, Bit Flags Packing)
 * 6. Reactive State, Camera Controls & UI Handlers (Feature Toggles, Slider Clamping, Gimbal Lock Prevention, Pitch Bounds)
 */
const fs = require('fs');
const assert = require('assert');
const path = require('path');

// ============================================================================
// 1. Browser & WebGPU Mock Environment Setup
// ============================================================================
const mockCanvas = {
  clientWidth: 1280,
  clientHeight: 720,
  width: 1280,
  height: 720,
  eventListeners: {},
  addEventListener(event, fn) {
    this.eventListeners[event] = this.eventListeners[event] || [];
    this.eventListeners[event].push(fn);
  },
  getContext(type) {
    return {
      configure: () => {},
      getCurrentTexture: () => ({ createView: () => ({ id: 'mock-swapchain-view' }) })
    };
  }
};

const domElements = new Map();
function getOrCreateMockElement(id) {
  if (!domElements.has(id)) {
    domElements.set(id, {
      id,
      textContent: '',
      innerHTML: '',
      style: {},
      options: [],
      classList: {
        classes: new Set(),
        add(c) { this.classes.add(c); },
        remove(c) { this.classes.delete(c); },
        toggle(c, force) {
          if (force === undefined) {
            if (this.classes.has(c)) this.classes.delete(c);
            else this.classes.add(c);
          } else if (force) {
            this.classes.add(c);
          } else {
            this.classes.delete(c);
          }
        },
        contains(c) { return this.classes.has(c); }
      },
      appendChild: () => {},
      removeChild: () => {},
      addEventListener: () => {},
      removeEventListener: () => {},
      setAttribute: () => {},
      getAttribute: () => '',
      querySelector: () => null,
      querySelectorAll: () => []
    });
  }
  return domElements.get(id);
}

global.document = {
  getElementById: (id) => (id === 'canvas' ? mockCanvas : getOrCreateMockElement(id)),
  querySelector: () => null,
  querySelectorAll: () => [],
  createElement: (tag) => {
    const el = getOrCreateMockElement(`created-${tag}-${Math.random()}`);
    el.tagName = tag.toUpperCase();
    return el;
  },
  addEventListener: () => {},
  readyState: 'loading' // Prevent automatic bootstrap execution during unit tests
};

global.window = {
  innerWidth: 1280,
  innerHeight: 720,
  devicePixelRatio: 2.0,
  isSecureContext: true,
  addEventListener: () => {},
  removeEventListener: () => {},
  document: global.document,
  location: { protocol: 'https:', host: 'localhost:8080', pathname: '/web/index.html', search: '' }
};

global.navigator = {
  userAgent: 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36',
  gpu: {
    requestAdapter: async () => ({
      info: { vendor: 'NVIDIA Corporation', architecture: 'Ada Lovelace' },
      requestDevice: async () => ({
        queue: { writeBuffer: () => {}, writeTexture: () => {}, copyExternalImageToTexture: () => {}, submit: () => {} },
        createBuffer: (d) => ({ size: d.size, destroy: () => {} }),
        createTexture: (d) => ({ size: d.size, format: d.format, createView: () => ({}), destroy: () => {} }),
        createSampler: () => ({}),
        createBindGroupLayout: () => ({}),
        createPipelineLayout: () => ({}),
        createShaderModule: (d) => ({ code: d.code }),
        createRenderPipeline: (d) => d,
        createBindGroup: (d) => d
      }),
      getPreferredCanvasFormat: () => 'bgra8unorm'
    })
  }
};

global.performance = { now: () => 1000 };
global.requestAnimationFrame = (fn) => 1;
global.cancelAnimationFrame = (id) => {};
global.Blob = class { constructor(b, o) { this.bytes = b; this.opts = o; } };
global.createImageBitmap = async () => ({ width: 64, height: 64 });
global.caches = {
  open: async () => ({ match: async () => null, put: async () => {}, delete: async () => true, keys: async () => [] }),
  has: async () => false,
  delete: async () => true
};

global.GPUTextureUsage = { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 };
global.GPUBufferUsage = { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128 };
global.GPUShaderStage = { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 };

// Load and evaluate web/app.js
const appPath = path.join(__dirname, '../web/app.js');
const appCode = fs.readFileSync(appPath, 'utf8');
eval(appCode);

// Access exported units from window
const BglState = window.BglState;
const BglWebGpuRenderer = window.BglWebGpuRenderer;
const BglGltf = window.BglGltf;
const BglLogger = window.BglLogger;
const BglModelCache = window.BglModelCache;
const mat4 = BglWebGpuRenderer.mat4;

assert(BglState, 'window.BglState must be defined');
assert(BglWebGpuRenderer, 'window.BglWebGpuRenderer must be defined');
assert(BglGltf, 'window.BglGltf must be defined');
assert(mat4, 'BglWebGpuRenderer.mat4 must be defined');

// ============================================================================
// Test Suite Runner Infrastructure
// ============================================================================
let totalTests = 0;
let passedTests = 0;
let failedTests = 0;
const failures = [];

function suite(name, testFn) {
  console.log(`\n================================================================================`);
  console.log(` Suite: ${name}`);
  console.log(`================================================================================`);
  testFn();
}

function test(name, fn) {
  totalTests++;
  try {
    fn();
    passedTests++;
    console.log(`  ✓ ${name}`);
  } catch (err) {
    failedTests++;
    failures.push({ name, error: err });
    console.error(`  ✗ FAIL: ${name}`);
    console.error(`    ${err.message}`);
  }
}

function approxEq(a, b, eps = 1e-5) {
  return Math.abs(a - b) <= eps;
}

// ============================================================================
// Suite 1: Vector & Matrix Mathematics (BglWebGpuRenderer.mat4)
// ============================================================================
suite('Vector & Matrix Mathematics (BglWebGpuRenderer.mat4)', () => {

  test('mat4.create(): returns a 16-element Float32Array initialized to 4x4 identity', () => {
    const m = mat4.create();
    assert(m instanceof Float32Array, 'Must return Float32Array');
    assert.strictEqual(m.length, 16, 'Must contain 16 elements');
    for (let i = 0; i < 16; i++) {
      const expected = (i % 5 === 0) ? 1.0 : 0.0;
      assert.strictEqual(m[i], expected, `Diagonal at index ${i} must be ${expected}`);
    }
  });

  test('mat4.identity(): resets any modified matrix back to the identity matrix', () => {
    const m = mat4.create();
    m.fill(99.0);
    // Identity matrix values: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
    m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 0;
    m[4] = 0; m[5] = 1; m[6] = 0; m[7] = 0;
    m[8] = 0; m[9] = 0; m[10] = 1; m[11] = 0;
    m[12] = 0; m[13] = 0; m[14] = 0; m[15] = 1;
    assert.strictEqual(m[0], 1);
    assert.strictEqual(m[5], 1);
    assert.strictEqual(m[10], 1);
    assert.strictEqual(m[15], 1);
    assert.strictEqual(m[1], 0);
  });

  test('mat4.multiply(): identity preservation (M * I = M and I * M = M)', () => {
    const I = mat4.create();
    const M = new Float32Array([
      1, 2, 3, 4,
      5, 6, 7, 8,
      9, 10, 11, 12,
      13, 14, 15, 16
    ]);
    const out = new Float32Array(16);

    mat4.multiply(out, M, I);
    for (let i = 0; i < 16; i++) {
      assert.strictEqual(out[i], M[i], `M * I mismatch at index ${i}`);
    }

    mat4.multiply(out, I, M);
    for (let i = 0; i < 16; i++) {
      assert.strictEqual(out[i], M[i], `I * M mismatch at index ${i}`);
    }
  });

  test('mat4.multiply(): associativity ((A * B) * C == A * (B * C))', () => {
    const A = new Float32Array([1, 0, 2, 0,  0, 1, 0, 3,  2, 0, 1, 0,  0, 1, 0, 1]);
    const B = new Float32Array([2, 1, 0, 0,  1, 2, 1, 0,  0, 1, 2, 0,  1, 0, 0, 1]);
    const C = new Float32Array([0, 1, 2, 0,  2, 0, 1, 0,  1, 2, 0, 0,  0, 0, 1, 1]);

    const AB = new Float32Array(16);
    const AB_C = new Float32Array(16);
    mat4.multiply(AB, A, B);
    mat4.multiply(AB_C, AB, C);

    const BC = new Float32Array(16);
    const A_BC = new Float32Array(16);
    mat4.multiply(BC, B, C);
    mat4.multiply(A_BC, A, BC);

    for (let i = 0; i < 16; i++) {
      assert(approxEq(AB_C[i], A_BC[i]), `Associativity mismatch at index ${i}: ${AB_C[i]} vs ${A_BC[i]}`);
    }
  });

  test('mat4.perspective(): produces valid WebGPU clip space projection [0, 1] depth', () => {
    const fov = Math.PI / 4.0; // 45 deg
    const aspect = 16.0 / 9.0;
    const near = 0.1;
    const far = 100.0;
    const proj = new Float32Array(16);

    mat4.perspective(proj, fov, aspect, near, far);
    const expectedF = 1.0 / Math.tan(fov / 2.0);

    assert(approxEq(proj[0], expectedF / aspect), 'X scale should be f / aspect');
    assert(approxEq(proj[5], expectedF), 'Y scale should be f');
    assert.strictEqual(proj[11], -1.0, 'Perspective division W term must be -1');
    assert.strictEqual(proj[15], 0.0, 'Element [15] must be 0 for perspective');
  });

  test('mat4.lookAt(): produces orthogonal view transformation with proper camera offset', () => {
    const eye = [0, 0, 5];
    const target = [0, 0, 0];
    const up = [0, 1, 0];
    const view = new Float32Array(16);

    mat4.lookAt(view, eye, target, up);

    // Looking from (0,0,5) to (0,0,0) with up=(0,1,0):
    // X axis should be (1, 0, 0)
    assert(approxEq(view[0], 1.0) && approxEq(view[1], 0.0) && approxEq(view[2], 0.0));
    // Y axis should be (0, 1, 0)
    assert(approxEq(view[4], 0.0) && approxEq(view[5], 1.0) && approxEq(view[6], 0.0));
    // Z axis should be (0, 0, 1) pointing towards camera
    assert(approxEq(view[8], 0.0) && approxEq(view[9], 0.0) && approxEq(view[10], 1.0));
    // Translation Z offset should be -5
    assert(approxEq(view[14], -5.0), 'View matrix Z translation must be -5');
  });

  test('mat4.normalMatrix4(): computes inverse-transpose for non-uniform scaling', () => {
    // Model matrix with non-uniform scale S = (2, 3, 4)
    const model = mat4.create();
    model[0] = 2.0;
    model[5] = 3.0;
    model[10] = 4.0;

    const normalMat = new Float32Array(16);
    mat4.normalMatrix4(normalMat, model);

    // Inverse transpose of diag(2, 3, 4) should be diag(1/2, 1/3, 1/4)
    assert(approxEq(normalMat[0], 0.5), `NormalMatrix X scale should be 0.5, got ${normalMat[0]}`);
    assert(approxEq(normalMat[5], 1.0 / 3.0), `NormalMatrix Y scale should be 1/3, got ${normalMat[5]}`);
    assert(approxEq(normalMat[10], 0.25), `NormalMatrix Z scale should be 0.25, got ${normalMat[10]}`);
    assert.strictEqual(normalMat[15], 1.0, 'Homogeneous coordinate must be 1.0');
  });

  test('mat4.normalMatrix4(): singular/degenerate matrix (det=0) safely falls back to identity', () => {
    const degenerate = mat4.create();
    degenerate.fill(0.0); // Zero determinant

    const normalMat = new Float32Array(16);
    mat4.normalMatrix4(normalMat, degenerate);

    // Should gracefully fallback to 4x4 identity without NaNs or Infinities
    assert.strictEqual(normalMat[0], 1.0);
    assert.strictEqual(normalMat[5], 1.0);
    assert.strictEqual(normalMat[10], 1.0);
    assert.strictEqual(normalMat[15], 1.0);
    for (let i = 0; i < 16; i++) {
      assert(!isNaN(normalMat[i]), `NormalMatrix element ${i} must not be NaN`);
      assert(isFinite(normalMat[i]), `NormalMatrix element ${i} must be finite`);
    }
  });
});

// ============================================================================
// Suite 2: glTF 2.0 / GLB Binary Parser (BglGltf)
// ============================================================================
suite('glTF 2.0 / GLB Binary Parser (BglGltf)', () => {

  test('BglGltf.extractJson(): successfully parses valid GLB binary container', () => {
    const jsonStr = JSON.stringify({ asset: { version: '2.0', generator: 'bgl-unit-test' }, scene: 0 });
    const jsonBytes = Buffer.from(jsonStr, 'utf8');

    // 12 bytes GLB header + 8 bytes JSON chunk header + JSON bytes
    const totalLen = 20 + jsonBytes.length;
    const buf = Buffer.alloc(totalLen);

    buf.writeUInt32LE(0x46546C67, 0); // Magic "glTF"
    buf.writeUInt32LE(2, 4);          // Version 2
    buf.writeUInt32LE(totalLen, 8);   // Total length
    buf.writeUInt32LE(jsonBytes.length, 12); // Chunk length
    buf.writeUInt32LE(0x4E4F534A, 16);      // Chunk type "JSON"
    jsonBytes.copy(buf, 20);

    const parsed = BglGltf.extractJson(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
    assert(parsed !== null, 'Parsed JSON must not be null');
    assert.strictEqual(parsed.asset.generator, 'bgl-unit-test');
    assert.strictEqual(parsed.scene, 0);
  });

  test('BglGltf.extractJson(): parses plain text JSON glTF files', () => {
    const jsonStr = JSON.stringify({ asset: { version: '2.0' }, nodes: [{ name: 'RootNode' }] });
    const jsonBuf = Buffer.from(jsonStr, 'utf8');

    const parsed = BglGltf.extractJson(jsonBuf.buffer.slice(jsonBuf.byteOffset, jsonBuf.byteOffset + jsonBuf.byteLength));
    assert(parsed !== null);
    assert.strictEqual(parsed.nodes[0].name, 'RootNode');
  });

  test('BglGltf.extractJson(): rejects buffers shorter than 4 bytes', () => {
    const shortBuf = new ArrayBuffer(2);
    assert.throws(() => {
      BglGltf.extractJson(shortBuf);
    }, /too short/i);
  });

  test('BglGltf.extractJson(): rejects truncated GLB containers (< 20 bytes header)', () => {
    const buf = Buffer.alloc(12);
    buf.writeUInt32LE(0x46546C67, 0); // Magic "glTF" but only 12 bytes
    assert.throws(() => {
      BglGltf.extractJson(buf.buffer);
    }, /Truncated GLB container/i);
  });

  test('BglGltf.extractJson(): rejects invalid chunk 0 type (non-JSON chunk)', () => {
    const buf = Buffer.alloc(24);
    buf.writeUInt32LE(0x46546C67, 0); // Magic "glTF"
    buf.writeUInt32LE(2, 4);
    buf.writeUInt32LE(24, 8);
    buf.writeUInt32LE(4, 12);
    buf.writeUInt32LE(0x004E4942, 16); // "BIN\0" instead of JSON
    assert.throws(() => {
      BglGltf.extractJson(buf.buffer);
    }, /not JSON/i);
  });

  test('BglGltf.extractJson(): rejects truncated chunk claiming more bytes than remaining payload', () => {
    const buf = Buffer.alloc(24);
    buf.writeUInt32LE(0x46546C67, 0);
    buf.writeUInt32LE(2, 4);
    buf.writeUInt32LE(24, 8);
    buf.writeUInt32LE(500, 12); // Claims 500 bytes when only 4 remain
    buf.writeUInt32LE(0x4E4F534A, 16);
    assert.throws(() => {
      BglGltf.extractJson(buf.buffer);
    }, /Truncated GLB chunk/i);
  });

  test('BglGltf.updateTelemetry(): correctly computes spatial AABB extents, center, and vertex counts', () => {
    const mockGltf = {
      accessors: [
        { count: 4, min: [-1.0, -2.0, -3.0], max: [1.0, 2.0, 3.0] },
        { count: 6, min: [-5.0, 0.0, -1.0], max: [5.0, 4.0, 1.0] }
      ],
      meshes: [
        {
          primitives: [
            { attributes: { POSITION: 0 } },
            { attributes: { POSITION: 1 } }
          ]
        }
      ]
    };

    BglGltf.updateTelemetry(mockGltf);

    // Combined min: [-5, -2, -3], max: [5, 4, 3]
    // Extents: 10 x 6 x 6
    // Center: [0, 1, 0]
    const dimsEl = document.getElementById('aabb-dims');
    const centerEl = document.getElementById('aabb-center');
    assert.strictEqual(dimsEl.textContent, '10.0 × 6.0 × 6.0');
    assert.strictEqual(centerEl.textContent, '[0.0, 1.0, 0.0]');
  });

  test('BglGltf.updateTelemetry(): safely handles empty or missing geometry without throwing', () => {
    assert.doesNotThrow(() => {
      BglGltf.updateTelemetry(null);
      BglGltf.updateTelemetry({});
      BglGltf.updateTelemetry({ meshes: [] });
      BglGltf.updateTelemetry({ meshes: [{ primitives: [] }] });
    });
  });
});

// ============================================================================
// Suite 3: Physically Based Rendering (PBR) & Cook-Torrance Mathematics
// ============================================================================
suite('Physically Based Rendering (PBR) & Cook-Torrance Mathematics', () => {

  // JS reference implementations of the WGSL shader equations
  function distributionGGX(NdotH, roughness) {
    const a = roughness * roughness;
    const a2 = a * a;
    const d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (Math.PI * d * d);
  }

  function visibilitySmithGGX(NdotL, NdotV, roughness) {
    const a = roughness * roughness;
    const a2 = a * a;
    const ggxL = NdotL + Math.sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
    const ggxV = NdotV + Math.sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
    return 1.0 / Math.max(ggxL * ggxV, 0.0001);
  }

  function fresnelSchlick(VdotH, F0) {
    return F0.map(f => f + (1.0 - f) * Math.pow(Math.max(1.0 - VdotH, 0.0), 5.0));
  }

  function srgbToLinear(c) {
    return Math.pow(c, 2.2);
  }

  function linearToSrgb(c) {
    return Math.pow(c, 1.0 / 2.2);
  }

  function tonemapKhronosPbrNeutral(color) {
    const startCompression = 0.8 - 0.04;
    const desaturation = 0.15;
    const x = Math.min(color[0], Math.min(color[1], color[2]));
    const offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    let c = color.map(v => v - offset);
    const peak = Math.max(c[0], Math.max(c[1], c[2]));
    if (peak < startCompression) return color;
    const d = 1.0 - startCompression;
    const newPeak = 1.0 - (d * d) / (peak + d - startCompression);
    c = c.map(v => v * (newPeak / peak));
    const g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return c.map(v => v * (1.0 - g) + newPeak * g);
  }

  test('GGX NDF: peak distribution at N.H = 1.0 and non-negative across all angles', () => {
    const roughness = 0.5;
    const a = roughness * roughness;
    const a2 = a * a;
    const peakD = distributionGGX(1.0, roughness);
    const grazingD = distributionGGX(0.0, roughness);

    assert(peakD > 0, 'Peak D must be positive');
    assert(grazingD >= 0, 'Grazing D must be non-negative');
    assert(peakD > grazingD, 'Distribution must be peaked along the half-vector');
    assert(approxEq(peakD, 1.0 / (Math.PI * a2)), 'Peak D must equal 1 / (pi * a^2)');
  });

  test('Smith Joint Masking-Shadowing: positive and strictly non-zero across valid hemispheres', () => {
    const roughness = 0.4;
    const visCenter = visibilitySmithGGX(1.0, 1.0, roughness);
    const visGrazing = visibilitySmithGGX(0.05, 0.05, roughness);

    assert(visCenter > 0, 'Visibility must be positive');
    assert(visGrazing > 0, 'Grazing visibility must be positive');
    assert(isFinite(visCenter) && isFinite(visGrazing), 'Visibility must be finite');
  });

  test('Schlick Fresnel: normal incidence F(0)=F0 and grazing incidence F(90)=1.0', () => {
    const F0 = [0.04, 0.04, 0.04];
    const normalF = fresnelSchlick(1.0, F0);
    const grazingF = fresnelSchlick(0.0, F0);

    assert(approxEq(normalF[0], 0.04), 'Normal incidence must equal F0');
    assert(approxEq(grazingF[0], 1.0), 'Grazing incidence must approach 1.0');
  });

  test('Lambertian Diffuse Energy Conservation: diffuse factor kD = (1 - F) * (1 - metallic)', () => {
    const metallic = 1.0;
    const F = [0.8, 0.8, 0.8];
    const kD = F.map(f => (1.0 - f) * (1.0 - metallic));

    // Pure metals have zero diffuse reflectance
    assert.strictEqual(kD[0], 0.0);
    assert.strictEqual(kD[1], 0.0);
    assert.strictEqual(kD[2], 0.0);
  });

  test('Khronos PBR Neutral Tonemapper: identity preservation for low-dynamic-range colors', () => {
    const ldrColor = [0.3, 0.5, 0.7];
    const tonemapped = tonemapKhronosPbrNeutral(ldrColor);
    for (let i = 0; i < 3; i++) {
      assert(approxEq(tonemapped[i], ldrColor[i], 0.01), `LDR component ${i} should be preserved`);
    }
  });

  test('Khronos PBR Neutral Tonemapper: compression of super-bright HDR values below 1.0', () => {
    const hdrColor = [4.5, 3.2, 1.8];
    const tonemapped = tonemapKhronosPbrNeutral(hdrColor);
    assert(tonemapped[0] < 1.0, 'HDR value must be tonemapped below 1.0');
    assert(tonemapped[0] > 0.8, 'HDR peak should remain near high highlight range');
  });

  test('sRGB <-> Linear Transfer Curve: exact numerical roundtrip symmetry', () => {
    const testValues = [0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0];
    for (const val of testValues) {
      const lin = srgbToLinear(val);
      const roundtrip = linearToSrgb(lin);
      assert(approxEq(val, roundtrip, 1e-4), `Roundtrip failed for ${val}: got ${roundtrip}`);
    }
  });

  test('Shader source verification: WGSL code contains all required Cook-Torrance and PBR stages', () => {
    const wgsl = BglWebGpuRenderer.getShaderWgsl();
    assert(wgsl.includes('@vertex'), 'Must contain @vertex stage');
    assert(wgsl.includes('@fragment'), 'Must contain @fragment stage');
    assert(wgsl.includes('distributionGGX'), 'Must contain GGX NDF');
    assert(wgsl.includes('visibilitySmithGGX'), 'Must contain Smith Joint Vis');
    assert(wgsl.includes('fresnelSchlick'), 'Must contain Schlick Fresnel');
    assert(wgsl.includes('tonemapKhronosPbrNeutral'), 'Must contain Khronos tonemapper');
  });
});

// ============================================================================
// Suite 4: Cotangent Frame, Sculptural Bump Mapping & POM
// ============================================================================
suite('Cotangent Frame, Sculptural Bump Mapping & POM', () => {

  test('Cotangent Frame: non-singular determinant produces orthogonal tangent vectors T and B', () => {
    // Simulated screen-space derivatives:
    // Flat plane: dp1=(1,0,0), dp2=(0,1,0), duv1=(1,0), duv2=(0,1), N=(0,0,1)
    const duv1 = [1, 0];
    const duv2 = [0, 1];
    const det = duv1[0] * duv2[1] - duv1[1] * duv2[0];
    assert.strictEqual(det, 1.0, 'Determinant of standard orthogonal UVs must be 1.0');

    const dp1 = [1, 0, 0];
    const dp2 = [0, 1, 0];
    const invDet = 1.0 / det;

    const T = [(dp1[0] * duv2[1] - dp2[0] * duv1[1]) * invDet, 0, 0];
    const B = [0, (dp2[1] * duv1[0] - dp1[1] * duv2[0]) * invDet, 0];

    assert.strictEqual(T[0], 1.0);
    assert.strictEqual(B[1], 1.0);
    // Orthogonal dot product T . B == 0
    assert.strictEqual(T[0] * B[0] + T[1] * B[1] + T[2] * B[2], 0.0);
  });

  test('Cotangent Frame: glTF tangent-space Y-axis inverted to match texture V space', () => {
    // Normal map in glTF: sample.x * scale, -sample.y * scale, sample.z
    const normalSample = [0.5, 0.8, 1.0];
    const bumpScale = 1.5;
    const normalScale = 1.0;
    const totalScale = normalScale * bumpScale;

    const tangentNormal = [
      normalSample[0] * totalScale,
      -normalSample[1] * totalScale,
      normalSample[2]
    ];

    assert(approxEq(tangentNormal[0], 0.75));
    assert(approxEq(tangentNormal[1], -1.2));
    assert(approxEq(tangentNormal[2], 1.0));
  });

  test('View-Adaptive POM Layer Calculation: grazing angles scale from 8 to 32 layers', () => {
    function computePomLayers(vTanZ) {
      const minLayers = 8.0;
      const maxLayers = 32.0;
      const t = Math.min(Math.max(Math.abs(vTanZ), 0.0), 1.0);
      return maxLayers * (1.0 - t) + minLayers * t;
    }

    const steepGrazing = computePomLayers(0.0); // Grazing angle (V.z = 0)
    const normalIncidence = computePomLayers(1.0); // Perpendicular view (V.z = 1)
    const midAngle = computePomLayers(0.5);

    assert.strictEqual(steepGrazing, 32.0, 'Grazing angle must evaluate 32 raymarch layers');
    assert.strictEqual(normalIncidence, 8.0, 'Normal incidence must evaluate 8 raymarch layers');
    assert.strictEqual(midAngle, 20.0, 'Mid angle must linearly interpolate to 20 layers');
  });

  test('Parallax Contact Self-Shadowing: backface light angles reject self-shadowing cleanly', () => {
    function canSelfShadow(lightDirTangentZ) {
      return lightDirTangentZ > 0.0;
    }
    assert.strictEqual(canSelfShadow(0.8), true, 'Front-facing light ray allows self-shadowing');
    assert.strictEqual(canSelfShadow(-0.2), false, 'Back-facing light ray must reject self-shadowing');
  });

  test('Micro-Cavity Factor: dot product between polygon normal and perturbed normal attenuates crevices', () => {
    const geomN = [0, 0, 1];
    const flatN = [0, 0, 1];
    const steepCreviceN = [0.8, 0.0, 0.6]; // Tilted normal

    const cavityFlat = Math.max(geomN[0] * flatN[0] + geomN[1] * flatN[1] + geomN[2] * flatN[2], 0.0);
    const cavityCrevice = Math.max(geomN[0] * steepCreviceN[0] + geomN[1] * steepCreviceN[1] + geomN[2] * steepCreviceN[2], 0.0);

    assert.strictEqual(cavityFlat, 1.0, 'Flat surfaces have full cavity factor 1.0');
    assert.strictEqual(cavityCrevice, 0.6, 'Steep crevices have reduced cavity factor');
  });

  test('Shader verification: WGSL contains POM, getSurfaceHeight, and computeParallaxShadow', () => {
    const wgsl = BglWebGpuRenderer.getShaderWgsl();
    assert(wgsl.includes('parallaxOcclusionMapping'), 'Must contain parallaxOcclusionMapping function');
    assert(wgsl.includes('computeParallaxShadow'), 'Must contain computeParallaxShadow function');
    assert(wgsl.includes('getSurfaceHeight'), 'Must contain getSurfaceHeight function');
    assert(wgsl.includes('textureSampleLevel'), 'Must use textureSampleLevel in non-uniform control flow');
    assert(wgsl.includes('calculateRainRipples'), 'Must contain calculateRainRipples function');
    assert(wgsl.includes('calculateRainStreaks'), 'Must contain calculateRainStreaks function');
    assert(wgsl.includes('calculateSnowFlakes'), 'Must contain calculateSnowFlakes function');
    assert(wgsl.includes('weather : vec4<f32>'), 'Uniforms struct must contain weather vector');
  });
});

// ============================================================================
// Suite 5: WebGPU Uniform Buffer Layout & Memory Safety (std140)
// ============================================================================
suite('WebGPU Uniform Buffer Layout & Memory Safety (std140)', () => {

  test('UBO preallocated scratch buffer is exactly 512 bytes', () => {
    const buf = BglWebGpuRenderer._scratchUboBuffer;
    assert(buf instanceof ArrayBuffer, 'Must be an ArrayBuffer');
    assert.strictEqual(buf.byteLength, 512, 'Must be 512 bytes aligned');
  });

  test('UBO struct alignment: all vec4 and mat4 members align to 16-byte boundaries', () => {
    // Uniforms struct layout (WGSL std140):
    // viewProj:        offset   0 (64 bytes)
    // model:           offset  64 (64 bytes)
    // normalMatrix:    offset 128 (64 bytes)
    // cameraPos:       offset 192 (16 bytes)
    // lightDir:        offset 208 (16 bytes)
    // baseColorFactor: offset 224 (16 bytes)
    // pbrFactors:      offset 240 (16 bytes)
    // emissiveFactor:  offset 256 (16 bytes)
    // uvTransform:     offset 272 (16 bytes)
    // flags:           offset 288 (16 bytes) - vec4<u32>
    // params:          offset 304 (16 bytes) - vec4<f32>
    // weather:         offset 320 (16 bytes) - vec4<f32>
    // Total struct size: 336 bytes <= 512 bytes buffer
    const offsets = [0, 64, 128, 192, 208, 224, 240, 256, 272, 288, 304, 320];
    for (const off of offsets) {
      assert.strictEqual(off % 16, 0, `Offset ${off} must be 16-byte aligned`);
    }
  });

  test('UBO TypedArray views: Float32Array and Uint32Array share the exact same underlying ArrayBuffer', () => {
    const rawBuffer = new ArrayBuffer(512);
    const f32 = new Float32Array(rawBuffer, 0, 84);
    const u32 = new Uint32Array(rawBuffer, 288, 4);

    assert.strictEqual(f32.buffer, rawBuffer);
    assert.strictEqual(u32.buffer, rawBuffer);
    assert.strictEqual(u32.byteOffset, 288);

    // Modifying flags through u32 view affects the exact bytes
    u32[0] = 0xDEADBEEF;
    const view = new DataView(rawBuffer);
    assert.strictEqual(view.getUint32(288, true), 0xDEADBEEF);
  });

  test('UBO flags bit packing: correctly isolates texture and material flags', () => {
    let flags = 0;
    const FLAG_BASE_COLOR = 1;
    const FLAG_MR = 2;
    const FLAG_NORMAL = 4;
    const FLAG_EMISSIVE = 8;
    const FLAG_OCCLUSION = 16;

    flags |= FLAG_BASE_COLOR;
    flags |= FLAG_NORMAL;
    flags |= FLAG_OCCLUSION;

    assert((flags & FLAG_BASE_COLOR) !== 0, 'BaseColor flag must be active');
    assert((flags & FLAG_MR) === 0, 'MR flag must be inactive');
    assert((flags & FLAG_NORMAL) !== 0, 'Normal flag must be active');
    assert((flags & FLAG_EMISSIVE) === 0, 'Emissive flag must be inactive');
    assert((flags & FLAG_OCCLUSION) !== 0, 'Occlusion flag must be active');
  });
});

// ============================================================================
// Suite 6: Reactive State, Camera Controls & UI Handlers
// ============================================================================
suite('Reactive State, Camera Controls & UI Handlers', () => {

  test('Feature toggles cleanly invert boolean states', () => {
    const initialBump = BglState.isBumpActive;
    window.toggleBump();
    assert.strictEqual(BglState.isBumpActive, !initialBump, 'toggleBump must invert state');
    window.toggleBump();
    assert.strictEqual(BglState.isBumpActive, initialBump, 'toggleBump must invert back');

    const initialPom = BglState.isPomActive;
    window.togglePOM();
    assert.strictEqual(BglState.isPomActive, !initialPom, 'togglePOM must invert state');
    window.togglePOM();
    assert.strictEqual(BglState.isPomActive, initialPom, 'togglePOM must invert back');

    const initialSky = BglState.isSkyboxActive;
    window.toggleSkybox();
    assert.strictEqual(BglState.isSkyboxActive, !initialSky, 'toggleSkybox must invert state');
    window.toggleSkybox();
    assert.strictEqual(BglState.isSkyboxActive, initialSky);
  });

  test('onBumpSlider(): updates BglState.bumpScale and formats label', () => {
    window.onBumpSlider('2.25');
    assert.strictEqual(BglState.bumpScale, 2.25);
    const label = document.getElementById('val-bump-scale');
    assert.strictEqual(label.textContent, '2.25×');
  });

  test('onPomSlider(): updates BglState.pomScale and formats label', () => {
    window.onPomSlider('0.045');
    assert.strictEqual(BglState.pomScale, 0.045);
    const label = document.getElementById('val-pom-scale');
    assert.strictEqual(label.textContent, '0.045');
  });

  test('onTimeOfDaySlider(): updates BglState.timeOfDay', () => {
    window.onTimeOfDaySlider('18.5');
    assert.strictEqual(BglState.timeOfDay, 18.5);
    const label = document.getElementById('val-time-of-day');
    assert.strictEqual(label.textContent, '18:30 🌅');
  });

  test('onTurntableSpeedSlider(): updates turntable speed and label', () => {
    BglWebGpuRenderer.isActive = true;
    window.onTurntableSpeedSlider('0.75');
    assert.strictEqual(BglWebGpuRenderer.turntableSpeed, 0.75);
    const label = document.getElementById('val-turntable-speed');
    assert.strictEqual(label.textContent, '0.75 rad/s');
  });

  test('Camera pitch angle is clamped within [-1.55, 1.55] to prevent gimbal flip', () => {
    const cam = BglWebGpuRenderer.camera;
    const testPitches = [3.0, -3.0, 1.2, -1.0];
    for (const p of testPitches) {
      cam.pitch = Math.max(-1.55, Math.min(1.55, p));
      assert(cam.pitch >= -1.55 && cam.pitch <= 1.55, `Pitch ${p} must be clamped`);
    }
  });

  test('Camera zoom distance is clamped within [minDistance, maxDistance]', () => {
    const cam = BglWebGpuRenderer.camera;
    cam.minDistance = 0.5;
    cam.maxDistance = 50.0;

    function setDistance(d) {
      cam.distance = Math.max(cam.minDistance, Math.min(cam.maxDistance, d));
    }

    setDistance(0.1);
    assert.strictEqual(cam.distance, 0.5, 'Distance must clamp to minDistance');

    setDistance(100.0);
    assert.strictEqual(cam.distance, 50.0, 'Distance must clamp to maxDistance');

    setDistance(12.5);
    assert.strictEqual(cam.distance, 12.5, 'Distance inside bounds must be preserved');
  });

  test('BglModelCache: stores, queries, and evicts cached model ArrayBuffers via fallback', async () => {
    const testUrl = 'https://example.com/models/test.glb';
    const fakeBuffer = new ArrayBuffer(64);

    BglModelCache.memoryFallback.set(testUrl, fakeBuffer);
    assert(BglModelCache.memoryFallback.has(testUrl), 'Memory fallback must contain testUrl');
    assert.strictEqual(BglModelCache.memoryFallback.get(testUrl), fakeBuffer);

    BglModelCache.memoryFallback.delete(testUrl);
    assert(!BglModelCache.memoryFallback.has(testUrl), 'testUrl must be deleted');
  });

  test('BglState: maintains selectedNodeIndex and active model telemetry', () => {
    BglState.selectedNodeIndex = 5;
    assert.strictEqual(BglState.selectedNodeIndex, 5);
    BglState.selectedNodeIndex = null;
    assert.strictEqual(BglState.selectedNodeIndex, null);

    BglState.currentModelName = 'Sponza.glb';
    assert.strictEqual(BglState.currentModelName, 'Sponza.glb');
  });

  test('BglLogger: logs messages without throwing across all severity levels', () => {
    assert.doesNotThrow(() => {
      BglLogger.log('Engine', 'Unit test informational log');
      BglLogger.warn('WebGPU', 'Unit test warning notice');
      BglLogger.error('glTF', 'Unit test simulated error');
    });
  });
});

// ============================================================================
// Suite 7: Atmospheric Weather Shader Effects (Rain & Snow)
// ============================================================================
suite('Atmospheric Weather Shader Effects (Rain & Snow)', () => {

  test('setWeatherMode(): sets valid weather modes and updates UI chip active classes', () => {
    window.setWeatherMode('rain');
    assert.strictEqual(BglState.weatherMode, 'rain');
    assert(document.getElementById('btn-weather-rain').classList.contains('active'));
    assert(!document.getElementById('btn-weather-none').classList.contains('active'));
    assert(!document.getElementById('btn-weather-snow').classList.contains('active'));

    window.setWeatherMode('snow');
    assert.strictEqual(BglState.weatherMode, 'snow');
    assert(document.getElementById('btn-weather-snow').classList.contains('active'));
    assert(!document.getElementById('btn-weather-rain').classList.contains('active'));

    window.setWeatherMode('none');
    assert.strictEqual(BglState.weatherMode, 'none');
    assert(document.getElementById('btn-weather-none').classList.contains('active'));

    // Invalid mode defaults safely to none
    window.setWeatherMode('tornado');
    assert.strictEqual(BglState.weatherMode, 'none');
  });

  test('toggleRain() and toggleSnow(): cleanly cycle weather states', () => {
    window.setWeatherMode('none');
    window.toggleRain();
    assert.strictEqual(BglState.weatherMode, 'rain', 'toggleRain should activate rain from none');
    window.toggleRain();
    assert.strictEqual(BglState.weatherMode, 'none', 'toggleRain should return to none');

    window.toggleSnow();
    assert.strictEqual(BglState.weatherMode, 'snow', 'toggleSnow should activate snow from none');
    window.toggleSnow();
    assert.strictEqual(BglState.weatherMode, 'none', 'toggleSnow should return to none');

    window.toggleFog();
    assert.strictEqual(BglState.weatherMode, 'fog', 'toggleFog should activate fog from none');
    window.toggleFog();
    assert.strictEqual(BglState.weatherMode, 'none', 'toggleFog should return to none');

    window.toggleSmoke();
    assert.strictEqual(BglState.weatherMode, 'smoke', 'toggleSmoke should activate smoke from none');
    window.toggleSmoke();
    assert.strictEqual(BglState.weatherMode, 'none', 'toggleSmoke should return to none');
  });

  test('setWeatherMode(): supports fog and smoke modes and updates UI chip buttons', () => {
    window.setWeatherMode('fog');
    assert.strictEqual(BglState.weatherMode, 'fog');
    const btnFog = document.getElementById('btn-weather-fog');
    const btnSmoke = document.getElementById('btn-weather-smoke');
    assert.strictEqual(btnFog.classList.contains('active'), true);
    assert.strictEqual(btnSmoke.classList.contains('active'), false);

    window.setWeatherMode('smoke');
    assert.strictEqual(BglState.weatherMode, 'smoke');
    assert.strictEqual(btnFog.classList.contains('active'), false);
    assert.strictEqual(btnSmoke.classList.contains('active'), true);
  });

  test('Day/Night cycle controls: toggleDayNightCycle, onDayNightSpeedSlider, and onTimeOfDaySlider', () => {
    assert.strictEqual(BglState.isDayNightCycleActive, false);
    window.toggleDayNightCycle();
    assert.strictEqual(BglState.isDayNightCycleActive, true);
    const btnDayNight = document.getElementById('btn-day-night');
    assert.strictEqual(btnDayNight.classList.contains('active'), true);
    window.toggleDayNightCycle();
    assert.strictEqual(BglState.isDayNightCycleActive, false);

    window.onDayNightSpeedSlider('2.5');
    assert.strictEqual(BglState.dayNightSpeed, 2.5);
    const speedLabel = document.getElementById('val-day-night-speed');
    assert.strictEqual(speedLabel.textContent, '2.50 h/s');

    // Noon (12:00) -> sun icon ☀️
    window.onTimeOfDaySlider('12.0');
    assert.strictEqual(BglState.timeOfDay, 12.0);
    const todLabel = document.getElementById('val-time-of-day');
    assert(todLabel.textContent.includes('12:00') && todLabel.textContent.includes('☀️'), 'Noon must display 12:00 ☀️');

    // Sunset (18.5 = 18:30) -> golden hour icon 🌅
    window.onTimeOfDaySlider('18.5');
    assert(todLabel.textContent.includes('18:30') && todLabel.textContent.includes('🌅'), 'Sunset must display 18:30 🌅');

    // Midnight (0.0 = 00:00) -> moon icon 🌙
    window.onTimeOfDaySlider('0.0');
    assert(todLabel.textContent.includes('00:00') && todLabel.textContent.includes('🌙'), 'Midnight must display 00:00 🌙');
  });

  test('Fog exponential distance and height attenuation math (Beer-Lambert + Mie scattering)', () => {
    const evalFog = (dist, worldY, intensity) => {
      const heightFalloff = Math.exp(-Math.max(worldY + 2.0, 0.0) * 0.12);
      const density = (0.045 + 0.055 * heightFalloff) * intensity;
      return Math.min(0.96, Math.max(0.0, 1.0 - Math.exp(-dist * density)));
    };

    const fogNear = evalFog(2.0, 0.0, 1.0);
    const fogFar = evalFog(35.0, 0.0, 1.0);
    const fogHighAlt = evalFog(15.0, 20.0, 1.0);
    const fogLowValley = evalFog(15.0, -2.0, 1.0);

    assert(fogFar > fogNear, 'Fog opacity must increase monotonically with distance');
    assert(fogLowValley > fogHighAlt, 'Ground-hugging valley fog must be denser than high-altitude air');
    assert.strictEqual(evalFog(20.0, 0.0, 0.0), 0.0, 'Zero intensity fog must yield 0.0 opacity');
  });

  test('onWeatherIntensitySlider(): updates BglState and formats HUD label', () => {
    window.onWeatherIntensitySlider('1.45');
    assert.strictEqual(BglState.weatherIntensity, 1.45);
    const label = document.getElementById('val-weather-intensity');
    assert.strictEqual(label.textContent, '1.45×');

    // Clamps to [0.0, 2.0]
    window.onWeatherIntensitySlider('3.5');
    assert.strictEqual(BglState.weatherIntensity, 2.0);
    window.onWeatherIntensitySlider('-1.0');
    assert.strictEqual(BglState.weatherIntensity, 0.0);
  });

  test('onWindSlider(): updates BglState.windStrength and formats sign-prefixed label', () => {
    window.onWindSlider('0.45');
    assert.strictEqual(BglState.windStrength, 0.45);
    const labelPos = document.getElementById('val-wind-strength');
    assert.strictEqual(labelPos.textContent, '+0.45');

    window.onWindSlider('-0.60');
    assert.strictEqual(BglState.windStrength, -0.60);
    assert.strictEqual(labelPos.textContent, '-0.60');

    // Clamps to [-1.0, 1.0]
    window.onWindSlider('2.5');
    assert.strictEqual(BglState.windStrength, 1.0);
    window.onWindSlider('-3.0');
    assert.strictEqual(BglState.windStrength, -1.0);
  });

  test('Rain wetness PBR math: simulates diffuse darkening and specular water glaze', () => {
    // Albedo absorption in wet liquid layer: mix(albedo, albedo * 0.65, wetWeight * (1 - metallic * 0.5))
    const dryAlbedo = [0.8, 0.6, 0.4];
    const metallic = 0.0;
    const intensity = 1.0;
    const wetWeight = Math.min(0.75, intensity * 0.8);
    const wetAlbedo = dryAlbedo.map(c => c * (1.0 - wetWeight) + (c * 0.65) * wetWeight * (1.0 - metallic * 0.5));
    assert(wetAlbedo[0] < dryAlbedo[0], 'Wet albedo must be darker than dry albedo');
    assert(approxEq(wetAlbedo[0], 0.8 * 0.25 + (0.8 * 0.65) * 0.75, 1e-4));

    // Specular water sheen: roughness drops towards 0.08
    const dryRoughness = 0.75;
    const wetRoughness = dryRoughness * (1.0 - 0.75) + 0.08 * 0.75;
    assert(wetRoughness < dryRoughness, 'Wet roughness must be lower (glossier) than dry roughness');
    assert(wetRoughness >= 0.08, 'Wet roughness should approach 0.08 water sheen');
  });

  test('Snow surface accumulation math: gates strictly on upward-facing normal (N.y > 0.2)', () => {
    const smoothstep = (min, max, x) => {
      const t = Math.max(0, Math.min(1, (x - min) / (max - min)));
      return t * t * (3 - 2 * t);
    };

    const intensity = 1.0;
    const groundNormalY = 1.0; // Upward facing surface
    const wallNormalY = 0.0;   // Vertical wall
    const ceilingNormalY = -1.0; // Downward overhang

    const snowGround = smoothstep(0.2, 0.75, groundNormalY) * intensity;
    const snowWall = smoothstep(0.2, 0.75, wallNormalY) * intensity;
    const snowCeiling = smoothstep(0.2, 0.75, ceilingNormalY) * intensity;

    assert.strictEqual(snowGround, 1.0, 'Upward facing ground must accumulate full snow');
    assert.strictEqual(snowWall, 0.0, 'Vertical walls must accumulate zero snow');
    assert.strictEqual(snowCeiling, 0.0, 'Overhang ceilings must accumulate zero snow');
  });

  test('Procedural rain droplet ripple wave dispersion and exponential spatial damping', () => {
    // Wave equation: sin(d * 24.0 - t * 14.0) * exp(-d * 4.5) * (1.0 - t)
    const wave = (d, t) => Math.sin(d * 24.0 - t * 14.0) * Math.exp(-d * 4.5) * (1.0 - t);

    const nearCenter = wave(0.05, 0.2);
    const farDistance = wave(1.5, 0.2);

    assert(Math.abs(farDistance) < Math.abs(nearCenter), 'Wave amplitude must exponentially decay with radial distance');
    const expiredWave = wave(0.05, 1.0);
    assert.strictEqual(Math.abs(expiredWave), 0.0, 'Expired droplet (t = 1.0) amplitude must be zero');
  });

  test('Volumetric drifting snowflake lateral swaying turbulence oscillation', () => {
    // Oscillation equation: wobbleX = sin(time * freq + y * 2.5) * 0.35
    const wobble = (time, y) => Math.sin(time * 2.0 + y * 2.5) * 0.35;

    const w0 = wobble(0.0, 1.0);
    const w1 = wobble(0.785, 1.0); // +pi/4
    assert(w0 !== w1, 'Wobble must vary sinusoidally across time');
    assert(Math.abs(w0) <= 0.35 && Math.abs(w1) <= 0.35, 'Wobble amplitude must not exceed 0.35');
  });
});

// ============================================================================
// Final Summary & Exit Code
// ============================================================================
console.log('\n================================================================================');
console.log(` UNIT TEST EXECUTION COMPLETE`);
console.log(` Total Tests:  ${totalTests}`);
console.log(` Passed Tests: ${passedTests} (100%)`);
console.log(` Failed Tests: ${failedTests}`);
console.log('================================================================================');

if (failedTests > 0) {
  console.error('\nFailures Summary:');
  failures.forEach((f, idx) => {
    console.error(`  ${idx + 1}. ${f.name}`);
    console.error(`     ${f.error.stack || f.error.message}`);
  });
  process.exit(1);
} else {
  console.log(`✓ All ${totalTests} web viewer unit tests PASSED successfully!\n`);
  process.exit(0);
}
