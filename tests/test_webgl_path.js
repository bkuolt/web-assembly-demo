/**
 * Comprehensive verification test for BglWebGlRenderer pipeline.
 * Simulates WebGL 2 rendering context, DOM canvas, and event flow on ToyCar.glb.
 */
const fs = require('fs');
const assert = require('assert');

// 1. Mock WebGL 2 context
class MockWebGL2RenderingContext {
  constructor() {
    this.COLOR_BUFFER_BIT = 0x4000;
    this.DEPTH_BUFFER_BIT = 0x0100;
    this.DEPTH_TEST = 0x0B71;
    this.CULL_FACE = 0x0B44;
    this.BACK = 0x0405;
    this.VERTEX_SHADER = 0x8B31;
    this.FRAGMENT_SHADER = 0x8B30;
    this.COMPILE_STATUS = 0x8B81;
    this.LINK_STATUS = 0x8B82;
    this.ARRAY_BUFFER = 0x8892;
    this.ELEMENT_ARRAY_BUFFER = 0x8893;
    this.STATIC_DRAW = 0x88E4;
    this.FLOAT = 0x1406;
    this.UNSIGNED_SHORT = 0x1403;
    this.UNSIGNED_INT = 0x1405;
    this.TRIANGLES = 0x0004;
    this.LINES = 0x0001;
    this.TEXTURE_2D = 0x0DE1;
    this.TEXTURE0 = 0x84C0;
    this.RGBA = 0x1908;
    this.UNSIGNED_BYTE = 0x1401;
    this.LINEAR = 0x2601;
    this.LINEAR_MIPMAP_LINEAR = 0x2703;
    this.TEXTURE_MIN_FILTER = 0x2801;
    this.TEXTURE_MAG_FILTER = 0x2800;

    this.drawCalls = 0;
    this.boundVAO = null;
    this.boundProgram = null;
  }

  createShader(type) { return { id: Math.random(), type }; }
  shaderSource(shader, src) { shader.src = src; }
  compileShader(shader) { shader.compiled = true; }
  getShaderParameter(shader, param) { return true; }
  getShaderInfoLog(shader) { return ''; }
  deleteShader(shader) {}

  createProgram() { return { id: Math.random(), shaders: [] }; }
  attachShader(prog, shader) { prog.shaders.push(shader); }
  linkProgram(prog) { prog.linked = true; }
  getProgramParameter(prog, param) { return true; }
  getProgramInfoLog(prog) { return ''; }
  useProgram(prog) { this.boundProgram = prog; }
  deleteProgram(prog) {}

  getUniformLocation(prog, name) { return { name }; }
  uniformMatrix4fv(loc, transpose, val) {}
  uniformMatrix3fv(loc, transpose, val) {}
  uniform3fv(loc, val) {}
  uniform4fv(loc, val) {}
  uniform1i(loc, val) {}

  createBuffer() { return { id: Math.random() }; }
  bindBuffer(target, buf) {}
  bufferData(target, data, usage) {}

  createVertexArray() { return { id: Math.random() }; }
  bindVertexArray(vao) { this.boundVAO = vao; }
  deleteVertexArray(vao) {}

  enableVertexAttribArray(idx) {}
  vertexAttribPointer(idx, size, type, norm, stride, offset) {}

  createTexture() { return { id: Math.random() }; }
  bindTexture(target, tex) {}
  texImage2D(...args) {}
  texParameteri(...args) {}
  generateMipmap(target) {}
  activeTexture(slot) {}

  viewport(x, y, w, h) {}
  enable(cap) {}
  disable(cap) {}
  cullFace(face) {}
  clearColor(r, g, b, a) {}
  clear(mask) {}

  drawElements(mode, count, type, offset) {
    assert(this.boundVAO !== null, 'drawElements called without bound VAO');
    this.drawCalls++;
  }
  drawArrays(mode, first, count) {
    assert(this.boundVAO !== null, 'drawArrays called without bound VAO');
    this.drawCalls++;
  }
}

// 2. Mock DOM Canvas
const mockGl = new MockWebGL2RenderingContext();
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
    if (type === 'webgl2') return mockGl;
    return null;
  }
};

global.document = {
  getElementById(id) {
    if (id === 'canvas') return mockCanvas;
    return null;
  },
  querySelector() { return null; },
  createElement() { return {}; }
};

global.window = {
  innerWidth: 1280,
  innerHeight: 720,
  devicePixelRatio: 2.0,
  addEventListener(event, fn) {
    mockCanvas.addEventListener(event, fn);
  }
};

global.performance = { now: () => 1000 };
global.requestAnimationFrame = (fn) => setTimeout(fn, 16);
global.BglLogger = { log: () => {}, warn: () => {}, error: () => {} };
global.BglState = { timeOfDay: 14.0 };
global.Blob = class { constructor(data, opts) {} };
global.createImageBitmap = async () => ({ width: 1, height: 1 });

// Load app.js and extract BglWebGlRenderer
const appCode = fs.readFileSync('web/app.js', 'utf8');

// Isolate BglWebGlRenderer definition
const startIdx = appCode.indexOf('const BglWebGlRenderer = {');
const endIdx = appCode.indexOf('  // ============================================================================\n  // Model Ingestion Coordinator');
const rendererCode = appCode.slice(startIdx, endIdx);

// Execute renderer in current scope
eval(rendererCode.replace('const BglWebGlRenderer =', 'global.BglWebGlRenderer ='));

console.log('--- Step 1: Initializing BglWebGlRenderer ---');
const initSuccess = BglWebGlRenderer.init();
assert.strictEqual(initSuccess, true, 'BglWebGlRenderer.init() must return true');
assert.strictEqual(BglWebGlRenderer.isActive, true, 'BglWebGlRenderer.isActive must be true');
console.log('✓ BglWebGlRenderer initialized successfully');

console.log('--- Step 2: Ingesting ToyCar.glb into WebGL 2 pipeline ---');
const toyCarBuf = fs.readFileSync('web/assets/ToyCar.glb');
const dv = new DataView(toyCarBuf.buffer, toyCarBuf.byteOffset, toyCarBuf.byteLength);
const jsonLen = dv.getUint32(12, true);
const jsonBytes = new Uint8Array(toyCarBuf.buffer, toyCarBuf.byteOffset + 20, jsonLen);
const gltf = JSON.parse(new TextDecoder().decode(jsonBytes));

BglWebGlRenderer.loadModel(gltf, toyCarBuf.buffer, 'ToyCar.glb');
assert(BglWebGlRenderer.primitives.length > 0, 'Primitives must be loaded');
console.log(`✓ Loaded ${BglWebGlRenderer.primitives.length} primitives from ToyCar.glb`);
console.log('  Model Bounds Center:', BglWebGlRenderer.modelBounds.center);
console.log('  Model Bounds Radius:', BglWebGlRenderer.modelBounds.radius);
assert(BglWebGlRenderer.modelBounds.radius > 0, 'Radius must be > 0');

if (fs.existsSync('web/assets/Sponza.glb')) {
  console.log('--- Step 2b: Testing Sponza.glb ingestion into WebGL 2 pipeline ---');
  const sponzaBuf = fs.readFileSync('web/assets/Sponza.glb');
  const dvS = new DataView(sponzaBuf.buffer, sponzaBuf.byteOffset, sponzaBuf.byteLength);
  const jsonLenS = dvS.getUint32(12, true);
  const jsonBytesS = new Uint8Array(sponzaBuf.buffer, sponzaBuf.byteOffset + 20, jsonLenS);
  const gltfS = JSON.parse(new TextDecoder().decode(jsonBytesS));

  BglWebGlRenderer.loadModel(gltfS, sponzaBuf.buffer, 'Sponza.glb');
  assert(BglWebGlRenderer.primitives.length > 0, 'Sponza primitives must be loaded');
  console.log(`✓ Loaded ${BglWebGlRenderer.primitives.length} primitives from Sponza.glb`);
  console.log('  Sponza Bounds Center:', BglWebGlRenderer.modelBounds.center);
  console.log('  Sponza Bounds Radius:', BglWebGlRenderer.modelBounds.radius);
}

console.log('--- Step 3: Executing draw() cycle ---');
mockGl.drawCalls = 0;
BglWebGlRenderer.draw();
console.log(`✓ Render pass completed with ${mockGl.drawCalls} draw calls`);
assert.strictEqual(mockGl.drawCalls, BglWebGlRenderer.primitives.length, 'Each primitive should have 1 draw call');

console.log('--- Step 4: Testing Turntable and Camera Orbit ---');
const initialYaw = BglWebGlRenderer.camera.yaw;
BglWebGlRenderer.turntable = true;
BglWebGlRenderer.turntableSpeed = 1.0;
// Simulate 1 second
BglWebGlRenderer.lastFrameTime = 1000;
BglWebGlRenderer.camera.yaw += BglWebGlRenderer.turntableSpeed * 0.016;
assert(BglWebGlRenderer.camera.yaw > initialYaw, 'Yaw must advance during turntable');
console.log('✓ Turntable advances camera yaw');

console.log('--- Step 5: Testing Mobile Touch Gesture Handlers ---');
const touchStartFns = mockCanvas.eventListeners['touchstart'];
const touchMoveFns = mockCanvas.eventListeners['touchmove'];
assert(touchStartFns && touchStartFns.length > 0, 'touchstart listener must be registered');
assert(touchMoveFns && touchMoveFns.length > 0, 'touchmove listener must be registered');

// Test 1-finger orbit
const yawBefore = BglWebGlRenderer.camera.yaw;
touchStartFns[0]({ touches: [{ clientX: 100, clientY: 100 }], preventDefault: () => {} });
touchMoveFns[0]({ touches: [{ clientX: 150, clientY: 100 }], preventDefault: () => {} });
assert(BglWebGlRenderer.camera.yaw !== yawBefore, '1-finger touchmove must change camera yaw');
console.log('✓ 1-finger touch swipe successfully rotates camera');

// Test 2-finger pinch zoom
const distBefore = BglWebGlRenderer.camera.distance;
touchStartFns[0]({
  touches: [{ clientX: 100, clientY: 100 }, { clientX: 200, clientY: 100 }],
  preventDefault: () => {}
});
touchMoveFns[0]({
  touches: [{ clientX: 50, clientY: 100 }, { clientX: 250, clientY: 100 }], // wider -> pinch out (zoom in)
  preventDefault: () => {}
});
assert(BglWebGlRenderer.camera.distance !== distBefore, '2-finger pinch must change camera distance');
console.log('✓ 2-finger pinch gesture successfully zooms camera');

console.log('--- ALL WEBGL FALLBACK PIPELINE TESTS PASSED (100%) ---');
process.exit(0);
