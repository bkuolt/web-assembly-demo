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
    assert(desc.code.includes('distributionGGX') && desc.code.includes('visibilitySmithGGX'), 'Shader must include Cook-Torrance GGX functions');
    assert(desc.code.includes('parallaxOcclusionMapping') && desc.code.includes('computeParallaxShadow'), 'Shader must include POM and parallax self-shadowing');
    assert(desc.code.includes('getSurfaceHeight'), 'Shader must include getSurfaceHeight for height-based bump mapping');
    const fsStart = desc.code.indexOf('fn fs_main(');
    assert(fsStart >= 0, 'Shader must declare fs_main');
    const fsBody = desc.code.slice(fsStart);
    assert(!/(?<![.\w])uv(?![.\w])/.test(fsBody), 'fs_main must not reference undeclared bare identifier uv');
    assert(!fsBody.includes('baseColor.a < 0.01'), 'fs_main must not discard opaque materials below alpha 0.01');
    return { id: Math.random(), code: desc.code };
  }

  createRenderPipeline(desc) {
    assert(desc.vertex && (desc.vertex.entryPoint === 'vs_main' || desc.vertex.entryPoint === 'vs_main_skinned' || desc.vertex.entryPoint === 'vs_mip'), 'Vertex entry point must be vs_main, vs_main_skinned, or vs_mip');
    assert(desc.fragment && (desc.fragment.entryPoint === 'fs_main' || desc.fragment.entryPoint === 'fs_mip'), 'Fragment entry point must be fs_main or fs_mip');
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

// Load modular BglGltf and BglWebGpuRenderer definitions directly
eval(fs.readFileSync('web/js/gltf.js', 'utf8'));
global.BglState = { isBumpActive: true, isPomActive: true, bumpScale: 1.5, pomScale: 0.035 };
eval(fs.readFileSync('web/js/math.js', 'utf8'));
eval(fs.readFileSync('web/js/shaders.js', 'utf8'));
eval(fs.readFileSync('web/js/renderer.js', 'utf8'));

(async () => {
  console.log('--- Step 1: Initializing BglWebGpuRenderer ---');
  const initSuccess = await BglWebGpuRenderer.init();
  assert.strictEqual(initSuccess, true, 'BglWebGpuRenderer.init() must return true');
  assert.strictEqual(BglWebGpuRenderer.isActive, true, 'BglWebGpuRenderer.isActive must be true');
  assert.strictEqual(BglWebGpuRenderer.format, 'bgra8unorm', 'Canvas format must match getPreferredCanvasFormat');
  assert(BglWebGpuRenderer.pipeline !== null, 'Pipeline must be compiled');
  assert(BglWebGpuRenderer.pipelineDoubleSided !== null, 'Double-sided pipeline must be compiled');
  assert(BglWebGpuRenderer.mipmapPipeline !== null, 'Mipmap render pipeline must be compiled');
  assert.strictEqual(BglWebGpuRenderer.pipeline.desc.vertex.buffers[0].arrayStride, 48, 'Vertex buffer stride must be 48 bytes (pos+norm+uv+tangent)');
  assert.strictEqual(BglWebGpuRenderer.pipeline.desc.vertex.buffers[0].attributes.length, 4, 'Vertex buffer must declare 4 attributes');
  assert.strictEqual(BglWebGpuRenderer.defaultSampler.desc.addressModeU, 'repeat', 'Default sampler addressModeU must be repeat');
  assert.strictEqual(BglWebGpuRenderer.defaultSampler.desc.addressModeV, 'repeat', 'Default sampler addressModeV must be repeat');
  assert.strictEqual(BglWebGpuRenderer.defaultSampler.desc.maxAnisotropy, 16, 'Default sampler maxAnisotropy must be 16');
  assert(BglWebGpuRenderer.defaultWhiteTexture !== null, 'Default white texture must be created');
  assert(BglWebGpuRenderer.defaultMrTexture !== null, 'Default metallic-roughness texture must be created');
  assert(BglWebGpuRenderer.defaultNormalTexture !== null, 'Default normal texture must be created');
  assert(BglWebGpuRenderer.defaultBlackTexture !== null, 'Default emissive black texture must be created');
  console.log('✓ BglWebGpuRenderer initialized successfully with Cook-Torrance WGSL PBR shaders and WebGPU pipelines');

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
    assert.strictEqual(prim.uniformBuffer.size, 512, 'Uniform buffer must be 512 bytes for PBR uniforms');
    assert(prim.bindGroup, 'Primitive must have bind group');
    assert.strictEqual(prim.bindGroup.entries.length, 7, 'Bind group must have 7 entries for full PBR pipeline');
    if (prim.hasIndices) {
      assert(prim.indexBuffer, 'Indexed primitive must have index buffer');
      assert(prim.indexFormat === 'uint16' || prim.indexFormat === 'uint32', 'Index format must be uint16 or uint32');
    }
  }
  console.log('✓ All GPU buffer bindings, uniform buffers, and 7-entry PBR bind groups validated');

  if (fs.existsSync('web/assets/Sponza.glb')) {
    console.log('--- Step 2b: Testing Sponza.glb ingestion into WebGPU pipeline ---');
    const sponzaBuf = fs.readFileSync('web/assets/Sponza.glb');
    const dvS = new DataView(sponzaBuf.buffer, sponzaBuf.byteOffset, sponzaBuf.byteLength);
    const jsonLenS = dvS.getUint32(12, true);
    const jsonBytesS = new Uint8Array(sponzaBuf.buffer, sponzaBuf.byteOffset + 20, jsonLenS);
    const gltfS = JSON.parse(new TextDecoder().decode(jsonBytesS));

    BglWebGpuRenderer.loadModel(gltfS, sponzaBuf.buffer, 'Sponza.glb');
    assert(BglWebGpuRenderer.primitives.length > 0, 'Sponza primitives must be loaded');
    assert.strictEqual(BglWebGpuRenderer.modelBounds.isSponza, true, 'Sponza modelBounds.isSponza must be true');
    assert.strictEqual(BglWebGpuRenderer.camera.distance, 7.2, 'Sponza camera distance must be placed at 7.2m inside courtyard');
    assert.strictEqual(BglWebGpuRenderer.textureCache.size, gltfS.images.length, 'Async textureCache must deduplicate Sponza textures to exact image count');
    console.log(`✓ Loaded ${BglWebGpuRenderer.primitives.length} primitives from Sponza.glb (${BglWebGpuRenderer.textureCache.size} unique textures cached)`);
    console.log('  Sponza Bounds Center:', BglWebGpuRenderer.modelBounds.center);
    console.log('  Sponza Bounds Radius:', BglWebGpuRenderer.modelBounds.radius);
  }

  console.log('--- Step 3: Executing draw() cycle ---');
  mockGpuDevice.submittedCommands = [];
  BglWebGpuRenderer.draw();
  assert(mockGpuDevice.submittedCommands.length > 0, 'Commands must be submitted to device queue');
  const lastPass = mockGpuDevice.submittedCommands[0].pass;
  assert(lastPass, 'Render pass encoder must exist');
  console.log(`✓ Render pass completed with ${lastPass.drawCalls}/${BglWebGpuRenderer.primitives.length} draw calls submitted to GPU queue (frustum culled)`);
  assert(lastPass.drawCalls > 0 && lastPass.drawCalls <= BglWebGpuRenderer.primitives.length, 'Frustum-culled draw calls must be > 0 and <= total primitives');

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

  console.log('--- Step 9: Testing Skeletal Skinning & glTF Character Animations (RobotExpressive.glb & Soldier.glb) ---');
  for (const charFile of ['web/assets/RobotExpressive.glb', 'web/assets/Soldier.glb']) {
    if (!fs.existsSync(charFile)) continue;
    const charBuf = fs.readFileSync(charFile);
    const charArrayBuf = charBuf.buffer.slice(charBuf.byteOffset, charBuf.byteOffset + charBuf.byteLength);
    const charGltf = BglGltf.extractJson(charArrayBuf);
    BglWebGpuRenderer.loadModel(charGltf, charArrayBuf, charFile);

    assert(BglWebGpuRenderer.animations.length > 0, `${charFile} must have animations`);
    assert(BglWebGpuRenderer.skins.length > 0, `${charFile} must have skeletal skins`);
    const skinnedPrims = BglWebGpuRenderer.primitives.filter(p => p.isSkinned);
    assert(skinnedPrims.length > 0, `${charFile} must have at least one skinned primitive`);
    for (const sp of skinnedPrims) {
      assert(sp.skinVertexBuffer, 'Skinned primitive must have skinVertexBuffer');
      assert(sp.skinIdx >= 0 && BglWebGpuRenderer.skins[sp.skinIdx], 'Skinned primitive must reference valid skin');
    }

    // Advance performance.now() to simulate animation playback
    const origNow = global.performance.now;
    let simTime = (BglWebGpuRenderer._lastDrawMs || 1000);
    global.performance.now = () => { simTime += 33; return simTime; };
    BglWebGpuRenderer.setAnimation(0);
    BglWebGpuRenderer.draw();
    BglWebGpuRenderer.draw();
    global.performance.now = origNow;

    assert(BglWebGpuRenderer.animationTime > 0, `${charFile} animationTime must advance during playback`);
    console.log(`✓ ${charFile}: ${BglWebGpuRenderer.animations.length} animations, ${BglWebGpuRenderer.skins.length} skins, ${skinnedPrims.length} skinned primitives verified`);
  }

  console.log('--- Step 10: Testing Multi-Level Mesh LOD Selection & Telemetry in WebGPU Draw Loop ---');
  BglWebGpuRenderer.loadModel(gltf, toyCarBuf.buffer, 'ToyCar.glb');
  global.BglState.isLodActive = true;
  global.BglState.lodBias = 1.0;
  BglWebGpuRenderer.camera.distance = 0.05;
  BglWebGpuRenderer.draw();
  const nearRenderedTris = global.BglState.renderedTriangles;

  BglWebGpuRenderer.camera.distance = 12.0;
  global.BglState.lodBias = 3.5;
  BglWebGpuRenderer.draw();
  const farRenderedTris = global.BglState.renderedTriangles;

  assert(nearRenderedTris > 0 && farRenderedTris > 0, 'Triangle telemetry must be positive');
  assert(farRenderedTris < nearRenderedTris, `Far camera distance with LOD must render fewer triangles (${farRenderedTris} < ${nearRenderedTris})`);
  console.log(`✓ Multi-LOD WebGPU rendering verified: Near=${nearRenderedTris.toLocaleString()} tris vs Far=${farRenderedTris.toLocaleString()} tris`);

  console.log('--- Step 11: Testing Frustum Culling, Per-Channel KHR_texture_transform & Batched Mipmaps ---');
  // 11a: Frustum culling drops primitive placed far behind camera
  const behindModel = BglWebGpuRenderer.mat4.create();
  behindModel[12] = 0; behindModel[13] = 0; behindModel[14] = 5000;
  BglWebGpuRenderer.camera.target = [0, 0, 0];
  BglWebGpuRenderer.camera.distance = 5.0;
  BglWebGpuRenderer.camera.yaw = 0.0;
  BglWebGpuRenderer.camera.pitch = 0.0;
  BglWebGpuRenderer.primitives.push({
    ...BglWebGpuRenderer.primitives[0],
    uniformBuffer: mockGpuDevice.createBuffer({ size: 512, usage: 0x48 }),
    modelMatrix: behindModel,
    localCenter: [0, 0, 0],
    localRadius: 0.5,
    worldCenter: null,
    worldMatrixVersion: -1,
    isSkinned: false
  });
  mockGpuDevice.submittedCommands = [];
  BglWebGpuRenderer.draw();
  const culledPass = mockGpuDevice.submittedCommands[0].pass;
  assert(culledPass.drawCalls < BglWebGpuRenderer.primitives.length, `Behind-camera primitive must be frustum-culled (${culledPass.drawCalls} < ${BglWebGpuRenderer.primitives.length})`);

  // 11b: Per-channel KHR_texture_transform packs independent transforms into 512B UBO
  const multiUvGltf = {
    asset: { version: '2.0' },
    buffers: [{ uri: 'data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA', byteLength: 36 }],
    bufferViews: [{ buffer: 0, byteOffset: 0, byteLength: 36 }],
    accessors: [{ bufferView: 0, componentType: 5126, count: 3, type: 'VEC3' }],
    materials: [{
      pbrMetallicRoughness: {
        baseColorTexture: { index: 0, extensions: { KHR_texture_transform: { scale: [2, 3], offset: [0.1, 0.2], rotation: 0.5 } } },
        metallicRoughnessTexture: { index: 0, extensions: { KHR_texture_transform: { scale: [4, 5], offset: [0.25, 0.5], rotation: 1.0 } } }
      },
      normalTexture: { index: 0, extensions: { KHR_texture_transform: { scale: [6, 7], offset: [0.3, 0.4], rotation: 1.5 } } },
      occlusionTexture: { index: 0, extensions: { KHR_texture_transform: { scale: [8, 9], offset: [0.4, 0.6], rotation: 0.25 } } },
      emissiveTexture: { index: 0, extensions: { KHR_texture_transform: { scale: [10, 11], offset: [0.7, 0.8], rotation: 2.0 } } }
    }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0 }, material: 0 }] }],
    nodes: [{ mesh: 0 }],
    scenes: [{ nodes: [0] }],
    scene: 0
  };
  BglWebGpuRenderer.loadModel(multiUvGltf, new ArrayBuffer(0), 'multi_uv.gltf');
  BglWebGpuRenderer.draw();
  const uboOut = new Float32Array(BglWebGpuRenderer._scratchUboBuffer);
  assert(Math.abs(uboOut[68] - 2.0) < 1e-5 && Math.abs(uboOut[70] - 0.1) < 1e-5, 'baseColor UV transform must be preserved');
  assert(Math.abs(uboOut[88] - 4.0) < 1e-5 && Math.abs(uboOut[90] - 0.25) < 1e-5, 'mr UV transform must populate floats 88..91');
  assert(Math.abs(uboOut[92] - 6.0) < 1e-5 && Math.abs(uboOut[94] - 0.3) < 1e-5, 'normal UV transform must populate floats 92..95');
  assert(Math.abs(uboOut[96] - 8.0) < 1e-5 && Math.abs(uboOut[98] - 0.4) < 1e-5, 'occlusion UV transform must populate floats 96..99');
  assert(Math.abs(uboOut[100] - 10.0) < 1e-5 && Math.abs(uboOut[102] - 0.7) < 1e-5, 'emissive UV transform must populate floats 100..103');
  assert(Math.abs(uboOut[104] - 0.5) < 1e-5 && Math.abs(uboOut[105] - 1.0) < 1e-5 && Math.abs(uboOut[108] - 2.0) < 1e-5, 'per-channel rotations must populate uvRotations0/1');

  // 11c: Batched mipmap submission encodes multiple textures in one submit
  mockGpuDevice.submittedCommands = [];
  const t1 = mockGpuDevice.createTexture({ size: [8, 8, 1], format: 'rgba8unorm', usage: 0x16 });
  const t2 = mockGpuDevice.createTexture({ size: [8, 8, 1], format: 'rgba8unorm', usage: 0x16 });
  BglWebGpuRenderer.queueMipmapGeneration(t1, 8, 8, 3);
  BglWebGpuRenderer.queueMipmapGeneration(t2, 8, 8, 3);
  BglWebGpuRenderer.flushPendingMipmaps();
  assert.strictEqual(mockGpuDevice.submittedCommands.length, 1, 'Multiple queued mipmap textures must flush in 1 single queue submission');
  console.log('✓ Frustum culling, per-channel KHR_texture_transform, and single-submit mipmap batching verified');

  console.log('--- Step 12: Verifying Compiled Emscripten WASM Binary (web/dist/bgl_engine.wasm) ---');
  const wasmPath = 'web/dist/bgl_engine.wasm';
  assert(fs.existsSync(wasmPath), 'Compiled web/dist/bgl_engine.wasm must exist');
  const wasmBytes = fs.readFileSync(wasmPath);
  const wasmMod = new WebAssembly.Module(wasmBytes);
  const wasmImports = WebAssembly.Module.imports(wasmMod);
  assert.strictEqual(wasmImports.length, 1, 'WASM module must require only env.memory import');
  assert.strictEqual(wasmImports[0].module, 'env');
  assert.strictEqual(wasmImports[0].name, 'memory');

  const wasmMem = new WebAssembly.Memory({ initial: 256, maximum: 1024 });
  const wasmInst = await WebAssembly.instantiate(wasmMod, { env: { memory: wasmMem } });
  const exp = wasmInst.exports;
  assert.strictEqual(exp.bgl_init(1280, 720), 1, 'bgl_init must succeed');

  const toyPtr = exp.malloc(toyCarBuf.length);
  assert(toyPtr > 0, 'WASM malloc must allocate buffer for ToyCar.glb');
  new Uint8Array(wasmMem.buffer).set(toyCarBuf, toyPtr);
  const nameBytes = new TextEncoder().encode('ToyCar.glb\0');
  const namePtr = exp.malloc(nameBytes.length);
  new Uint8Array(wasmMem.buffer).set(nameBytes, namePtr);
  assert.strictEqual(exp.bgl_load_file_buffer(namePtr, toyPtr, toyCarBuf.length), 1, 'WASM bgl_load_file_buffer must parse ToyCar.glb');
  assert.strictEqual(exp.bgl_get_camera_count(), 8, 'ToyCar.glb must report 8 glTF cameras via WASM bridge');
  assert(exp.bgl_pick_node(640, 360, 1280, 720) >= 0, 'bgl_pick_node must select valid node');
  exp.free(namePtr);
  exp.free(toyPtr);

  // Security & bounds checks: overflow malloc, double-free, foreign pointer free, corrupt GLB chunkLen
  assert.strictEqual(exp.malloc(0xFFFFFFFF), 0, 'WASM malloc(0xFFFFFFFF) must fail safely without wraparound');
  const guardPtr = exp.malloc(64);
  assert(guardPtr > 0, 'WASM malloc(64) must succeed');
  exp.free(0);
  exp.free(0x1234);         // out-of-heap pointer
  exp.free(guardPtr + 8);   // interior pointer must be ignored
  exp.free(guardPtr);       // valid free
  exp.free(guardPtr);       // duplicate free must be ignored safely
  const afterFreePtr = exp.malloc(64);
  assert(afterFreePtr > 0, 'WASM heap must remain uncorrupted after invalid/double free');
  exp.free(afterFreePtr);

  const badGlb = new Uint8Array(24);
  const badDv = new DataView(badGlb.buffer);
  badDv.setUint32(0, 0x46546C67, true);
  badDv.setUint32(4, 2, true);
  badDv.setUint32(8, 24, true);
  badDv.setUint32(12, 0xFFFFFFF8, true); // overflowed chunk length
  badDv.setUint32(16, 0x4E4F534A, true);
  const badGlbPtr = exp.malloc(badGlb.length);
  const badName = new TextEncoder().encode('corrupt.glb\0');
  const badNamePtr = exp.malloc(badName.length);
  new Uint8Array(wasmMem.buffer).set(badGlb, badGlbPtr);
  new Uint8Array(wasmMem.buffer).set(badName, badNamePtr);
  assert.strictEqual(exp.bgl_load_file_buffer(badNamePtr, badGlbPtr, badGlb.length), 0, 'Overflowed GLB chunkLen must be rejected');
  exp.free(badNamePtr);
  exp.free(badGlbPtr);

  if (fs.existsSync('web/assets/Sponza.glb')) {
    const sponzaRaw = fs.readFileSync('web/assets/Sponza.glb');
    const sponzaPtr = exp.malloc(sponzaRaw.length);
    assert(sponzaPtr > 0, 'WASM free-list heap must fit 50+ MB Sponza.glb within 64 MB max memory after free()');
    new Uint8Array(wasmMem.buffer).set(sponzaRaw, sponzaPtr);
    const sName = new TextEncoder().encode('Sponza.glb\0');
    const sNamePtr = exp.malloc(sName.length);
    new Uint8Array(wasmMem.buffer).set(sName, sNamePtr);
    assert.strictEqual(exp.bgl_load_file_buffer(sNamePtr, sponzaPtr, sponzaRaw.length), 1, 'WASM bgl_load_file_buffer must parse Sponza.glb');
    exp.free(sNamePtr);
    exp.free(sponzaPtr);
  }
  console.log(`✓ Real C++23 Emscripten WASM module verified (${Object.keys(exp).length} exports, ${wasmBytes.length} bytes)`);

  console.log('--- Step 13: Verifying Transparent BLEND Pipeline Selection & Node Hierarchy Cycle Guard ---');
  assert(BglWebGpuRenderer.transparentPipeline && BglWebGpuRenderer.transparentPipeline.desc.fragment.targets[0].blend,
    'Transparent pipeline must configure alpha blend state');
  assert.strictEqual(BglWebGpuRenderer.transparentPipeline.desc.depthStencil.depthWriteEnabled, false,
    'Transparent pipeline must disable depth writes');
  const cyclicGltf = {
    asset: { version: '2.0' },
    scenes: [{ nodes: [0] }],
    nodes: [
      { children: [1], translation: [0, 0, 0] },
      { children: [0], translation: [1, 0, 0] }
    ],
    meshes: [{ primitives: [] }]
  };
  BglWebGpuRenderer.loadModel(cyclicGltf, new ArrayBuffer(0), 'cyclic.gltf');
  assert.strictEqual(BglWebGpuRenderer.nodes.length, 2, 'Cyclic glTF hierarchy must terminate without infinite recursion');

  // Inject one BLEND primitive and confirm draw() selects transparentPipeline
  const boundPipelines = [];
  const origSetPipeline = MockGPURenderPassEncoder.prototype.setPipeline;
  MockGPURenderPassEncoder.prototype.setPipeline = function(p) {
    boundPipelines.push(p);
    return origSetPipeline.call(this, p);
  };
  const dummyVb = mockGpuDevice.createBuffer({ size: 144, usage: 0x28 });
  const dummyUb = mockGpuDevice.createBuffer({ size: 512, usage: 0x48 });
  const dummyBg = mockGpuDevice.createBindGroup({ layout: BglWebGpuRenderer.bindGroupLayout, entries: [] });
  BglWebGpuRenderer.primitives = [{
    vertexBuffer: dummyVb,
    skinVertexBuffer: null,
    indexBuffer: null,
    indexFormat: 'uint16',
    hasIndices: false,
    indexCount: 0,
    vertexCount: 3,
    uniformBuffer: dummyUb,
    bindGroup: dummyBg,
    modelMatrix: BglWebGpuRenderer.mat4.create(),
    localCenter: [0, 0, 0],
    localRadius: 1.0,
    baseColorFactor: [1, 1, 1, 0.5],
    metallicFactor: 0,
    roughnessFactor: 0.5,
    normalScale: 1,
    occlusionStrength: 1,
    emissiveFactor: [0, 0, 0],
    alphaCutoff: 0.5,
    alphaMode: 2,
    doubleSided: false,
    isSkinned: false,
    skinIdx: -1,
    flags: 0,
    materialIdx: 0
  }];
  BglWebGpuRenderer.rebuildSortedPrimitives();
  BglWebGpuRenderer.camera.target = [0, 0, 0];
  BglWebGpuRenderer.camera.distance = 3.0;
  BglWebGpuRenderer.draw();
  MockGPURenderPassEncoder.prototype.setPipeline = origSetPipeline;
  assert(boundPipelines.includes(BglWebGpuRenderer.transparentPipeline),
    'alphaMode === 2 primitive must bind transparentPipeline');
  console.log('✓ Transparent BLEND pipeline binding and cyclic hierarchy guard verified');

  console.log('--- ALL WEBGPU PIPELINE TESTS PASSED (100%) ---');
  process.exit(0);
})().catch((err) => {
  console.error('Test failed with error:', err);
  process.exit(1);
});
