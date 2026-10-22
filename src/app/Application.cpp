/**
 * @file Application.cpp
 * @brief Implementation of Application runtime loop.
 */
#include "app/Application.hpp"
#include "core/Log.hpp"

namespace bgl::app {

Application::Application() = default;

Application::~Application() {
    Shutdown();
}

core::Result<void> Application::Initialize(uint32_t width, uint32_t height) {
    core::InitLogging("info");
    BGL_LOG_INFO("Initializing bgl Engine v0.6.0...");

    gpu::ContextConfig cfg;
    cfg.width = width;
    cfg.height = height;
    if (auto res = gpuContext_.Initialize(cfg); !res) {
        return res;
    }

    if (auto res = pipelineManager_.Initialize(); !res) {
        return res;
    }

    soundEngine_.Initialize();
    gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);

    core::AABB worldBounds;
    worldBounds.minBound = glm::vec3(-100.0f);
    worldBounds.maxBound = glm::vec3(100.0f);
    quadtree_ = std::make_unique<scene::Quadtree>(worldBounds);

    running_ = true;
    BGL_LOG_INFO("bgl Engine initialized successfully.");
    return {};
}

void Application::Update(float deltaTime) {
    if (!running_) return;

    gamepad_.Update();
    const auto& gp = gamepad_.GetState();

    if (gp.connected) {
        camera_.Pan(gp.leftStick.x * deltaTime * 5.0f, gp.leftStick.y * deltaTime * 5.0f);
        camera_.Orbit(gp.rightStick.x * deltaTime * 2.0f, gp.rightStick.y * deltaTime * 2.0f);
        camera_.Zoom((gp.rightTrigger - gp.leftTrigger) * deltaTime * 8.0f);
    }

    camera_.Update(16.0f / 9.0f);
    soundEngine_.UpdateListener(camera_.GetPosition(), glm::normalize(camera_.GetTarget() - camera_.GetPosition()));
}

void Application::Render() {
    if (!running_) return;

    glm::mat4 vp = camera_.GetViewProjectionMatrix();
    skybox_.Render(vp);

    pipelineManager_.BindPipeline(gpu::RenderAlphaMode::Opaque);

    if (showBoundingBoxes_) {
        pipelineManager_.BindWireframePipeline();
    }
}

void Application::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped file '{}' ({} bytes)", filename, data.size());
    if (filename.ends_with(".glb") || filename.ends_with(".gltf")) {
        auto res = gltfLoader_->LoadFromMemory(data, filename);
        if (res) {
            activeScene_ = *res;
            BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", filename);
        } else {
            BGL_LOG_ERROR("Failed to load dropped glTF file: {}", res.error());
        }
    }
}

void Application::Shutdown() {
    if (running_) {
        soundEngine_.Shutdown();
        running_ = false;
        BGL_LOG_INFO("bgl Engine shutdown completed cleanly.");
    }
}

} // namespace bgl::app

