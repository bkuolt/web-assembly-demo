# Master Specification & Architecture Guide (bgl-Engine)
**Project:** Modular C++23 WebGPU & glTF 2.0/2.1 High-Performance Renderer (`bgl`-Engine)  
**Target Environments:** Linux (amd64) Desktop (`bgl_viewer`) & WebAssembly (`wasm32-unknown-emscripten`)  
**Version:** `0.9.6`

---

## 1. Core Architectural Directives

1. **Unified Root Namespace:**
   - All C++ code must reside within the root namespace `bgl`.
   - Modules are strictly organized as:
     - `bgl::core`: Math, types, AABB (Arvo algorithm), Ray (Möller-Trumbore), Plane, Frustum, Camera, block-based memory pools, PMR staging arenas, POSIX signal handling, C++23 `std::stacktrace` crash diagnosis, NetworkFetcher (HTTP/HTTPS/data/file), logging.
     - `bgl::gpu`: WebGPU device context, swapchain surface, 4x MSAA, WGSL PBR shaders, procedural atmospheric skybox, HDR post-processing tonemapping (Khronos PBR Neutral, AgX, ACES), Directional PCF Shadow Mapping, True HDR IBL (Split-Sum BRDF LUT, GGX prefiltered cubemap, L2 Spherical Harmonics), batched 3D wireframe bounding box pipelines.
     - `bgl::scene`: High-speed multithreaded glTF ingestion (`fastgltf`), modular texture decoders (PNG, JPEG, WebP, KTX2 Basis Universal, Radiance HDR, OpenEXR), 16x anisotropic filtering, full CPU mip-map generation, spatial Quadtree partitioning with loose bounds, Hi-Z `OcclusionBuffer`, Möller-Trumbore ray picking.
     - `bgl::text`: Dynamic RGBA8 font atlas, FreeType 2 glyph rasterization, HarfBuzz Unicode text shaping, 2D batched quad renderer.
     - `bgl::ui`: In-engine glassmorphism Debug Overlay HUD with smoothed FPS, camera metrics, scene info, and spatial telemetry.
     - `bgl::audio`: 3D spatial sound engine via `miniaudio`.
     - `bgl::app`: Multiplatform application lifecycle, CLI argument parsing (`--help`, `--version`, `--formats`, `--asset`, etc.), and runtime event loop.
2. **Modern C++23 Standard:**
   - Strict 100% C++23 (`-std=c++23`).
   - No exceptions across module or C-ABI boundaries: use `bgl::core::Result<T>` (`std::expected<T, std::string>`).
   - Use `std::span` for zero-copy memory buffers, `std::string_view` for string parameters, `constexpr` for static computations, `std::format` for type-safe formatting, and strict RAII for all WebGPU and OS handles.
3. **Documentation:**
   - Clean, concise, and exclusively in **English**. All public types, functions, and structs include Doxygen comments (`/** ... */` or `///`).
4. **Zero-GLFW Architecture:**
   - GLFW is strictly prohibited.
   - **WebAssembly:** Native Emscripten HTML5 API (`<emscripten/html5.h>`).
   - **Native (Linux amd64):** SDL2 for windowing and hardware inputs.
5. **Memory Architecture:**
   - No uncontrolled `new`/`delete`.
   - Utilize block-based `bgl::core::MemoryPool<T>` and `std::pmr` resources (`std::pmr::monotonic_buffer_resource`, `std::pmr::vector`) for geometry staging and glTF ingestion.
   - Cache-line friendly 64-byte aligned vertex structures (`bgl::core::Vertex`).
   - Std140 16-byte aligned uniform buffers (`PrimitiveUniforms`, `AabbUniforms`, `GlobalUniforms`).
6. **Modular Shared Libraries (`.so`):**
   - Built with `-fPIC` into distinct dynamic shared libraries:
     - `libbgl_core.so`
     - `libbgl_gpu.so`
     - `libbgl_scene.so`
     - `libbgl_text.so`
     - `libbgl_ui.so`
     - `libbgl_audio.so`
     - `libbgl_app.so`

---

## 2. Conan 2.x Dependency Management

All third-party dependencies are pinned via `conan.lock` and managed deterministically through `conanfile.py`:
- `fastgltf/0.7.2` (glTF 2.1 SIMD binary and JSON parser)
- `glm/1.0.1` (Vector and matrix linear algebra)
- `spdlog/1.14.0` (Fast structured logging and scoped microsecond timers)
- `libpng/1.6.43` (In-memory PNG decoding)
- `libjpeg-turbo/3.0.2` (High-speed JPEG decoding)
- `libwebp/1.3.2` (WebP image decoding)
- `ktx/4.3.2` (KTX & KTX2 texture decoding, Basis Universal UASTC/ETC1S transcoding)
- `miniaudio/0.11.21` (3D Spatial Audio and Web Audio backend)
- `freetype/2.13.2` (TrueType font rasterization and glyph metrics)
- `harfbuzz/8.3.0` (Unicode script and bidirectional text shaping)
- `gtest/1.14.0` (Unit testing framework)

---

## 3. WebGPU Material & Rendering Pipeline

- **Unified Bind Group Layout**:
  - Binding 0: Uniform buffer `PrimitiveUniforms` (352 bytes, 16-byte aligned).
  - Binding 1: Sampler `defaultSampler` (16x anisotropic filtering, repeat addressing).
  - Bindings 2..9: Sampled 2D textures (BaseColor, MetallicRoughness, Normal, Emissive, Occlusion, Clearcoat, Transmission, Sheen).
  - Binding 10: Environment Cubemap Texture (`texture_cube<f32>`).
  - Binding 11: Split-Sum BRDF LUT Texture (`texture_2d<f32>`).
  - Binding 12: Directional Shadow Map Sampler (`sampler_comparison`).
  - Binding 13: Directional Shadow Map Depth Texture (`texture_depth_2d`).
  - Binding 14: Scene Lighting Uniform Buffer `SceneLightingUniforms` (L2 Spherical Harmonics, Forward+ clusters).
- **Cook-Torrance PBR Model**:
  - GGX normal distribution $D$, Smith-GGX visibility $V$, Schlick Fresnel $F$.
  - Multi-lobe support for car paint lacquer (`Clearcoat`) and fabric microfiber sheen (`Sheen`).
  - Refractive dielectric transmission for glass (`Transmission`, `IOR`).
  - True HDR IBL with WebGPU compute-generated Split-Sum BRDF LUT ($128\times 128$ RGBA16F), pre-filtered GGX specular cubemap mip chains, and $L_2$ Spherical Harmonics diffuse irradiance.
  - Forward+ clustered lighting with 16x8x8 frustum clusters ($1,024$ clusters) supporting directional, point, and spot lights (`KHR_lights_punctual`).
- **Directional PCF Shadow Mapping**:
  - $2048\times 2048$ `Depth32Float` shadow map depth pass with slope-scale depth bias and 9-tap $3\times 3$ percentage-closer filtering.
- **Tonemapping & Post-Processing**:
  - HDR `RGBA16Float` render target with Khronos PBR Neutral, AgX (Blender 4), and ACES Filmic tonemapping operators.
- **Procedural Atmospheric Skybox**:
  - Full-screen procedural far-plane shader with Rayleigh-like zenith-to-horizon gradients, solar disc, and soft halo corona.
- **3D Wireframe Bounding Boxes & AABBs**:
  - Single-draw batched wireframe line renderer:
    - Root Scene AABB: Vivid Gold (`#ffd700`).
    - Mesh Primitives: Electric Cyan (`#38bdf8`).
    - Quadtree Ground Partitions: Emerald Green (`#10b981`).
- **Text & HUD Rendering**:
  - HarfBuzz Unicode shaping + FreeType 2 rasterization into dynamic 1024×1024 RGBA8 atlas.
  - Batched 2D quads with alpha blending (`SrcAlpha` / `OneMinusSrcAlpha`), 4x MSAA render-pass compatibility, and orthographic projection.

---

## 4. Interactive Controls & Hotkeys

- **Left Mouse Button Drag**: Orbit camera around target.
- **Right Mouse Button / Shift + Drag**: Pan camera laterally and vertically.
- **Mouse Wheel**: Smooth zoom in / out.
- **B Key**: Toggle 3D Wireframe Bounding Boxes & AABBs.
- **Q Key**: Toggle 3D Quadtree ground partition boundaries.
- **C Key**: Toggle Quadtree Hierarchical Occlusion Culling.
- **K Key**: Cycle Skybox Mode (Sharp / Blurred / Hidden).
- **V Key**: Toggle Bump & Tangent-Space Normal Mapping.
- **L Key**: Toggle Trilinear Mipmapping (16x Anisotropic vs Mip 0).
- **M Key**: Toggle Directional Shadow Mapping (2048x2048 PCF).
- **R Key**: Toggle Environment Cubemap Reflections & Specular IBL.
- **U Key**: Toggle Ambient Occlusion & Micro-Cavity Shading.
- **T Key**: Cycle Tonemapper (Khronos PBR Neutral / AgX / ACES).
- **E Key**: Cycle Environment Preset (Mediterranean Courtyard, Sunset Studio, Overcast Atelier, Night Neon, Custom HDR).
- **+ / - Key**: Increase / Decrease Exposure.
- **[ / ] Key**: Rotate Environment (-5° / +5°).
- **O / F3 Key**: Toggle In-Engine FreeType 2 & HarfBuzz Debug Overlay HUD.
- **Double-Click / Middle Mouse Button**: Pick geometry node with RayPicker.
- **File Drag-and-Drop**: Drop any `.glb`, `.gltf`, or HTTP/HTTPS URL directly onto the window.
- **ESC**: Exit viewer.

---

## 5. Web Client Runtime Architecture (`web/app.js`)

- **Streaming WebAssembly Compilation**:
  - Leverages `WebAssembly.instantiateStreaming` to compile WASM bytecode concurrently in a background thread while downloading across the network, with seamless fallback to `fetch().arrayBuffer()`.
- **High-Speed Streaming Asset Downloader (`BglNetwork`)**:
  - Streams chunks via `ReadableStream` reader, calculating real-time downloaded bytes, total payload, percentage, and bandwidth speed in MB/s for fluid UI updates.
- **Structured Subsystem Logging (`BglLogger`)**:
  - Console badges with millisecond timestamps (`[Engine]`, `[Wasm]`, `[WebGPU]`, `[Network]`, `[glTF]`, `[Perf]`, `[UI]`, `[Error]`) and `performance.now()` operation benchmarking.
- **User-Facing Toast Notifications (`BglNotifier`)**:
  - Floating glassmorphism notifications (`#user-toast-container`) for network errors (HTTP 404/500), CORS restrictions, glTF corruption, invalid file drops, and WebGPU context loss.
- **Scene Graph Hierarchy & JSON Inspector (`BglGltf`)**:
  - Live ASCII/visual tree diagram with node icons, primitive/vertex counters, material names, bounding box telemetry, and interactive collapsible JSON inspector.
