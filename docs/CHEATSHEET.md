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
| Change render & RT settings | `GetEngine().Settings().exposure = 1.4f;` / `.bloom = false;` / `.rayTracing = true;` / `.rtPathTracing = true;` |
| Poll keyboard | `if (bgl::Input::IsKeyPressed(bgl::Key::Escape)) Quit();` |
| Poll mouse | `glm::vec2 delta = bgl::Input::MouseDelta();` |
| Debug UI slider & tooltip | `bgl::gui::Begin("Panel"); bgl::gui::Slider("EV", GetEngine().Settings().exposure, 0.2f, 4.0f); bgl::gui::Tooltip("Exposure"); bgl::gui::End();` |
| Toggle hover & modals | `bgl::gui::SetHoverEffects(true); bgl::gui::ShowMessage("Title", "Hello!"); bgl::gui::ShowLoadingScreen("Loading", 0.5f, "Assets...");` |
| Load & switch UI fonts | `bgl::gui::SetDefaultFont("sans"); bgl::gui::LoadFont("code", "mono", 20.0f); bgl::gui::PushFont("code"); ... bgl::gui::PopFont();` |
| Animated 2D sprite | `bgl::AnimatedSprite s; s.texture = *GetEngine().LoadSpriteTexture("sheet.png"); s.AddClip(bgl::SpriteClip::FromGrid("walk", 4, 2)); s.Play("walk"); s.Update(dt); GetEngine().DrawSprite(s);` |
| Camera-facing 3D billboard | `bgl::Billboard b; b.texture = bgl::SpriteTexture::CreateRadialGlow(); b.mode = bgl::BillboardMode::CylindricalY; b.blend = bgl::SpriteBlend::Additive; GetEngine().DrawBillboard(b);` |
| Create entity + point light | `auto e = GetEngine().CreateEntity("Lamp"); e.SetPosition({0, 1, 0}); e.AddLight({.intensity = 5.0f});` |
| Find & reparent glTF node | `if (auto car = GetEngine().FindEntity("ToyCar")) e.SetParent(car);` |
