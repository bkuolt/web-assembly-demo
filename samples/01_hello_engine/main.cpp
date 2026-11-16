// Minimal bgl Engine app: load a glTF model and slowly orbit around it.
// Run from the repository root:  ./build/Release/sample_01_hello_engine
#include <bgl/bgl.hpp>

class HelloEngine : public bgl::App {
public:
    void OnStart() override {
        if (auto result = GetEngine().LoadModel("assets/ToyCar.glb"); !result) {
            BGL_LOG_ERROR("Could not load model: {}", result.error());
            Quit();
        }
    }

    void OnUpdate(float deltaTime) override {
        if (bgl::Input::IsKeyPressed(bgl::Key::Escape)) {
            Quit();
        }
        GetEngine().GetCamera().Orbit(0.3f * deltaTime, 0.0f);
    }
};

BGL_RUN(HelloEngine)
