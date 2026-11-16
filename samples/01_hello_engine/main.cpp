// Minimal bgl Engine app: load a glTF model, orbit cleanly, and drive the sleek SDF GUI.
// Run from the repository root:  ./build/Release/sample_01_hello_engine
#include <bgl/bgl.hpp>

class HelloEngine : public bgl::App {
public:
    void OnStart() override {
        bgl::gui::SetDefaultFont("sans");
        if (auto result = GetEngine().LoadModel("assets/ToyCar.glb"); !result) {
            bgl::gui::ShowMessage("Load Failed", result.error(), bgl::gui::MessageType::Error);
        }
    }

    void OnUpdate(float deltaTime) override {
        if (bgl::Input::IsKeyPressed(bgl::Key::Escape)) {
            Quit();
        }
        if (autoOrbit_) {
            GetEngine().GetCamera().Orbit(0.3f * deltaTime, 0.0f);
        }
    }

    void OnGui() override {
        bgl::gui::Begin("Hello bgl");
        if (bgl::gui::Section("Scene Controls", true)) {
            bgl::gui::Slider("Exposure", GetEngine().Settings().exposure, 0.2f, 4.0f);
            bgl::gui::Tooltip("Adjust HDR camera exposure");
            bgl::gui::Toggle("Auto Orbit", autoOrbit_, "Continuously orbit the active camera");
            if (bgl::gui::Button("About bgl GUI")) {
                bgl::gui::ShowMessage("bgl Engine GUI",
                                      "Frosted-glass SDF widgets with hover highlights and OS font discovery.");
            }
        }
        bgl::gui::End();
    }

private:
    bool autoOrbit_{true};
};

BGL_RUN(HelloEngine)
