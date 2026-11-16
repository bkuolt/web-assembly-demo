# Engine Refactoring Progress

## Baseline (T00)
- Date: 2026-11-16
- Build: OK
- Tests: 4 passed, 0 failed (`AllUnitTests`, `NativeHeadlessSmokeTest`, `WebGPUPipelineTest`, `WebUnitTests`; `BrowserE2ETest` excluded)
- Pre-existing failures: none
- clang-format: OK

## Tasks
| Task | Status | Commit | Notes |
|------|--------|--------|-------|
| T01 | DONE | `81762cb` | Moved viewer sources to `apps/viewer/` (including `ApplicationUI.cpp`, `ApplicationModelLoader.cpp`) |
| T02 | DONE | `46f077f` | Renamed `bgl_app` to `bgl_viewer_lib`, added `bgl::` aliases, `scripts/check_layering.py` + CTest |
| T03 | DONE | `b2d54d3` | Extracted `EngineCore`; `Application` inherits from `EngineCore` (`OnSceneActivated`, `OnFileDrop`) |
| T04 | DONE | `7835aec` | Renamed `kEngineName` to `"bgl Engine"` |
| T05 | DONE | `c75d0c2` | Added `include/bgl/Types.hpp`, `Settings.hpp`, `bgl.hpp` + `test_render_settings.cpp` |
| T06 | DONE | `da24cbe` | Added `include/bgl/Input.hpp`, `InputState`, `InputBackendSdl`, `test_input_state.cpp` |
| T07 | DONE | `490fc7c` | Added `bgl::Engine` (Pimpl) + `EngineImpl` + `ApplySettings` |
| T08 | DONE | `af1de75` | Added `bgl::App` + `BGL_RUN` + `RunApp` |
| T09 | DONE | `c29b8a4` | Added `samples/01_hello_engine` + `BGL_BUILD_SAMPLES` + `samples` layering rule |
| T10 | DONE | `2d207ce` | Added `bgl::gui` (`Begin`/`End`/`Label`/`Button`/`Checkbox`/`Slider`) + `01_hello_engine` `OnGui` |
| T11 | DONE | `c61d330` | Added `bgl::Entity` + `EntityOps` + `test_entity.cpp` + `samples/02_entities_and_lights` |
| T12 | DONE | `29af6fb` | Verified `LayeringCheck`, zero `WGPU`/`SDL` in `include/bgl/`, zero `Application` in `src/`/`samples/` |
| T13 | DONE | `29af6fb` | Updated `README.md`, `ARCHITECTURE.md`, `GETTING_STARTED.md`, `CHEATSHEET.md`, `AGENT_INSTRUCTIONS.md`, `CHANGELOG.md` |
| T14 | DONE | `a9d9e9a` | Expanded API unit tests (`test_engine_api.cpp`, `EntityHandle`, `InputState`, `RenderSettings`) and cleaned up helpers |
| T15 | DONE | | Replaced `#pragma once` with include guards (56 headers); decomposed oversized `.cpp` files (`EngineCoreScene`, `ApplicationInput`, `WebExports`, `TextureDecoderHdr`, `PipelineMath`, `ImmediateUIOverlays`, `FastGltfSceneBuilder`, `PostProcessPassTargets`) |

## Deviations from the Guide
- Pre-existing split in repo: `ApplicationUI.cpp` and `ApplicationModelLoader.cpp` already existed alongside `Application.cpp`.
