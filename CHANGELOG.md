# Changelog

All notable changes to **WebAssembly an WebGPU Demo (bgl-Engine)** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased / 0.7.0] - 2026-10-22

### Added
- **Bump & Tangent-Space Normal Mapping (`[V]` / `--no-bump` / `--bump`):**
  - Automatic MikkTSpace / Lengyel triangle tangent & bitangent basis generation (`MeshPrimitive::ComputeTangents()`) with Gram-Schmidt orthogonalization and handedness sign (`tangent.w`) whenever a glTF primitive omits explicit `TANGENT` attributes.
  - Multithreaded per-vertex tangent orthogonalization (`std::jthread`) for large meshes ($\ge 8\,192$ vertices).
  - Dual normal perturbation in `kPbrShaderWGSL`: combines tangent-space normal maps (`normalTexture`) with screen-space derivative height bump mapping (`dpdx`, `dpdy`, scaled by `ubo.bumpScale`).
- **Advanced Mipmapping Support (`[L]` / `--no-mipmaps` / `--mipmaps`):**
  - 3D vector-renormalized normal map mip downsampling (`PipelineManager::GenerateNextMipLevel(..., isNormalMap = true)`) that decodes $2\times 2$ normal texels into $[-1, 1]^3$, averages, renormalizes to unit length, and re-packs to `RGBA8Unorm` to preserve specular fidelity across mip levels.
  - Multithreaded CPU mip-pyramid generation across all scene textures in `PipelineManager::PrepareScene()` using lock-free `std::atomic<size_t>` work-stealing across `std::jthread` workers, plus row-parallel downsampling for high-resolution textures.
  - Live runtime sampler switching (`PipelineManager::SetMipmappingEnabled()`) between **16× Anisotropic Trilinear Mipmapping** (`defaultSampler_`, `lodMaxClamp = 32.0`) and **Base-Mip-0-Only** (`noMipSampler_`, `lodMaxClamp = 0.0`).
- **Directional Shadow Mapping with 3×3 PCF (`[M]` / `--no-shadows` / `--shadows`):**
  - $2048\times 2048$ `Depth32Float` directional shadow depth pre-pass (`PipelineManager::RenderShadowPass()`) recorded on the active `WGPUCommandEncoder` via `Context::PrepareFrame()` prior to the 4× MSAA color pass.
  - Scene-fitted right-handed zero-to-one orthographic light matrix (`PipelineManager::ComputeDirectionalLightMatrix()` via `glm::orthoRH_ZO`) enclosing all 8 world-space AABB corners.
  - Dedicated 1-entry `shadowBindGroupLayout_` to prevent WebGPU read/write resource hazards on `shadowDepthTexture_`, paired with hardware depth bias (`depthBias = 2`, `depthBiasSlopeScale = 2.0`).
  - 9-tap $3\times 3$ Percentage-Closer Filtering (`sampleShadowPCF`) using `textureSampleCompareLevel` and slope-scaled normal offset bias.
- **Environment Cubemap Reflections & Specular IBL (`[R]` / `--no-reflections` / `--reflections`):**
  - Pre-filtered 6-face atmospheric environment cubemap (`64×64×6`, `WGPUTextureViewDimension_Cube`) with 7 roughness-convolved mip levels (`PipelineManager::GenerateEnvCubemapFaceMip()`), generated in parallel across 6 `std::jthread` worker threads.
  - Analytic Karis split-sum BRDF approximation (`envBRDFApprox`) and roughness-based mip LOD sampling (`textureSampleLevel`) for metallic, dielectric, and clearcoat reflections.
- **Dedicated Rendering Features Unit Test Suite (`tests/test_rendering_features.cpp`):**
  - Added 10 specialized unit tests covering tangent generation on all 6 axis-aligned cube faces, mirrored UV handedness flipping, degenerate UV fallback, POT/NPOT mip level counts, full mip pyramid color conservation, normal-map unit vector preservation, 8-corner shadow matrix containment, depth ordering, cubemap sky/ground luminance, and 352-byte UBO layout.

### Changed
- **Application Branding:** Renamed application display title across native SDL2 window, In-Engine Debug Overlay HUD, CLI `--help`/`--version`, and Web UI to **"WebAssembly an WebGPU Demo"**.
- **Modern C++23 Idioms (`{}` & `std::span`):**
  - Refactored member initializers, local variables, and aggregates across `bgl::core`, `bgl::gpu`, `bgl::scene`, `bgl::text`, `bgl::ui`, `bgl::audio`, and `bgl::app` to use C++23 uniform brace initialization `{}`.
  - Adopted `std::span` across buffer and range APIs (`PipelineManager::GenerateNextMipLevel`, `CliOptions::Parse`, `Quadtree::InsertNodes`, `QuadtreeNode::GetItems`, `RayPicker::Pick`, `TextRenderer::DrawQuads`).
- **Documentation & Live Demo Links:** Added live GitHub Pages and `raw.githack.com` browser preview links and badges to `README.md`, along with a comprehensive architectural deep-dive and this retroactive `CHANGELOG.md`.

---

## [0.6.8] - 2026-10-22

### Added
- **Modular CMake Texture Format Options:** Added independent build toggles `-DBGL_ENABLE_PNG=ON`, `-DBGL_ENABLE_JPEG=ON`, `-DBGL_ENABLE_WEBP=ON`, and `-DBGL_ENABLE_KTX=ON` (all enabled by default) with runtime format querying (`./bgl_viewer --formats`).
- **POSIX Crash Signal Handler & C++23 Stacktrace (`bgl::core::CrashHandler`):**
  - Installed `sigaction` handlers with an alternate signal stack (`sigaltstack`) for `SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL`, and `SIGBUS`.
  - Dual callstack diagnostics combining async-signal-safe `execinfo` backtraces with demangled C++23 `std::stacktrace`.
- **Desktop CLI Argument Parser (`bgl::app::CliOptions`):**
  - Added `--help`, `--version`, `--formats`, `--asset`, `--width`, `--height`, `--log-level`, `--no-skybox`, `--no-hud`, `--no-bounds`, `--quadtree`, and `--test-crash [segv|abrt|fpe]`.
- **Minimal Web App Packaging (`scripts/package_web.py` & `package_web` CMake Target):**
  - Automated packaging of a self-contained ~4.27 MB distribution archive (`dist/bgl_web_app_minimal.zip`) with SHA-256 verification.
- **1 GB Megacity Stress-Test Benchmark:**
  - Verified multithreaded glTF 2.0 ingestion and Quadtree spatial indexing on a 1.00 GB / 5.25M triangle / 1,380 texture asset (~245 MB/s ingestion throughput).
- **Expanded GTest Unit Test Suite:**
  - Grew test coverage to 46 unit tests across 11 test suites (`TypesMathTest`, `CameraTest`, `MemoryPoolTest`, `AssetManagerTest`, `QuadtreeTest`, `RayPickerTest`, `GltfLoaderTest`, `TextureDecoderTest`, `TextRenderingTest`, `CrashHandlerTest`, `CommandLineTest`).

### Fixed
- **Batched Single-Draw 3D Wireframe AABB Rendering:**
  - Fixed WebGPU uniform buffer clobbering where multiple `wgpuQueueWriteBuffer` calls before command submission overwrote previous AABB transforms. All scene, primitive, and quadtree wireframes are now batched into a single dynamic vertex buffer (`512 KB`) and drawn in a single GPU draw call.
- **Quadtree Spatial Containment & Loose Bounds (`bgl::scene::Quadtree`):**
  - Fixed boundary leaks for geometry straddling quadrant dividers by enforcing strict `ContainsXZ` checks and maintaining `looseBounds_` per node, achieving **60% to 82% frustum culling ratios** in Sponza without visual popping.
- **Sponza Unit-Scale Transform & Courtyard Camera Framing:**
  - Fixed root node `0.008` scale transform propagation so Sponza spans its true world dimensions ($29.8\text{ m} \times 12.4\text{ m} \times 18.3\text{ m}$) with human eye-level courtyard camera framing.

---

## [0.6.7] - 2026-10-22

### Added
- **KTX & KTX2 Texture Support with Basis Universal Transcoding:**
  - Integrated `libktx` (`ktx/4.3.2`) via Conan 2.x to decode KTX1 and KTX2 containers and transcode UASTC / ETC1S supercompressed payloads to RGBA8 (`KTX_TTF_RGBA32`).
  - Added magic-byte sniffing (`\xABKTX 11\xBB` and `\xABKTX 20\xBB`), MIME routing (`image/ktx2`), and `.ktx`/`.ktx2` extension detection.
- **Multithreaded glTF Asset Ingestion (`bgl::scene::FastGltfLoader`):**
  - Parallelized texture decoding across hardware worker threads (`std::jthread` + `std::atomic<size_t>`) and parallelized mesh primitive attribute extraction.

---

## [0.6.6] - 2026-10-22

### Added
- **FreeType 2 & HarfBuzz Text Rendering Engine (`bgl::text`):**
  - Dynamic $1024\times 1024$ RGBA8 Shelf-Packing GPU Font Atlas (`FontAtlas`) powered by FreeType 2 (`freetype/2.13.2`).
  - Full Unicode text shaping and kerning (`TextShaper`) via HarfBuzz (`harfbuzz/8.3.0`).
  - Zero-dependency embedded fallback font (`LiberationMono-Regular.ttf` in `EmbeddedFont.cpp`).
  - Batched 2D alpha-blended WebGPU quad renderer (`TextRenderer`) compatible with 4× MSAA render passes.
- **In-Engine Debug Overlay HUD (`bgl::ui::DebugOverlay`):**
  - Real-time glassmorphism HUD displaying smoothed FPS, frame time (ms), active scene metrics, camera coordinates, Quadtree culling telemetry, and interactive hotkey states (`[O]` / `[F3]`).

### Changed
- **Memory Pool & PMR Optimizations:** Improved `MemoryPool` union storage alignment and `ScratchArena` reset semantics.

---

## [0.6.5] - 2026-10-22

### Added
- **Procedural Atmospheric Skybox (`bgl::gpu::Skybox`):**
  - Fullscreen Rayleigh-inspired zenith-to-horizon atmospheric scattering shader with solar disk, corona glow, and ground hemisphere bounce, rendered at far-plane depth (`1.0`).
- **3D Wireframe AABB Bounding Boxes (`[B]` & `[Q]`):**
  - Added Gold scene-root AABB, Cyan per-primitive AABBs, and Emerald green Quadtree partition wireframe visualizations, plus a live Spatial AABB Telemetry card in the Web UI.

---

## [0.6.4] - 2026-10-22

### Added
- **Sponza Atrium Default Benchmark Scene:**
  - Bundled `assets/Sponza.glb` (103 primitives, 69 textures) with automatic fallback to `assets/ToyCar.glb`.
- **4× Hardware MSAA & 16× Anisotropic Filtering:**
  - Enabled 4× multisample anti-aliasing on color and depth attachments and 16× anisotropic texture sampling across PBR materials.
- **Initial XZ-Plane Quadtree & Gribb-Hartmann Frustum Culling:**
  - Introduced `bgl::scene::Quadtree` and WebGPU `[0, 1]` clip-space frustum plane extraction (`Frustum::FromViewProjection`).

---

## [0.6.2] & [0.6.3] - 2026-10-21

### Added
- **CI/CD Pipeline & Reproducible Lockfile:**
  - Added GitHub Actions workflow and local pipeline verification scripts.
  - Unified English logging across all modules and updated deterministic `conan.lock`.

---

## [0.6.1] - 2026-10-20

### Added
- **Complete glTF 2.0 & KHR Material Extensions:**
  - Implemented Cook-Torrance GGX PBR shader with support for `KHR_materials_unlit`, `KHR_materials_clearcoat`, `KHR_materials_transmission`, `KHR_materials_ior`, `KHR_materials_sheen`, `KHR_materials_specular`, and `KHR_materials_emissive_strength`.
- **Interactive Web Scene Graph Tree & JSON Inspector:**
  - Added right-side collapsible hierarchy tree diagram, search filter, node property telemetry, and syntax-highlighted glTF JSON inspector.

---

## [0.6.0] - 2026-10-19

### Added
- **Initial Modular C++23 Architecture (`bgl`):**
  - Created shared library modules `bgl_core`, `bgl_gpu`, `bgl_audio`, `bgl_scene`, and `bgl_app` targeting both Linux amd64 (`wgpu-native` + SDL2) and WebAssembly (`Emscripten` + HTML5 WebGPU).
  - Integrated Conan 2.x package management (`fastgltf`, `glm`, `spdlog`, `miniaudio`, `libpng`, `libjpeg-turbo`, `libwebp`).
  - Implemented Möller-Trumbore 3D ray picking (`RayPicker`), orbital camera (`Camera`), and 3D spatial audio (`SoundEngine`).

