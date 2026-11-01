# bgl-Engine Web Viewer & 3D Runtime

A high-performance, zero-allocation native WebGPU 3D scene viewer and glTF 2.0 Physically Based Rendering (PBR) runtime. Designed for seamless, photorealistic execution across modern WebGPU-enabled browsers (including Google Chrome, Microsoft Edge, and Mozilla Firefox).

[![Live GitHack Demo](https://img.shields.io/badge/Live_Demo-GitHack_Preview-059669?style=for-the-badge&logo=webassembly)](https://raw.githack.com/bkuolt/web-assembly-demo/main/web/index.html)
[![WebGPU](https://img.shields.io/badge/Pipeline-WebGPU_WGSL_PBR-10b981?style=for-the-badge&logo=webgpu)](web/app.js)
[![PBR](https://img.shields.io/badge/Shading-Cook--Torrance_GGX-blue?style=for-the-badge)](web/app.js)
[![Tests Passing](https://img.shields.io/badge/E2E_Tests-100%25_Passing-22c55e?style=for-the-badge)](tests/test_browsers_e2e.py)

---

## Table of Contents
1. [Architectural Overview](#architectural-overview)
2. [Physically Based Rendering (PBR) Shader Pipeline](#physically-based-rendering-pbr-shader-pipeline)
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
                                     v
                      +-----------------------------+
                      |      BglWebGpuRenderer      |
                      | - W3C WebGPU Device & Queue |
                      | - WGSL Cook-Torrance PBR    |
                      | - 7-Slot Texture Bind Groups|
                      | - 4-Byte Buffer Alignment   |
                      | - Zero-Allocation Loop      |
                      +--------------+--------------+
                                     ^
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
| **State** | `BglState` | Central reactive telemetry store for camera, bounds, rendering backend, and diurnal sun angle. |
| **UI Controller** | `BglUI` | DOM element binding, camera dropdown, HUD collapse, and glTF Inspector manager. |
| **Offline Cache** | `BglModelCache` | `CacheStorage` API abstraction with in-memory map fallback for offline asset persistence. |
| **Loading Screen** | `BglLoadingScreen`| Multi-stage visual progress overlay with real-time download speed meters. |
| **Network Fetcher** | `BglNetwork` | Chunked `ReadableStream` fetcher with byte transfer metrics and abort controllers. |
| **Wasm Bridge** | `BglWasmBridge` | WebAssembly streaming loader and native C++ engine binding interface. |
| **glTF Ingestion** | `BglGltf` | Binary GLB / JSON glTF parser, hierarchy tree generator, and AABB calculator. |
| **WebGPU Engine** | `BglWebGpuRenderer`| Native hardware-accelerated WebGPU PBR renderer with WGSL shaders and 4-byte buffer alignment. |

---

## Physically Based Rendering (PBR) Shader Pipeline

The renderer implements a fully conformant **glTF 2.0 Metallic-Roughness PBR pipeline** written in WebGPU Shading Language (WGSL):

### 1. Microfacet Specular BRDF (Cook-Torrance)
- **Normal Distribution Function ($D$)**: Trowbridge-Reitz GGX:
  $$D(\vec{h}) = \frac{\alpha^2}{\pi ((\vec{n}\cdot\vec{h})^2(\alpha^2 - 1) + 1)^2}, \quad \alpha = \text{roughness}^2$$
- **Masking-Shadowing Function ($Vis = \frac{G}{4(\vec{n}\cdot\vec{l})(\vec{n}\cdot\vec{v})}$)**: Smith Joint Masking-Shadowing:
  $$Vis = \frac{1}{\max(\text{ggxL} \cdot \text{ggxV}, 10^{-4})}, \quad \text{ggxX} = (\vec{n}\cdot\vec{x}) + \sqrt{\alpha^2 + (1 - \alpha^2)(\vec{n}\cdot\vec{x})^2}$$
- **Fresnel Reflectance ($F$)**: Schlick Fresnel approximation:
  $$F = F_0 + (1 - F_0)(1 - \vec{v}\cdot\vec{h})^5$$
  - Dielectric base reflectance: $F_0 = 0.04$
  - Metallic reflectance: $F_0 = \text{mix}(0.04, \text{baseColor.rgb}, \text{metallic})$
- **Energy Conservation**:
  $$k_S = F, \quad k_D = (1 - k_S)(1 - \text{metallic})$$
  $$\text{diffuseBRDF} = k_D \cdot \frac{\text{diffuseColor}}{\pi}$$

### 2. Multi-Texture PBR Bind Group (7 Bindings)
Each primitive binds a 7-resource bind group:
1. `Binding 0`: Uniform Buffer (512-byte aligned UBO).
2. `Binding 1`: Linear filtering sampler (`GPUFilterMode.linear`).
3. `Binding 2`: `baseColorTexture` (decoded to linear space via $2.2$ gamma exponent).
4. `Binding 3`: `metallicRoughnessTexture` (glTF 2.0 standard: green = roughness, blue = metallic).
5. `Binding 4`: `normalTexture` (screen-space cotangent/MikkTSpace frame normal mapping).
6. `Binding 5`: `emissiveTexture` (emissive surfaces with HDR contribution).
7. `Binding 6`: `occlusionTexture` (red = ambient occlusion cavity map).

### 3. Tactile 3D Bump Mapping & Parallax Occlusion Mapping (POM)
To achieve true sculptural depth and plastic relief on flat surfaces:
- **Analytic Cotangent Frame ($\text{TBN}$)**:
  Solves the $2\times 2$ surface gradient system directly from screen-space derivatives:
  $$\begin{pmatrix} duv_{1,x} & duv_{1,y} \\ duv_{2,x} & duv_{2,y} \end{pmatrix} \begin{pmatrix} \vec{T} \\ \vec{B} \end{pmatrix} = \begin{pmatrix} \vec{dp}_1 \\ \vec{dp}_2 \end{pmatrix}$$
  $$\det = duv_{1,x} \cdot duv_{2,y} - duv_{1,y} \cdot duv_{2,x}$$
  $$\vec{T} = \frac{1}{\det}(\vec{dp}_1 \cdot duv_{2,y} - \vec{dp}_2 \cdot duv_{1,y}), \quad \vec{B} = \frac{1}{\det}(\vec{dp}_2 \cdot duv_{1,x} - \vec{dp}_1 \cdot duv_{2,x})$$
  Orthogonalized via Gram-Schmidt against $\vec{N}_{\text{geom}}$ with handedness preservation.
- **glTF 2.0 Tangent-Space Alignment & Relief Scaling**:
  Corrects tangent space orientation ($+X = \text{right}, +Y = \text{up} = -\vec{B}$ in texture space) and scales by dynamic `bumpScale` ($0.0\times$ to $3.0\times$, default $1.5\times$):
  $$\vec{N}_{\text{tangent}} = \text{normalize}\left(\text{sample}_x \cdot s, \; -\text{sample}_y \cdot s, \; \text{sample}_z\right), \quad s = \text{normalScale} \cdot \text{bumpScale}$$
- **Steep Parallax Occlusion Mapping (POM) & UV Displacements**:
  Raymarches adaptive surface height layers ($8$ to $32$ layers depending on camera grazing angle) along the tangent-space view vector $\vec{V}_{\text{tangent}}$, shifting texture coordinates to reveal true 3D geometric depth.
- **Parallax Contact Self-Shadowing**:
  Raymarches from the displaced surface point along the tangent-space sun light vector $\vec{L}_{\text{tangent}}$, casting crisp contact micro-shadows into surface crevices.
- **Luminance & Cavity Surface Gradient Bump Fallback**:
  For glTF models without dedicated normal maps, derives spatial surface gradients $\nabla H = \left(\frac{\partial H}{\partial u}, \frac{\partial H}{\partial v}\right)$ from base color luminance and ambient occlusion cavity textures, providing tactile surface relief across all models.
- **Horizon Micro-Shadowing & Cavity Occlusion**:
  Attenuates lighting in deep crevices where $\vec{N}_{\text{geom}} \cdot \vec{N}_{\text{perturbed}}$ dips, while enhancing micro-roughness specular glints on bump ridges.

### 4. Dual-Lobe Directional Lighting & Ambient Specular Reflection
- **Sun Key Light**: Diurnal sun position computed from engine `timeOfDay` with warm solar radiance ($\vec{E}_{\text{sun}} = [2.5, 2.42, 2.30]$).
- **Sky Fill Light**: Cool directional secondary bounce ($\vec{E}_{\text{sky}} = [0.28, 0.36, 0.48]$).
- **Hemispheric Ambient Irradiance**: Direction-dependent dual-color irradiance gradient:
  $$\vec{E}_{\text{irr}} = \text{mix}(\vec{C}_{\text{ground}}, \vec{C}_{\text{sky}}, 0.5 \cdot n_y + 0.5) \cdot 0.9$$
- **Environment Specular Reflection**: Roughness-attenuated environment reflection based on reflection vector $\vec{R} = \text{reflect}(-\vec{V}, \vec{N})$ and roughness-dependent Fresnel:
  $$F_{\text{roughness}} = F_0 + (\max(1 - \text{roughness}, F_0) - F_0)(1 - \vec{N}\cdot\vec{V})^5$$
- **Texture Transforms**: Fully supports `KHR_texture_transform` with scale and offset.

### 5. HDR Tonemapping & sRGB Display Encoding
- **Khronos PBR Neutral Tonemapper**: Uses the official Khronos Group recommendation for glTF 2.0 PBR rendering, eliminating clipped highlights and preserving high-dynamic-range color saturation.
- **sRGB Transfer Curve**: Output colors are mapped to standard sRGB space ($\gamma = 2.2$).
- **AlphaMode**: Supports `OPAQUE` (0), `MASK` (1) with `alphaCutoff` discarding, and `BLEND` (2).

---

## Zero-Allocation Render Loop & Performance

To prevent garbage collection (GC) micro-stutters and guarantee steady 60 FPS / 120 FPS frame rates on high-refresh displays:

1. **Preallocated Scratch Matrices & Vectors**:
   `BglWebGpuRenderer.draw()` allocates 0 heap objects during rendering. Reusable typed array buffers are initialized once during setup:
   ```javascript
   // Preallocated scratch objects (web/app.js)
   _scratchEye: new Float32Array(3),
   _scratchLightDir: new Float32Array(3),
   _scratchProj: null,
   _scratchView: null,
   _scratchViewProj: null,
   _scratchNormalMat: new Float32Array(16),
   _scratchUboBuffer: new ArrayBuffer(512),
   _scratchUboF32: new Float32Array(this._scratchUboBuffer, 0, 72),
   _scratchUboU32: new Uint32Array(this._scratchUboBuffer, 288, 4),
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
  }
  for (const entry of this.textureCache.values()) {
    if (entry.texture && entry.texture.destroy) entry.texture.destroy();
  }
  this.textureCache.clear();
  this.primitives = [];
  ```
- **Texture Cache Deduplication**:
  glTF textures sharing source images are uploaded to the GPU only once and cached in `this.textureCache`, drastically reducing VRAM consumption and upload times.

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

| Platform / Browser | Native Backend | Shading Architecture | Verified Features |
| :--- | :---: | :---: | :--- |
| **Google Chrome (Desktop & Mobile)** | WebGPU (Dawn) | Cook-Torrance PBR (WGSL) | Full PBR, Turntable, Wireframe, CacheStorage |
| **Microsoft Edge (Desktop)** | WebGPU (Dawn) | Cook-Torrance PBR (WGSL) | Hardware acceleration, full HUD controls |
| **Mozilla Firefox (Desktop)** | WebGPU (wgpu) | Cook-Torrance PBR (WGSL) | 4-byte buffer alignment, universal WGSL |
| **Apple Safari (macOS / iOS)**| WebGPU (WebGPU API) | Cook-Torrance PBR (WGSL) | Touch gestures, responsive viewport |

---

## Automated Verification & Test Suites

The web runtime is verified by two complementary automated test suites:

### 1. WebGPU Pipeline Test (`tests/test_webgpu_path.js`)
Simulates standard W3C WebGPU device, command encoder, and WGSL pipelines:
```bash
node tests/test_webgpu_path.js
```
- Tests: Ingestion of `ToyCar.glb` and `Sponza.glb`, Cook-Torrance GGX functions, 7-slot PBR bind groups, 4-byte index buffer alignment, 512-byte UBO layout, zero-allocation render loop, and GPU buffer destruction.

### 2. Multi-Browser End-to-End Test (`tests/test_browsers_e2e.py`)
Controls real instances of **Google Chrome** (via Chrome DevTools Protocol) and **Mozilla Firefox** (via Marionette/geckodriver) over a local HTTP server:
```bash
python3 tests/test_browsers_e2e.py
```
- Tests: Real canvas frame rendering, WebGPU device initialization, glTF model parsing, Cook-Torrance PBR pipeline execution, and zero JavaScript runtime errors.
