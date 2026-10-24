# Changelog

All notable changes to **WebAssembly and WebGPU Demo (bgl-Engine)** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.8.1] - 2026-10-24

### Added
- **Browser Compatibility Warning System (`web/index.html`, `web/app.js`, `web/style.css`):**
  - Implemented pre-flight checks validating WebAssembly support, WebGPU API availability (`navigator.gpu`), GPU adapter initialization (`requestAdapter`), and Secure Context compliance (`window.isSecureContext`).
  - Created a glassmorphism warning dialog with detailed technical diagnostic cards and browser troubleshooting guidance (Chrome 113+, Edge 113+, Firefox `dom.webgpu.enabled`, hardware acceleration, and GPU driver requirements).
  - Integrated status warnings into the in-engine telemetry HUD and wired dismiss controls to mouse events and the `Escape` key.
- **glTF Image Deduplication & Mesh Pre-Parsing Optimizations (`bgl::scene::FastGltfLoader`):**
  - Implemented image decoding deduplication by processing unique `asset.images` entries across worker threads, eliminating redundant decoding passes for textures sharing image buffers.
  - Implemented mesh pre-parsing to compute vertex buffers, fallback flat normals, and MikkTSpace / Lengyel tangents once per `asset.meshes` before instantiating scene nodes.
- **Arvo Bounding Box Transformation Algorithm (`bgl::core::AABB::Transformed`):**
  - Replaced 8-corner matrix multiplication with Arvo's Center-Extents transformation algorithm ($c' = M \cdot c$ and $e'_k = \sum_j |M_{jk}| e_j$), accelerating hierarchical spatial queries and view frustum culling.
- **Conan 2.x Lockfile Synchronization (`conan.lock`, `conanfile.py`):**
  - Updated recipe package version to `0.8.1` and regenerated the Conan 2.x lockfile to lock all 17 direct and transitive dependencies and build tools.
- **In-Code API Documentation Modernization:**
  - Enriched Doxygen class headers, method descriptions, parameter tags (`@param`, `@return`), and architectural documentation across `Types.hpp`, `Pipeline.hpp`, `Context.hpp`, `Skybox.hpp`, and `FastGltfLoader.hpp`.
- **MeshPrimitive Copy Safety & Lifetime Management (`bgl::scene::MeshPrimitive`):**
  - Added dedicated copy constructor and assignment operator that deep-copy CPU geometry buffers while ensuring GPU resource handles remain safely initialized to `nullptr`, eliminating double-free hazards during node instantiation.
- **Unified Ambient Occlusion Controls:**
  - Wired `--no-ao` / `--ao` flags into CLI parsing (`CommandLine.cpp`), exposed `bgl_toggle_ao()` in Emscripten WASM exports (`main.cpp`), and added unit test coverage in `test_command_line.cpp`.

---

## [0.8.0] - 2026-10-24

### Added
- **HDR Render Target (RGBA16Float) & Tonemapping Post-Processing:**
  - Implemented 4x MSAA `WGPUTextureFormat_RGBA16Float` HDR scene rendering resolved to `hdrResolveTexture_`, followed by a fullscreen tonemapping pass onto the swapchain surface (`BeginPostProcessPass()`).
  - Added selectable tonemappers with real-time exposure control: **Khronos PBR Neutral**, **AgX** (Blender 4.0 curve), and **ACES Filmic**, with automatic linear-to-sRGB conversion when rendering to linear swapchains.
- **HDR Environment Maps (.hdr / .exr) & True IBL:**
  - Added Radiance `.hdr` (RGBE with adaptive RLE) and OpenEXR `.exr` (FP16/FP32 scanline) image decoders (`TextureDecoder::DecodeHDR`, `TextureDecoder::DecodeEXR`).
  - Implemented $L_2$ Spherical Harmonics (9 coefficients) CPU diffuse irradiance reconstruction and convolved sun/sky extraction.
  - Added WebGPU Compute Shaders (`@compute`):
    - `kBrdfLutComputeWGSL`: computes the $128\times 128$ `RGBA16Float` Split-Sum BRDF LUT.
    - `kGgxPrefilterComputeWGSL`: computes GGX importance-sampled specular cubemap mip chains across roughness levels into `texture_storage_2d_array<rgba16float, write>`.
- **Selectable & Rotatable HDR Environments:**
  - Added 4 built-in procedural HDR environments (*Mediterranean Courtyard*, *Sunset Studio*, *Overcast Atelier*, *Night Neon*) plus custom `.hdr`/`.exr` loading via drag & drop and CLI.
  - Added environment rotation (`envRotation`) affecting IBL diffuse/specular reflections and rotating the directional sun shadow map.
  - Skybox background modes: **Sharp** (Mip 0), **Blurred** (prefiltered specular LOD), and **Hidden** (dark studio background with active IBL).
- **glTF KHR_lights_punctual & Clustered Forward+ Shading:**
  - Parsed `asset.lights` and `node.lightIndex` in `FastGltfLoader` for Directional, Point, and Spot lights.
  - Implemented a Forward+ 3D frustum cluster grid ($16 \times 8 \times 8 = 1024$ clusters) with per-cluster 32-bit light masks and angular spot falloff in WGSL.
- **Native Block-Compressed GPU Textures:**
  - Enabled `WGPUFeatureName_TextureCompressionBC`, `WGPUFeatureName_TextureCompressionASTC`, and `WGPUFeatureName_TextureCompressionETC2`.
  - Transcoded KTX2 Basis Universal textures directly into hardware formats (`BC7_RGBA`, `ASTC_4x4_RGBA`, `ETC2_RGBA8`) with full 4x4 block-aligned mip chains uploaded straight to WebGPU.
- **Ambient Occlusion & Micro-Cavity Control:**
  - Added glTF ambient occlusion support with real-time toggle hotkey `[U]` and HUD telemetry display.
- **Accurate Triangle Wireframe Edge Generation:**
  - Added `gpuWireframeIndexBuffer` with 6 line indices per triangle $(a \to b, b \to c, c \to a)$ so wireframe rendering draws complete, closed geometric wireframe triangles without edge dropouts.
- **Centralized Engine Versioning (`bgl::core::Version.hpp`):**
  - Unified branding and version strings across desktop viewer, CLI, telemetry HUD, CMake, and WebAssembly web interface.

### Fixed
- **WebGPU Attachment Format & Depth Stencil State Alignment:**
  - Ensured all 3D scene and skybox render pipelines target `RGBA16Float` within the HDR pass.
  - Explicitly configured `TextRenderer` with `depthStencil = nullptr` matching the swapchain post-process pass attachment descriptor.
- **Overdraw Optimization via Deferred Skybox Rendering:**
  - Rendered the Atmospheric Skybox ($Z = 1.0$) after opaque geometry, allowing the GPU depth test to reject occluded skybox fragments and eliminating atmospheric shader overdraw on all geometry pixels.
- **MemoryPool Move Semantics Safety (`bgl::core::MemoryPool`):**
  - Implemented move constructor and assignment with `std::exchange` to safely nullify `freeHead_` and reset `allocatedCount_` on source instances.
- **Camera Framing Radius Calculation:**
  - Corrected bounding sphere radius calculation from `AABB::Extents()` by removing redundant 0.5x scaling.
- **CLI Resolution Input Validation:**
  - Added range clamping $[64, 16384]$ on `--width` and `--height` arguments.
- **Asset Ingestion Logging Overhead:**
  - Reduced per-primitive and per-attribute logging from `INFO` to `DEBUG` in `FastGltfLoader`, dramatically accelerating large scene ingestion.

---

## [0.7.2] - 2026-10-22

### Fixed
- **Audio Engine (`bgl::audio::SoundEngine`):** Stored `ma_sound` instances as `std::unique_ptr<ma_sound>` in `activeSounds_` (preventing dangling internal miniaudio pointers caused by value-copying) and properly stopped/uninitialized existing sounds before reusing an ID.
- **Web Inspector Security (`web/app.js`):** Replaced `innerHTML` assignments in `showNodeDetails` with DOM elements and `textContent` to eliminate XSS vectors, escaped node search queries via `CSS.escape()` in `filterNodes`, and ignored keyboard shortcuts when `Ctrl`/`Alt`/`Meta` modifiers are held.
- **Wireframe Buffer Synchronization (`bgl::gpu::PipelineManager`):** Added `BatchSceneAABBs()`, `BatchQuadtreeBounds()`, and per-frame aligned dynamic vertex buffer offsets (`aabbFrameByteOffset_`) in `FlushAABBLines()` so simultaneously active Scene AABBs and Quadtree Bounds are rendered in a single batch without overwriting offset 0.
- **Texture Decoder Hardening (`bgl::scene::TextureDecoder`):**
  - Isolated `setjmp`/`longjmp` error-recovery scopes in `DecodePNG` and `DecodeJPEG` from C++ objects with non-trivial destructors (`std::vector`, `DecodedImage`).
  - Added `png_set_add_alpha` for palette PNGs (`PNG_COLOR_TYPE_PALETTE`) without `tRNS` chunks so 8-bit palette textures reliably expand to 4-byte RGBA.
  - Enforced 64-bit `size_t` dimension arithmetic and `16384x16384` dimension bounds across PNG, JPEG, WebP, and KTX2 decoders, and queried `ktxTexture_GetImageOffset` for level-0 KTX2 mip data.
- **Ray-Picking World-Space AABB & Index Safety (`bgl::scene::RayPicker`):** Fixed `RayPicker::Pick()` to transform primitive AABBs by `worldMat` before testing ray intersection, added index buffer bounds validation, and wired interactive ray-picking (middle-click / double-click) into `Application::Update()`.

### Changed
- **glTF 2.0 Color Space, Normal Matrix & Extensions (`bgl::gpu::PipelineManager`, `bgl::scene::FastGltfLoader`):**
  - Uploaded `baseColor`, `emissive`, and `sheen` textures as `WGPUTextureFormat_RGBA8UnormSrgb` while keeping `normal`, `metallicRoughness`, and `occlusion` maps linear (`WGPUTextureFormat_RGBA8Unorm`).
  - Transformed vertex normals in `vs_main` using the $3\times 3$ cofactor/inverse-transpose matrix of `ubo.model` for accurate shading under non-uniform scaling, restricted backface normal flipping to `DoubleSided` materials, and removed artificial `baseColor` luminance derivative noise from materials with tangent-space normal maps.
  - Added support for `KHR_texture_basisu` (`basisuImageIndex`) and `EXT_texture_webp` (`webpImageIndex`), enabled `fastgltf::Options::LoadExternalBuffers`, respected `asset.defaultScene`, and added `bufferView` slice bounds checks.
- **Modern C++23 Refactoring (`bgl::gpu::Handle`, `bgl::gpu::Context`, `bgl::gpu::PipelineManager`, `bgl::text`, `bgl::scene`, `bgl::core`):**
  - Introduced `bgl::gpu::Handle<T, Deleter>` and `constexpr ToWgpuStringView(std::string_view)` in [`src/gpu/Handle.hpp`](src/gpu/Handle.hpp) and migrated all WebGPU resource-owning classes (`Context`, `PipelineManager`, `Skybox`, `FontAtlas`, `TextRenderer`) to zero-overhead RAII handles, eliminating manual `wgpu...Release` boilerplate and `strlen(...)` calls.
  - Adopted C++23 explicit object parameters / deducing `this` (`[&](this auto&& self, ...)`) for recursive lambdas (`Application::CacheSceneMetrics`, `FastGltfLoader::LoadFromMemory`, `RayPicker::Pick`), `std::bit_width` (`<bit>`) in `PipelineManager::CalculateMipLevels`, `std::to_underlying` (`<utility>`) in `PipelineManager::RenderPrimitive`, `std::ranges::equal` / `std::ranges::find_if`, `std::span` for HarfBuzz glyph buffers (`TextShaper::ShapeUTF8`), and `std::unique_ptr` custom deleters for C library handles (`hb_buffer_t`, `CURL`, `ktxTexture`, `WebPFree`).
  - Centralized scene loading in `Application::LoadScene()`, ordered `Application::Shutdown()` so WebGPU resources are released before `SDL_DestroyWindow()`, and implemented native SDL2 `SDL_GameController` polling in `Gamepad::Update()`.

---

## [0.7.1] - 2026-10-22

### Changed
- **Modular Source Organization (`bgl::scene` & `bgl::core`):**
  - Extracted `MeshPrimitive`, `SceneNode`, and `Scene` method implementations (including multithreaded MikkTSpace/Lengyel `ComputeTangents()`, hierarchical `UpdateTransforms()`, and WebGPU resource cleanup) from [`src/scene/Scene.hpp`](src/scene/Scene.hpp) into a dedicated compilation unit [`src/scene/Scene.cpp`](src/scene/Scene.cpp), removing `<thread>` from public headers and ensuring every subsystem in `src/scene/` follows a clean `.hpp`/`.cpp` separation.
  - Added [`core::AABB::Transformed()`](src/core/Types.hpp) to centralize 8-corner world-space AABB transformation across `SceneNode::UpdateTransforms()`, `PipelineManager::PrepareScene()`, and `PipelineManager::RenderSceneAABBs()`.
- **Git Commit History Standardization:**
  - Standardized historical git commit messages across the repository to follow clear, descriptive Conventional Commits formatting (`feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `ci`, `chore`).

---

## [0.7.0] - 2026-10-22

### Added
- **HTTP/HTTPS URL Support & External glTF URI Streaming via `libcurl` (`bgl::core::NetworkFetcher`):**
  - Integrated **`libcurl`** (`BGL_HAS_CURL`) on native desktop and Fetch API on WebAssembly to stream `.glb` and `.gltf` models directly from `http://`, `https://`, `file://`, and `data:` URIs via CLI (`--asset <url>` or positional argument) and programmatic APIs (`Application::LoadSceneFromUri()`).
  - **URL Drag-and-Drop:** Added support for dragging and dropping URLs (`SDL_DROPFILE` / `SDL_DROPTEXT` on native SDL2 desktop and `text/uri-list` / `text/plain` in `web/app.js`, plus `?model=<url>` browser query parameter).
  - **External glTF Buffer & Texture URI Resolution:** Upgraded `FastGltfLoader::LoadFromMemory()` to parse both binary `.glb` and JSON `.gltf` files (`parser_.loadGltf`) and automatically resolve relative or absolute external URIs (`NetworkFetcher::ResolveRelativeUri`) for binary geometry buffers (`.bin`) and external textures (`.png`, `.jpg`, `.webp`, `.ktx2`), downloading textures concurrently across `std::jthread` worker threads via thread-safe `libcurl` handles.
- **Quadtree Hierarchical Occlusion Culling & Performance Optimizations (`[C]` / `--no-occlusion` / `--occlusion`):**
  - Implemented `bgl::scene::OcclusionBuffer` (an L1-cache-resident $32\times 16$ screen-space Hierarchical-Z depth tile buffer), Front-to-Back child quadrant and primitive distance sorting, and sub-pixel screen-space contribution culling in `Quadtree::QueryVisible()`.
  - Added static shadow map caching (`shadowMapDirty_` in `PipelineManager::RenderShadowPass()`), hardware back-face culling (`opaqueBackCulledPipeline_` with `WGPUCullMode_Back`) for single-sided meshes, pre-cached AABB wireframe geometry (`cachedSceneAabbVertices_`), and unlocked present mode (`WGPUPresentMode_Immediate` / `Mailbox` without artificial sleep throttling).
- **Bump & Tangent-Space Normal Mapping (`[V]` / `--no-bump` / `--bump`):**
  - Automatic MikkTSpace / Lengyel triangle tangent & bitangent basis generation (`MeshPrimitive::ComputeTangents()`) with Gram-Schmidt orthogonalization and handedness sign (`tangent.w`) whenever a glTF primitive omits explicit `TANGENT` attributes or provides degenerate tangents.
  - Scale-invariant `getTBN` normalization in `vs_main` and `fs_main` (preserving unit-length tangents on scaled scenes such as `Sponza.glb` with `scale = 0.008`), paired with true UV-texel-space central-difference height gradients (`dHdU`, `dHdV`) via `textureSampleGrad` on `baseColorTexture` and `occlusionTexture`, `bumpCavity` self-occlusion, and courtyard fill lighting (`L_fill`).
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
- **Expanded Unit Test Suite (`65` Total Tests across 12 Suites):**
  - Added comprehensive tests in `tests/test_rendering_features.cpp`, `tests/test_quadtree_lod.cpp`, and `tests/test_command_line.cpp` covering Bump Mapping (including scaled model matrix TBN invariance), Mipmapping, Shadow Mapping, Environment Cubemap Reflections, Quadtree Hierarchical Occlusion Culling, `NetworkFetcher` URI classification/resolution, `data:`/`file://` fetching, and end-to-end HTTP/1.1 `libcurl` streaming of external `.gltf` + `.bin` + `.png` assets.

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

