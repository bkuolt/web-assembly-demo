# Master-Spezifikation & Arbeitsanweisung für den AI-Coding-Agenten (Antigravity)
**Projekt:** Modularer C++23 WebGPU glTF 2.1 High-Performance Renderer (`bgl`-Engine)  
**Zielumgebung:** Linux (amd64) – Dual-Target: WebAssembly (`wasm32-unknown-emscripten`) & Nativer Desktop-Build (`bgl_viewer`)  
**Version:** `0.6.0`

---

## 1. Strikte Kernrichtlinien (Non-Negotiable Rules)

1. **Einheitlicher Namespace:**
   - Sämtlicher C++-Code MUSS zwingend im Root-Namespace `bgl` liegen.
   - Module gliedern sich in: `bgl::core`, `bgl::gpu`, `bgl::scene`, `bgl::audio` und `bgl::app`.
2. **Sprachstandard C++23:**
   - 100 % modernes C++23 (`-std=c++23`).
   - Keine C++-Exceptions über Modul- oder C-ABI-Grenzen. Nutze `bgl::core::Result<T>` (`std::expected<T, std::string>`).
   - Nutze `std::span` für speicherschonende Views, `constexpr` für Konstanten und striktes RAII für alle WebGPU-, Audio- und OS-Ressourcen.
3. **Dokumentation:**
   - Schlank, präzise, ausnahmslos auf **Englisch**. Jede Klasse, Methode und Struktur wird mit Doxygen-Kommentaren (`///`) versehen.
4. **Zero-GLFW-Architektur:**
   - GLFW ist strikt verboten.
   - **Wasm:** Native Emscripten HTML5 API (`<emscripten/html5.h>`).
   - **Nativ (Linux amd64):** SDL2/SDL3 für Windowing und Hardware-Events.
5. **Speicherarchitektur:**
   - Kein unkontrolliertes `new`/`delete`.
   - Einsatz von Block-MemoryPools (`bgl::core::MemoryPool<T>`) und polymorpher Speicherverwaltung (`std::pmr::unsynchronized_pool_resource`, `std::pmr::monotonic_buffer_resource`) für Geometrie-Puffer und temporäre Dekodier-Staging-Bereiche.
6. **Modulare Shared Libraries (`.so`):**
   - Kompiliert mit `-fPIC` in separate dynamische Bibliotheken:
     - `libbgl_core.so`
     - `libbgl_gpu.so`
     - `libbgl_scene.so`
     - `libbgl_audio.so`
     - `libbgl_app.so`

---

## 2. Tech-Stack & Conan 2.x Rezept (`conanfile.py`)

Abhängigkeiten werden über Conan 2.x und zwei Profile (`profiles/emscripten_wasm` und `profiles/linux_amd64`) aufgelöst:
- `fastgltf/0.7.2` (glTF 2.1 Parser, SIMD-beschleunigt)
- `glm/1.0.1` (Vektor- und Matrizenrechnung)
- `spdlog/1.14.0` (Strukturiertes Logging mit Mikro- und Millisekunden-Timern)
- `libpng/1.6.43` (PNG-Dekodierung im Arbeitsspeicher)
- `libjpeg-turbo/3.0.2` (JPEG-Dekodierung)
- `libwebp/1.3.2` (Native WebP-Dekodierung via `EXT_texture_webp`)
- `ktx/4.3.2` (BasisU / KTX2 Texturtranskodierung)
- `miniaudio/0.11.21` (3D Spatial Audio & Web Audio Backend)
- `gtest/1.14.0` (Unit-Testing Framework)

---

## 3. Granulare CMake-Optionen (`CMakeLists.txt`)

Jedes Feature und Format muss per CMake ein- und ausschaltbar sein:
```cmake
option(BGL_ENABLE_FORMAT_PNG   "Enable PNG decoding via libpng"         ON)
option(BGL_ENABLE_FORMAT_JPEG  "Enable JPEG decoding via libjpeg-turbo" ON)
option(BGL_ENABLE_FORMAT_WEBP  "Enable WebP decoding via libwebp"       ON)
option(BGL_ENABLE_FORMAT_KTX2  "Enable KTX2/BasisU decoding via libktx" ON)
option(BGL_ENABLE_AUDIO        "Enable 3D Audio via miniaudio"          ON)
option(BGL_ENABLE_GAMEPAD      "Enable Gamepad controller support"      ON)
option(BGL_ENABLE_SKYBOX       "Enable Skybox and IBL reflections"      ON)
option(BGL_ENABLE_MODEL_TOYCAR "Include default ToyCar.glb model"       ON)
option(BGL_BUILD_TESTING       "Build GTest unit test suite"            ON)
```

---

## 4. Projektstruktur (Blueprint)

```text
.
├── .github/workflows/ci.yml       # CI/CD Pipeline (Native & Wasm Build + GTest)
├── .vscode/                       # VS Code Konfiguration (clangd, tasks.json)
├── assets/
│   ├── ToyCar.glb                 # Standard-glTF 2.1 Modell
│   └── env/skybox.ktx2            # Cubemap für Skybox & IBL
├── docker/
│   └── Dockerfile                 # Reproduzierbarer Build-Container (Ubuntu 24.04, emsdk, Conan)
├── profiles/
│   ├── emscripten_wasm            # Conan-Profil für WebAssembly
│   └── linux_amd64                # Conan-Profil für Linux amd64 Nativ
├── src/
│   ├── core/                      # -> libbgl_core.so
│   │   ├── Log.hpp                # spdlog Makros, ScopedTimer, dynamic level
│   │   ├── Types.hpp              # AABB, Ray, Vertex, 16-Byte Uniforms, Result<T>
│   │   ├── MemoryPool.hpp/.cpp    # Block MemoryPool & PMR-Ressourcen
│   │   ├── NetworkFetcher.hpp/.cpp# Emscripten Fetch / libcurl Wrapper
│   │   ├── Gamepad.hpp/.cpp       # Gamepad API mit 0.12f Deadzone
│   │   └── Camera.hpp/.cpp        # Orbit-, Pan-, Zoom-Kamera
│   ├── gpu/                       # -> libbgl_gpu.so
│   │   ├── Context.hpp/.cpp       # WebGPU RAII Wrapper & Surface-Handling
│   │   ├── Shaders.hpp            # Eingebettete WGSL Shader (PBR, IBL, Wireframe)
│   │   ├── Skybox.hpp/.cpp        # Skybox Pipeline & Cubemap IBL Pass
│   │   └── Pipeline.hpp/.cpp      # Opaque, Mask, Blend & Bounding-Box Pipelines
│   ├── scene/                     # -> libbgl_scene.so
│   │   ├── Scene.hpp              # Szenengraph & std::pmr Container
│   │   ├── AssetManager.hpp/.cpp  # 64-Bit FNV-1a Hashing, Deduplizierung, PMR Scratch-Arena
│   │   ├── TextureDecoder.hpp/.cpp# libpng, libjpeg, libwebp & ktx Decoder
│   │   ├── FastGltfLoader.hpp/.cpp# glTF 2.1 Loader, Tangenten, 4-Byte Alignment
│   │   ├── Quadtree.hpp/.cpp      # Räumliche XZ-Partitionierung, Frustum-Culling, LOD-Stufen
│   │   └── RayPicker.hpp/.cpp     # Unproject Ray & Möller-Trumbore Dreiecks-Schnitt
│   ├── audio/                     # -> libbgl_audio.so
│   │   └── SoundEngine.hpp/.cpp   # miniaudio Wrapper, KHR_audio glTF-Node-Bindung
│   ├── app/                       # -> libbgl_app.so
│   │   └── Application.hpp/.cpp   # Event-Loop, Drag&Drop, UI-Bridge, JS-Ladebalken
│   └── main.cpp                   # Entrypoint für bgl_viewer (Nativ) & Wasm-Loop
├── tests/                         # -> bgl_unit_tests
│   ├── test_main.cpp
│   ├── test_memory_pool.cpp
│   ├── test_asset_manager.cpp
│   ├── test_quadtree_lod.cpp
│   └── test_ray_picker.cpp
├── web/
│   └── shell.html                 # Minimal HTML5 Canvas, JS-Ladebalken, BBox-Toggle & Drag&Drop
├── CMakeLists.txt                 # Modulares CMake mit Shared Libraries
├── conanfile.py                   # Conan 2.x Definition
├── Taskfile.yml                   # Zentrale Automatisierungsbefehle
└── README.md                      # Schlanke, englische Dokumentation
```

---

## 5. Detaillierte Funktions-Spezifikationen

### 5.1 MemoryPool & Polymorphe Allokatoren (`bgl::core`)

* Implementiere `bgl::core::MemoryPool<T, BlockSize>` mit Freelist für feste Blockgrößen.
* Implementiere eine Staging-Scratch-Arena auf Basis von `std::pmr::monotonic_buffer_resource` für temporäre Dekodier- und Vertex-Puffer, die nach jedem Frame oder Asset-Upload in $O(1)$ zurückgesetzt wird.
* Verhindere Heap-Fragmentierung in WebAssembly durch `std::pmr::vector` für Vertices, Indizes und Szenenknoten.

### 5.2 AssetManager & glTF 2.1 Netzwerk-Streaming (`bgl::scene`)

* **glTF 2.1 Support:** Nutzt `fastgltf::Parser` mit `EXT_texture_webp`, `KHR_texture_basisu` und `KHR_audio`.
* **Deduplizierung & Caching:** 64-Bit FNV-1a Hash über Asset-URIs bzw. Speicher-Spans. Mehrfach referenzierte Texturen oder Meshes werden nur ein einziges Mal dekodiert und in den VRAM übertragen.
* **Asynchrones Laden:** HTTP/HTTPS-Downloads für `.glb` und mehrteilige `.gltf`-Dateien (inkl. externer `.bin`-Buffer und Texturen) via Emscripten Fetch API (`-sFETCH=1`) im Browser bzw. `libcurl` im nativen Build.

### 5.3 Quadtree LOD & Polygon-Interaktivität (Ray-Picking)

* **Quadtree:** Räumliche 2D-Hierarchie auf der XZ-Ebene. Führt Frustum-Culling durch und schaltet Geometrie basierend auf der Kamera-Distanz in Detailstufen (`LOD0` nah, `LOD1` mittel, `LOD2` fern).
* **Ray-Picking (`bgl::scene::RayPicker`):**
1. *Broad-Phase:* Ray-AABB-Test gegen die Bounding-Boxes des Quadtrees.
2. *Narrow-Phase:* Exakter Möller-Trumbore-Algorithmus gegen die Dreiecke des Meshes. Das getroffene Polygon wird visuell hervorgehoben und in den Log ausgegeben.

### 5.4 Gamepad & 3D Spatial Audio

* **Gamepad (`bgl::core::Gamepad`):** W3C Gamepad API (Wasm) bzw. SDL2 Game-Controller (Nativ).
* Radiale Deadzone: `0.12f`.
* Linker Stick = Pan; Rechter Stick = Orbit; Triggers = stufenloser Zoom.
* Button A = Ray-Picking im Fadenkreuz; Button Y = Bounding-Box-Toggle.
* **Audio (`bgl::audio::SoundEngine`):** `miniaudio`-Integration. Bindet Audio-Emitters an Szenenknoten (z. B. Motorsound an das `ToyCar.glb`) und berechnet positionsabhängigen 3D-Klang basierend auf der Kameraposition.

### 5.5 Browser-Shell & JavaScript-Bridge (`web/shell.html`)

* Bereitstellung von `window.updateLoadProgress(percent, msg)` für den Shader- und Modellladebalken.
* Drag-and-Drop: Ermöglicht das direkte Hineinziehen von `.glb`-, `.gltf`- und `.webp`-Dateien. Der Speicher wird via `_malloc` und `Module.HEAPU8` direkt in den C++-Heap übergeben (`_bgl_load_file_buffer`).
* Tastaturkürzel: Taste `B` schaltet AABB-Bounding-Boxes ein/aus.

---

## 6. Schritt-für-Schritt Umsetzungsphasen

* **Phase 1: Toolchain, Conan & CMake Setup**
  * Erstelle `conanfile.py`, `profiles/`, `CMakeLists.txt` (mit allen modularen `.so`-Targets und CMake-Optionen) sowie `Taskfile.yml`.
  * Stelle sicher, dass `task setup` und `task configure` ohne Fehler durchlaufen.

* **Phase 2: Core-Infrastruktur & Speicher-Management**
  * Implementiere `bgl::core::Log` (spdlog mit ScopedTimer), `bgl::core::Types` und `bgl::core::MemoryPool` inklusive PMR-Wrappern.
  * Implementiere `bgl::core::Camera` und `bgl::core::Gamepad` ohne GLFW.

* **Phase 3: WebGPU Kontext, Pipeline & Audio**
  * Implementiere `bgl::gpu::Context` (Dual-Target: Wasm-Canvas & SDL-Surface), eingebettete WGSL-PBR-Shader und Skybox/IBL.
  * Integriere `bgl::audio::SoundEngine` via `miniaudio`.

* **Phase 4: AssetManager, glTF 2.1 Loader & Textur-Decoder**
  * Implementiere `bgl::scene::TextureDecoder` (WebP, KTX2, PNG, JPEG).
  * Implementiere `bgl::scene::AssetManager` und `bgl::scene::FastGltfLoader` mit asynchronem Streaming und Content-Hashing.
  * Binde das Standardmodell `assets/ToyCar.glb` ein.

* **Phase 5: Quadtree LOD, Ray-Picking & Shell-Integration**
  * Implementiere den `bgl::scene::Quadtree` und `bgl::scene::RayPicker` (Möller-Trumbore).
  * Richte `web/shell.html` mit Ladebalken, BBox-Toggle und Drag-and-Drop ein.

* **Phase 6: Testing, Docker & Release-Tagging**
  * Implementiere alle GTest-Suiten in `tests/` und führe sie mit `task test` aus.
  * Erstelle das `docker/Dockerfile` für reproduzierbare Builds.
  * Schließe den Release-Prozess ab und setze den annotierten Git-Tag `0.6.0`:
    `git tag -a 0.6.0 -m "Release 0.6.0: Full modular bgl renderer"`

