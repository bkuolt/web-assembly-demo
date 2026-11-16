# bgl Engine Cheatsheet

Single umbrella include:
```cpp
#include <bgl/bgl.hpp>
```

## App Lifecycle
```cpp
class MyApp : public bgl::App {
public:
    bgl::EngineConfig Configure() override { return {.width = 1280, .height = 720, .title = "My App"}; }
    void OnStart() override { (void)GetEngine().LoadModel("assets/ToyCar.glb"); }
    void OnUpdate(float dt) override { /* game/camera logic */ }
    void OnGui() override { /* bgl::gui::* calls */ }
};
BGL_RUN(MyApp)
```

## Common Calls

| Task | Snippet |
| :--- | :--- |
| Load glTF/GLB (file or URL) | `GetEngine().LoadModel("assets/ToyCar.glb");` |
| Load glTF/GLB from memory | `GetEngine().LoadModel(bytes, "memory.glb");` |
| Frame whole scene | `GetEngine().FrameAll();` |
| Orbit / move camera | `bgl::Camera& cam = GetEngine().GetCamera(); cam.Orbit(0.5f * dt, 0.0f); cam.SetMode(bgl::CameraMode::Fly);` |
| Change render settings | `GetEngine().Settings().exposure = 1.4f;` / `.bloom = false;` / `.weather = bgl::Weather::Rain;` |
| Poll keyboard | `if (bgl::Input::IsKeyPressed(bgl::Key::Escape)) Quit();` |
| Poll mouse | `glm::vec2 delta = bgl::Input::MouseDelta();` |
| Debug UI slider | `bgl::gui::Begin("Panel"); bgl::gui::Slider("EV", GetEngine().Settings().exposure, 0.2f, 4.0f); bgl::gui::End();` |
| Create entity + point light | `auto e = GetEngine().CreateEntity("Lamp"); e.SetPosition({0, 1, 0}); e.AddLight({.intensity = 5.0f});` |
| Find & reparent glTF node | `if (auto car = GetEngine().FindEntity("ToyCar")) e.SetParent(car);` |
