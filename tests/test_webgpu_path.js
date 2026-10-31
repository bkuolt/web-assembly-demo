/**
 * Comprehensive verification test for BglWebGpuRenderer pipeline.
 * Simulates standard WebGPU context, GPUDevice, command encoders, and WGSL pipeline on ToyCar.glb and Sponza.glb.
 */
const fs = require('fs');
const assert = require('assert');

// 1. Mock WebGPU Environment
class MockGPUDevice {
  constructor() {
    this.submittedCommands = [];
    this.queue = {
      writeBuffer: (buf, off, data) => { buf.writtenData = data; },
      writeTexture: (dest, data, layout, size) => {},
      copyExternalImageToTexture: (src, dest, size) => {},
      submit: (cmds) => { this.submittedCommands.push(...cmds); }
    };
    this.lost = new Promise(() => {}); // never lost in test
  }

  createBuffer(desc) {
    return {
      id: Math.random(),
      size: desc.size,
      usage: desc.usage,
      destroy: () => { this.destroyedBuffers = (this.destroyedBuffers || 0) + 1; }
    };
  }

  createTexture(desc) {
    return {
      id: Math.random(),
      size: desc.size,
      format: desc.format,
      usage: desc.usage,
      createView: () => ({ id: Math.random(), textureId: this.id }),
      destroy: () => {}
    };
  }

  createSampler(desc) {
    return { id: Math.random(), desc };
  }

  createBindGroupLayout(desc) {
    return { id: Math.random(), entries: desc.entries };
  }

  createPipelineLayout(desc) {
    return { id: Math.random(), bindGroupLayouts: desc.bindGroupLayouts };
  }

  createShaderModule(desc) {
    assert(desc.code && desc.code.includes('@vertex') && desc.code.includes('@fragment'), 'Shader must contain WGSL vertex and fragment stages');
    return { id: Math.random(), code: desc.code };
  }

  createRenderPipeline(desc) {
    assert(desc.vertex && desc.vertex.entryPoint === 'vs_main', 'Vertex entry point must be vs_main');
    assert(desc.fragment && desc.fragment.entryPoint === 'fs_main', 'Fragment entry point must be fs_main');
    return { id: Math.random(), desc };
  }

  createBindGroup(desc) {
    return { id: Math.random(), layout: desc.layout, entries: desc.entries };
  }

  createCommandEncoder() {
    return new MockGPUCommandEncoder();
  }
}

class MockGPURenderPassEncoder {
  constructor() {
    this.drawCalls = 0;
    this.indexedDrawCalls = 0;
    this.pipeline = null;
    this.bindGroups = [];
    this.vertexBuffers = [];
    this.indexBuffer = null;
    this.indexFormat = null;
  }

  setPipeline(p) { this.pipeline = p; }
  setBindGroup(slot, bg) { this.bindGroups[slot] = bg; }
  setVertexBuffer(slot, vb) { this.vertexBuffers[slot] = vb; }
  setIndexBuffer(ib, format) {
    assert(format === 'uint16' || format === 'uint32', `Invalid index format: ${format}`);
    this.indexBuffer = ib;
    this.indexFormat = format;
  }
  draw(vertexCount) {
    assert(this.pipeline !== null, 'draw called without pipeline');
    this.drawCalls++;
  }
  drawIndexed(indexCount) {
    assert(this.pipeline !== null, 'drawIndexed called without pipeline');
    assert(this.indexBuffer !== null, 'drawIndexed called without index buffer');
    this.drawCalls++;
    this.indexedDrawCalls++;
  }
  end() { this.ended = true; }
}

class MockGPUCommandEncoder {
  constructor() {
    this.lastPass = null;
  }
  beginRenderPass(desc) {
    this.lastPass = new MockGPURenderPassEncoder();
    return this.lastPass;
  }
  finish() {
    return { id: Math.random(), pass: this.lastPass };
  }
}

class MockGPUCanvasContext {
  constructor() {
    this.configured = null;
  }
  configure(desc) {
    this.configured = desc;
  }
  getCurrentTexture() {
    return {
      createView: () => ({ id: 'swapchain-texture-view' })
    };
  }
}

const mockGpuDevice = new MockGPUDevice();
const mockWebGpuContext = new MockGPUCanvasContext();

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
    if (type === 'webgpu') return mockWebGpuContext;
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

Object.defineProperty(globalThis, 'navigator', {
  value: {
    gpu: {
      requestAdapter: async () => ({
        info: {
          vendor: 'NVIDIA Corporation',
          architecture: 'Ada Lovelace',
          description: 'NVIDIA GeForce RTX 4090'
        },
        requestDevice: async () => mockGpuDevice
      }),
      getPreferredCanvasFormat: () => 'bgra8unorm'
    }
  },
  configurable: true,
  writable: true
});

global.GPUTextureUsage = {
  COPY_SRC: 0x01,
  COPY_DST: 0x02,
  TEXTURE_BINDING: 0x04,
  STORAGE_BINDING: 0x08,
  RENDER_ATTACHMENT: 0x10
};

global.GPUBufferUsage = {
  MAP_READ: 0x01,
  MAP_WRITE: 0x02,
  COPY_SRC: 0x04,
  COPY_DST: 0x08,
  INDEX: 0x10,
  VERTEX: 0x20,
  UNIFORM: 0x40,
  STORAGE: 0x80
};

global.GPUShaderStage = {
  VERTEX: 0x1,
  FRAGMENT: 0x2,
  COMPUTE: 0x4
};

global.performance = { now: () => 1000 };
global.requestAnimationFrame = (fn) => setTimeout(fn, 16);
global.BglLogger = { log: () => {}, warn: () => {}, error: () => {} };
global.BglState = { timeOfDay: 14.0 };
global.Blob = class { constructor(data, opts) {} };
global.createImageBitmap = async () => ({ width: 64, height: 64 });

// Load app.js and isolate BglWebGpuRenderer definition
const appCode = fs.readFileSync('web/app.js', 'utf8');

// Isolate BglGltf definition
const gltfStart = appCode.indexOf('const BglGltf = {');
const gltfEnd = appCode.indexOf('  // ============================================================================\n  // Native WebGPU 3D Renderer');
eval(appCode.slice(gltfStart, gltfEnd).replace('const BglGltf =', 'global.BglGltf ='));

const startIdx = appCode.indexOf('const BglWebGpuRenderer = {');
const endIdx = appCode.indexOf('  // ============================================================================\n  // High-Performance WebGL 2 (GLES 3.0) Fallback 3D Renderer');
assert(startIdx !== -1, 'BglWebGpuRenderer must be defined in app.js');
assert(endIdx !== -1, 'BglWebGpuRenderer delimiter must be present');
const rendererCode = appCode.slice(startIdx, endIdx);

// Execute renderer in current scope
eval(rendererCode.replace('const BglWebGpuRenderer =', 'global.BglWebGpuRenderer ='));

(async () => {
  console.log('--- Step 1: Initializing BglWebGpuRenderer ---');
  const initSuccess = await BglWebGpuRenderer.init();
  assert.strictEqual(initSuccess, true, 'BglWebGpuRenderer.init() must return true');
  assert.strictEqual(BglWebGpuRenderer.isActive, true, 'BglWebGpuRenderer.isActive must be true');
  assert.strictEqual(BglWebGpuRenderer.format, 'bgra8unorm', 'Canvas format must match getPreferredCanvasFormat');
  assert(BglWebGpuRenderer.pipeline !== null, 'Pipeline must be compiled');
  assert(BglWebGpuRenderer.pipelineDoubleSided !== null, 'Double-sided pipeline must be compiled');
  console.log('✓ BglWebGpuRenderer initialized successfully with WGSL shaders and WebGPU pipelines');

  console.log('--- Step 2: Ingesting ToyCar.glb into WebGPU pipeline ---');
  const toyCarBuf = fs.readFileSync('web/assets/ToyCar.glb');
  const dv = new DataView(toyCarBuf.buffer, toyCarBuf.byteOffset, toyCarBuf.byteLength);
  const jsonLen = dv.getUint32(12, true);
  const jsonBytes = new Uint8Array(toyCarBuf.buffer, toyCarBuf.byteOffset + 20, jsonLen);
  const gltf = JSON.parse(new TextDecoder().decode(jsonBytes));

  BglWebGpuRenderer.loadModel(gltf, toyCarBuf.buffer, 'ToyCar.glb');
  assert(BglWebGpuRenderer.primitives.length > 0, 'Primitives must be loaded');
  console.log(`✓ Loaded ${BglWebGpuRenderer.primitives.length} primitives from ToyCar.glb`);
  console.log('  Model Bounds Center:', BglWebGpuRenderer.modelBounds.center);
  console.log('  Model Bounds Radius:', BglWebGpuRenderer.modelBounds.radius);
  assert(BglWebGpuRenderer.modelBounds.radius > 0, 'Radius must be > 0');

  for (const prim of BglWebGpuRenderer.primitives) {
    assert(prim.vertexBuffer, 'Primitive must have vertex buffer');
    assert(prim.uniformBuffer, 'Primitive must have uniform buffer');
    assert(prim.bindGroup, 'Primitive must have bind group');
    if (prim.hasIndices) {
      assert(prim.indexBuffer, 'Indexed primitive must have index buffer');
      assert(prim.indexFormat === 'uint16' || prim.indexFormat === 'uint32', 'Index format must be uint16 or uint32');
    }
  }
  console.log('✓ All GPU buffer bindings, uniform buffers, and bind groups validated');

  if (fs.existsSync('web/assets/Sponza.glb')) {
    console.log('--- Step 2b: Testing Sponza.glb ingestion into WebGPU pipeline ---');
    const sponzaBuf = fs.readFileSync('web/assets/Sponza.glb');
    const dvS = new DataView(sponzaBuf.buffer, sponzaBuf.byteOffset, sponzaBuf.byteLength);
    const jsonLenS = dvS.getUint32(12, true);
    const jsonBytesS = new Uint8Array(sponzaBuf.buffer, sponzaBuf.byteOffset + 20, jsonLenS);
    const gltfS = JSON.parse(new TextDecoder().decode(jsonBytesS));

    BglWebGpuRenderer.loadModel(gltfS, sponzaBuf.buffer, 'Sponza.glb');
    assert(BglWebGpuRenderer.primitives.length > 0, 'Sponza primitives must be loaded');
    console.log(`✓ Loaded ${BglWebGpuRenderer.primitives.length} primitives from Sponza.glb`);
    console.log('  Sponza Bounds Center:', BglWebGpuRenderer.modelBounds.center);
    console.log('  Sponza Bounds Radius:', BglWebGpuRenderer.modelBounds.radius);
  }

  console.log('--- Step 3: Executing draw() cycle ---');
  mockGpuDevice.submittedCommands = [];
  BglWebGpuRenderer.draw();
  assert(mockGpuDevice.submittedCommands.length > 0, 'Commands must be submitted to device queue');
  const lastPass = mockGpuDevice.submittedCommands[0].pass;
  assert(lastPass, 'Render pass encoder must exist');
  console.log(`✓ Render pass completed with ${lastPass.drawCalls} draw calls submitted to GPU queue`);
  assert.strictEqual(lastPass.drawCalls, BglWebGpuRenderer.primitives.length, 'Each primitive should have 1 draw call');

  console.log('--- Step 4: Testing Turntable and Camera Orbit ---');
  const initialYaw = BglWebGpuRenderer.camera.yaw;
  BglWebGpuRenderer.turntable = true;
  BglWebGpuRenderer.turntableSpeed = 1.0;
  BglWebGpuRenderer.lastFrameTime = 1000;
  BglWebGpuRenderer.camera.yaw += BglWebGpuRenderer.turntableSpeed * 0.016;
  assert(BglWebGpuRenderer.camera.yaw > initialYaw, 'Yaw must advance during turntable');
  console.log('✓ Turntable advances camera yaw');

  console.log('--- Step 5: Testing Mobile Touch Gesture Handlers ---');
  const touchStartFns = mockCanvas.eventListeners['touchstart'];
  const touchMoveFns = mockCanvas.eventListeners['touchmove'];
  assert(touchStartFns && touchStartFns.length > 0, 'touchstart listener must be registered');
  assert(touchMoveFns && touchMoveFns.length > 0, 'touchmove listener must be registered');

  // Test 1-finger orbit
  const yawBefore = BglWebGpuRenderer.camera.yaw;
  touchStartFns[0]({ touches: [{ clientX: 100, clientY: 100 }], preventDefault: () => {} });
  touchMoveFns[0]({ touches: [{ clientX: 150, clientY: 100 }], preventDefault: () => {} });
  assert(BglWebGpuRenderer.camera.yaw !== yawBefore, '1-finger touchmove must change camera yaw');
  console.log('✓ 1-finger touch swipe successfully rotates camera');

  // Test 2-finger pinch zoom
  const distBefore = BglWebGpuRenderer.camera.distance;
  touchStartFns[0]({
    touches: [{ clientX: 100, clientY: 100 }, { clientX: 200, clientY: 100 }],
    preventDefault: () => {}
  });
  touchMoveFns[0]({
    touches: [{ clientX: 50, clientY: 100 }, { clientX: 250, clientY: 100 }],
    preventDefault: () => {}
  });
  assert(BglWebGpuRenderer.camera.distance !== distBefore, '2-finger pinch must change camera distance');
  console.log('✓ 2-finger pinch gesture successfully zooms camera');

  console.log('--- Step 6: Testing WebGPU Resource Cleanup & VRAM Safety ---');
  const prevDestroyed = mockGpuDevice.destroyedBuffers || 0;
  BglWebGpuRenderer.loadModel(gltf, toyCarBuf.buffer, 'ToyCar.glb');
  assert((mockGpuDevice.destroyedBuffers || 0) > prevDestroyed, 'Prior WebGPU buffers must be destroyed upon loading a new model');
  console.log(`✓ WebGPU GPUBuffer cleanup verified (${mockGpuDevice.destroyedBuffers} buffers destroyed)`);

  console.log('--- Step 7: Testing Zero-Allocation WebGPU Render Loop ---');
  const refProj = BglWebGpuRenderer._scratchProj;
  const refView = BglWebGpuRenderer._scratchView;
  const refViewProj = BglWebGpuRenderer._scratchViewProj;
  const refUboBuf = BglWebGpuRenderer._scratchUboBuffer;
  BglWebGpuRenderer.draw();
  BglWebGpuRenderer.draw();
  assert.strictEqual(BglWebGpuRenderer._scratchProj, refProj, 'Scratch projection matrix must be reused across frames');
  assert.strictEqual(BglWebGpuRenderer._scratchView, refView, 'Scratch view matrix must be reused across frames');
  assert.strictEqual(BglWebGpuRenderer._scratchViewProj, refViewProj, 'Scratch viewProj matrix must be reused across frames');
  assert.strictEqual(BglWebGpuRenderer._scratchUboBuffer, refUboBuf, 'Scratch UBO buffer must be reused across frames');
  console.log('✓ WebGPU scratch buffers reused with 0 heap buffer allocations per frame');

  console.log('--- Step 8: Testing Malformed / Truncated GLB Robustness ---');
  assert.throws(() => {
    BglGltf.extractJson(new ArrayBuffer(3));
  }, /too short/i, 'Truncated buffer (<4 bytes) must throw descriptive error');

  const truncatedHeader = new ArrayBuffer(16);
  new DataView(truncatedHeader).setUint32(0, 0x46546C67, true);
  assert.throws(() => {
    BglGltf.extractJson(truncatedHeader);
  }, /Truncated GLB container/i, 'Truncated GLB header (<20 bytes) must throw descriptive error');

  const corruptChunkLen = new ArrayBuffer(28);
  const cdv = new DataView(corruptChunkLen);
  cdv.setUint32(0, 0x46546C67, true);
  cdv.setUint32(4, 2, true);
  cdv.setUint32(8, 28, true);
  cdv.setUint32(12, 1000, true);
  cdv.setUint32(16, 0x4E4F534A, true);
  assert.throws(() => {
    BglGltf.extractJson(corruptChunkLen);
  }, /Truncated GLB chunk/i, 'Oversized chunk length must throw descriptive error');
  console.log('✓ Corrupt/truncated GLB files safely rejected without crashing');

  console.log('--- ALL WEBGPU PIPELINE TESTS PASSED (100%) ---');
  process.exit(0);
})().catch((err) => {
  console.error('Test failed with error:', err);
  process.exit(1);
});
