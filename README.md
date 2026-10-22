# bgl-Engine (v0.6.7)

Modular C++23 WebGPU & glTF 2.0/2.1 High-Performance 3D Renderer for Linux amd64 Desktop and WebAssembly (Wasm).

---

## Overview

**bgl-Engine** is a modern, high-performance 3D rendering engine built in strict C++23 with a hardware WebGPU backend (`wgpu-native` on desktop, HTML5 WebGPU in browser). It features complete glTF 2.0 and KHR material model support, zero-GLFW architecture, PMR-backed memory management, HarfBuzz & FreeType 2 text shaping, spatial Quadtree partitioning, procedural atmospheric skybox, and dual-target deployment.

### Key Architecture Highlights

- **100% Modern C++23 & Root Namespace `bgl`**:
  - `bgl::core`: Math, types, AABB, ray intersection, camera, memory pools, PMR arenas, logging.
  - `bgl::gpu`: WebGPU device context, swapchain surface, 4x MSAA, WGSL PBR shaders, procedural atmospheric skybox, 3D wireframe bounding boxes.
  - `bgl::scene`: High-speed multithreaded glTF ingestion (`fastgltf`), KTX / KTX2 texture decoding with Basis Universal transcoding, 16x anisotropic texture filtering with full CPU mip-mapping, spatial Quadtree partitioning, Möller-Trumbore ray picking.
  - `bgl::text`: Dynamic RGBA8 font atlas, FreeType 2 glyph rasterization, HarfBuzz Unicode script and bidi text shaping, batched 2D quad renderer with alpha blending.
  - `bgl::ui`: In-engine glassmorphism Debug Overlay HUD displaying real-time FPS, frame timing, camera coordinates, glTF scene metrics, and spatial culling telemetry.
  - `bgl::audio`: 3D spatial sound engine via `miniaudio`.
  - `bgl::app`: Multiplatform application lifecycle and runtime loop.
- **Multithreaded glTF Asset Ingestion**:
  - Lock-free parallel texture decoding across hardware worker threads (`std::jthread`, `std::atomic<size_t>` task dispatch).
  - Parallel scene node extraction, transform hierarchy evaluation, and mesh primitive parsing (vertex attributes, bounding boxes, indices).
  - Sub-second loading of large scenes like Sponza (69 multi-megabyte textures decoded concurrently).
- **KTX & KTX2 Texture Support with Basis Universal Transcoding**:
  - Full KTX1 and KTX2 container ingestion via `libktx` (`ktx/4.3.2`).
  - Automatic on-the-fly transcoding of Basis Universal compressed textures (UASTC and ETC1S) to 32-bit RGBA (`KTX_TTF_RGBA32`).
  - Content-based magic-byte sniffing (`\xABKTX 11\xBB` and `\xABKTX 20\xBB`), MIME type detection, and path extension routing.
- **HarfBuzz & FreeType 2 Text Rendering Pipeline**:
  - Precise Unicode text shaping via HarfBuzz (`hb_buffer_t`, `hb_shape`).
  - High-fidelity glyph rasterization via FreeType 2 into a dynamic 1024x1024 RGBA8 font atlas.
  - Zero-dependency embedded fallback font (Liberation Mono Regular) allowing standalone execution anywhere.
  - Batched 2D WebGPU render pipeline with alpha blending (`SrcAlpha / OneMinusSrcAlpha`), 4x MSAA render-pass compatibility, and orthographic projection.
- **AAAA Visual Fidelity & Sponza Benchmark Scene**:
  - Sponza atrium loaded by default with 69 multi-megabyte textures decoded concurrently (PNG, JPEG, WebP, KTX2).
  - 16x Anisotropic filtering (`maxAnisotropy = 16`) across all samplers.
  - Full CPU box-filter mip-map generation chain down to 1x1 texels.
  - Procedural atmospheric skybox with Rayleigh-like zenith-to-horizon gradients, solar corona, and far-plane depth clamping.
  - 3D Wireframe Bounding Boxes: Scene root AABB in Gold, Mesh Primitives in Cyan, Quadtree ground partitions in Emerald Green.
- **Complete glTF 2.0 & KHR Material Model**:
  - Core PBR Metallic-Roughness (Cook-Torrance GGX $D$, Smith-GGX $V$, Schlick Fresnel $F$).
  - Tangent-space Normal Mapping (with vertex `TANGENT` ingestion and screen-space derivative fallbacks).
  - Ambient Occlusion (AO) with configurable strength.
  - Emissive maps with strength multipliers (`KHR_materials_emissive_strength`).
  - Clearcoat lacquer layer (`KHR_materials_clearcoat`).
  - Transmission and dielectric refraction (`KHR_materials_transmission`, `KHR_materials_ior`).
  - Charlie microfiber sheen for textiles and fabrics (`KHR_materials_sheen`).
  - Specular color and factor tinting (`KHR_materials_specular`).
  - Double-sided rendering and unlit shading (`KHR_materials_unlit`).
  - Alpha modes: `Opaque`, `Mask` (alpha cutoff discard), and `Blend`.
- **Zero-GLFW Architecture**:
  - **Linux Desktop**: Native SDL2 windowing and hardware event handling.
  - **WebAssembly**: Native Emscripten HTML5 WebGPU context (`<emscripten/html5.h>`).
- **Reproducible Conan 2.x Dependency Management**:
  - All third-party dependencies resolved via Conan 2.x with explicit lockfile pinning (`conan.lock`).
  - FreeType (`freetype/2.13.2`), HarfBuzz (`harfbuzz/8.3.0`), and KTX (`ktx/4.3.2`).
  - Zero system library divergence across build environments.

---

## Repository Structure

```text
.
├── CMakeLists.txt                 # Modern target-based CMake configuration (C++23)
├── conanfile.py                   # Conan 2.x package recipe
├── conan.lock                     # Deterministic dependency lockfile
├── assets/
│   ├── Sponza.glb                 # High-detail Sponza architectural benchmark scene
│   ├── ToyCar.glb                 # Benchmark glTF model (Clearcoat, Sheen, Transmission, AO)
│   └── fonts/
│       └── LiberationMono-Regular.ttf # High-legibility monospace font
├── src/
│   ├── core/                      # -> libbgl_core.so
│   │   ├── Camera.hpp/.cpp        # Orbit, Pan, Zoom camera with WebGPU reverse-Z projection
│   │   ├── Gamepad.hpp/.cpp       # Controller support with 0.12f deadzone calibration
│   │   ├── Log.hpp/.cpp           # spdlog integration and RAII ScopedTimer profiling
│   │   ├── MemoryPool.hpp/.cpp    # Block memory pool & PMR scratch arena
│   │   ├── NetworkFetcher.hpp/.cpp# Asynchronous network ingestion
│   │   └── Types.hpp              # AABB, Ray, Frustum, 64-byte Vertex, GlobalUniforms, Result<T>
│   ├── gpu/                       # -> libbgl_gpu.so
│   │   ├── Context.hpp/.cpp       # Hardware WebGPU device, adapter, surface, depth buffers (4x MSAA)
│   │   ├── Pipeline.hpp/.cpp      # 10-slot BindGroupLayout, 16x aniso, AABB wireframe pipelines
│   │   ├── Shaders.hpp            # Full glTF 2.0 Cook-Torrance PBR & Wireframe WGSL shaders
│   │   └── Skybox.hpp/.cpp        # Procedural Atmospheric Skybox with solar corona
│   ├── scene/                     # -> libbgl_scene.so
│   │   ├── AssetManager.hpp/.cpp  # 64-bit FNV-1a caching and PMR geometry allocation
│   │   ├── FastGltfLoader.hpp/.cpp# SIMD glTF binary parser with full KHR extension support
│   │   ├── Quadtree.hpp/.cpp      # 4-way spatial partition and LOD culling on XZ ground plane
│   │   ├── RayPicker.hpp/.cpp     # Hierarchical Möller-Trumbore ray-triangle intersection
│   │   ├── Scene.hpp              # Hierarchical scene graph and PMR mesh primitives
│   │   └── TextureDecoder.hpp/.cpp# High-performance PNG, JPEG, WebP, and KTX2 decoding with CPU mip-chains
│   ├── text/                      # -> libbgl_text.so
│   │   ├── EmbeddedFont.hpp/.cpp  # Zero-dependency embedded TrueType font data
│   │   ├── FontAtlas.hpp/.cpp     # FreeType 2 rasterizer & dynamic 1024x1024 RGBA8 GPU texture atlas
│   │   ├── TextShaper.hpp/.cpp    # HarfBuzz Unicode text shaper & multi-line layout engine
│   │   └── TextRenderer.hpp/.cpp  # WebGPU 2D batched quad renderer with alpha blending
│   ├── ui/                        # -> libbgl_ui.so
│   │   └── DebugOverlay.hpp/.cpp  # In-engine glassmorphism HUD telemetry overlay
│   ├── audio/                     # -> libbgl_audio.so
│   │   └── SoundEngine.hpp/.cpp   # 3D spatial audio listener and sound source management
│   ├── app/                       # -> libbgl_app.so
│   │   └── Application.hpp/.cpp   # Main application lifecycle and runtime loop
│   └── main.cpp                   # Native desktop viewer entry point
├── tests/                         # GTest unit test suite
│   ├── test_main.cpp
│   ├── test_memory_pool.cpp
│   ├── test_asset_manager.cpp
│   ├── test_quadtree_lod.cpp
│   ├── test_ray_picker.cpp
│   └── test_text_rendering.cpp
└── web/                           # Web viewer client
    ├── index.html                 # Interactive UI (Tree Diagram, JSON Inspector, Telemetry Card)
    └── dist/                      # Precompiled WebAssembly artifacts
```

---

## Building and Running

### Prerequisites

- Modern C++23 compiler (GCC 13+ or Clang 17+)
- CMake 3.28+
- Ninja build system
- Conan 2.x
- Vulkan SDK and graphic drivers with WebGPU support

### 1. Install Dependencies with Conan

```bash
# Generate CMake toolchain and dependency files using Conan 2.x lockfile
conan install . -s build_type=Release --build=missing
```

### 2. Configure and Build

```bash
# Configure with CMake using Conan-generated toolchain
cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake
cmake --build build/Release -j$(nproc)
```

### 3. Run Unit Tests

```bash
ctest --test-dir build/Release --output-on-failure
```

### 4. Launch Native Desktop Viewer

The executable automatically resolves relative RPATHs to its shared libraries and WebGPU backend:

```bash
./build/Release/bgl_viewer
```

*Controls:*
- **Left Mouse Button Drag**: Orbit camera around target.
- **Right Mouse Button / Shift + Drag**: Pan camera laterally and vertically.
- **Mouse Wheel**: Smooth zoom.
- **B Key**: Toggle 3D Wireframe Bounding Boxes & AABBs (Gold for scene root, Cyan for mesh primitives).
- **Q Key**: Toggle 3D Quadtree spatial ground partition boundaries (Emerald green).
- **K Key**: Toggle Procedural Atmospheric Skybox with solar corona.
- **O / F3 Key**: Toggle In-Engine FreeType 2 & HarfBuzz Debug Overlay HUD.
- **File Drag-and-Drop**: Drop any `.glb` or `.gltf` model directly into the native viewer window.
- **ESC**: Exit viewer.

### 5. Launch WebAssembly Browser Viewer

Serve the `web/` directory with any static HTTP server:

```bash
python3 -m http.server 8080 --directory web
```

Navigate to `http://localhost:8080` to inspect models, view the interactive scene graph tree hierarchy, and inspect the raw glTF JSON structure.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
