# Security & Code Quality Audit Report — `bgl-Engine` (`bgl-wasm`)

> [!NOTE]
> **Remediation Status (`v1.2.0`):** All Waves 1–5 security, concurrency, GPU/WGSL, browser CSP/WebGPU, build/CI hardening, and regression-test tasks defined in this audit report have been implemented and verified in `v1.2.0`.

> **Audit Classification:** AAA Security, Memory Safety, Concurrency, GPU/WGSL, Browser WebGPU, Build/CI & Supply-Chain Audit  
> **Target Repository:** `bgl-Engine` v1.1.0 (`/home/bastian/code/bgl-wasm`, baseline commit `ad410ee` on `develop`)  
> **Audience:** Engineering Team & Execution Playbook (structured as atomic, deterministic task cards so even a lightweight model such as Gemini Flash / Flash-Lite can execute each wave safely without ambiguity)  
> **Scope Exclusion:** `third_party/` upstream source/binaries (except integration, linkage & integrity verification of `third_party/wgpu-native/`), `build*/`, `.cache/`, `dist/`, and generated bitmap payload inside `src/text/EmbeddedFont.cpp`.

---

## 1. Executive Summary & Risk Matrix

`bgl-Engine` implements an ambitious native C++23 + WebGPU desktop renderer alongside a browser WebGPU viewer, including glTF 2.0/2.1 parsing, mesh LOD generation, CPU/compute BVH ray tracing, custom Radiance HDR/OpenEXR/KTX2 decoders, HarfBuzz/FreeType text shaping, and immediate-mode UI. Under adversarial inspection, however, **significant security vulnerabilities, undefined behavior, silent test gaps, and a fundamental architectural split between the native C++ engine and the browser runtime** were identified.

### Severity Overview

| Domain | Critical | High | Medium | Low / Info | Top Risk Theme |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Asset Parsers, Textures, Text & Scene Graph** | 2 | 4 | 3 | 1 | Morph-target/skin/animation OOM & integer overflow, sparse accessor bypass, cyclic scene-graph stack overflow / leak, FreeType `MONO` heap OOB read, PNG/JPEG/KTX2/HDR validation gaps |
| **Network, WASM C-ABI, Crash Handler & Process/CLI** | 2 | 4 | 4 | 2 | SSRF to RFC1918/link-local/IMDS (`CURLOPT_OPENSOCKETFUNCTION` absent), cleartext HTTP & redirect downgrade, local `.gltf` `file://`/`..` path escape & `LoadExternalBuffers` bypass, async-signal-unsafe crash handler, WASM `this` UAF & OOB slices |
| **GPU Layer & WGSL Shaders** | 2 | 5 | 5 | 2 | Morph-staging heap buffer overflow (`32-bit size_t`), `wgpuBufferMapAsync(userdata1 = this)` UAF, `RenderSelectionOutline` `Uint32` vs `Uint16` OOB read, raw-pointer ABA stale bind groups on resize, missing `device.lost`/null resource checks, BVH `NaN`/unclamped indices |
| **Web Frontend & Browser WebGPU Runtime** | 0 | 5 | 6 | 1 | Uncontrolled DOM/stack recursion (`renderTreeDiagram`, `createJsonElement`) & $2^N$ DAG blowup, unvalidated `?model=`/`?url=`/`img.src` SSRF & credentialed fetch, missing CSP + 40+ inline HTML handlers, unbounded `ReadableStream` fetch DoS, negative/OOB glTF accessor reads & SVG/image bombs |
| **Core Engine, UI, Audio & Entity Graph** | 0 | 2 | 3 | 1 | Non-virtual `EngineCore::Shutdown()` causing worker UAF, dangling `std::span` in `ImmediateUI::PendingDropdownOverlay`, recursive `EntityOps::IsDescendantOrSelf`, `AssetManager` hash collision / unbounded cache |
| **Build System, Supply Chain, CI/CD & Tests** | 0 | 4 | 7 | 4 | Missing ELF hardening (`_FORTIFY_SOURCE=3`, Full RELRO, `-fstack-protector-strong`, `_GLIBCXX_ASSERTIONS`) & WASM `--stack-first`, unverified `libwgpu_native.so` integrity, unpinned CI actions & broad token permissions, broken sanitizer presets & zero CI sanitizers/fuzzing, tautological golden test |
| **Total** | **6** | **24** | **28** | **11** | **69 Actionable Findings** |

### Architectural Truth Check (Critical Reality Gap)

> [!CAUTION]
> **`web/dist/bgl_engine.wasm` (`src/wasm/wasm_bridge.cpp`, ~7.7 KB) is NOT the C++ engine.**
> It is an isolated freestanding WASM stub whose loader (`bgl_load_file_buffer`) only counts top-level commas in JSON keys (`"nodes"`, `"cameras"`, `"meshes"`, `"animations"`) and whose `bgl_pick_node` hardcodes a center NDC box returning node `0`. All actual browser rendering runs in pure JavaScript (`web/js/renderer.js` + `web/js/shaders.js`), while `apps/viewer/WebExports.cpp` defines an incompatible, unused second C-ABI for an unbuildable `if(EMSCRIPTEN)` CMake branch. Because UI toggles, sliders, camera selection, gamepad polling, and touch events in `web/app.js` only mutate `gState` in `wasm_bridge.cpp`, **many advertised browser HUD controls are non-functional**.

---

## 2. How to Use This Playbook (Instructions for Lightweight Execution Agents)

Every fix below is packaged as an atomic **Task Card** ordered into **5 strict dependency waves**:

- **Wave 1 (`SEC-01` .. `SEC-08`)**: Memory safety, integer overflows, cycle detection & DoS caps in Scene, Parsers, Decoders, Fonts, UI, and Engine lifecycle.
- **Wave 2 (`SEC-09` .. `SEC-15`)**: Network SSRF/path-traversal firewall, CrashHandler async-signal safety, WASM bridge / C-ABI bounds & `WasmMalloc` hardening, Core/Math/Audio `NaN` guards.
- **Wave 3 (`SEC-16` .. `SEC-22`)**: GPU resource null/overflow checks, UAF elimination in async callbacks/outline/bind groups, BVH & WGSL shader `NaN`/bounds/TDR hardening.
- **Wave 4 (`SEC-23` .. `SEC-30`)**: Web Frontend CSP + inline-handler removal, URL validator + safe `fetch()`, streaming caps, worker timeouts, tree/JSON/DAG caps, glTF accessor/image bounds, `device.lost` & `BglFrameScheduler` unification.
- **Wave 5 (`SEC-31` .. `SEC-38` & `QUAL-01` .. `QUAL-08`)**: Compiler/linker/WASM hardening, `libwgpu_native.so` SHA-256 verification manifest, CI/CD permissions & `clang-tidy`/sanitizer/fuzzer improvements, fake-test replacement, and comprehensive C++/JS adversarial regression tests.

### Mandatory Invariants for All Edits
1. **Language & ABI:** Strict **C++23** (`-std=c++23`). Never throw C++ exceptions across module or `extern "C"` boundaries; use `bgl::core::Result<T>` (`std::expected<T, std::string>`). Never introduce GLFW.
2. **Module Layering (`scripts/check_layering.py`):** Respect strict upward-only module boundaries (`core` -> `audio`/`scene`/`gpu`/`text` -> `ui` -> `engine` -> `apps/viewer`). Do not include internal headers in `include/bgl/`.
3. **Formatting & Docs:** Keep `clang-format --dry-run -Werror` and `doxygen Doxyfile` (`WARN_AS_ERROR = YES`) completely clean. Write code comments/docstrings in concise English.
4. **No Test Cheating:** Never weaken assertions or skip failing checks. Every security fix must ship with a negative/adversarial test proving the exploit input is rejected cleanly.

---

## 3. Detailed Findings & Execution Task Cards

---

### WAVE 1 — C++ Parsers, Scene Graph, Image Decoders, Fonts, & UI Lifetimes

#### [SEC-01] Multi-Node Cycle Crash & Shared-Ptr Leak in Scene Hierarchy + Unbounded Recursive Traversals
- **Severity:** Critical | **CWE:** CWE-674 (Uncontrolled Recursion), CWE-401 (Memory Leak via `shared_ptr` Cycle), CWE-400
- **Target Files:**
  - `src/scene/FastGltfSceneBuilder.cpp:544-552`
  - `src/scene/Scene.cpp:154-174`
  - `src/scene/FastGltfLoader.cpp:89-111`
  - `src/scene/RayPicker.cpp:52-130`
  - `src/engine/EntityOps.cpp:24-37`
- **Verified Defect:**
  `BuildSceneHierarchy` only checks `childIdx != i && !isChild[childIdx]`. Two nodes referencing each other (`node[0].children = [1]`, `node[1].children = [0]`) satisfy `!isChild[childIdx]` on their first edge each, forming a `std::shared_ptr<SceneNode>` reference cycle. Immediately afterward on line 572, `scene.Update()` calls `SceneNode::UpdateTransforms()` which recurses without depth or `visited` tracking (`SIGSEGV` stack overflow), and even if stack survives, the `shared_ptr` cycle leaks the entire scene subtree forever. The same unguarded recursion exists in `RayPicker::Pick`, `LogSceneSummary::printTree`, and `EntityOps::IsDescendantOrSelf`.
- **Invariant to Enforce:** No parent edge may be added if `childIdx` is already an ancestor of `i` (union-find cycle check), and every scene-graph traversal must enforce both a `visited` pointer set and a hard max depth guard (`kMaxSceneGraphDepth = 256`).
- **Exact Fix (`src/scene/FastGltfSceneBuilder.cpp:544-552`):**
  - Replace lines 544–552 with disjoint-set (Union-Find) ancestor cycle rejection plus tree in-degree guard:
  ```cpp
    std::vector<bool> isChild(asset.nodes.size(), false);
    std::vector<size_t> ufParent(asset.nodes.size());
    std::iota(ufParent.begin(), ufParent.end(), size_t{0});
    auto findRoot = [&ufParent](size_t x) noexcept {
        while (ufParent[x] != x) {
            ufParent[x] = ufParent[ufParent[x]];
            x = ufParent[x];
        }
        return x;
    };
    for (size_t i{0}; i < asset.nodes.size(); ++i) {
        if (!loadedNodes[i]) {
            continue;
        }
        for (size_t childIdx : asset.nodes[i].children) {
            if (childIdx >= loadedNodes.size() || !loadedNodes[childIdx] || childIdx == i || isChild[childIdx]) {
                continue;
            }
            const size_t rootParent{findRoot(i)};
            const size_t rootChild{findRoot(childIdx)};
            if (rootParent == rootChild) {
                BGL_LOG_WARN("[glTF Loader] Rejected cyclic node relationship {} -> {}", i, childIdx);
                continue;
            }
            ufParent[rootChild] = rootParent;
            loadedNodes[i]->children.push_back(loadedNodes[childIdx]);
            isChild[childIdx] = true;
        }
    }
  ```
  *(Ensure `#include <numeric>` is at top of `FastGltfSceneBuilder.cpp`.)*
- **Exact Fix (`src/scene/Scene.cpp`, `src/scene/RayPicker.cpp`, `src/scene/FastGltfLoader.cpp`, `src/engine/EntityOps.cpp`):**
  - Thread `size_t depth = 0` + `std::unordered_set<const SceneNode*>` (or non-owning visited check) through `SceneNode::UpdateTransforms` (and break cycles in `Scene::~Scene()` by clearing `node->children` across `allNodes`), `RayPicker::Pick` (`visited` + `depth > 256`), `LogSceneSummary::printTree` (`visited` + `depth > 64`), and `EntityOps::IsDescendantOrSelf` (`visited` set + depth cap).
  - **Pitfall:** Do not change `SceneNode::UpdateTransforms(const glm::mat4& parentTransform = glm::mat4{1.0f})` public default signature; add an internal helper `UpdateTransformsImpl(const glm::mat4&, std::unordered_set<const SceneNode*>&, size_t depth)`.
- **Regression Test (`tests/test_gltf_loader.cpp`):** Build an in-memory glTF JSON with `"nodes": [{"children":[1]}, {"children":[0]}]` and verify `LoadFromMemory` terminates cleanly without crashing or leaking `SceneNode`s.

---

#### [SEC-02] Morph-Target Allocation Bomb, Sparse Accessor Bypass, `vertexCount == 0` Index Bypass & `NaN` Bounds in glTF Parsing
- **Severity:** Critical | **CWE:** CWE-770 (Allocation Without Limits), CWE-190 (Integer Overflow), CWE-20
- **Target Files:**
  - `src/scene/FastGltfParser.cpp:23-44, 128-244, 415-425`
  - `src/scene/FastGltfSceneBuilder.cpp:68-115, 160-220`
- **Verified Defects:**
  1. `ValidateAccessorBounds` (`FastGltfParser.cpp:31-33`) unconditionally returns `true` for sparse accessors before verifying backing bufferView or sparse index/value sizes, and ignores accessor component byte width (checks `sizeof(uint8_t)` / hardcodes `minElemBytes`), allowing tiny JSON files to trigger multi-gigabyte allocations.
  2. `ParseMeshPrimitive` (`FastGltfParser.cpp:195-217`) resizes `meshPrim.morphTargets` (`prim.targets.size()`) and allocates 3 `glm::vec3` vectors of size `vertices.size()` per target **without any cap** (`10000` targets × `100000` vertices = 36 GB OOM!).
  3. Index clamping (`FastGltfParser.cpp:232`: `if (vertexCount > 0 && val >= vertexCount)`) fails to clamp when `vertexCount == 0`.
  4. Non-finite vertex `pos` (`NaN`/`Inf`) poisons `meshPrim.boundingBox` -> `NaN`.
  5. `ParseInstancingTransforms` (`FastGltfSceneBuilder.cpp:75`), `ParseSkins` (`lines 165-178`), and `ParseAnimations` (`lines 209-218`) never bound `instCount`, `gltfSkin.joints.size()`, `ibmAccessor.count`, `inputAcc.count`, or `outputAcc.count` against backing `bufferView.byteLength` or safe limits (`kMaxInstancesPerNode`, `kMaxSkinJoints`, `kMaxAnimationKeyframes`) BEFORE calling `.resize()` / `.reserve()`.
  6. `ParseAnimations` does not sort or reject non-monotonic / `NaN` timestamps (`timestamps`), and `AnimationSampler::Sample` (`src/scene/Animation.cpp:33-34, 74-96`) fails to check `offset0 + stride <= keyframeValues.size()` and `glm::length(q) > 1e-6f` before `glm::normalize(q)`.
- **Exact Fix:**
  1. Cap `kMaxMorphTargetsPerPrimitive = 64u` (GPU evaluates max 8, CPU allows up to 64 bounded by `kMaxTotalMorphVertices = 2'000'000u`), cap `kMaxPrimitiveVertices = 5'000'000u`, `kMaxPrimitiveIndices = 15'000'000u`.
  2. Check accessor indices (`< asset.accessors.size()`) on ALL attributes (`POSITION`, `NORMAL`, `TEXCOORD_0`, `TANGENT`, `JOINTS_0`, `WEIGHTS_0`, `indicesAccessor`, and morph targets), check `ValidateAccessorBounds` for ALL accessors (including sparse: verify `acc.sparse->count <= acc.count`, `indicesBufferView < asset.bufferViews.size()`, `valuesBufferView < asset.bufferViews.size()`, and byte lengths fit `sparse->count`).
  3. Validate vertex `pos` finiteness (`std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)` -> fallback `{0,0,0}`); validate `weights` finiteness (`sum` finite); clamp `joints` or validate `vertexCount == 0` -> clear `indices` if `vertexCount == 0`.
  4. In `FastGltfSceneBuilder.cpp`: add `ValidateSimpleAccessor(asset, accIdx, minElemBytes, maxCount)` for `EXT_mesh_gpu_instancing` (`kMaxInstances = 1'000'000u`), `Skin::inverseBindMatrices` (`kMaxSkinJoints = 4096u`), and `AnimationSampler` (`kMaxAnimKeyframes = 2'000'000u`); sanitize `timestamps` (`std::isfinite`, non-decreasing) and `Animation.cpp` `offset0` + quaternion zero-length guard.
- **Regression Test:** Add GTest cases in `tests/test_gltf_loader.cpp` and `tests/test_animation.cpp` for oversized `prim.targets`, fake huge `count` accessors, `vertexCount == 0` with non-zero indices, `NaN` positions/quaternions, and sparse accessor bombs.

---

#### [SEC-03] FreeType `FT_PIXEL_MODE_MONO` / Color Glyph Heap Out-of-Bounds Read & Unbounded Font File Reads
- **Severity:** High | **CWE:** CWE-125 (Out-of-bounds Read), CWE-770
- **Target Files:**
  - `src/text/FontAtlas.cpp:325-360`
  - `src/text/FontLibrary.cpp:391-406`
- **Verified Defect:**
  `FontAtlas::RasterizeAndPack` (`src/text/FontAtlas.cpp:351-352`) indexes `rowPtr[x]` for `x in [0, bitmap.width)` assuming `bitmap.pixel_mode == FT_PIXEL_MODE_GRAY` (`1 byte/pixel`). When FreeType loads a 1-bit monochrome bitmap font (`FT_PIXEL_MODE_MONO`), `absPitch` is `ceil(width / 8.0)` — `rowPtr[x]` reads up to **8× past the end of `bitmap.buffer`**! Conversely, color/BGRA bitmaps (`FT_PIXEL_MODE_BGRA`, 4 bytes/pixel) misread channels as grayscale alpha. Separately, `FontLibrary::ReadFontBytes` (`FontLibrary.cpp:391-406`) opens arbitrary paths without `is_regular_file` check or file-size limit (`kMaxFontFileSize = 64 MiB`).
- **Exact Fix (`src/text/FontAtlas.cpp:347-359`):**
  ```cpp
    for (uint32_t y = 0; y < bitmap.rows; ++y) {
        const uint32_t srcRow = (bitmap.pitch >= 0) ? y : (bitmap.rows - 1u - y);
        const size_t absPitch = static_cast<size_t>(bitmap.pitch >= 0 ? bitmap.pitch : -bitmap.pitch);
        const uint8_t* rowPtr = bitmap.buffer + static_cast<size_t>(srcRow) * absPitch;
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            uint8_t alpha = 0;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                if (x < absPitch) {
                    alpha = rowPtr[x];
                }
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                const size_t byteIdx = x >> 3u;
                if (byteIdx < absPitch) {
                    alpha = ((rowPtr[byteIdx] & (0x80u >> (x & 7u))) != 0u) ? 255u : 0u;
                }
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                const size_t byteIdx = static_cast<size_t>(x) * 4u + 3u;
                if (byteIdx < absPitch) {
                    alpha = rowPtr[byteIdx];
                }
            }
            const size_t dstIdx = (static_cast<size_t>(targetY + y) * atlasDim_ + (targetX + x)) * 4u;
            cpuAtlas_[dstIdx + 0] = 255;
            cpuAtlas_[dstIdx + 1] = 255;
            cpuAtlas_[dstIdx + 2] = 255;
            cpuAtlas_[dstIdx + 3] = alpha;
        }
    }
  ```
- **Exact Fix (`src/text/FontLibrary.cpp:391-406`):** Check `std::filesystem::is_regular_file(filePath, ec)`, reject symlinks outside font dirs or enforce `file_size <= 64u * 1024u * 1024u` before allocating `std::vector<uint8_t>`.

---

#### [SEC-04] Texture Decoder Hardening (`libpng` rowbytes/channels, `libjpeg` components, `KTX2` format/mip bounds, `HDR/EXR` `NaN`/`Inf`)
- **Severity:** High | **CWE:** CWE-787 (Out-of-bounds Write), CWE-125, CWE-681, CWE-20
- **Target Files:**
  - `src/scene/TextureDecoder.cpp:124-138, 171-185, 435-480`
  - `src/scene/TextureDecoderHdr.cpp:220-226, 396-408`
  - `tests/fuzz/fuzz_texture_decoder.cpp:10-23`
- **Verified Defects:**
  1. `ReadPngStreamIsolateSetjmp` (`TextureDecoder.cpp:124-137`) does not verify `png_get_channels(png_ptr, info_ptr) == 4` and `png_get_rowbytes(png_ptr, info_ptr) == static_cast<size_t>(width) * 4u` before `png_read_image`.
  2. `ReadJpegStreamIsolateSetjmp` (`TextureDecoder.cpp:171-186`) does not check `cinfo.output_components == 3` before writing `rowBuffer[x * 3u + 2u]`.
  3. `DecodeKTX2` (`TextureDecoder.cpp:461-478`): When `!transcodedCompressed`, arbitrary non-RGBA8 formats (e.g. R8, RG8, RGBA16F, or pre-compressed GPU formats when `NeedsTranscoding()` is false) enter `else { img.rgbaData.resize(imageSize); }` while claiming `channels = 4, isCompressed = false`, causing `PipelineScene.cpp:188` (`bytesPerRow = img.width * 4`) to read OOB or fail upload! Also `width`/`height` for pre-compressed KTX2 vs BasisU needs explicit format validation.
  4. `DecodeHDR` and `DecodeEXR` (`TextureDecoderHdr.cpp:224, 401`) allow `NaN`, `Inf`, and negative float channel values into `rgbaFloat`, later corrupting IBL cubemaps and `auto_exposure.wgsl`.
- **Exact Fix:**
  - PNG: verify `png_get_channels(png_ptr, info_ptr) == 4 && png_get_rowbytes(png_ptr, info_ptr) == static_cast<size_t>(width) * 4u`.
  - JPEG: verify `cinfo.output_components == 3 && cinfo.output_width == cinfo.image_width` (after checking `width * height <= kMaxTexturePixels`).
  - KTX2: when `!transcodedCompressed`, check `imageSize` strictly equals `expectedRgbaSize` or `expectedRgbSize` (expand 1-ch/2-ch if `imageSize == pixels` or `2*pixels`, otherwise reject unsupported pixel byte-layout instead of passing short `rgbaData` with `channels = 4`).
  - HDR/EXR: sanitize output channels (`std::isfinite(val) ? std::max(val, 0.0f) : 0.0f` for R/G/B, clamp alpha `[0, 1]`).
  - Update `tests/fuzz/fuzz_texture_decoder.cpp` to also fuzz `DecodeHDR`, `DecodeEXR`, `DecodeHdrAuto`, and compressed `DecodeKTX2` targets.

---

#### [SEC-05] Dangling `std::span` (Use-After-Free) & Missing Bounds Check in `ImmediateUI::Dropdown` Overlay
- **Severity:** High | **CWE:** CWE-416 (Use After Free), CWE-125
- **Target Files:**
  - `src/ui/ImmediateUI.hpp:521-530`
  - `src/ui/ImmediateUIOverlays.cpp:150-185, 349-373`
- **Verified Defect:**
  `PendingDropdownOverlay::options` (`ImmediateUI.hpp:525`) stores a non-owning `std::span<const char* const>` copied in `ImmediateUI::Dropdown()` and read later in `EndFrame()` -> `EmitDropdownOverlay()`. Any caller constructing options in a temporary `std::vector<const char*>` triggers **stack/heap Use-After-Free**. Additionally, `ImmediateUIOverlays.cpp:161` accesses `options[selectedIndex]` without checking `selectedIndex < options.size()`.
- **Exact Fix:**
  - Change `std::span<const char* const> options{};` to `std::vector<std::string> options{};` inside `PendingDropdownOverlay`.
  - In `Dropdown()`, clamp/validate `selectedIndex < options.size()` before indexing `options[selectedIndex]`, and copy valid strings into `pendingOverlay_.options`.
  - In `EmitDropdownOverlay()`, read `ov.options[i]` (`std::string_view`).
- **Regression Test:** Add test in `tests/test_text_rendering.cpp` or new UI test building dropdown options in an inner scope, destroying the backing `vector`, and calling `EndFrame()` under ASan.

---

#### [SEC-06] Non-Virtual `EngineCore::Shutdown()` Causes Worker Thread UAF + Concurrent AssetManager/TextureCache Races
- **Severity:** High | **CWE:** CWE-416 (Use After Free), CWE-362 (Data Race), CWE-770
- **Target Files:**
  - `src/engine/EngineCore.hpp:68`
  - `apps/viewer/Application.hpp:66`
  - `apps/viewer/ApplicationModelLoader.cpp:182-192, 234-260, 319-330`
  - `src/scene/AssetManager.hpp`, `src/scene/AssetManager.cpp:13-85`
  - `src/engine/Engine.cpp:17-46`
- **Verified Defects:**
  1. `EngineCore::Shutdown()` (`EngineCore.hpp:68`) is **non-virtual**, hidden by `Application::Shutdown()`. Polymorphic shutdown via `EngineCore*`/`~EngineCore` skips `ApplicationModelLoader::CancelPendingSceneLoads()`, clearing `assetManager_` while `task->worker` (`std::jthread` capturing raw `this` and `assetManager_`) is still running.
  2. `ApplicationModelLoader::RetireActiveLoadAsync()` imposes no upper limit on concurrent `cancellingLoads_`, allowing rapid drops to spawn dozens of blocked curl threads stalling shutdown.
  3. `AssetManager::textureCache_` (`AssetManager.cpp:14-58`) keys **both** path hashes and content hashes in the same `unordered_map<uint64_t, ...>` without verifying path string match (64-bit hash collision confusion) and grows without eviction/bound.
- **Exact Fix:**
  1. Declare `virtual void Shutdown();` in `EngineCore.hpp` and `void Shutdown() override;` in `Application.hpp`.
  2. Limit `cancellingLoads_` to 4 active retired loads (join oldest before launching a new one if full) and wire libcurl `CURLOPT_XFERINFOFUNCTION` progress cancellation into `NetworkFetcher::FetchSync` if needed, or store a `std::shared_ptr<CancelToken>` cleanly.
  3. Separate `pathCache_` (`unordered_map<std::string, ...>` or verify path match) and `contentCache_` (`unordered_map<uint64_t, ...>`) in `AssetManager`, and cap max cached textures (`kMaxCachedTextures = 512`) to prevent unbounded RAM growth across multiple model loads.

---

### WAVE 2 — Network Firewall (SSRF / Path Traversal), CrashHandler, WASM Bridge & Core Validation

#### [SEC-07] SSRF to Loopback/RFC1918/Cloud Metadata, Cleartext HTTP & HTTPS->HTTP Redirect Downgrades
- **Severity:** Critical | **CWE:** CWE-918 (SSRF), CWE-319 (Cleartext Transmission)
- **Target Files:**
  - `src/core/NetworkFetcher.cpp:144-147, 540-598`
  - `src/core/NetworkFetcher.hpp`
- **Verified Defect:**
  `NetworkFetcher::FetchSync` sets `CURLOPT_FOLLOWLOCATION = 1L` and `CURLOPT_REDIR_PROTOCOLS_STR = "http,https"` with no IP/socket validation (`CURLOPT_OPENSOCKETFUNCTION`) and no HTTPS-only enforcement, allowing `http://169.254.169.254/...`, `http://127.0.0.1/...`, RFC1918 LAN targets, DNS rebinding, and HTTPS->HTTP downgrades. Line `583` treats `httpCode == 0` as success. Additionally, `EnsureCurlGlobalInitialized()` never registers `curl_global_cleanup` via `atexit`.
- **Exact Fix (`src/core/NetworkFetcher.cpp`):**
  1. Add POSIX socket IP filter `IsBlockedIpAddress(const sockaddr*)` blocking `0.0.0.0/8`, `127.0.0.0/8`, `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `169.254.0.0/16`, `100.64.0.0/10`, `224.0.0.0/4`, IPv6 `::`, `::1`, `fc00::/7`, `fe80::/10`, `ff00::/8`, and `::ffff:0:0/96` IPv4-mapped loopback/private addresses via `CURLOPT_OPENSOCKETFUNCTION`.
  2. Enforce `https` only for remote URLs (`CURLOPT_PROTOCOLS_STR = "https"`, `CURLOPT_REDIR_PROTOCOLS_STR = "https"`, `CURLOPT_SSL_VERIFYPEER = 1L`, `CURLOPT_SSL_VERIFYHOST = 2L`, `CURLOPT_SSLVERSION = CURL_SSLVERSION_TLSv1_2`, `CURLOPT_MAXFILESIZE_LARGE`). Also reject `httpCode < 200 || httpCode >= 300`.
  3. Add URL check on initial URL scheme in `FetchSync`: block plaintext `http://` unless an explicit opt-in API parameter or test helper is used (note: keep local file reading isolated from remote scheme). Check `tests/test_network_fetcher.cpp` first so any existing unit test URL assumptions are preserved/updated properly!
  *(Pitfall: verify `test_network_fetcher.cpp` expectations before breaking any unit tests asserting `IsHttpUrl("http://...")` vs `FetchSync("http://...")`!)*
- **Regression Test:** Verify `FetchSync("http://127.0.0.1:80/x")`, `FetchSync("https://127.0.0.1/x")`, `FetchSync("http://169.254.169.254/latest/meta-data/")`, and non-loopback HTTP are rejected cleanly.

---

#### [SEC-08] Local File Disclosure & Directory Escape via `ResolveExternalAssetUri`, `OnFileDrop` & `fastgltf::Options::LoadExternalBuffers`
- **Severity:** Critical | **CWE:** CWE-22 (Path Traversal), CWE-73
- **Target Files:**
  - `src/core/NetworkFetcher.cpp:190-200, 400-435, 490-505`
  - `src/scene/FastGltfLoader.cpp:183-196, 252-270`
  - `apps/viewer/ApplicationModelLoader.cpp:194-204`
  - `apps/viewer/WebExports.cpp:394-405`
- **Verified Defects:**
  1. `ResolveExternalAssetUri` (`NetworkFetcher.cpp:425-432`) only locks down `IsHttpUrl` and empty/`data:` bases; for any non-empty local filename (`"foo.gltf"`), line `432` executes `return resolved;`, allowing `"../../../../etc/passwd"`, `"file:///etc/passwd"`, and `"https://attacker.com/exfil"` subresources!
  2. Dropped in-memory buffers (`OnFileDrop(filename, data)`) pass `filename` as `baseUri`, causing both `ResolveExternalAssetUri` and `FastGltfLoader.cpp:192` (`LoadExternalBuffers`) to read arbitrary local files from `CWD`.
  3. `FastGltfLoader::LoadFromFile` (`lines 254-264`) uses `dataBuffer.loadFromFile` without checking `std::filesystem::file_size` (`<= 2 GiB` for local desktop assets; remote/URI `NetworkFetcher::kMaxAssetFileSize` is `256 MiB` native / `48 MiB` WASM).
  4. `FetchSync` `file://` handler strips `localhost/` but not non-empty authorities like `file://host/path`, and `DecodeBase64` (`line 86`) / `data:` URIs omit `kMaxAssetFileSize` caps + 32-bit overflow checks.
- **Exact Fix:**
  1. Treat `memory://` prefix (and empty/`data:`) in `ResolveExternalAssetUri` as memory-only (only `data:` subresources allowed), and pass `"memory://" + SanitizeUri(name)` from `OnFileDrop` / `bgl_load_file_buffer` instead of bare filenames!
  2. For local filesystem `baseUri`, reject ANY external scheme (`http:`, `https:`, `file:`) in `relativeUri`, reject absolute paths, canonicalize with `weakly_canonical`, and verify `target.lexically_relative(baseDir)` stays strictly inside `baseDir` (no leading `..`).
  3. Remove `fastgltf::Options::LoadExternalBuffers` in `FastGltfLoader.cpp:192` — `ResolveExternalBuffers()` (`line 211`) ALREADY resolves all `URI` buffers through `NetworkFetcher::ResolveExternalAssetUri` and `NetworkFetcher::FetchSync`!
  4. Enforce `kMaxAssetFileSize` (`48 MiB` on `BGL_PLATFORM_WASM`, `256 MiB` on native network/URI fetches; `2 GiB` on local native `FastGltfLoader::LoadFromFile`) across `ReadLocalFileBytes`, `LoadFromFile` (`file_size`), `data:` URIs, `DecodeBase64` (`encoded.size() <= 4 * (kMaxAssetFileSize / 3)`), and Emscripten fetch. Also open files cleanly via POSIX `O_RDONLY | O_NONBLOCK` or verify `is_regular_file` safely.

---

#### [SEC-09] Async-Signal-Unsafe Heap/Lock Calls in `CrashHandler`, `INT64_MIN` UB & Re-Install Corruption
- **Severity:** High | **CWE:** CWE-479 (Non-reentrant Function in Signal Handler), CWE-190, CWE-362
- **Target Files:**
  - `src/core/CrashHandler.cpp:89-128, 192-265, 268-317`
  - `tests/test_crash_handler.cpp:49-56`
- **Verified Defects:**
  1. `SignalActionHandler` calls `std::cerr << std::stacktrace::current()`, `try/catch`, and `::backtrace()` (first call triggers `dlopen`/`malloc`), risking allocator/iostream deadlocks during `SIGSEGV`/`SIGABRT`.
  2. `SafeFormatDec(INT64_MIN)` negates signed `INT64_MIN` (UB); `SafeFormatHex` shifts `uintptr_t` by up to `60` bits (UB on 32-bit targets).
  3. Calling `InstallCrashHandler()` multiple times overwrites `g_oldAction*` with `SignalActionHandler` itself; subsequent `UninstallCrashHandler()` restores the dangling handler while freeing `g_altStackMemory` (existing `CrashHandlerTest.MultipleConsecutiveInstallCalls` never verifies `sigaction` state!).
  4. Raising `sig` at end of `SignalActionHandler` without unblocking `sig` and `::_exit(128 + sig)` fallback can fail to re-deliver.
- **Exact Fix:** Remove `std::cerr`, `std::stacktrace::current()`, `try/catch` (and unused `<cxxabi.h>`/`<dlfcn.h>`) from `SignalActionHandler`, compute `0ULL - static_cast<uint64_t>(val)` in `SafeFormatDec`, cast to `uint64_t` in `SafeFormatHex`, add idempotent `std::atomic<bool> g_installed{false}` guard in `InstallCrashHandler`/`UninstallCrashHandler`, unblock `sig` + call `::_exit(128 + sig)` fallback, and verify `SIG_DFL` restoration in `tests/test_crash_handler.cpp` + add `INT64_MIN` test!

---

#### [SEC-10] WASM Bridge (`wasm_bridge.cpp`) & `WebExports.cpp` Bounds, Exceptions, `NaN` Guards & O(N²) Allocator DoS
- **Severity:** High | **CWE:** CWE-125, CWE-248, CWE-407, CWE-416, CWE-190
- **Target Files:**
  - `src/wasm/wasm_bridge.cpp:50-186, 442-479, 557-684`
  - `apps/viewer/WebExports.cpp:285-442`
  - `src/core/Camera.cpp:33-96`
  - `src/core/MemoryTracker.cpp:30-50`
- **Verified Defects:**
  1. `wasm_bridge.cpp`: `bgl_load_file_buffer(name, data, len)` never checks if `[data, data + len)` lies inside `CurrentMemoryEndBytes()`; `bgl_camera_orbit` / float setters accept `NaN`/`Inf`; `WasmFree` walks the entire `gHeapHead` list in `CoalesceFreeBlocks()` (`O(N^2)`) and never shrinks `gHeapTop` when freeing the tail block; `BlockHeader` lacks magic integrity checking.
  2. `WebExports.cpp`: `main()` exposes `g_pApp = &app` BEFORE `app.Initialize()` succeeds; `bgl_get_camera_name` returns a pointer to a mutated `static std::string`; `bgl_load_file_buffer`/`bgl_load_url` perform unbounded `strlen` and lack `try/catch` around `extern "C"`; `bgl_set_debug_view` misses modes `5..6`.
  3. `Camera.cpp` (`Orbit`, `Pan`, `Move*`, `Zoom`) & `EngineCore`: `std::clamp(NaN, min, max)` returns `NaN`, permanently poisoning view/projection matrices.
  4. `MemoryTracker.cpp:34-35, 45`: Signed `long` multiplication (`totalPages * pageSize`, `ru_maxrss * 1024L`) overflows cleanly on 32-bit/large values with no `< 0` check.
- **Exact Fix:**
  - In `wasm_bridge.cpp`: validate `IsRangeInWasmMemory(data, len)` using `uintptr_t` overflow-safe subtraction against `CurrentMemoryEndBytes()`; guard `std::isfinite` on all float inputs; add `magic` + `prev` doubly-linked O(1) coalescing + tail shrink in `WasmFree`; check `clang-tidy src/wasm/wasm_bridge.cpp` passes with zero warnings!
  - In `WebExports.cpp`: publish `g_pApp` only after `Initialize()` succeeds, add `noexcept` + `try { ... } catch (...)` wrappers on C-ABI boundaries, sanitize finite floats, check string lengths with `strnlen`, fix `bgl_set_debug_view` modes `5..6`.
  - In `Camera.cpp` & `MemoryTracker.cpp`: reject `!std::isfinite(...)` inputs in `Camera`, check `>= 0` and overflow-safe `size_t` multiplication in `MemoryTracker.cpp`.

---

#### [SEC-11] CLI Argument Swallowing, Unsigned Resize Wrap, & Log Injection Sanitization
- **Severity:** Medium | **CWE:** CWE-117 (Log Injection), CWE-20, CWE-190
- **Target Files:**
  - `src/core/Log.hpp`, `src/core/Log.cpp`
  - `apps/viewer/CommandLine.cpp:150-220, 246-300`
  - `apps/viewer/Application.cpp:116-255`
- **Verified Defects:**
  1. `Application.cpp:150-151`: Negative `SDL_WINDOWEVENT_RESIZED` `data1`/`data2` wraps to `~4e9` when cast to `uint32_t`. Also `Application::Update` computes `clampedDelta` at line 116 but passes raw unclamped `deltaTime` at lines 207, 223–255!
  2. `CommandLine.cpp`: `-w --headless` causes `ConsumeValue` to consume `--headless` and silently lose it; unknown flags and out-of-range dimensions are silently dropped; `--no-shadows=false` ignores `=false`; `--weather=invalid` silently overwrites to `0`.
  3. Untrusted dropped URIs (`Application.cpp:195`), node names (`EngineCoreScene.cpp:173`), and server error strings are logged verbatim with control/ANSI chars (`\r`, `\n`, `\x1b`).
- **Exact Fix:** Check `data1 > 0 && data2 > 0` before `gpuContext_.Resize`; use `clampedDelta` in `Application::Update`; add `core::SanitizeForLog(std::string_view)` escaping ASCII control bytes `< 0x20` and `0x7F` at log call sites; harden `CommandLine.cpp` so missing/invalid flag values don't swallow subsequent `--` flags and warn/reject invalid options cleanly.

---

### WAVE 3 — GPU Pipeline, Resource Lifetimes & WGSL Shader Hardening

#### [SEC-12] Morph Target Buffer Integer Overflow (`32-bit size_t`) & Excess Binding Size (`PipelineScene.cpp`, WGSL)
- **Severity:** Critical | **CWE:** CWE-190, CWE-122 / CWE-787
- **Target Files:**
  - `src/gpu/PipelineScene.cpp:47-59, 104-153, 174-212, 285-306, 555-564`
  - `src/gpu/shaders/pbr.wgsl`, `shadow.wgsl`, `wireframe.wgsl`, `outline.wgsl`, `rt_gbuffer.wgsl`
- **Verified Defect & Exact Fix:** Cap GPU morph targets at `kMaxGpuMorphTargets = 8u`, check `uint64_t` multiplication against `kMaxMorphBufferBytes = 128ull * 1024ull * 1024ull` and `SIZE_MAX`, validate `texture != nullptr` after every `wgpuDeviceCreateTexture` and `encoder != nullptr` after `wgpuDeviceCreateCommandEncoder` (also guard overflow in `(byteSize + 3u) & ~size_t{3}` in `PipelineScene.cpp:48` and check `mipLevels` non-empty + 4x4 block-aligned before compressed upload), and guard `in.vertexIndex < drawUbo.vertexCount` in the 5 morph-reading shaders!

---

#### [SEC-13] Async Callback UAF (`RayTracingPass` `MapAsync`, `ShaderManager` `PopErrorScope`) & `Context` Device-Lost / Frame Encoder Leak
- **Severity:** Critical | **CWE:** CWE-416 (Use After Free), CWE-755, CWE-401
- **Target Files:**
  - `src/gpu/RayTracingPass.hpp`, `src/gpu/RayTracingPass.cpp:614-646`, `src/gpu/RayTracingPassPipelines.cpp:497-508`
  - `src/gpu/ShaderManager.cpp:329-448, 500-547`
  - `src/gpu/Context.hpp`, `src/gpu/Context.cpp:370-550`
  - `src/engine/EngineCore.cpp:203-235`
- **Verified Defects & Exact Fixes:**
  1. Replace raw `this` in `RayTracingPass::PollGpuTimestamps` (`wgpuBufferMapAsync`) with heap-allocated `std::shared_ptr<TimestampMapState>` cleared in `RayTracingPass::Shutdown()` (also clear `gbufferDrawsScratch_` in `Shutdown()` and query `maxStorageBufferBindingSize` to guard `pathAccumBuffer_` on high-DPI viewports!).
  2. Allocate `PopScopeState` via `std::shared_ptr` in `ShaderManager::CompileWgslModule` (`wgpuDevicePopErrorScope`). Also harden `ShaderManager::ReadSourceFile` against symlinks outside `shaderDir_` (`is_symlink` + `weakly_canonical` prefix verify) and add `#include` expansion caps (`kMaxTotalIncludes = 256`, `kMaxPreprocessedBytes = 4u * 1024u * 1024u`).
  3. Call `Shutdown()` at top of `Context::Initialize()`, clamp width/height >= 1, register `deviceLostCallbackInfo` and `uncapturedErrorCallbackInfo` on `WGPUDeviceDescriptor` (fixing off-by-one `supportsDesktopRayTracing_ = maxStorageBuffersPerShaderStage_ >= 7u` on line 406!), and add `Context::AbortFrame()` called on early returns in `EngineCore::Render()`.

---

#### [SEC-14] `RenderSelectionOutline` Index Format (`Uint32` vs `Uint16`), Resize ABA BindGroup Stale Cache, `DynamicInstanceBuffer` / `GpuRayPicker` Overflow
- **Severity:** High | **CWE:** CWE-125, CWE-416 (ABA Handle Stale Cache), CWE-190
- **Target Files:**
  - `src/gpu/PipelineEffects.cpp:420-466, 549-554`
  - `src/gpu/PostProcessPass.cpp:196-224`
  - `src/gpu/DrawHelpers.hpp:193-280`
  - `src/gpu/Uniforms.hpp:165-178`
  - `src/gpu/Pipeline.cpp:380-464`
  - `src/gpu/GpuRayPicker.cpp:240-260`, `src/gpu/shaders/gpu_ray_pick.wgsl`
- **Verified Defects & Exact Fixes:**
  1. `PipelineEffects.cpp:549-553`: Bind `prim.indexFormat` and draw `lod0.indexCount` / `lod0.firstIndex` instead of hardcoded `WGPUIndexFormat_Uint32` + `prim.indices.size()`; track `selectionMaskTexture_` in `MemoryTracker`.
  2. `PostProcessPass::ReleaseResolutionTargets()` (`PostProcessPass.cpp:196-224`): Reset `depthLinBindGroup_`, `autoExpBindGroup_`, `cachedDepthReadView_`, and `cachedAutoExpHdrView_`; call `postProcessPass_.InvalidateBindGroup()` on resize!
  3. `DrawHelpers.hpp:197-203` & `Uniforms.hpp:170`: Guard `size_t` overflow and `nextBytes *= 2` wraparound; mark `identityWritten_ = true` ONLY when `wgpuBufferGetMappedRange` succeeds.
  4. `PipelineManager::Initialize`: Call `Shutdown()` first and check returns from `CreateFallbackTextures`/`InitMipGenPipelines`.
  5. `GpuRayPicker`: Prevent `uint32_t` `(primitiveCount + 63) / 64` wraparound and use integer `u32` uniform field instead of lossy `f32` `rayDirection.w`.

---

#### [SEC-15] BVH Out-of-Bounds / `NaN` Sorting UB, WGSL BVH Stack/TDR Hardening & Shader `NaN` Sanitization
- **Severity:** High | **CWE:** CWE-125, CWE-758 (STL Strict-Weak-Ordering UB), CWE-400 (GPU TDR)
- **Target Files:**
  - `src/gpu/RayTracingSceneBvh.cpp:75-150, 250-268, 315-405`
  - `src/gpu/PipelineRender.cpp:95-140, 495-520`
  - `src/gpu/PipelineMath.cpp:74-90`, `src/gpu/PipelineIBL.cpp:195-265`, `src/gpu/Skybox.cpp:240-255`, `src/gpu/RayTracingPass.cpp:55-75`
  - `src/gpu/shaders/common/rt_traversal.wgsl`, `auto_exposure.wgsl`, `bloom.wgsl`, `tonemap.wgsl`, `pbr.wgsl`
- **Verified Defects & Exact Fixes:**
  1. `RayTracingSceneBvh.cpp`: Clamp `outReorderedIndices[...]` with `std::min(..., vertCount - 1u)`; bounds-check `nodeIdx`/`instIdx`/`localNodeIdx`/`i0..i2` in `TraverseInstanceBlas`/`TraceRayCpu`; filter non-finite vertices/transforms before `static_cast<uint32_t>` and `std::nth_element` (preventing `NaN` strict-weak-ordering UB); check all 16 entries of `modelMat` in `pushInstance`.
  2. `PipelineRender.cpp` / `PipelineMath.cpp` / `PipelineIBL.cpp` / `Skybox.cpp`: Guard `glm::inverse` with finite + non-singular determinant (`std::abs(det) > 1e-8f`); filter `NaN`/`Inf` in `BuildClusteredLightGrid`, `SetTimeOfDay`, IBL sampling, and `std::sort` comparators.
  3. WGSL shaders: Grow `rt_traversal.wgsl` stacks to `array<u32, 64>`, add `arrayLength` index bounds & `maxSteps = 4096u` TDR loop caps; add IEEE-safe finite HDR sanitization in `auto_exposure.wgsl` & `bloom.wgsl` and safe normalizes in `pbr.wgsl`.

---

### WAVE 4 — Web Frontend & Browser WebGPU Hardening (`web/`, `index.html`, `scripts/`)

#### [SEC-16] Non-Recursive / Depth+Node Capped glTF Tree & JSON Inspector + `O(V+E)` Node Hierarchy Update
- **Severity:** High | **CWE:** CWE-674, CWE-400
- **Target Files:** `web/js/gltf.js:199-531`, `web/js/renderer.js:1430-1456`
- **Exact Fix:** Track `visited = new Uint8Array(nodes.length)`, `MAX_DEPTH = 128`, `MAX_TREE_NODES = 10000` in `BglGltf.renderTreeDiagram`; track `depth <= 24` & `{ count, max: 4000 }` budget in `createJsonElement` (and remove duplicate `data-node-index` vs `data-node-idx` mismatch); convert `updateNodeHierarchy` in `renderer.js` to reuse `this._hierarchyVisited` (`Uint8Array` permanently marked per traversal) + iterative stack (fixing $2^N$ DAG blowup AND eliminating per-frame `new Set()` allocation!).

#### [SEC-17] Strict URL Allowlist, `credentials: 'omit'`, `referrer` Policy, CSP & Removal of Inline HTML Handlers
- **Severity:** High | **CWE:** CWE-918, CWE-319, CWE-693, CWE-79, CWE-200
- **Target Files:** `web/index.html`, `index.html`, `scripts/stage_pages.sh`, `web/app.js`, `web/js/core.js`, `web/js/renderer.js`
- **Exact Fix:**
  1. Add `<meta name="referrer" content="no-referrer">` and `<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob: https://raw.githubusercontent.com; connect-src 'self' https://raw.githubusercontent.com; worker-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none';">` to `web/index.html` (and clean CSP on `index.html` / `scripts/stage_pages.sh` with external redirect JS or `<meta refresh>` instead of inline `<script>`!).
  2. Replace all 40+ inline `onclick`/`onchange`/`oninput` handlers in `web/index.html` with `data-action` / `data-input` attributes wired cleanly via delegated listeners in `web/app.js`; replace `innerHTML` in `web/app.js:27, 406` with DOM creation (`document.createElement` + `.replaceChildren()`).
  3. Add `BglNetwork.validateAssetUrl(url, { allowData })` enforcing `https:` (or localhost dev HTTP / same-origin `assets/*.gl(tf|b)|png|jpg|jpeg|webp`), blocking embedded credentials, and passing `{ signal, credentials: 'omit', mode: 'cors' }` in `fetch()`; gate arbitrary third-party `?model=`/`?url=` query strings via allowlist or explicit user confirmation; validate `imgObj.uri` before `img.src = ...`.

#### [SEC-18] Bounded `ReadableStream` Download, Model Request Race Cancellation, Validate-Before-Cache, Worker Timeout & Safe glTF Bounds in `web/`
- **Severity:** High | **CWE:** CWE-400, CWE-770, CWE-125, CWE-190, CWE-362, CWE-843
- **Target Files:** `web/js/core.js`, `web/app.js`, `web/js/gltf.js`, `web/gltf_worker.js`, `web/js/renderer.js`
- **Exact Fix:**
  1. Enforce `MAX_MODEL_BYTES = 128 * 1024 * 1024`, strict `/^\d+$/` `Content-Length` check, `reader.cancel()` on overflow, `AbortController` fetch timeout (60s), clear chunk refs while combining, cap `memoryFallback`, and do NOT pass `'local://...'` to `BglModelCache.put()`.
  2. Add `_modelRequestId` in `web/app.js` and abort active `BglLoadingScreen.abortController` before starting a new download/file drop; validate glTF parsing succeeds *before* calling `BglModelCache.put()`.
  3. Add worker timeout (`15000 ms`) with worker `.terminate()` in `BglGltf.extractJsonAsync`; validate `gltf` is a plain non-null object in `gltf_worker.js` and `gltf.js`; handle non-base64 `data:` URIs safely without `atob()` crashes.
  4. Harden `getBufferViewData`, `getAccessorData`, GLB `BIN` slicing, `generateLodData`, index type/bounds validation (`Uint16Array`/`Uint32Array`, `< vertexCount`, `posData.length % 3 === 0`), safe array indexing (`safeAt`), `onImageReady` (reject `0x0` & clamp dims `<= device.limits.maxTextureDimension2D`, restrict `Blob.type` to `image/png|jpeg|webp`, evict failed `textureCache` entries).
  5. Export `BglFrameScheduler` on `globalThis` in `core.js`, wrap `tick` steps in `try/catch`, add `device.lost` cleanup + `BglNotifier.error` + `uncapturederror` listener, remove redundant `checkBrowserCompatibility()` & `wasmMemory.grow` calls, fix `loadModelFromUrl` error propagation so `Sponza.glb` fallback works, remove unpinned `raw.githack.com` link in `web/README.md` / `README.md`, and replace unsafe CSS selector / console `%c` formatting in `app.js` & `core.js`.

---

### WAVE 5 — Build System, Compiler/WASM Hardening, Integrity Manifest, CI/CD & Test Integrity

#### [SEC-19] Compiler, Linker & WASM `--stack-first` Hardening + `libwgpu_native.so` Integrity Manifest + RPATH & `lsan.supp` Fixes
- **Severity:** High | **CWE:** CWE-119, CWE-494, CWE-426, CWE-401, CWE-829
- **Target Files:**
  - `CMakeLists.txt`, `CMakePresets.json`, `Taskfile.yml`, `docker/Dockerfile`
  - `.github/workflows/ci.yml`, `.github/workflows/pages.yml`, `scripts/run_ci_locally.sh`, `scripts/stage_pages.sh`, `scripts/check_layering.py`
  - `third_party/wgpu-native/wgpu-native-meta/SHA256SUMS` (new), `lsan.supp`
- **Exact Fixes:**
  1. **`CMakeLists.txt`:** Add `CheckPIESupported`, GCC/Clang non-Emscripten hardening (`-Wformat -Wformat-security -Werror=format-security -fstack-protector-strong -fstack-clash-protection`, `x86_64` `-fcf-protection=full`, Release `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3`, `_GLIBCXX_ASSERTIONS`, `-Wl,-z,relro,-z,now -Wl,-z,noexecstack -Wl,--exclude-libs,ALL` — checking ASan/UBSan compatibility cleanly!), separate `CMAKE_BUILD_RPATH` from portable `CMAKE_INSTALL_RPATH` (`$ORIGIN`, `$ORIGIN/../${CMAKE_INSTALL_LIBDIR}`, `CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE`), generate `Version.hpp` into `${CMAKE_CURRENT_BINARY_DIR}` instead of polluting tracked source tree, verify `third_party/wgpu-native/lib/libwgpu_native.so` via `file(SHA256 ...)` against pinned `SHA256SUMS`, and fix `BGL_BUILD_FUZZING` flags on `bgl_scene` / `bgl_core` or document properly.
  2. **`CMakePresets.json`:** Add `CMAKE_SHARED_LINKER_FLAGS` & `CMAKE_EXE_LINKER_FLAGS` to `ubsan` & `tsan` presets.
  3. **WASM `--stack-first`:** Add `--stack-first` to `wasm-ld` in `.github/workflows/ci.yml`, `.github/workflows/pages.yml`, and `scripts/run_ci_locally.sh`. Also tighten `.github/workflows/ci.yml` (`permissions: contents: read`, remove broad `restore-keys`, expand `clang-tidy` scope, fix `pages.yml` job-scoped permissions, guard `rm -rf` in `scripts/stage_pages.sh`).
  4. **`scripts/check_layering.py`:** Match both `"` and `<` includes (`#\s*include\s+[<"]([^>"]+)[>"]`), broaden `FORBIDDEN_IN_PUBLIC` (`webgpu|wgpu|sdl|curl|vulkan|freetype|ft2build|harfbuzz|ktx`), and cover `src/wasm`.
  5. **`lsan.supp`:** Remove overly broad `leak:pthread_once` & `leak:newlocale` (only suppress specific external driver DSOs if actually hit under ASan/LSan).
  6. **`Taskfile.yml` & `docker/Dockerfile`:** Stop deleting tracked `ConanPresets.json` on `clean`; add non-root `builder` user and fixed system deps (`libcurl4-openssl-dev`, `libvulkan-dev`, etc.) to `docker/Dockerfile`.

#### [SEC-20 / QUAL-01] Replace Fake/Tautological Tests & Add Comprehensive Security Regression Suites
- **Severity:** High / Medium | **CWE:** CWE-391
- **Target Files:**
  - `tests/test_golden_image.cpp`, `tests/test_audio_engine.cpp`, `tests/test_crash_handler.cpp`
  - `tests/test_gltf_loader.cpp`, `tests/test_texture_decoders.cpp`, `tests/test_network_fetcher.cpp`, `tests/test_command_line.cpp`, `tests/test_web_unit.js`, `tests/test_browsers_e2e.py`
- **Exact Fixes:**
  1. `test_golden_image.cpp`: Fix tautological `expectedHash = frameChecksum` to compare against actual pinned deterministic hex constant (and optionally GPU render check when available).
  2. `test_audio_engine.cpp`: Use `GTEST_SKIP()` cleanly when headless container lacks audio device instead of silently passing every assertion branch.
  3. `test_crash_handler.cpp`: Verify `sigaction` actually restores previous/default handler after multiple `InstallCrashHandler()` + `UninstallCrashHandler()` calls, and test `SafeFormatDec(INT64_MIN)` / signal-safe formatting.
  4. Add real adversarial C++ GTests & Node JS tests (`test_web_unit.js` importing/exercising real `web/js/core.js`, `web/js/gltf.js`, `web/gltf_worker.js`, `web/js/renderer.js` without copy-paste stubs):
     - glTF 2-node cycle & 30-level diamond DAG rejection
     - Sparse accessor & morph target count bomb rejection, `vertexCount == 0` index clamp, `NaN` vertex/quaternion handling
     - KTX2 invalid layout / high `numLevels` + HDR/EXR `NaN`/`Inf` sanitization
     - FreeType `FT_PIXEL_MODE_MONO` glyph rasterization & oversized font rejection
     - `ImmediateUI` dropdown destroyed-vector lifetime test
     - `NetworkFetcher` SSRF (`127.0.0.1`, `169.254.169.254`, `10.0.0.1`) & local path-traversal / `file://` / `memory://` subresource rejection
     - `test_web_unit.js`: `validateAssetUrl`, CSP/inline-handler check on `web/index.html`, cyclic/DAG `renderTreeDiagram` & `createJsonElement` bounds, `getBufferViewData` / `getAccessorData` negative/OOB/stride tests, `fetchWithProgress` spoofed `Content-Length` abort test, `BglFrameScheduler` single-frame error resilience.
     - `test_browsers_e2e.py`: Replace hardcoded ports with free ephemeral ports and remove `--no-sandbox` where possible / require explicit `--allow-skip`.

---

## 4. Verification Checklist (Must Pass After Wave 5)

```bash
# 1. Code formatting & architecture boundaries
find src apps include samples tests \( -name "*.cpp" -o -name "*.hpp" \) -print0 | xargs -0 clang-format --dry-run -Werror
python3 scripts/check_layering.py
doxygen Doxyfile

# 2. Build & Test (Release + ASan + UBSan)
cmake --build build/Release -j$(nproc)
ctest --test-dir build/Release --output-on-failure -E BrowserE2ETest

# 3. ASan & UBSan suites (verify zero leaks, UAFs, OOBs, or UB)
cmake --build build/asan -j$(nproc) && ctest --test-dir build/asan --output-on-failure -E BrowserE2ETest
cmake --build build/ubsan -j$(nproc) && ctest --test-dir build/ubsan --output-on-failure -E BrowserE2ETest

# 4. Web JS syntax, unit, & pipeline suites
node --check web/js/core.js web/js/gltf.js web/js/math.js web/js/shaders.js web/js/renderer.js web/app.js web/gltf_worker.js
node tests/test_web_unit.js
node tests/test_webgpu_path.js
```

---

## 5. Independent Quality Audit & Verification Status (`v1.2.0`)

Following the Waves 1–5 remediation, an independent codebase, shader, concurrency, and web-runtime quality audit was performed and every fix verified cleanly after the final code edit:

- **Additional Post-Wave Hardening & Fixes Applied:**
  - **Concurrency (`src/core/ThreadPool.hpp`, `src/core/ThreadPool.cpp`):** Scoped `WorkerJoinGuard` strictly inside an inner block so all worker threads join *before* inspecting `state->firstException` (fixing a race where caller-first completion could drop worker exceptions), initialized `activeWorkers` safely under `queueMutex_`, and eliminated empty catch handlers (`bugprone-empty-catch`) via `ExecuteTaskSafely()`.
  - **Core & Scene (`src/core/Types.hpp`, `src/scene/TextureDecoder.cpp`, `src/scene/Animation.hpp`, `src/scene/Scene.cpp`):** Centralized `AABB::IsValid()`, tightened KTX1/KTX2 validation (rejecting array/cubemap/3D/compressed-KTX1/non-Basis pre-compressed KTX2 and validating 8-bit uncompressed `VkFormat` channels with B/R swizzle), and eliminated multi-node `Skin::gpuJointBuffer` races via per-node `NodeGpuBinding` tracking.
  - **GPU & WGSL (`src/gpu/`, `src/gpu/shaders/`):** Added generation counters to defeat raw-pointer ABA reuse (`GpuCuller::boundsGeneration_`, `RayTracingPass::targetGeneration_`, `PipelineManager` cached target generations), preserved `boundsGeneration_` in `GpuCuller::MoveFrom`, replaced find-then-emplace in `RayTracingScene`, added null/bounds/overflow guards across `PostProcessPassRender`, `PipelineIBL`, `PipelineScene`, `PipelineEffects` (fixed `instStart` off-by-one), `SpriteRenderer`, and `RayTracingPass`, switched `AabbRenderer` / `PipelineHelpers` / `PipelineEffects` traversals to cycle/depth guards, wired `parallaxScale_` cleanly into `pbr.wgsl`, initialized `hitUv = rUv` before SSR binary search (`tonemap.wgsl`), and enforced `safeNormalize` + SSBO bounds checks (`pxIdx < arrayLength(&pathAccum)`).
  - **Browser WebGPU Client (`web/`):** Fixed WebGPU `[0, 1]` NDC depth projection and `[0, 1]` near-plane frustum extraction (`web/js/math.js`), raised `mat4.invert`/`normalMatrix4` determinant thresholds to `1e-30`, fixed `tonemapKhronosPbrNeutral` low-range toe return (`return c;`) and `safeNormalize` in `web/js/shaders.js`, enforced GLB v2 header/length validation (`web/gltf_worker.js`, `web/js/gltf.js`), normalized `BglModelCache` URL keys, decoded `data:` URIs locally without weakening CSP `connect-src`, added `BglWasmBridge.sendBufferToWasm` export guards (`web/js/core.js`), implemented `clearResources()`/`clearModelResources()`, per-node skin bindings, unindexed primitive bounds, strict `bufferViews` fallback rules, and `prim.mode`/`matrix` guards in `web/js/renderer.js`, and enforced glTF-before-state load ordering, `SELECT`/`Alt`/`Meta`/`blur` hotkey guards, and `flex` features toggle layout in `web/app.js`.
  - **Viewer (`apps/viewer/`):** Preserved `EnvironmentPreset::CustomHdr` dropdown switching (`ApplicationUI.cpp`) and returned `Application::GetLoadingStatus()` by `const std::string& noexcept` (`Application.hpp`, `ApplicationModelLoader.cpp`).

- **Verification Results Directly Observed After Final Code Edit:**
  - **Formatting, Layering & Python Checks:**
    - `clang-format --dry-run -Werror` across `src/`, `apps/`, `include/`, `samples/`, `tests/` — **Passed** (`exit 0`).
    - `python3 scripts/check_layering.py` — **Passed** (`layering check: OK (0 violation(s))`).
    - `python3 -m py_compile scripts/package_web.py tests/test_browsers_e2e.py` — **Passed** (`exit 0`).
  - **Static Analysis & Documentation:**
    - `run-clang-tidy -p build/Release` (across 70 translation units in `src/`, `apps/`, `samples/`) + `clang-tidy --config-file=.clang-tidy src/wasm/wasm_bridge.cpp` with `WarningsAsErrors: '*'` — **Passed** (`0` warnings/errors in project code).
    - `cppcheck -j$(nproc) --enable=warning,performance,portability --std=c++23 --error-exitcode=1` across `src/` & `apps/` (excluding generated `src/text/EmbeddedFont.cpp`) — **Passed** (`CPPCHECK_CLEAN`).
    - `doxygen Doxyfile` (`WARN_AS_ERROR = YES`) — **Passed** (`exit 0`).
  - **Native Release (`build/Release`) & Sanitizers (`build/asan`, `build/ubsan`):**
    - `./build/Release/bgl_unit_tests` — **337 tests from 51 test suites ran, 337 passed, 0 skipped, 0 failed** (plus **25x repeated `ThreadPoolTest.*`** stress verification passing `10/10` per iteration).
    - `ctest --preset default --output-on-failure -E BrowserE2ETest` — **5/5 CTest suites passed** (`AllUnitTests`, `LayeringCheck`, `NativeHeadlessSmokeTest`, `WebGPUPipelineTest`, `WebUnitTests`).
    - `./build/Release/bgl_viewer --headless assets/ToyCar.glb` — **Passed** (`exit 0`, 10 frames rendered, clean shutdown).
    - `ctest --preset asan --output-on-failure -E BrowserE2ETest` (`LSAN_OPTIONS=suppressions=lsan.supp`, without `leak:newlocale`) — **5/5 CTest suites passed** (`0` leaks/UAF/OOB).
    - `ctest --preset ubsan --output-on-failure -E BrowserE2ETest` — **5/5 CTest suites passed** (`0` UB findings).
  - **Web & Real Browser Verification:**
    - `node --check web/js/core.js web/js/gltf.js web/js/math.js web/js/shaders.js web/js/renderer.js web/app.js web/gltf_worker.js tests/test_web_unit.js tests/test_webgpu_path.js` — **Passed** (`exit 0`).
    - `node tests/test_web_unit.js` — **80/80 tests passed** (`0` failed).
    - `node tests/test_webgpu_path.js` — **Passed** (`ALL WEBGPU PIPELINE TESTS PASSED (100%)`, verifying `ToyCar.glb`, `Sponza.glb`, `RobotExpressive.glb`, `Soldier.glb`, and `web/dist/bgl_engine.wasm`).
    - `python3 tests/test_browsers_e2e.py` — **Passed** (`ALL BROWSER E2E TESTS PASSED`: real Mozilla Firefox + real Google Chrome native WebGPU active, `ToyCar.glb` 3 primitives rendered, live CSP `data-action` UI toggle verified, zero console/runtime errors).
  - **Scope Note:** `em++` is not installed locally in this environment, so `web/dist/bgl_engine.wasm` was validated at its checked-in binary artifact (`54` exports, `7757` bytes) while `src/wasm/wasm_bridge.cpp` was verified via `clang-tidy --config-file=.clang-tidy` (`WarningsAsErrors: '*'`).
