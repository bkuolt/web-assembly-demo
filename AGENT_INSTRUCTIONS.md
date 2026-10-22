# Master Specification & Architecture Guide (bgl-Engine)
**Project:** Modular C++23 WebGPU & glTF 2.0/2.1 High-Performance Renderer (`bgl`-Engine)  
**Target Environments:** Linux (amd64) Desktop (`bgl_viewer`) & WebAssembly (`wasm32-unknown-emscripten`)  
**Version:** `0.6.7`

---

## 1. Core Architectural Directives

1. **Unified Root Namespace:**
   - All C++ code must reside within the root namespace `bgl`.
   - Modules are strictly organized as:
     - `bgl::core`: Math, types, AABB, ray intersection, camera, memory pools, PMR staging arenas, POSIX signal handling, C++23 `std::stacktrace` crash diagnosis, logging.
     - `bgl::gpu`: WebGPU device context, swapchain surface, 4x MSAA, WGSL PBR shaders, atmospheric skybox, AABB wireframe pipelines.
     - `bgl::scene`: High-speed multithreaded glTF ingestion (`fastgltf`), modular texture decoders (PNG, JPEG, WebP, KTX2) with CMake compile toggles, Basis Universal transcoding, 16x anisotropic filtering, full CPU mip-map generation, spatial Quadtree partitioning, Möller-Trumbore ray picking.
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
  - Binding 0: Uniform buffer `PrimitiveUniforms` (272 bytes, 16-byte aligned).
  - Binding 1: Sampler `defaultSampler` (16x anisotropic filtering, repeat addressing).
  - Bindings 2..9: Sampled 2D textures (BaseColor, MetallicRoughness, Normal, Emissive, Occlusion, Clearcoat, Transmission, Sheen).
- **Fallback Textures**:
  - 1x1 White: BaseColor, MetallicRoughness, Occlusion, Clearcoat, Transmission, Sheen.
  - 1x1 Flat Normal: Tangent space (128, 128, 255).
  - 1x1 Black: Emissive.
- **Cook-Torrance PBR Model**:
  - GGX normal distribution $D$.
  - Smith-GGX visibility $V$.
  - Schlick Fresnel $F$.
  - Multi-lobe support for car paint lacquer (`Clearcoat`) and fabric microfiber sheen (`Sheen`).
  - Refractive dielectric transmission for glass (`Transmission`, `IOR`).
- **Procedural Atmospheric Skybox**:
  - Full-screen procedural far-plane shader with Rayleigh-like zenith-to-horizon gradients, solar disc, and soft halo corona.
- **3D Wireframe Bounding Boxes & AABBs**:
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
- **Mouse Wheel**: Smooth zoom.
- **B Key**: Toggle 3D Wireframe Bounding Boxes & AABBs.
- **Q Key**: Toggle 3D Quadtree spatial ground partition boundaries.
- **K Key**: Toggle Procedural Atmospheric Skybox.
- **O / F3 Key**: Toggle In-Engine FreeType 2 & HarfBuzz Debug Overlay HUD.
- **File Drag-and-Drop**: Drop any `.glb` or `.gltf` model directly into the native viewer window.
- **ESC**: Exit viewer.
