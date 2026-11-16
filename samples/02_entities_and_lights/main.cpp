// Loads a model, adds a red point light above it and orbits the light around the model.
#include <bgl/bgl.hpp>
#include <cmath>

class EntitiesAndLights : public bgl::App {
public:
    void OnStart() override {
        if (auto result = GetEngine().LoadModel("assets/ToyCar.glb"); !result) {
            BGL_LOG_ERROR("Could not load model: {}", result.error());
            Quit();
            return;
        }
        // Entities created after LoadModel() belong to the loaded scene.
        light_ = GetEngine().CreateEntity("RedLight");
        light_.SetPosition({0.0f, 0.5f, 0.0f});
        light_.AddLight({.type = bgl::LightType::Point, .color = {1.0f, 0.1f, 0.1f}, .intensity = 5.0f, .range = 3.0f});
    }

    void OnUpdate(float deltaTime) override {
        time_ += deltaTime;
        light_.SetPosition({std::cos(time_) * 0.4f, 0.5f, std::sin(time_) * 0.4f});
        if (bgl::Input::IsKeyPressed(bgl::Key::Escape)) {
            Quit();
        }
    }

private:
    bgl::Entity light_{};
    float time_{0.0f};
};

BGL_RUN(EntitiesAndLights)
