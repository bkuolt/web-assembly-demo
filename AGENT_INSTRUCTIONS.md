# Master Specification & Architecture Guide (bgl-Engine)
**Project:** Modular C++23 WebGPU & glTF 2.0/2.1 High-Performance Renderer (`bgl`-Engine)  
**Target Environments:** Linux (amd64) Desktop (`bgl_viewer`) & WebAssembly (`wasm32-unknown-emscripten`)  
**Version:** `0.6.3`

---

## 1. Core Architectural Directives

1. **Unified Root Namespace:**
   - All C++ code must reside within the root namespace `bgl`.
   - Modules are organized as: `bgl::core`, `bgl::gpu`, `bgl::scene`, `bgl::audio`, and `bgl::app`.
2. **Modern C++23 Standard:**
   - Strict 100% C++23 (`-std=c++23`).
   - No exceptions across module or C-ABI boundaries: use `bgl::core::Result<T>` (`std::expected<T, std::string>`).
   - Use `std::span` for zero-copy memory buffers, `std::string_view` for string parameters, `constexpr` for static computations, and strict RAII for all WebGPU and OS handles.
3. **Documentation:**
   - Clean, concise, and exclusively in **English**. All public types, functions, and structs include Doxygen comments (`/** ... */` or `///`).
4. **Zero-GLFW Architecture:**
   - GLFW is prohibited.
   - **WebAssembly:** Native Emscripten HTML5 API (`<emscripten/html5.h>`).
   - **Native (Linux amd64):** SDL2 for windowing and hardware inputs.
5. **Memory Architecture:**
   - No uncontrolled `new`/`delete`.
   - Utilize block-based `bgl::core::MemoryPool<T>` and `std::pmr` resources (`std::pmr::monotonic_buffer_resource`, `std::pmr::vector`) for geometry staging and glTF ingestion.
6. **Modular Shared Libraries (`.so`):**
   - Built with `-fPIC` into distinct dynamic shared libraries:
     - `libbgl_core.so`
     - `libbgl_gpu.so`
     - `libbgl_audio.so`
     - `libbgl_scene.so`
     - `libbgl_app.so`

---

## 2. Conan 2.x Dependency Management

All third-party dependencies are pinned via `conan.lock` and managed through `conanfile.py`:
- `fastgltf/0.7.2` (glTF 2.1 SIMD binary and JSON parser)
- `glm/1.0.1` (Vector and matrix linear algebra)
- `spdlog/1.14.0` (Fast structured logging and scoped microsecond timers)
- `libpng/1.6.43` (In-memory PNG decoding)
- `libjpeg-turbo/3.0.2` (High-speed JPEG decoding)
- `libwebp/1.3.2` (WebP image decoding)
- `miniaudio/0.11.21` (3D Spatial Audio and Web Audio backend)
- `gtest/1.14.0` (Unit testing framework)

---

## 3. WebGPU Material Pipeline

- **Unified Bind Group Layout**:
  - Binding 0: Uniform buffer `PrimitiveUniforms` (272 bytes, 16-byte aligned).
  - Binding 1: Sampler `defaultSampler`.
  - Bindings 2..9: Sampled 2D textures (BaseColor, MetallicRoughness, Normal, Emissive, Occlusion, Clearcoat, Transmission, Sheen).
- **Fallback Textures**:
  - 1x1 White: BaseColor, MetallicRoughness, Occlusion, Clearcoat, Transmission, Sheen.
  - 1x1 Flat Normal: Tangent space (128, 128, 255).
  - 1x1 Black: Emissive.
- **Cook-Torrance PBR Model**:
  - GGX normal distribution $D$.
  - Smith-GGX visibility $V$.
  - Schlick Fresnel $F$.
  - Multi-lobe support for car paint lacquer (`Clearcoat`) and fabric textiles (`Sheen`).
  - Refractive dielectric transmission for glass (`Transmission`, `IOR`).
