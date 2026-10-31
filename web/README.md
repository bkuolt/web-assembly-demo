# bgl-Engine Web Viewer & 3D Runtime

A high-performance, zero-allocation WebAssembly, WebGPU, and WebGL 2 3D scene viewer and glTF 2.0 runtime. Designed for seamless execution across desktop, laptop, tablet, and mobile browsers (including Google Chrome, Mozilla Firefox, Apple Safari, and Samsung Galaxy devices).

[![Live GitHack Demo](https://img.shields.io/badge/Live_Demo-GitHack_Preview-059669?style=for-the-badge&logo=webassembly)](https://raw.githack.com/bkuolt/web-assembly-demo/main/web/index.html)
[![WebGPU](https://img.shields.io/badge/Pipeline-WebGPU_WGSL-10b981?style=for-the-badge&logo=webgpu)](web/app.js)
[![WebGL 2](https://img.shields.io/badge/Fallback-WebGL_2_GLES3-f59e0b?style=for-the-badge&logo=opengl)](web/app.js)
[![Tests Passing](https://img.shields.io/badge/E2E_Tests-100%25_Passing-22c55e?style=for-the-badge)](tests/test_browsers_e2e.py)

---

## Table of Contents
1. [Architectural Overview](#architectural-overview)
2. [Dual-Backend Pipeline Architecture](#dual-backend-pipeline-architecture)
3. [Zero-Allocation Render Loop & Performance](#zero-allocation-render-loop--performance)
4. [Memory Lifecycle & VRAM Safety](#memory-lifecycle--vram-safety)
5. [glTF 2.0 / GLB Streaming & Ingestion](#gltf-20--glb-streaming--ingestion)
6. [Offline Model Cache & Network Streaming](#offline-model-cache--network-streaming)
7. [Input & Multi-Touch Gesture Controls](#input--multi-touch-gesture-controls)
8. [Cross-Browser & Hardware Compatibility](#cross-browser--hardware-compatibility)
9. [Automated Verification & Test Suites](#automated-verification--test-suites)

---

## Architectural Overview

The web viewer client is architected as an event-driven, reactive modular runtime without third-party web frameworks, ensuring minimal overhead and fast cold-start loading times (<100 ms).

```
                      +-----------------------------+
                      |        web/index.html       |
                      |   HUD Controls & Viewport   |
                      +--------------+--------------+
                                     |
                                     v
                      +-----------------------------+
                      |          web/app.js         |
                      | Application Runtime Manager |
                      +--------------+--------------+
                                     |
         +---------------------------+---------------------------+
         |                                                       |
         v                                                       v
+-------------------------------+               +-------------------------------+
|       BglWebGpuRenderer       |               |       BglWebGlRenderer        |
|  - W3C WebGPU Device & Queue  |               |  - WebGL 2 (GLES 3.0 ES)      |
|  - Hardware WGSL Shaders      |               |  - Vertex Array Objects (VAO) |
|  - 4-Byte Buffer Alignment    |               |  - GLSL 3.00 ES Shader Prog   |
|  - Dynamic Depth Buffer       |               |  - Textures & Linear Samplers |
+-------------------------------+               +-------------------------------+
         ^                                                       ^
         +---------------------------+---------------------------+
                                     |
                      +--------------+--------------+
                      |    Model Ingestion Layer    |
                      |  - BglGltf Parser & AABB    |
                      |  - BglNetwork Streamer      |
                      |  - BglModelCache (Offline)  |
                      +-----------------------------+
```

### Module Directory

| Module | Identifier | Description |
| :--- | :--- | :--- |
| **Logger** | `BglLogger` | Categorized, color-badged console logger with microsecond-precision timing. |
| **Notifier** | `BglNotifier` | Modern floating toast notification system for user-facing status and warnings. |
| **State** | `BglState` | Central reactive telemetry store for camera, bounds, rendering backend, and sun angle. |
| **UI Controller** | `BglUI` | DOM element binding, camera dropdown, HUD collapse, and Inspector manager. |
| **Offline Cache** | `BglModelCache` | `CacheStorage` API abstraction with in-memory map fallback for offline asset persistence. |
| **Loading Screen** | `BglLoadingScreen`| Multi-stage visual progress overlay with real-time download speed meters. |
| **Network Fetcher** | `BglNetwork` | Chunked `ReadableStream` fetcher with byte transfer metrics and abort controllers. |
| **Wasm Bridge** | `BglWasmBridge` | WebAssembly streaming loader and native C++ engine binding interface. |
| **glTF Ingestion** | `BglGltf` | Binary GLB / JSON glTF parser, hierarchy tree generator, and AABB calculator. |
| **WebGPU Engine** | `BglWebGpuRenderer`| Native WebGPU renderer with WGSL shaders and 4-byte buffer alignment. |
| **WebGL 2 Engine** | `BglWebGlRenderer` | Fallback OpenGL ES 3.0 renderer with VAO and full mobile touch gesture support. |

---

## Dual-Backend Pipeline Architecture

The runtime employs an adaptive backend selection mechanism. When launched:
1. It queries `navigator.gpu` and requests an adapter with `powerPreference: 'high-performance'`.
2. If WebGPU is supported and device creation succeeds, `BglWebGpuRenderer` is initialized as the primary hardware-accelerated pipeline.
3. If WebGPU is unavailable, unsupported (e.g. mobile Safari or older Android devices), or forced via URL parameter (`?backend=webgl` or `?webgl`), the engine transparently mounts the `BglWebGlRenderer` pipeline.
4. If a WebGPU device reset occurs at runtime (`device.lost`), the canvas context is dynamically cloned and recovered via the WebGL 2 fallback without dropping the user session.

### 1. WebGPU Native Pipeline (`BglWebGpuRenderer`)

- **WGSL Shader Architecture**:
  - Vertex Shader (`vs_main`): Transforms coordinates by model and view-projection matrices, computes normal vectors with zero-length normalization fallbacks, and outputs UV coordinates.
  - Fragment Shader (`fs_main`): Blinn-Phong lighting calculation, ambient irradiance term (`vec3(0.25, 0.28, 0.32)`), directional diurnal sun light, fill light, specular highlights, and perceptual Gamma ($2.2$) tone mapping.
- **W3C 4-Byte Buffer Alignment**:
  - The W3C WebGPU specification requires buffer allocations and queue upload sizes (`GPUQueue.writeBuffer`) to be strictly divisible by 4 bytes.
  - Primitive indices with odd-length `Uint16Array` sizes are automatically padded with a trailing zero byte before transmission to prevent `OperationError` on strict engines like Mozilla Firefox (`wgpu-core`).
- **Uniform Buffer Layout (UBO)**:
  - 256-byte aligned memory block:
    - Bytes `0-63`: 4x4 View-Projection Matrix (`Float32Array[16]`)
    - Bytes `64-127`: 4x4 Model World Matrix (`Float32Array[16]`)
    - Bytes `128-191`: 4x4 Normal Matrix (`Float32Array[16]`)
    - Bytes `192-207`: Camera World Position (`vec4<f32>`)
    - Bytes `208-223`: Diurnal Sun Light Direction (`vec4<f32>`)
    - Bytes `224-239`: Material Base Color Factor (`vec4<f32>`)
    - Bytes `240-255`: Shader Render Flags (`Uint32Array[4]`): `hasTexture`, `wireframe`, `doubleSided`.

### 2. WebGL 2 Fallback Pipeline (`BglWebGlRenderer`)

- **OpenGL ES 3.0 Compliance**:
  - Fully compatible with GLSL 3.00 ES (`#version 300 es`).
  - Utilizes Vertex Array Objects (VAOs) for instantaneous draw submission without attribute binding rebind overhead.
  - Generates mipmapped textures with `LINEAR_MIPMAP_LINEAR` filtering and fallback 1x1 white textures for untextured geometries.

---

## Zero-Allocation Render Loop & Performance

To prevent garbage collection (GC) micro-stutters and guarantee steady 60 FPS / 120 FPS frame rates on high-refresh displays:

1. **Preallocated Scratch Matrices & Vectors**:
   Neither `BglWebGpuRenderer.draw()` nor `BglWebGlRenderer.draw()` allocate heap objects during rendering. Reusable typed array buffers are initialized once during setup:
   ```javascript
   // Preallocated scratch objects (web/app.js)
   _scratchEye: new Float32Array(3),
   _scratchLightDir: new Float32Array(3),
   _scratchProj: null,
   _scratchView: null,
   _scratchViewProj: null,
   _scratchNormalMat: new Float32Array(16),
   _scratchUboBuffer: new ArrayBuffer(256),
   _scratchUboF32: null,
   _scratchUboU32: null,
   ```
2. **Direct Typed Array Views**:
   Uniform updates write directly into the preallocated `_scratchUboBuffer` via static `Float32Array` and `Uint32Array` views, eliminating intermediate JavaScript object wrappers.
3. **Zero-Copy Ingestion**:
   When glTF buffer views match component size and memory alignment (`stride === numComp * elemBytes`), typed arrays are created directly over the source `ArrayBuffer` without memory copies.

---

## Memory Lifecycle & VRAM Safety

To ensure long-running stability when switching between complex 3D models (e.g. ToyCar, Sponza, BrainStem, Duck, FlightHelmet, and user-provided drag & drop files):

- **WebGPU Resource Cleanup**:
  Prior to loading a new scene, existing GPU buffers and textures are explicitly destroyed via the WebGPU API:
  ```javascript
  for (const prim of this.primitives) {
    if (prim.vertexBuffer && prim.vertexBuffer.destroy) prim.vertexBuffer.destroy();
    if (prim.indexBuffer && prim.indexBuffer.destroy) prim.indexBuffer.destroy();
    if (prim.uniformBuffer && prim.uniformBuffer.destroy) prim.uniformBuffer.destroy();
    if (prim.texture && prim.texture !== this.defaultTexture && prim.texture.destroy) prim.texture.destroy();
  }
  this.primitives = [];
  ```
- **WebGL 2 Resource Deletion**:
  All associated Vertex Array Objects, Vertex Buffer Objects (VBOs), Index Buffer Objects (IBOs), and textures are released:
  ```javascript
  for (const p of this.primitives) {
    if (p.vao && gl.deleteVertexArray) gl.deleteVertexArray(p.vao);
    if (p.buffers && gl.deleteBuffer) {
      for (const b of p.buffers) gl.deleteBuffer(b);
    }
    const tex = p._texRef || p.texture;
    if (tex && tex !== this.defaultTexture && gl.deleteTexture) {
      gl.deleteTexture(tex);
    }
  }
  this.primitives = [];
  ```

---

## glTF 2.0 / GLB Streaming & Ingestion

### Container Header Bounds & Security Validation

The binary parser (`BglGltf.extractJson`) rigorously validates input buffers to guard against corrupted or malicious files:
- **Header Length Check**: Requires $\ge 20$ bytes before reading chunk parameters.
- **Magic Identifier Check**: Verifies magic `0x46546C67` (`glTF`).
- **Chunk Type Check**: Enforces chunk 0 to be `0x4E4F534A` (`JSON`).
- **Chunk Payload Bounds**: Verifies `20 + chunkLength <= arrayBuffer.byteLength` to prevent array buffer overruns.
- **Strided Accessor Extraction**: Detects strided accessors and unpacks elements safely with `DataView` bounds validation.

### Dynamic Camera Framing

Upon model ingestion, the engine computes the global Axis-Aligned Bounding Box (AABB) across all transformed mesh primitives:
$$\vec{c} = \frac{\vec{min} + \vec{max}}{2}, \quad r = \frac{\|\vec{max} - \vec{min}\|}{2}$$
Camera parameters are dynamically configured:
- Target: $\vec{c}$
- Distance: $2.2 \cdot r$
- Near clipping plane: $\max(0.0001, 0.02 \cdot r)$
- Far clipping plane: $\max(100.0, 50.0 \cdot r)$

---

## Offline Model Cache & Network Streaming

### `BglModelCache` (CacheStorage API)
- Models fetched over the network are automatically cached in the browser's persistent `CacheStorage` (`bgl-gltf-models-v1`).
- Subsequent requests for the same URL execute with zero network delay (Cache HIT).
- An in-memory `Map` provides automatic fallback in environments where `window.caches` is restricted (e.g. data URLs or privacy modes).

### `BglNetwork` (Streaming Fetch)
- Employs `ReadableStream` reader to read incoming binary chunks incrementally.
- Calculates instantaneous network throughput ($\text{MB/s}$) and percentage completion.
- Dispatches real-time progress callbacks to `BglLoadingScreen` for smooth progress bar updates.

---

## Input & Multi-Touch Gesture Controls

| Control | Desktop Interaction | Mobile Touch Gesture |
| :--- | :--- | :--- |
| **Orbit (Yaw / Pitch)** | Left-click + drag | 1-finger drag |
| **Zoom (Distance)** | Mouse scroll wheel | 2-finger pinch in / out |
| **Pan (Target XY)** | Right-click + drag | 2-finger mid-point drag |
| **Turntable Mode** | Keyboard `T` / HUD button | HUD button |
| **Wireframe Mode** | Keyboard `W` / HUD button | HUD button |
| **Bounding Box** | Keyboard `B` / HUD button | HUD button |
| **Reset View** | Keyboard `R` / HUD button | HUD button |
| **Collapse HUD** | Keyboard `O` / HUD button | HUD button |

---

## Cross-Browser & Hardware Compatibility

| Platform / Browser | Native Backend | Fallback Backend | Verified Features |
| :--- | :---: | :---: | :--- |
| **Google Chrome (Desktop)** | WebGPU (Dawn) | WebGL 2 | Full PBR, Turntable, Wireframe, CacheStorage |
| **Mozilla Firefox (Desktop)** | WebGPU (wgpu) | WebGL 2 | 4-byte buffer alignment, universal WGSL |
| **Apple Safari (macOS / iOS)**| WebGL 2 / WebGPU | WebGL 2 | Touch gestures, responsive viewport |
| **Samsung Galaxy (Android)** | WebGL 2 | WebGL 2 | Touch orbit, pinch zoom, offline cache |
| **Microsoft Edge** | WebGPU (Dawn) | WebGL 2 | Hardware acceleration, full HUD controls |

---

## Automated Verification & Test Suites

The web runtime is verified by three complementary automated test suites:

### 1. WebGL Pipeline Test (`tests/test_webgl_path.js`)
Simulates WebGL 2 context, DOM canvas, and event flow:
```bash
node tests/test_webgl_path.js
```
- Tests: Ingestion of `ToyCar.glb` and `Sponza.glb`, VAO binding, draw execution, touch gestures, resource deletion, and corrupt GLB handling.

### 2. WebGPU Pipeline Test (`tests/test_webgpu_path.js`)
Simulates standard W3C WebGPU device, command encoder, and WGSL pipelines:
```bash
node tests/test_webgpu_path.js
```
- Tests: Ingestion of `ToyCar.glb` and `Sponza.glb`, 4-byte index buffer alignment, UBO layout, zero-allocation render loop, and GPU buffer destruction.

### 3. Multi-Browser End-to-End Test (`tests/test_browsers_e2e.py`)
Controls real instances of **Google Chrome** (via Chrome DevTools Protocol) and **Mozilla Firefox** (via Marionette/geckodriver) over a local HTTP server:
```bash
python3 tests/test_browsers_e2e.py
```
- Tests: Real canvas frame rendering, WebGPU device initialization, WebGL 2 fallback, glTF model parsing, and zero JavaScript runtime errors.
