# bgl-Engine (v0.6.2)

Modular C++23 WebGPU & glTF 2.0/2.1 High-Performance 3D Renderer for Linux amd64 Desktop and WebAssembly (Wasm).

---

## Overview

**bgl-Engine** is a modern, high-performance 3D rendering engine built in strict C++23 with a hardware WebGPU backend (`wgpu-native` on desktop, HTML5 WebGPU in browser). It features complete glTF 2.0 and KHR material model support, zero-GLFW architecture, PMR-backed memory management, and dual-target deployment.

### Key Architecture Highlights

- **100% Modern C++23 & Root Namespace `bgl`**:
  - `bgl::core`: Math, types, AABB, ray intersection, camera, memory pools, PMR arenas, logging.
  - `bgl::gpu`: WebGPU device context, swapchain surface, WGSL PBR shaders, pipeline management.
  - `bgl::scene`: High-speed glTF ingestion (`fastgltf`), multi-format texture decoding, PMR scene graph, spatial Quadtree, Möller-Trumbore ray picking.
  - `bgl::audio`: 3D spatial sound engine via `miniaudio`.
  - `bgl::app`: Multiplatform application lifecycle and event loops.
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
- **PMR & Memory Pool Architecture**:
  - Eliminates heap fragmentation through `bgl::core::MemoryPool<T>` and `std::pmr::monotonic_buffer_resource` scratch arenas for high-throughput glTF ingestion.
  - 64-byte aligned vertex layouts (`bgl::core::Vertex`) matching GPU cache-line boundaries.
  - 16-byte aligned uniform structures conforming to WebGPU std140 layout requirements.
- **Reproducible Conan 2.x Dependency Management**:
  - All third-party dependencies resolved via Conan 2.x with explicit lockfile pinning (`conan.lock`).
  - Zero system library divergence across build environments.

---

## Repository Structure

```text
.
├── CMakeLists.txt                 # Modern target-based CMake configuration (C++23)
├── conanfile.py                   # Conan 2.x package recipe
├── conan.lock                     # Deterministic dependency lockfile
├── assets/
│   └── ToyCar.glb                 # Benchmark glTF model (Clearcoat, Sheen, Transmission, AO)
├── src/
│   ├── core/                      # -> libbgl_core.so
│   │   ├── Camera.hpp/.cpp        # Orbit, Pan, Zoom camera with WebGPU reverse-Z projection
│   │   ├── Gamepad.hpp/.cpp       # Controller support with 0.12f deadzone calibration
│   │   ├── Log.hpp/.cpp           # spdlog integration and RAII ScopedTimer profiling
│   │   ├── MemoryPool.hpp/.cpp    # Block memory pool & PMR scratch arena
│   │   ├── NetworkFetcher.hpp/.cpp# Asynchronous network ingestion
│   │   └── Types.hpp              # AABB, Ray, 64-byte Vertex, GlobalUniforms, Result<T>
│   ├── gpu/                       # -> libbgl_gpu.so
│   │   ├── Context.hpp/.cpp       # Hardware WebGPU device, adapter, surface, depth buffers
│   │   ├── Pipeline.hpp/.cpp      # 10-slot BindGroupLayout, 1x1 fallback textures, render passes
│   │   ├── Shaders.hpp            # Full glTF 2.0 Cook-Torrance PBR WGSL shader
│   │   └── Skybox.hpp/.cpp        # Cubemap reflections and background pipeline
│   ├── scene/                     # -> libbgl_scene.so
│   │   ├── AssetManager.hpp/.cpp  # 64-bit FNV-1a caching and PMR geometry allocation
│   │   ├── FastGltfLoader.hpp/.cpp# SIMD glTF binary parser with full KHR extension support
│   │   ├── Quadtree.hpp/.cpp      # 4-way spatial partition and LOD culling on XZ ground plane
│   │   ├── RayPicker.hpp/.cpp     # Hierarchical Möller-Trumbore ray-triangle intersection
│   │   ├── Scene.hpp              # Hierarchical scene graph and PMR mesh primitives
│   │   └── TextureDecoder.hpp/.cpp# High-performance PNG, JPEG, WebP memory decoding
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
│   └── test_ray_picker.cpp
└── web/                           # Web viewer client
    ├── index.html                 # Interactive UI (Tree Diagram, JSON Inspector, Drop Zone)
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
- **B Key**: Toggle bounding box wireframe debug overlay.
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
