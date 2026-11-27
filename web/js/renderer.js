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

    mat4: globalThis.BglWebGpuMath.mat4,

    getShaderWgsl() {
      return globalThis.BglWebGpuShaders.pbrWgsl;
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
            if (typeof BglNotifier !== 'undefined' && BglNotifier.error) {
              BglNotifier.error('WebGPU Context Lost', `The GPU device was lost (${info.reason || 'unknown'}).`);
            }
            this.isActive = false;
            if (this._pendingMipmaps) this._pendingMipmaps.length = 0;
            this.clearResources();
          });
        }
        if (typeof this.device.addEventListener === 'function') {
          this.device.addEventListener('uncapturederror', (ev) => {
            const msg = ev && ev.error && ev.error.message ? ev.error.message : String(ev && ev.error ? ev.error : 'Uncaptured GPU error');
            BglLogger.error('WebGPU', msg);
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

        const transparentBlend = {
          color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
          alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' }
        };
        const createScenePipeline = (layout, vertexEntry, buffers, cullMode, isTransparent) => this.device.createRenderPipeline({
          layout,
          vertex: { module: shaderModule, entryPoint: vertexEntry, buffers },
          fragment: {
            module: shaderModule,
            entryPoint: 'fs_main',
            targets: [isTransparent ? { format: this.format, blend: transparentBlend } : { format: this.format }]
          },
          primitive: { topology: 'triangle-list', cullMode },
          depthStencil: {
            format: 'depth24plus',
            depthWriteEnabled: !isTransparent,
            depthCompare: 'less'
          }
        });

        this.pipeline = createScenePipeline(this.pipelineLayout, 'vs_main', [vertexBufferLayout], 'back', false);
        this.pipelineDoubleSided = createScenePipeline(this.pipelineLayout, 'vs_main', [vertexBufferLayout], 'none', false);
        this.skinnedPipeline = createScenePipeline(
          this.skinnedPipelineLayout, 'vs_main_skinned', [vertexBufferLayout, skinVertexBufferLayout], 'back', false
        );
        this.skinnedPipelineDoubleSided = createScenePipeline(
          this.skinnedPipelineLayout, 'vs_main_skinned', [vertexBufferLayout, skinVertexBufferLayout], 'none', false
        );
        this.transparentPipeline = createScenePipeline(this.pipelineLayout, 'vs_main', [vertexBufferLayout], 'back', true);
        this.transparentPipelineDoubleSided = createScenePipeline(
          this.pipelineLayout, 'vs_main', [vertexBufferLayout], 'none', true
        );
        this.transparentSkinnedPipeline = createScenePipeline(
          this.skinnedPipelineLayout, 'vs_main_skinned', [vertexBufferLayout, skinVertexBufferLayout], 'back', true
        );
        this.transparentSkinnedPipelineDoubleSided = createScenePipeline(
          this.skinnedPipelineLayout, 'vs_main_skinned', [vertexBufferLayout, skinVertexBufferLayout], 'none', true
        );

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
      if (this._eventsInitialized) return;
      this._eventsInitialized = true;
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

    computePositionBounds(posData) {
      let minX = Infinity, minY = Infinity, minZ = Infinity;
      let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
      for (let i = 0; i < posData.length; i += 3) {
        const x = posData[i], y = posData[i + 1], z = posData[i + 2];
        if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
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
      return { minX, minY, minZ, maxX, maxY, maxZ, extX, extY, extZ, localCenter, localRadius };
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
      const bounds = this.computePositionBounds(posData);
      const { minX, minY, minZ, extX, extY, extZ, localCenter, localRadius } = bounds;

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

    clearModelResources() {
      for (const prim of this.primitives) {
        if (prim.vertexBuffer && prim.vertexBuffer.destroy) prim.vertexBuffer.destroy();
        if (prim.skinVertexBuffer && prim.skinVertexBuffer.destroy) prim.skinVertexBuffer.destroy();
        if (prim.indexBuffer && prim.indexBuffer.destroy) prim.indexBuffer.destroy();
        if (prim.uniformBuffer && prim.uniformBuffer.destroy) prim.uniformBuffer.destroy();
      }
      for (const skin of this.skins) {
        if (skin.nodeBindings && typeof skin.nodeBindings.values === 'function') {
          for (const nb of skin.nodeBindings.values()) {
            if (nb.jointBuffer && nb.jointBuffer !== skin.jointBuffer && nb.jointBuffer.destroy) {
              nb.jointBuffer.destroy();
            }
          }
        }
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
      this.modelBounds = null;
    },

    clearResources() {
      this._loadGeneration = (this._loadGeneration || 0) + 1;
      this.clearModelResources();
      if (this.depthTexture && this.depthTexture.destroy) this.depthTexture.destroy();
      if (this.defaultWhiteTexture && this.defaultWhiteTexture.destroy) this.defaultWhiteTexture.destroy();
      if (this.defaultMrTexture && this.defaultMrTexture.destroy) this.defaultMrTexture.destroy();
      if (this.defaultNormalTexture && this.defaultNormalTexture.destroy) this.defaultNormalTexture.destroy();
      if (this.defaultBlackTexture && this.defaultBlackTexture.destroy) this.defaultBlackTexture.destroy();
      this.depthTexture = null;
      this.depthTextureView = null;
      this.defaultWhiteTexture = null;
      this.defaultWhiteTextureView = null;
      this.defaultMrTexture = null;
      this.defaultMrTextureView = null;
      this.defaultNormalTexture = null;
      this.defaultNormalTextureView = null;
      this.defaultBlackTexture = null;
      this.defaultBlackTextureView = null;
      this.defaultSampler = null;
      this.mipmapSampler = null;
      this.bindGroupLayout = null;
      this.pipelineLayout = null;
      this.skinBindGroupLayout = null;
      this.skinnedPipelineLayout = null;
      this.pipeline = null;
      this.pipelineDoubleSided = null;
      this.skinnedPipeline = null;
      this.skinnedPipelineDoubleSided = null;
      this.transparentPipeline = null;
      this.transparentPipelineDoubleSided = null;
      this.transparentSkinnedPipeline = null;
      this.transparentSkinnedPipelineDoubleSided = null;
      this.mipmapBindGroupLayout = null;
      this.mipmapPipelineLayout = null;
      this.mipmapPipeline = null;
      this.device = null;
      this.context = null;
      this.adapter = null;
      this.isActive = false;
    },

    decodeEmbeddedBuffers(gltf, maxBufferBytes) {
      const predecoded = (gltf && Array.isArray(gltf._bglEmbeddedBuffers)) ? gltf._bglEmbeddedBuffers : null;
      if (predecoded) {
        return predecoded.slice();
      }
      const embeddedBuffers = [];
      if (!gltf || !Array.isArray(gltf.buffers)) {
        return embeddedBuffers;
      }
      const maxBufs = Math.min(gltf.buffers.length, 64);
      let totalDecoded = 0;
      for (let i = 0; i < maxBufs; ++i) {
        const buf = gltf.buffers[i];
        if (!buf || typeof buf.uri !== 'string' || !/^data:/i.test(buf.uri)) continue;
        const commaIdx = buf.uri.indexOf(',');
        if (commaIdx === -1) continue;
        const meta = buf.uri.slice(5, commaIdx).toLowerCase();
        const payload = buf.uri.slice(commaIdx + 1);
        try {
          let bytes;
          if (meta.includes(';base64')) {
            const remainingBudget = maxBufferBytes - totalDecoded;
            if (payload.length > Math.ceil(remainingBudget * 4 / 3) + 4) {
              throw new Error('Encoded base64 embedded buffer exceeds size limit');
            }
            const binStr = atob(payload);
            if (binStr.length > remainingBudget) {
              throw new Error('Decoded embedded buffer exceeds size limit');
            }
            bytes = new Uint8Array(binStr.length);
            for (let b = 0; b < binStr.length; ++b) {
              bytes[b] = binStr.charCodeAt(b);
            }
          } else {
            bytes = new TextEncoder().encode(decodeURIComponent(payload));
            if (bytes.byteLength > maxBufferBytes || totalDecoded + bytes.byteLength > maxBufferBytes) {
              throw new Error('Decoded embedded buffer exceeds size limit');
            }
          }
          totalDecoded += bytes.byteLength;
          embeddedBuffers[i] = bytes.buffer;
        } catch (e) {
          BglLogger.warn('glTF', 'Failed to decode embedded buffer URI:', e);
        }
      }
      return embeddedBuffers;
    },

    loadModel(gltf, arrayBuffer, modelName) {
      if (!this.isActive || !this.device) return;
      const loadGen = (this._loadGeneration = (this._loadGeneration || 0) + 1);

      // Release previous model GPU resources to prevent VRAM memory leak or stale bounds
      this.clearModelResources();
      if (!gltf || typeof gltf !== 'object') return;

      const MAX_BUFFER_BYTES = 128 * 1024 * 1024;
      let binBuffer = null;
      if (arrayBuffer && arrayBuffer.byteLength >= 12) {
        const dv = new DataView(arrayBuffer, 0, 12);
        const magic = dv.getUint32(0, true);
        if (magic === 0x46546C67) {
          const version = dv.getUint32(4, true);
          const totalLen = dv.getUint32(8, true);
          const limit = (version === 2 && totalLen >= 12 && totalLen <= arrayBuffer.byteLength)
            ? totalLen
            : arrayBuffer.byteLength;
          let byteOffset = 12;
          while (byteOffset + 8 <= limit) {
            const chunkDv = new DataView(arrayBuffer, byteOffset, 8);
            const chunkLen = chunkDv.getUint32(0, true);
            const chunkType = chunkDv.getUint32(4, true);
            if (chunkLen > limit - byteOffset - 8) {
              BglLogger.warn('glTF', `Truncated GLB chunk (${chunkLen} bytes at offset ${byteOffset}).`);
              break;
            }
            if (chunkType === 0x004E4942) {
              binBuffer = arrayBuffer.slice(byteOffset + 8, byteOffset + 8 + chunkLen);
              break;
            }
            byteOffset += 8 + chunkLen;
          }
        }
      }

      const embeddedBuffers = this.decodeEmbeddedBuffers(gltf, MAX_BUFFER_BYTES);
      const hasExternalFirstBufUri = Array.isArray(gltf.buffers) && gltf.buffers[0] && typeof gltf.buffers[0].uri === 'string';

      const getBufferViewData = (viewIdx) => {
        if (!Number.isInteger(viewIdx) || viewIdx < 0 || !gltf.bufferViews || !gltf.bufferViews[viewIdx]) return null;
        const bv = gltf.bufferViews[viewIdx];
        const offset = bv.byteOffset === undefined ? 0 : bv.byteOffset;
        const length = bv.byteLength === undefined ? 0 : bv.byteLength;
        const bufferIdx = bv.buffer === undefined ? 0 : bv.buffer;
        if (!Number.isInteger(offset) || offset < 0 || !Number.isInteger(length) || length < 0 ||
            !Number.isInteger(bufferIdx) || bufferIdx < 0) {
          BglLogger.warn('glTF', `BufferView ${viewIdx} has invalid negative/non-integer bounds.`);
          return null;
        }
        let src = null;
        if (bufferIdx === 0) {
          if (binBuffer) src = binBuffer;
          else if (embeddedBuffers[0]) src = embeddedBuffers[0];
          else if (!hasExternalFirstBufUri) src = arrayBuffer;
        } else {
          src = embeddedBuffers[bufferIdx] || null;
        }
        if (!src || offset > src.byteLength || length > src.byteLength - offset) {
          BglLogger.warn('glTF', `BufferView ${viewIdx} out of bounds: offset ${offset} + length ${length} > ${src ? src.byteLength : 0}`);
          return null;
        }
        return { buffer: src, byteOffset: offset, byteLength: length };
      };

      const TYPE_COMPONENTS = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT2: 4, MAT3: 9, MAT4: 16 };
      const getAccessorData = (accIdx) => {
        if (!Number.isInteger(accIdx) || accIdx < 0 || !gltf.accessors || !gltf.accessors[accIdx]) return null;
        const acc = gltf.accessors[accIdx];
        if (!Number.isInteger(acc.count) || acc.count <= 0 || acc.count > 20000000) return null;
        if (acc.bufferView === undefined) return null;
        const bv = gltf.bufferViews ? gltf.bufferViews[acc.bufferView] : null;
        if (!bv) return null;
        const bvData = getBufferViewData(acc.bufferView);
        if (!bvData) return null;
        const byteOffset = acc.byteOffset === undefined ? 0 : acc.byteOffset;
        if (!Number.isInteger(byteOffset) || byteOffset < 0) return null;

        let TypedArray = Float32Array;
        if (acc.componentType === 5126) TypedArray = Float32Array;
        else if (acc.componentType === 5123) TypedArray = Uint16Array;
        else if (acc.componentType === 5125) TypedArray = Uint32Array;
        else if (acc.componentType === 5121) TypedArray = Uint8Array;
        else if (acc.componentType === 5122) TypedArray = Int16Array;
        else if (acc.componentType === 5120) TypedArray = Int8Array;
        else return null;

        const numComp = TYPE_COMPONENTS[acc.type];
        if (!numComp) return null;
        const elemBytes = TypedArray.BYTES_PER_ELEMENT;
        const minStride = numComp * elemBytes;
        const stride = bv.byteStride === undefined ? minStride : bv.byteStride;
        if (!Number.isInteger(stride) || stride < minStride) {
          BglLogger.warn('glTF', `Accessor ${accIdx} has invalid byteStride ${stride}.`);
          return null;
        }

        const requiredBytes = byteOffset + (acc.count - 1) * stride + minStride;
        if (!Number.isSafeInteger(requiredBytes) || requiredBytes > bvData.byteLength) {
          BglLogger.warn('glTF', `Accessor ${accIdx} exceeds buffer view byte length.`);
          return null;
        }

        const absByteOffset = bvData.byteOffset + byteOffset;
        if (stride === numComp * elemBytes && (absByteOffset % elemBytes === 0)) {
          return new TypedArray(bvData.buffer, absByteOffset, acc.count * numComp);
        }

        const totalElems = acc.count * numComp;
        const out = new TypedArray(totalElems);
        const dView = new DataView(bvData.buffer, bvData.byteOffset, bvData.byteLength);
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

      const getAccessorAsFloat32 = (accIdx, forceNormalize = false) => {
        const raw = getAccessorData(accIdx);
        if (!raw) return null;
        if (raw instanceof Float32Array) return raw;
        const acc = gltf.accessors[accIdx];
        const out = new Float32Array(raw.length);
        const isNorm = !!(acc.normalized || forceNormalize);
        if (isNorm && acc.componentType === 5121) { // UNSIGNED_BYTE
          for (let i = 0; i < raw.length; ++i) out[i] = raw[i] / 255.0;
        } else if (isNorm && acc.componentType === 5123) { // UNSIGNED_SHORT
          for (let i = 0; i < raw.length; ++i) out[i] = raw[i] / 65535.0;
        } else if (isNorm && acc.componentType === 5120) { // BYTE
          for (let i = 0; i < raw.length; ++i) out[i] = Math.max(raw[i] / 127.0, -1.0);
        } else if (isNorm && acc.componentType === 5122) { // SHORT
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
            translation: (Array.isArray(gn.translation) && gn.translation.length >= 3)
              ? [gn.translation[0], gn.translation[1], gn.translation[2]]
              : [0, 0, 0],
            rotation: (Array.isArray(gn.rotation) && gn.rotation.length >= 4)
              ? [gn.rotation[0], gn.rotation[1], gn.rotation[2], gn.rotation[3]]
              : [0, 0, 0, 1],
            scale: (Array.isArray(gn.scale) && gn.scale.length >= 3)
              ? [gn.scale[0], gn.scale[1], gn.scale[2]]
              : [1, 1, 1],
            matrix: (Array.isArray(gn.matrix) && gn.matrix.length === 16) ? new Float32Array(gn.matrix) : null,
            children: Array.isArray(gn.children) ? gn.children.slice() : [],
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

        const sceneIdx = Number.isInteger(gltf.scene) ? gltf.scene : 0;
        if (gltf.scenes && gltf.scenes[sceneIdx] && Array.isArray(gltf.scenes[sceneIdx].nodes)) {
          this.rootNodes = gltf.scenes[sceneIdx].nodes.slice();
        } else {
          const isChild = new Uint8Array(this.nodes.length);
          for (const node of this.nodes) {
            for (const cIdx of node.children) {
              if (Number.isInteger(cIdx) && cIdx >= 0 && cIdx < this.nodes.length) {
                isChild[cIdx] = 1;
              }
            }
          }
          for (let i = 0; i < this.nodes.length; ++i) {
            if (!isChild[i]) this.rootNodes.push(i);
          }
        }

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
            jointCount,
            inverseBindMatrices,
            jointMatricesData,
            jointBuffer,
            bindGroup,
            meshNodeIdx: -1,
            nodeBindings: new Map()
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
            if (prim.mode !== undefined && prim.mode !== 4) {
              BglLogger.warn('WebGPU', `Skipping non-triangle primitive mode ${prim.mode}.`);
              continue;
            }
            if (!prim.attributes || prim.attributes.POSITION === undefined) continue;

            const posData = getAccessorData(prim.attributes.POSITION);
            if (!posData || posData.length < 3 || posData.length % 3 !== 0) continue;

            const normData = prim.attributes.NORMAL !== undefined ? getAccessorData(prim.attributes.NORMAL) : null;
            const uvData = prim.attributes.TEXCOORD_0 !== undefined ? getAccessorData(prim.attributes.TEXCOORD_0) : null;
            const tanData = prim.attributes.TANGENT !== undefined ? getAccessorData(prim.attributes.TANGENT) : null;
            const jointsData = prim.attributes.JOINTS_0 !== undefined ? getAccessorData(prim.attributes.JOINTS_0) : null;
            const weightsData = prim.attributes.WEIGHTS_0 !== undefined ? getAccessorAsFloat32(prim.attributes.WEIGHTS_0, true) : null;
            let indexData = prim.indices !== undefined ? getAccessorData(prim.indices) : null;

            const vertexCount = Math.floor(posData.length / 3);
            if (indexData) {
              let hasOob = false;
              for (let k = 0; k < indexData.length; ++k) {
                if (indexData[k] >= vertexCount) {
                  hasOob = true;
                  break;
                }
              }
              if (hasOob) {
                BglLogger.warn('glTF', 'Clamping out-of-bounds primitive index entries.');
                const clamped = indexData instanceof Uint32Array ? new Uint32Array(indexData.length) : new Uint16Array(indexData.length);
                for (let k = 0; k < indexData.length; ++k) {
                  clamped[k] = indexData[k] < vertexCount ? indexData[k] : 0;
                }
                indexData = clamped;
              }
            }

            for (const nIdx of nodeIdxList) {
              const worldMatrix = (nIdx >= 0 && this.nodes[nIdx]) ? this.nodes[nIdx].worldMatrix : this.mat4.create();
              for (let i = 0; i < posData.length; i += 3) {
                const lx = posData[i], ly = posData[i + 1], lz = posData[i + 2];
                if (!Number.isFinite(lx) || !Number.isFinite(ly) || !Number.isFinite(lz)) continue;
                const wx = worldMatrix[0] * lx + worldMatrix[4] * ly + worldMatrix[8] * lz + worldMatrix[12];
                const wy = worldMatrix[1] * lx + worldMatrix[5] * ly + worldMatrix[9] * lz + worldMatrix[13];
                const wz = worldMatrix[2] * lx + worldMatrix[6] * ly + worldMatrix[10] * lz + worldMatrix[14];
                minBound[0] = Math.min(minBound[0], wx); maxBound[0] = Math.max(maxBound[0], wx);
                minBound[1] = Math.min(minBound[1], wy); maxBound[1] = Math.max(maxBound[1], wy);
                minBound[2] = Math.min(minBound[2], wz); maxBound[2] = Math.max(maxBound[2], wz);
              }
            }

            const hasNormals = normData && normData.length >= vertexCount * 3;
            const hasUvs = uvData && uvData.length >= vertexCount * 2;
            const hasTangents = tanData && tanData.length >= vertexCount * 4;
            const interleaved = new Float32Array(vertexCount * 12);
            for (let i = 0; i < vertexCount; ++i) {
              const base = i * 12;
              const pIdx = i * 3;
              interleaved[base + 0] = posData[pIdx + 0];
              interleaved[base + 1] = posData[pIdx + 1];
              interleaved[base + 2] = posData[pIdx + 2];

              if (hasNormals) {
                interleaved[base + 3] = normData[pIdx + 0];
                interleaved[base + 4] = normData[pIdx + 1];
                interleaved[base + 5] = normData[pIdx + 2];
              } else {
                interleaved[base + 3] = 0.0;
                interleaved[base + 4] = 1.0;
                interleaved[base + 5] = 0.0;
              }

              if (hasUvs) {
                const uIdx = i * 2;
                interleaved[base + 6] = uvData[uIdx + 0];
                interleaved[base + 7] = uvData[uIdx + 1];
              } else {
                interleaved[base + 6] = 0.0;
                interleaved[base + 7] = 0.0;
              }

              if (hasTangents) {
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
            if (indexData && indexData.length >= 3) {
              let validIndexData = indexData;
              let hasOobIndex = false;
              for (let k = 0; k < indexData.length; ++k) {
                if (indexData[k] >= vertexCount) {
                  hasOobIndex = true;
                  break;
                }
              }
              if (hasOobIndex) {
                BglLogger.warn('glTF', `Clamping out-of-bounds primitive index (>= ${vertexCount}).`);
                const cloned = new Uint32Array(indexData.length);
                for (let k = 0; k < indexData.length; ++k) {
                  cloned[k] = indexData[k] < vertexCount ? indexData[k] : 0;
                }
                validIndexData = cloned;
              }
              const lodInfo = this.generateLodData(posData, hasNormals ? normData : null, validIndexData, vertexCount);
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
            } else {
              const unindexedBounds = this.computePositionBounds(posData);
              localCenter = unindexedBounds.localCenter;
              localRadius = unindexedBounds.localRadius;
            }
            const hasValidIndices = !!(indexBuffer && indexCount >= 3);

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
              let skinBindGroup = null;
              if (isSkinned) {
                const skin = this.skins[skinIdx];
                if (skin.meshNodeIdx < 0) {
                  skin.meshNodeIdx = nIdx;
                }
                if (skin.nodeBindings.has(nIdx)) {
                  skinBindGroup = skin.nodeBindings.get(nIdx).bindGroup;
                } else if (skin.nodeBindings.size === 0) {
                  skin.nodeBindings.set(nIdx, {
                    jointBuffer: skin.jointBuffer,
                    bindGroup: skin.bindGroup,
                    jointMatricesData: skin.jointMatricesData
                  });
                  skinBindGroup = skin.bindGroup;
                } else {
                  const jCount = skin.jointCount || Math.max(1, skin.joints.length);
                  const nodeJointData = new Float32Array(jCount * 16);
                  nodeJointData.set(skin.jointMatricesData);
                  const nodeJointBuf = this.device.createBuffer({
                    size: Math.max(64, jCount * 64),
                    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST
                  });
                  this.device.queue.writeBuffer(nodeJointBuf, 0, nodeJointData);
                  const nodeBg = this.device.createBindGroup({
                    layout: this.skinBindGroupLayout,
                    entries: [{ binding: 0, resource: { buffer: nodeJointBuf } }]
                  });
                  skin.nodeBindings.set(nIdx, {
                    jointBuffer: nodeJointBuf,
                    bindGroup: nodeBg,
                    jointMatricesData: nodeJointData
                  });
                  skinBindGroup = nodeBg;
                }
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
                hasIndices: hasValidIndices,
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
                skinBindGroup,
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
                    if (!this.device || this._loadGeneration !== loadGen) {
                      if (typeof bmp.close === 'function') bmp.close();
                      this.textureCache.delete(imgIdx);
                      return;
                    }
                    const maxDim = (this.device.limits && this.device.limits.maxTextureDimension2D) ? this.device.limits.maxTextureDimension2D : 8192;
                    if (!bmp || bmp.width <= 0 || bmp.height <= 0 || bmp.width > maxDim || bmp.height > maxDim) {
                      BglLogger.warn('WebGPU', `Rejected texture #${imgIdx} with invalid dimensions ${bmp ? bmp.width : 0}x${bmp ? bmp.height : 0}.`);
                      if (bmp && typeof bmp.close === 'function') bmp.close();
                      this.textureCache.delete(imgIdx);
                      return;
                    }
                    const mipLevelCount = Math.max(1, Math.floor(Math.log2(Math.max(bmp.width, bmp.height))) + 1);
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
                    this.textureCache.delete(imgIdx);
                    BglLogger.warn('WebGPU', `Failed to upload texture image #${imgIdx}:`, texErr);
                  }
                };

                const decodeBitmap = (srcObj) => {
                  if (this._loadGeneration !== loadGen) return;
                  const opts = { colorSpaceConversion: 'none' };
                  createImageBitmap(srcObj, opts).then(onImageReady).catch(() => {
                    if (this._loadGeneration !== loadGen) return;
                    createImageBitmap(srcObj).then(onImageReady).catch((err) => {
                      this.textureCache.delete(imgIdx);
                      BglLogger.warn('WebGPU', `createImageBitmap failed for image #${imgIdx}:`, err);
                    });
                  });
                };

                if (imgObj.bufferView !== undefined) {
                  const imgView = getBufferViewData(imgObj.bufferView);
                  if (imgView) {
                    const allowedMimes = new Set(['image/png', 'image/jpeg', 'image/webp']);
                    const safeMime = (imgObj.mimeType && allowedMimes.has(imgObj.mimeType)) ? imgObj.mimeType : 'image/png';
                    const blob = new Blob([new Uint8Array(imgView.buffer, imgView.byteOffset, imgView.byteLength)], {
                      type: safeMime
                    });
                    decodeBitmap(blob);
                  } else {
                    this.textureCache.delete(imgIdx);
                  }
                } else if (typeof imgObj.uri === 'string') {
                  const uriCheck = (globalThis.BglNetwork && globalThis.BglNetwork.validateAssetUrl)
                    ? globalThis.BglNetwork.validateAssetUrl(imgObj.uri, { allowData: true, allowCustomHttps: true })
                    : { ok: /^(?:https:\/\/|assets\/|data:image\/(?:png|jpeg|webp)[;,])/i.test(imgObj.uri.trim()), url: imgObj.uri.trim() };
                  if (!uriCheck.ok) {
                    this.textureCache.delete(imgIdx);
                    BglLogger.warn('WebGPU', `Blocked unsafe external texture URI #${imgIdx}: ${uriCheck.reason || 'Disallowed URI'}`);
                  } else {
                    const safeTexUrl = uriCheck.url;
                    const img = new Image();
                    img.crossOrigin = 'anonymous';
                    img.referrerPolicy = 'no-referrer';
                    img.onload = () => {
                      decodeBitmap(img);
                    };
                    img.onerror = () => {
                      this.textureCache.delete(imgIdx);
                      BglLogger.warn('WebGPU', `Failed to load external texture image #${imgIdx}: ${safeTexUrl}`);
                    };
                    img.src = safeTexUrl;
                  }
                } else {
                  this.textureCache.delete(imgIdx);
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
      const nodeCount = this.nodes.length;
      if (nodeCount === 0) return;
      if (!this._hierarchyVisited || this._hierarchyVisited.length < nodeCount) {
        this._hierarchyVisited = new Uint8Array(nodeCount);
      } else {
        this._hierarchyVisited.fill(0, 0, nodeCount);
      }
      if (!this._hierarchyStack) this._hierarchyStack = [];
      const visited = this._hierarchyVisited;
      const stack = this._hierarchyStack;

      for (let r = this.rootNodes.length - 1; r >= 0; --r) {
        stack.push({ idx: this.rootNodes[r], parentWorld: null });
      }
      while (stack.length > 0) {
        const frame = stack.pop();
        const nodeIdx = frame.idx;
        if (!Number.isInteger(nodeIdx) || nodeIdx < 0 || nodeIdx >= nodeCount || visited[nodeIdx]) {
          continue;
        }
        const node = this.nodes[nodeIdx];
        if (!node) continue;
        visited[nodeIdx] = 1;
        if (node.matrix) {
          node.localMatrix.set(node.matrix);
        } else {
          this.mat4.fromRotationTranslationScale(node.localMatrix, node.rotation, node.translation, node.scale);
        }
        if (frame.parentWorld) {
          this.mat4.multiply(node.worldMatrix, frame.parentWorld, node.localMatrix);
        } else {
          node.worldMatrix.set(node.localMatrix);
        }
        if (Array.isArray(node.children)) {
          for (let i = node.children.length - 1; i >= 0; --i) {
            const cIdx = node.children[i];
            if (Number.isInteger(cIdx) && cIdx >= 0 && cIdx < nodeCount && !visited[cIdx]) {
              stack.push({ idx: cIdx, parentWorld: node.worldMatrix });
            }
          }
        }
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
        const bindings = (skin.nodeBindings && skin.nodeBindings.size > 0)
          ? skin.nodeBindings.entries()
          : [[skin.meshNodeIdx, { jointBuffer: skin.jointBuffer, jointMatricesData: skin.jointMatricesData }]];
        for (const [mIdx, binding] of bindings) {
          const meshNode = (mIdx >= 0 && this.nodes[mIdx]) ? this.nodes[mIdx] : null;
          if (meshNode) {
            this.mat4.invert(invMeshWorld, meshNode.worldMatrix);
          } else {
            this.mat4.identity(invMeshWorld);
          }
          const out = binding.jointMatricesData;
          for (let j = 0; j < skin.joints.length; ++j) {
            const jNode = this.nodes[skin.joints[j]];
            if (jNode) {
              const ibm = skin.inverseBindMatrices.subarray(j * 16, j * 16 + 16);
              this.mat4.multiply(jointMat, jNode.worldMatrix, ibm);
              this.mat4.multiply(out.subarray(j * 16, j * 16 + 16), invMeshWorld, jointMat);
            }
          }
          this.device.queue.writeBuffer(binding.jointBuffer, 0, out);
        }
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
      const dt = this.lastFrameTime ? Math.max(0.0, Math.min((time - this.lastFrameTime) * 0.001, 0.1)) : (1.0 / 60.0);
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
        const isTransparent = prim.alphaMode === 2;
        const pipeline = isSkinnedDraw
          ? (isTransparent
              ? (prim.doubleSided ? this.transparentSkinnedPipelineDoubleSided : this.transparentSkinnedPipeline)
              : (prim.doubleSided ? this.skinnedPipelineDoubleSided : this.skinnedPipeline))
          : (isTransparent
              ? (prim.doubleSided ? this.transparentPipelineDoubleSided : this.transparentPipeline)
              : (prim.doubleSided ? this.pipelineDoubleSided : this.pipeline));

        if (pipeline !== lastPipeline) {
          pass.setPipeline(pipeline);
          lastPipeline = pipeline;
        }
        if (prim.bindGroup !== lastBg0) {
          pass.setBindGroup(0, prim.bindGroup);
          lastBg0 = prim.bindGroup;
        }
        if (isSkinnedDraw) {
          const skinBg = prim.skinBindGroup || this.skins[prim.skinIdx].bindGroup;
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
        if (shouldUpdateDom) {
          const elLod = (uiCache && uiCache.elLodTelemetry) || document.getElementById('lod-telemetry');
          if (elLod) {
            if (frameTotalTris > 0) {
              const savedPct = Math.max(0, Math.round(((frameTotalTris - frameRenderedTris) / frameTotalTris) * 100));
              elLod.textContent = `${frameRenderedTris.toLocaleString()} / ${frameTotalTris.toLocaleString()} Tris (-${savedPct}%) [L0:${frameLodCounts[0]} L1:${frameLodCounts[1]} L2:${frameLodCounts[2]} L3:${frameLodCounts[3]}]`;
            } else {
              elLod.textContent = '--';
            }
          }
        }
      }

      pass.end();
      this.device.queue.submit([commandEncoder.finish()]);
    }
  };


  globalThis.BglWebGpuRenderer = BglWebGpuRenderer;
})();
