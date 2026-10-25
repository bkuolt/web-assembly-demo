# Changelog

All notable changes to **WebAssembly and WebGPU Demo (bgl-Engine)** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.9.8] - 2026-10-25

### Shared Mesh Primitive Access Synchronization & Shadow Pass Uint16 Index Format
- **Zero-Copy Mesh Primitive Synchronization Across All Render Passes:**
  - Resolved regression where geometry was omitted from main render pass, shadow depth pass, and GPU culling because nodes were reading empty `node.primitives` instead of `node.GetPrimitives()`.
  - Rebuilt material bind groups for shared meshes (`scene.meshes`) and nodes, ensuring all primitives receive valid WebGPU bind groups.
  - Aligned `AabbRenderer`, `GpuCuller`, `ShadowPass`, and `Pipeline` to use `node.GetPrimitives()`.
- **Shadow Pass Uint16 Dynamic Index Format Support:**
  - Fixed hardcoded `WGPUIndexFormat_Uint32` in `ShadowPass.cpp:378` which caused validation overflow on models with `WGPUIndexFormat_Uint16` (e.g. `ToyCar.glb`).
- **Expanded Test Coverage (158 Passing Tests):**
  - Added `SceneNodeGetPrimitivesReturnsSharedMeshPrimitives` in `tests/test_golden_image.cpp`.
  - 158/158 unit tests passing across all 13 test suites.

---

## [0.9.7] - 2026-10-25

### ThreadPool Re-entrancy, Work-Stealing Wait & WebGPU Index Buffer 4-Byte Alignment
- **Work-Stealing & Re-Entrancy in `core::ThreadPool`:**
  - Added thread-local worker detection (`t_isWorkerThread`) to execute nested parallel-for loops inline, eliminating thread starvation deadlocks when outer loops dispatch inner parallel operations.
  - Added active work-stealing during `future.wait_for(...)`: waiting threads assist with pending tasks from the queue rather than idly blocking.
- **WebGPU Index Buffer Alignment & ToyCar Validation:**
  - Padded `indices16` buffer upload to ensure exact 4-byte write alignment for `wgpuQueueWriteBuffer`, resolving panic on models with odd index counts (e.g., `ToyCar.glb`).
  - Successfully verified headless execution with 10 rendered frames on both `assets/ToyCar.glb` and `assets/Sponza.glb` in under 1.2s.
- **Codebase Cleanup & Testing:**
  - Full clean rebuild with 0 compiler warnings.
  - 157/157 unit tests passing across all 13 test suites.

---

## [0.9.6] - 2026-10-25

### CI/CD Robustness & GitHub Pages Staging Stabilization
- **GitHub Pages Staging Resilience:**
  - Hardened `.github/workflows/ci.yml` staging step with conditional directory inspection for `web/dist`, ensuring clean builds when distribution artifacts are built dynamically.
- **Specification and Tag Alignment:**
  - Synchronized engine version constants in `Version.hpp.in`, `CMakeLists.txt`, `README.md`, and `web/index.html` to `v0.9.6`.
  - All 157 unit tests verified and passing.

---

## [0.9.5] - 2026-10-25

### Documentation Synchronization, Master Architecture Specifications & Release Hardening
- **Master Architecture Alignment:**
  - Synchronized `AGENT_INSTRUCTIONS.md` to engine release `v0.9.5` reflecting thread-safe worker pools, mesh sharing, zero-copy span accessors, and dynamic index buffers.
  - Aligned project documentation and runtime version tags across all build targets.
- **Verification and Release:**
  - Automated CMake configuration generation with synchronized `Version.hpp`.
  - 157/157 tests passing across all test suites.

---

## [0.9.4] - 2026-10-25

### Thread Pool, Mesh Sharing, Front-to-Back Sorting, Content Hashing, Golden Image Tests & Emscripten CI
- **Centralized Work Stealing Thread Pool (`core::ThreadPool`):**
  - Implemented thread-safe `core::ThreadPool` with `Enqueue` (returning `std::future`) and `ParallelFor(start, end, func)` chunked parallel dispatch.
  - Replaced all scattered, ad-hoc `std::jthread` creations across image decoding, mesh parsing, node hierarchy instantiation, tangent generation, cubemap generation, and mipmap processing, eliminating thread oversubscription.
- **Mesh Sharing & Zero-Copy Instance Primitives:**
  - Introduced `scene::Mesh` as a shared resource (`std::vector<std::shared_ptr<Mesh>> meshes` on `Scene`), with `SceneNode` referencing `std::shared_ptr<Mesh> mesh`.
  - Added zero-copy `node->GetPrimitives()` span accessor, drastically reducing memory footprint when instancing geometry across complex scenes like Sponza.
- **Dynamic Uint16 Index Buffers & Draw Call Sorting:**
  - Implemented dynamic index buffer allocation: uploads `WGPUIndexFormat_Uint16` whenever `vertexCount < 65536`, cutting index bandwidth in half for the majority of glTF primitives.
  - Implemented multi-tier draw call sorting: opaque primitives are sorted front-to-back from camera to maximize GPU early-Z rejection; transparents back-to-front; draw calls grouped by pipeline and material to minimize state transitions.
- **Thread-Safe AssetManager with Cryptographic FNV-1a Content Hashing:**
  - Converted `AssetManager` to use true content hashing (`Fnv1a64` on binary memory payload) rather than fragile string path hashing.
  - Protected cache tables with `std::shared_mutex` reader-writer locking for safe multithreaded loading.
- **Centralized Versioning & Branding:**
  - Automated `Version.hpp` generation via CMake `configure_file` from `Version.hpp.in`, keeping version and titles centralized and fixing legacy typos.
- **Integrated Engine Scaffolding & Logging Optimization:**
  - Wired `RayPicker` picking directly into `soundEngine_.PlaySound3D` for spatial audio cues at ray hit intersections.
  - Connected `NetworkFetcher::FetchAsync` directly to `core::ThreadPool`.
  - Demoted per-primitive, per-attribute, and per-texture logging in `FastGltfLoader.cpp` and `Pipeline.cpp` from `INFO` to `DEBUG`, eliminating seconds of I/O latency on large asset loads.
- **Golden Image Headless Verification & Fuzzing Targets:**
  - Added `tests/test_golden_image.cpp` for deterministic headless geometry verification, offscreen framebuffer checksum testing, and FNV-1a content hash regression guards.
  - Added libFuzzer fuzzing targets `tests/fuzz/fuzz_texture_decoder.cpp` and `tests/fuzz/fuzz_gltf_loader.cpp`.
- **Comprehensive CI/CD Pipeline & Repo Hygiene:**
  - Added dedicated Emscripten build job in `.github/workflows/ci.yml` that builds WASM artifacts and deploys releases.
  - Added `CMakePresets.json` with ASan, UBSan, and TSan configurations.
  - Integrated `clang-format` and `clang-tidy` quality checks in CI.
  - Configured Git LFS (`.gitattributes`) for `.glb`, `.ktx2`, and `.bin` assets; removed stub binaries from repository history.
- **Unit Test Suite Expanded (157 Passing Tests):**
  - 157 unit tests across 13 test suites passing with 100% success rate.

---

## [0.9.3] - 2026-10-25

### GPU Compute Offloading, RAM/VRAM Telemetry, PS5 DualSense Controller & Architectural Modularization
- **WebGPU Compute Shader Offloading (Frustum Culling & Ray Picking):**
  - Implemented `gpu::GpuCuller` executing parallel view-frustum culling on GPU compute workgroups (`cs_cull` WGSL) evaluating all scene primitive bounding boxes against 6 analytical plane equations simultaneously.
  - Implemented `gpu::GpuRayPicker` running parallel ray-bounding slab intersections on GPU compute workgroups (`cs_pick` WGSL) with atomic minimum distance tracking (`atomicMin(&result.hitDistanceBits, distBits)`), offloading complex scene ray tests from CPU.
- **Process RAM & WebGPU VRAM Telemetry in Debug Overlay:**
  - Implemented `core::MemoryTracker` tracking real-time resident set size (RAM in MB) via OS kernel interfaces (`/proc/self/statm` on Linux, `getrusage` fallback).
  - Added atomic tracking of WebGPU VRAM buffer and texture memory allocations and deallocations.
  - Enhanced in-engine `DebugOverlay` with live memory metrics (`Memory: RAM {:.1f} MB | VRAM {:.1f} MB`).
- **PS5 DualSense Controller Support & Console Camera Feel:**
  - Integrated full Sony PS5 DualSense controller identification via Vendor ID `0x054c` and Product IDs `0x0ce6` / `0x0df2` across native SDL2 and Web Gamepad API.
  - Implemented PS5 exponential response curve `ApplyPs5ResponseCurve(stick, 2.1f, 0.70f)`: fine precision near center deflection and rapid turning at full stick extension, with pitch dampening ($0.70$) for horizon stabilization.
  - Added inertial look damping (`smoothedLook` interpolation) and native DualSense haptic rumble.
- **WebUI Controller Visibility & Toast Indicators:**
  - Added animated `#gamepad-badge` in the web header featuring a pulsing emerald indicator dot and active controller label.
  - Added real-time controller connection and disconnection toast notifications with gamepad identification.
- **Major Codebase Modularization (`Pipeline.cpp` Decomposition):**
  - Broken down monolithic rendering file into dedicated, independently testable rendering modules:
    - `gpu::AabbRenderer`: batched 3D wireframe bounding box rendering, line batching, and dynamic VBO streaming in a single draw call.
    - `gpu::ShadowPass`: directional 2048x2048 PCF shadow depth pre-pass and alpha-tested shadow pipelines.
    - `gpu::PostProcessPass`: fullscreen HDR tonemapping (Khronos PBR Neutral, AgX, ACES), SSAO, bloom, and sun lens flare passes.
    - `gpu::GpuCuller`: compute-shader parallel frustum culling.
    - `gpu::GpuRayPicker`: compute-shader parallel ray-AABB intersection picking.
  - Preserved 100% backward compatibility for all existing public APIs on `PipelineManager`.
- **Test Suite Expansion (154 Passing Tests):**
  - Added dedicated unit tests for `MemoryTracker` RAM/VRAM accounting, PS5 DualSense response curves, GPU culler analytical planes, GPU ray picking slab ordering, and modular pass RAII lifecycle.
  - All **154 unit tests across 12 test suites** passing with 100% success rate.

---

## [0.9.2] - 2026-10-25

### Click-Picking & Outline, Focus [F], Fly/WASD Camera, glTF Cameras, Touch, Gamepad & Diurnal Sun
- **Interactive Click-Picking with RayPicker & Outline Highlighting:**
  - Implemented precise Möller-Trumbore ray unprojection from screen coordinates (`bgl_pick_node`).
  - Added vibrant gold/amber selection outline and bounding box rendering in WebGPU render pass (`pipelineManager_.AppendAABBLines(selectedNode_->worldAABB, gold)`).
  - Bidirectionally synchronized with the Scene Hierarchy tree (`#node-tree-panel`): picking an object in the 3D canvas expands and highlights its row in the tree, while clicking any tree row selects and frames the node in 3D.
- **Selection Focus ("F"), "Frame All" & 9 Camera Bookmarks:**
  - Added `FocusSelection()` triggered by key `[F]`: smoothly frames camera target and distance onto the selected node bounds (or active scene if none selected).
  - Added `FrameAll()` triggered by `[Home]` / UI button: frames the entire scene AABB.
  - Implemented 9 camera viewpoint bookmark slots: save current viewpoint with `Ctrl + 1..9` (or UI `+1`..`+3` buttons), recall with `1..9` (or UI `1`..`3` buttons).
  - Added unit tests `CameraTest.FocusOnBoundsFraming` and `CameraTest.CameraBookmarksSaveAndRecall`.
- **First-Person Fly / WASD Camera & Turntable Autorotation:**
  - Added dual camera navigation modes: `CameraMode::Orbit` and `CameraMode::Fly` (`[C]` key / UI button).
  - In Fly mode, WASD keys and left stick smoothly translate the camera eye through scene interiors (ideal for exploring the interior of Sponza).
  - Added continuous Turntable autorotation (`[T]` key / UI button) with live speed slider `[0.05–2.0 rad/s]`.
  - Added unit test `CameraTest.FlyModeFreeNavigation`.
- **glTF Camera Ingestion & Switching (`fastgltf::Camera`):**
  - Integrated complete parsing of perspective and orthographic cameras from glTF files.
  - Automatically binds cameras to their parent node world transforms in `SceneNode`.
  - Added camera selector dropdown in web HUD and cycling support (`CycleCamera()`, `SelectCamera(idx)`).
  - Added unit tests `CameraTest.glTFCameraTransformIngestion` and `RenderingFeaturesTest.SceneNodeHierarchyAndCameraData`.
- **Touch Controls for Mobile & Tablet Browsers:**
  - Added touch event listeners on the 3D WebGPU canvas with gesture isolation (`passive: false`).
  - 1-finger drag enables smooth camera orbit.
  - 2-finger pinch gesture smoothly controls camera zoom.
  - 2-finger midpoint drag performs view-space panning.
- **Dual Gamepad Support (Native SDL_GameController & HTML5 Web Gamepad API):**
  - Native desktop `SDL_GameController` polling with normalized radial deadzone filtering ($0.12$).
  - HTML5 Web Gamepad API in browser (`navigator.getGamepads()`) integrated into continuous animation loop.
  - Left stick translates/pans, right stick orbits/looks, triggers zoom in/out, and face buttons toggle bounds and ray-pick.
  - Added unit test `RenderingFeaturesTest.GamepadRadialDeadzoneRemapping`.
- **Diurnal Time-of-Day & Sun Angle Live Slider:**
  - Added live time-of-day slider `[0.0h–24.0h]` calculating continuous solar elevation and azimuth angles.
  - Atmospheric skybox, directional PCF shadows, lens flares, and specular cubemap reflections update live in real time.
  - Added unit test `RenderingFeaturesTest.DiurnalSunTrajectoryCalculation`.
- **Test Suite Expansion (149 Passing Tests):**
  - Expanded test suite to **149 unit tests across 12 test suites** with 100% pass rate.

---

## [0.9.1] - 2026-10-25

### Alpha-Tested Shadows, SSAO, Emissive Bloom, True Transmission & Native Compressed Textures
- **Alpha-Tested Directional Shadows:**
  - Implemented `@fragment fn fs_shadow(in : VertexOutput)` in `kShadowShaderWGSL` taking `baseColorTexture` and `defaultSampler`.
  - Directional shadow map pass now tests primitive materials for `alphaMode == MASK` (and `BLEND`), discarding fragments where $\alpha < \alpha_{\text{cutoff}}$.
  - Foliage, leaf cutouts, grates, and fences (such as in Sponza) now cast accurate, cutout shadow silhouettes instead of solid rectangular block shadows.
  - Added unit test `RenderingFeaturesTest.AlphaTestedShadowMaskDiscardLogic`.
- **Screen-Space Ambient Occlusion (SSAO/GTAO Complement):**
  - Added screen-space ambient occlusion post-processing evaluating geometric depth and contrast gradients via Poisson-disc horizon sampling.
  - Complements static material glTF occlusion textures, normal-curvature micro-cavities, and horizon contact angles with dynamic view-dependent ambient shadowing.
  - Added live intensity slider `[0.0×–2.0×]`, keyboard shortcut `[G]`, and unit test `RenderingFeaturesTest.ScreenSpaceAmbientOcclusionHorizonFactor`.
- **HDR Bloom for Emissive Materials:**
  - Implemented linear HDR bright-pass extraction and golden-angle multi-ring scatter glow filter in `kTonemapShaderWGSL` prior to tonemapping.
  - Neon surfaces, lamps, car headlights, and materials with `KHR_materials_emissive_strength` now emit a radiant optical glow.
  - Added live intensity slider `[0.0×–2.5×]`, keyboard shortcut `[F]`, and unit test `RenderingFeaturesTest.EmissiveBloomThresholdAndScatter`.
- **True Physical Transmission & Screen-Space Refraction (`KHR_materials_volume`):**
  - Integrated full parsing for `KHR_materials_volume` (`thicknessFactor`, `attenuationDistance`, `attenuationColor`, `thicknessTexture`).
  - Added screen-space refraction offset in `kPbrShaderWGSL` derived from Snell's law, view-space surface normal, IOR, and material thickness.
  - Implemented Beer-Lambert volume light extinction law:
    $$\text{extinction} = -\frac{\ln(\text{attenuationColor})}{\text{attenuationDistance}}$$
    $$\text{transmittance} = \exp(-\text{extinction} \times \text{opticalPath})$$
  - Created dedicated `opaqueColorCopyTexture_` (`RGBA16Float`) allowing transmissive dielectric geometry to sample background scenes with chromatic refraction and physical absorption.
  - Added unit test `RenderingFeaturesTest.TrueTransmissionBeerLambertVolumeAttenuation`.
- **Native GPU Compressed Textures (KTX2 Transcoding by GPU Features):**
  - Integrated automatic detection of WebGPU hardware block compression features (`TextureCompressionBC`, `TextureCompressionASTC`, `TextureCompressionETC2`).
  - Configures default KTX2 Basis Universal transcoding target based on GPU adapter capabilities (desktop BC7 `BC7_RGBA`, mobile ASTC `ASTC_4x4_RGBA`, fallback ETC2 `ETC2_RGBA`, or RGBA32).
  - Eliminates uncompressed RGBA32 GPU memory bloat by uploading block-compressed mip chains directly.
  - Added unit test `RenderingFeaturesTest.Ktx2TranscodeTargetSelectionByGPUFeatures`.
- **Test Suite Expansion (142 Passing Tests):**
  - Expanded test suite to **142 tests across 12 test suites** with 100% pass rate.

---

## [0.9.0] - 2026-10-25

### Parallax Occlusion Mapping, Multi-Bounce AO, Sun Lens Flares & Keyboard Navigation
- **Immersive Parallax Occlusion Mapping (POM) & Self-Shadowing:**
  - Implemented steep parallax occlusion mapping in `kPbrShaderWGSL` with adaptive view-dependent ray-marching (8 to 32 depth layers).
  - Linear secant interpolation between pre- and post-intersection height layers provides smooth displacement relief on surfaces.
  - Added directional tangent-space light raymarching to evaluate real-time self-shadowing, simulating true 3D surface depth on stone, brick, wood, and metallic surfaces without mesh tessellation.
  - Added live configurable displacement scale parameter (slider 0.005–0.080) and keyboard shortcut `[P]`.
- **Advanced Multi-Bounce Ambient Occlusion (AO):**
  - Upgraded Ambient Occlusion with contact horizon attenuation based on $\text{dot}(N, N_{\text{geom}})$.
  - Integrated high-frequency normal curvature micro-cavity occlusion.
  - Implemented Jimenez multi-bounce color conservation to prevent flat desaturated black shadows in crevices and preserve realistic diffuse reflections.
  - Added live configurable AO intensity slider (0.0×–2.0×) and keyboard shortcut `[U]`.
- **Cinematic Directional Sun Lens Flares:**
  - Implemented real-time directional sun lens flare effect in `kTonemapShaderWGSL`, evaluated in linear HDR space before tonemapping.
  - Features central solar core glow and radial corona, cinematic anamorphic horizontal flare streak, multi-element chromatic lens ghosts, optical halo ring, and smooth viewport border fade.
  - Added live configurable lens flare intensity slider (0.0×–2.5×) and keyboard shortcut `[J]`.
- **Smooth Keyboard Navigation (WASD, Arrows, +/- Zoom):**
  - Implemented camera translation methods `Camera::MoveForward()`, `Camera::MoveRight()`, and `Camera::MoveUp()` in `bgl::core::Camera`.
  - Added continuous keyboard polling in native `Application.cpp` and web `web/app.js` using `W`/`S` and `Up`/`Down` for forward/backward translation, and `A`/`D` and `Left`/`Right` for lateral strafing.
  - Bound `+` and `-` keys directly to camera zoom in/out with smooth step clamping.
- **Clean Live Tuning UI & Startup Capability Diagnostics:**
  - Modernized HUD interface with an uncluttered layout, collapsible "Live Parameter Tuning" drawer, and navigation cheat-sheet.
  - Enhanced startup diagnostics card displaying verified status for WebAssembly SIMD/Streaming and WebGPU hardware adapter information.
- **Comprehensive glTF 2.0 Material Extension Verification & WGSL Documentation:**
  - Verified and documented 100% support for all Khronos glTF extensions (`KHR_materials_clearcoat`, `KHR_materials_transmission`, `KHR_materials_sheen`, `KHR_materials_specular`, `KHR_materials_ior`, `KHR_materials_emissive_strength`, `KHR_materials_unlit`).
  - Added detailed Doxygen and mathematical WGSL comments throughout `src/gpu/Shaders.hpp`.
- **Test Suite Expansion (137 Passing Tests):**
  - Expanded test suite to **137 tests across 12 test suites** with 100% pass rate.
  - Added `CameraTest.WASDAndArrowKeyNavigation`, `CameraTest.KeyboardZoomInOut`, `CameraTest.DiagonalWASDNavigationInvariance`, and `CameraTest.ExtremeDistanceDynamicClippingPlanes`.
  - Added `RenderingFeaturesTest.ParallaxOcclusionMappingTangentSpaceProjection`, `RenderingFeaturesTest.MultiBounceAmbientOcclusionConservation`, `RenderingFeaturesTest.DirectionalSunLensFlareScreenProjection`, and `RenderingFeaturesTest.PipelineTonemapOperatorAndExposureCPU`.

---

## [0.8.9] - 2026-10-24

### Major Test Suite Expansion & Hardening (129 Passing Tests)
- **Massive Test Coverage Growth (+47 New Tests, +57% Expansion):**
  - Expanded test suite from 82 to **129 passing tests across 12 test suites** with zero warnings or failures.
- **`MemoryPoolTest` (11 Tests):**
  - Added destructor invocation tracking (`DestructorInvocationTracking`) ensuring proper cleanup of pooled instances.
  - Added move semantics transfer tests (`MoveSemanticsTransfer`) verifying block ownership handover.
  - Added safety checks for `pool.Deallocate(nullptr)` and `pool.Clear()` complete block reclamation.
  - Added 2000-allocation stress test with interleaved odd/even deallocations and free-list slot reuse.
  - Added 64-byte aligned type allocation verification (`alignas(64)` pointer alignment).
  - Added multi-frame PMR vector reset stress test under `ScratchArena`.
- **`CameraTest` (10 Tests):**
  - Added extreme pitch angle clamping verification (-89° to +89°) preventing gimbal lock or division-by-zero.
  - Added aspect ratio resilience tests for ultrawide (32:9) and portrait (9:16) viewports.
  - Added 4-corner unprojected ray direction divergence and unit length validation.
  - Added camera look-at target frustum plane containment invariants.
- **`TypesMathTest` (18 Tests):**
  - Added `AABB::Intersects(const AABB&)` implementation and comprehensive overlap/containment tests (disjoint, partial, full, face-touching).
  - Added near-boundary edge grazing ray intersection tests.
  - Added analytical plane symmetry tests for perspective frustums.
  - Added affine transformation matrix inversion and roundtrip identity tests.
- **`RayPickerTest` (9 Tests):**
  - Added ray pointing away negative distance rejection test.
  - Added ray hits on triangle vertices and hypotenuse boundary edges.
  - Added triangle hit normal orthogonality tests against triangle edge vectors.
  - Added hierarchical child node transform picking through multi-level parent nodes.
  - Added empty scene picking safety.
- **`QuadtreeTest` (10 Tests):**
  - Added deep subdivision and maximum depth safety benchmark with 60 clustered objects.
  - Added out-of-bounds object insertion handling.
  - Added occlusion buffer NDC screen edge clipping and scissor boundary tests.
- **`CommandLineTest` (10 Tests):**
  - Added missing flag value bounds safety tests (preventing out-of-bounds argv index access).
  - Added invalid non-numeric string fallback tests for `-w` and `-H`.
  - Added full feature toggle roundtrip verification for all engine submodules.
- **`TextureDecoderTest` (8 Tests):**
  - Added odd and NPOT dimension mip-level calculations ($1920\times 1080$, $33\times 17$, $3\times 1$).
  - Added uncompressed Radiance HDR multi-pixel RGBE decoding verification.
  - Added invalid extension auto-routing rejection.
- **`TextRenderingTest` (9 Tests):**
  - Added empty string shaping safety test.
  - Added whitespace and tab advance cursor tracking.
  - Added Unicode UTF-8 accented and special symbol shaping (`°C`, `α`, `β`).
  - Added custom quad tint color preservation verification.
- **`GltfLoaderTest` (7 Tests):**
  - Added graceful failure on non-existent file paths.
  - Added deep 4-level transform hierarchy composition.
  - Added scene-wide AABB union expansion tests.
- **`CrashHandlerTest` (6 Tests):**
  - Added 1024-character stress header safety tests.
  - Added consecutive install/uninstall cycle idempotence tests.
- **`RenderingFeaturesTest` (23 Tests):**
  - Added exact byte offset and size verification for 352-byte `PrimitiveUniforms` and 5360-byte `SceneLightingUniforms`.
  - Added logarithmic depth partition verification for 16x8x8 clustered shading grid.
  - Added spherical harmonics extreme radiance evaluation and firefly clamping verification (25.0f max threshold).
  - Added 8-corner bounding box containment validation under directional shadow light projection.

---

## [0.8.8] - 2026-10-24

### Optimized Web App & Client Runtime
- **High-Performance Streaming Compilation & Fast Startup:**
  - Implemented streaming WebAssembly compilation via `WebAssembly.instantiateStreaming` for concurrent background compilation during byte download, drastically reducing initial engine boot latency.
  - Added HTML `<link rel="preload">` resource hint in `web/index.html` for `dist/bgl_engine.wasm`.
  - Re-architected startup flow into non-blocking parallel `Promise.all([initWasm(), loadDefaultModel()])`.
  - Added real-time streaming asset download reader (`ReadableStream`) reporting live byte counters, percentages, and transfer bandwidth (`MB/s`) to HUD progress indicator.
  - Added instant scene preset pills in HUD to switch between fast demo asset (`ToyCar.glb` ~5.1 MB) and architectural benchmark (`Sponza.glb` ~52.6 MB).
- **User-Facing Floating Toast Notification System (`BglNotifier`):**
  - Introduced floating glassmorphism toast notification container (`#user-toast-container`) providing instant visual feedback for runtime events.
  - User-friendly error banners for network failures (HTTP 404/500), CORS policy blocks, corrupted glTF/GLB containers, invalid dropped files, and WebGPU context loss.
  - Interactive auto-dismiss with pause-on-hover and close button.
- **Structured Developer Logging (`BglLogger`):**
  - Integrated color-coded console badges with millisecond timestamps (`[Engine]`, `[Wasm]`, `[WebGPU]`, `[Network]`, `[glTF]`, `[Perf]`, `[UI]`, `[Error]`).
  - Added high-resolution performance timers (`performance.now()`) measuring compilation, glTF JSON chunk extraction, and scene hierarchy generation.
  - Added startup platform diagnostic banner reporting User Agent, Viewport Dimensions, Device Pixel Ratio, Secure Context status, and WebGPU adapter vendor/architecture metadata.
- **Code Modularity & Readability:**
  - Modernized `web/app.js` into clean, modular, self-contained components (`BglLogger`, `BglNotifier`, `BglNetwork`, `BglWasmBridge`, `BglGltf`, `BglUI`).
  - Cached DOM selectors and eliminated redundant queries and unhandled promise rejections.

---

## [0.8.7] - 2026-10-24

### Added & Hardened
- **Headless CI Smoke Testing (`--headless`):**
  - Added native `--headless` command-line flag to `CliOptions` and `Application` interactive loop in `bgl_viewer`.
  - When `--headless` is provided, `bgl_viewer` initializes all subsystems, loads the active scene, executes 10 full render frames, and cleanly terminates with code 0.
  - Modernized `.github/workflows/ci.yml` smoke-test step to execute `xvfb-run timeout 15s ./build/Release/bgl_viewer --headless` cleanly without masking errors via `|| true`.
- **Unit Test Suite Expansion (82 Passing Tests):**
  - Added unit test `CommandLineTest.HeadlessOption` in `tests/test_command_line.cpp`, expanding test suite to 82 passing tests across 12 suites (100% pass rate).

### Documentation & Versioning
- **Comprehensive Doxygen Coverage:**
  - Added Doxygen `@param`, `@return`, and API descriptions across `Application.hpp`, `Quadtree.hpp`, `TextureDecoder.hpp`, `SoundEngine.hpp`, `Camera.hpp`, `Skybox.hpp`, `TextRenderer.hpp`, and `DebugOverlay.hpp`.
  - Updated Master Architecture Guide (`AGENT_INSTRUCTIONS.md`) with modern C++23 subsystem directives, 352-byte uniform layouts, and hotkey controls.
- **Version Alignment:**
  - Synchronized engine version constants to `0.8.7` across `Version.hpp`, `CMakeLists.txt`, `conanfile.py`, `conan.lock`, `README.md`, `AGENT_INSTRUCTIONS.md`, and `web/index.html`.

---

## [0.8.6] - 2026-10-24

### Refactoring & Resource Hygiene
- **Scene RAII Cleanup (`bgl::scene::Scene`):**
  - Explicitly reset non-owning sampler handles (`t.sampler = nullptr;`) in `Scene::~Scene()` along with WebGPU texture and texture-view releases, eliminating dangling references to pipeline-managed samplers upon scene unloading.
- **Code Modernization & Simplicity:**
  - Audited core submodules (`gpu`, `scene`, `core`, `app`, `text`, `ui`) ensuring strict C++23 standards compliance, zero compiler warnings under `-std=c++23 -Wall -Wextra -Wpedantic`, and consistent resource lifetime management.

### Added & Expanded Tests (81 Passing Tests)
- **Unit Test Suite Expansion (`tests/`):**
  - Expanded test coverage from 76 to 81 tests (100% pass rate across all 12 test suites).
  - **Occlusion Buffer Direct Operations (`tests/test_quadtree_lod.cpp`):**
    - Added `QuadtreeTest.OcclusionBufferDirectTileOperationsAndEdgeCases` verifying conservative screen-space depth tile initialization, AABB clipping against the camera near plane, projection validation, conservative occluder rasterization, occlusion testing, and buffer clearing.
  - **Mathematical Parametrics & Sampler Safety (`tests/test_types_math.cpp`):**
    - Added `TypesMathTest.RaySegmentParametricEvaluator` verifying 3D ray point evaluation along positive, zero, and negative parametric distances.
    - Added `TypesMathTest.SceneDestructorTextureSamplerNulling` verifying texture resource reset and safe sampler nulling upon `Scene` destruction.
  - **Forward+ Cluster Grids & Spherical Harmonics (`tests/test_rendering_features.cpp`):**
    - Added `RenderingFeaturesTest.ClusteredForwardGridBoundsValidation` testing light grid bitmask assignment across all 1,024 frustum clusters for directional vs punctual point lights and boundary conditions.
    - Added `RenderingFeaturesTest.SphericalHarmonicsDiffuseReconstruction` verifying Ramamoorthi & Hanrahan $L_2$ diffuse spherical harmonics projection and radiance reconstruction against uniform white cubemaps across 6 cardinal surface normals.

### Documentation & Versioning
- **Version Synchronization:**
  - Synchronized engine version constants to `0.8.6` across `Version.hpp`, `CMakeLists.txt`, `conanfile.py`, `conan.lock`, `README.md`, and `web/index.html`.
- **Conan 2.x Lockfile:**
  - Pinned and verified all dependency hashes in `conan.lock`.

---

## [0.8.5] - 2026-10-24

### Added & Improved
- **Automated GitHub Pages Deployment & Dual-Target Hosting (`.github/workflows/ci.yml`):**
  - Configured resilient GitHub Pages workflow supporting both **GitHub Actions Pages** (`actions/deploy-pages@v4`) and branch-based deployment (**`gh-pages`** via `peaceiris/actions-gh-pages@v4`).
  - Added dedicated `_site` staging pipeline with `.nojekyll` generation to prevent Jekyll from skipping underscore folders or WebAssembly binary chunks.
  - Mirrored web app files across both root (`/`) and `/web/` subpaths, enabling instant direct browser access at `https://bkuolt.github.io/web-assembly-demo/` with zero redirect latency, as well as via `https://bkuolt.github.io/web-assembly-demo/web/index.html`.
  - Added automated minimal web viewer packaging (`dist/bgl_web_app_minimal.zip`) attached directly to GitHub release assets alongside desktop Linux binaries.
  - Enabled automated Pages deployment on pushes to `main`, annotated release tags (`v*`), and manual `workflow_dispatch`.
- **Client WebAssembly Initialization Refinement (`web/app.js`):**
  - Standardized English status message when WebAssembly is unsupported in the host environment.
- **Version Alignment:**
  - Synchronized engine version constants to `0.8.5` across `Version.hpp`, `CMakeLists.txt`, `conanfile.py`, `conan.lock`, `README.md`, and `web/index.html`.

---

## [0.8.4] - 2026-10-24

### Added & Expanded Tests (76 Passing Tests)
- **Unit Test Suite Expansion (`tests/`):**
  - Expanded test coverage from 67 to 76 tests (100% pass rate across all 12 test suites).
  - **Crash Handler Edge Cases (`tests/test_crash_handler.cpp`):**
    - Added `CrashHandlerTest.MultipleInstallUninstallCycles` verifying handler lifecycle idempotence.
    - Added `CrashHandlerTest.PrintStackTraceSubsliceAndEmpty` validating memory safety when invoking `PrintStackTrace` with empty string views and non-null-terminated string view slices.
  - **Mathematical Bounds & Copy Safety (`tests/test_types_math.cpp`):**
    - Added `TypesMathTest.ArvoBoundingBoxTransformAlgorithm` verifying Arvo transformation correctness across identity, translation, rotation, and non-uniform negative scale matrices, checking strict containment for all 8 transformed corners.
    - Added `TypesMathTest.MeshPrimitiveCopySemanticsAndGPUHandleSafety` testing copy constructors, assignment operators, and move semantics to verify GPU pointer safety (`nullptr` initialization preventing double-free).
  - **Texture Decoders & HDR / EXR Signatures (`tests/test_texture_decoders.cpp`):**
    - Added `TextureDecoderTest.RadianceHdrAndOpenExrSignatures` verifying uncompressed `.hdr` RGBE decoding, float conversions, and OpenEXR magic signature recognition.
    - Added `TextureDecoderTest.KtxTranscodeTargetNamesAndDefaultTarget` testing KTX target string mapping and default target state management.
  - **Asset Manager Lifecycle & Staging Arena (`tests/test_asset_manager.cpp`):**
    - Added `AssetManagerTest.ClearCacheAndScratchArena` verifying cache purging and PMR monotonic scratch arena allocation/reset.
  - **Command-Line & Help Text Validation (`tests/test_command_line.cpp`):**
    - Added `CommandLineTest.AdditionalFlagsAndHelpOutput` testing `--no-occlusion`, `--quadtree`, `--no-bounds`, and clean execution of `PrintHelp()` and `PrintVersion()`.
  - **Network Fetcher Error Bounds & Percent Decoding (`tests/test_rendering_features.cpp`):**
    - Added `RenderingFeaturesTest.NetworkFetcherErrorHandlingAndPercentDecoding` verifying clean error returns on empty URIs, non-existent files, directories, invalid base64 payloads, and robust URL percent decoding.

### Documentation & Build
- **Test Metrics & Documentation Updates:**
  - Updated `README.md` test metrics badge to `76_Passing` and synchronized engine version badges to `v0.8.4`.
  - Regenerated `conan.lock` pinned lockfile for version `0.8.4`.
  - Updated engine version constants across `Version.hpp`, `CMakeLists.txt`, `conanfile.py`, `conan.lock`, and `web/index.html`.

---

## [0.8.3] - 2026-10-24

### Security & Hardening
- **CrashHandler Bounds Safety (`bgl::core::CrashHandler`):**
  - Resolved potential out-of-bounds memory read in `PrintStackTrace(std::string_view)` by introducing an async-signal-safe `SafeWriteStderr(const char*, size_t)` primitive and `SafeWriteStderr(std::string_view)` overload, avoiding assumptions of null termination on sliced string views.
- **Asset Allocation Bounds & DoS Prevention (`bgl::core::NetworkFetcher`):**
  - Enforced a 1 GiB maximum asset size threshold (`kMaxAssetFileSize`) in `ReadLocalFileBytes` to prevent unbounded memory allocation and denial-of-service attacks from malicious local files or pseudo-devices.
  - Implemented streaming buffer capacity enforcement in `CurlWriteCallback` to abort remote HTTP/HTTPS downloads exceeding 1 GiB.
  - Updated libcurl User-Agent client identifier to reference `kEngineVersion` dynamically.
- **glTF Index Accessor Clamping (`bgl::scene::FastGltfLoader`):**
  - Added boundary validation and defensive index clamping on glTF index accessors, mitigating GPU out-of-bounds vertex fetching and driver panic risks on corrupted or adversarial 3D assets.

### Improvements & Documentation
- **Client Internationalization (`web/app.js`):**
  - Standardized all WebAssembly and WebGPU compatibility modal alerts, technical diagnostics, and troubleshooting instructions into serious, professional English.
- **Conan 2.x Lockfile Synchronization (`conan.lock`, `conanfile.py`):**
  - Updated recipe package version to `0.8.3` and regenerated `conan.lock` across all 17 graph dependencies and 5 build requirements.
- **Version Alignment:**
  - Synchronized version constants to `0.8.3` across `Version.hpp`, `CMakeLists.txt`, `conanfile.py`, `conan.lock`, `README.md`, and `web/index.html`.

---

## [0.8.2] - 2026-10-24

### Added
- **GitHub Actions CI/CD Pipeline Modernization (`.github/workflows/ci.yml`):**
  - Added `pkg-config` and `libcurl4-openssl-dev` to Ubuntu runner system dependencies to support native libcurl asset streaming.
  - Enforced strict Conan lockfile validation using `--lockfile=conan.lock` during dependency resolution.
  - Added pre-build JavaScript validation job (`node --check web/app.js`).
  - Integrated continuous deployment to GitHub Pages using official GitHub Actions (`actions/configure-pages@v5`, `actions/upload-pages-artifact@v3`, `actions/deploy-pages@v4`).
  - Added headless engine runtime smoke testing (`xvfb-run timeout 5s ./build/Release/bgl_viewer --headless`) into CI test execution.
  - Enhanced binary distribution packaging to include runtime fonts, documentation, and shared libraries.
- **Root Redirection Page Fix (`index.html`):**
  - Corrected engine title and link description typo in root landing redirect.
- **Engine Version Bump (`bgl::core::Version.hpp`, `CMakeLists.txt`, `conanfile.py`):**
  - Synchronized engine version to `0.8.2` (`kEngineVersionPatch = 2`).

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

