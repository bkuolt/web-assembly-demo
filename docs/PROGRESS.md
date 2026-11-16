# Fortschritt Engine-Umbau

## Baseline (T00)
- Datum: 2026-11-16
- Build: OK
- Tests: 4 bestanden, 0 fehlgeschlagen (AllUnitTests, NativeHeadlessSmokeTest, WebGPUPipelineTest, WebUnitTests; BrowserE2ETest ausgenommen)
- Bereits vorher fehlgeschlagen: keine
- clang-format: OK

## Aufgaben
| Task | Status | Commit | Notiz |
|------|--------|--------|-------|
| T01 | DONE | | Viewer nach apps/viewer verschoben (inkl. ApplicationUI.cpp, ApplicationModelLoader.cpp) |
| T02 | DONE | | bgl_viewer_lib, bgl:: Aliase, scripts/check_layering.py + CTest |
| T03 | DONE | | EngineCore extrahiert; Application erbt von EngineCore (inkl. OnSceneActivated/OnFileDrop) |
| T04 | DONE | | kEngineName -> "bgl Engine" |
| T05 | DONE | | include/bgl/Types.hpp, Settings.hpp, bgl.hpp + test_render_settings |
| T06 | DONE | | include/bgl/Input.hpp, InputState, InputBackendSdl, test_input_state |
| T07 | DONE | | bgl::Engine (Pimpl) + EngineImpl + ApplySettings |
| T08 | DONE | | bgl::App + BGL_RUN + RunApp |
| T09 | DONE | | samples/01_hello_engine + BGL_BUILD_SAMPLES + Layering-Regel |
| T10 | TODO | | |
| T11 | TODO | | |
| T12 | TODO | | |
| T13 | TODO | | |

## Abweichungen von der Anleitung
- Vorab-Split im Repo: `src/app/ApplicationUI.cpp` und `src/app/ApplicationModelLoader.cpp` existieren bereits neben `src/app/Application.cpp`.
