# bgl-Engine (v0.6.0)

Modular C++23 WebGPU & glTF 2.1 High-Performance 3D Renderer for Linux amd64 and WebAssembly (Wasm).

## Key Architecture Highlights
- **100% C++23 & Root Namespace `bgl`**: `bgl::core`, `bgl::gpu`, `bgl::scene`, `bgl::audio`, `bgl::app`.
- **Zero-GLFW**: Native Emscripten HTML5 API on WebAssembly and SDL2 on Linux desktop.
- **Modular Shared Libraries (`.so`)**:
  - `libbgl_core.so`: Memory pools, PMR arenas, Math, Types, Camera, Gamepad with 0.12f deadzone.
  - `libbgl_gpu.so`: WebGPU Context, WGSL PBR shaders, Skybox/IBL, Alpha/Wireframe pipelines.
  - `libbgl_audio.so`: 3D Spatial Audio via miniaudio.
  - `libbgl_scene.so`: glTF 2.1 fast loader, 64-bit FNV-1a caching, Texture decoders, Quadtree LOD, Möller-Trumbore ray picking.
  - `libbgl_app.so`: Application lifecycle, drag-and-drop, JS-bridge.
- **Memory Management**: Block `MemoryPool<T>` and `std::pmr` monotonic staging arenas.

## Building & Testing

### Quick Start with Taskfile
```bash
task setup
task configure
task build
task test
```

### Manual CMake
```bash
cmake -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DBGL_BUILD_TESTING=ON
cmake --build build-native -j$(nproc)
ctest --test-dir build-native --output-on-failure
```

