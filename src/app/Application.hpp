/**
 * @file Application.hpp
 * @brief Application lifecycle, event loop, UI-Bridge, and drag-and-drop file ingestion.
 */
#pragma once

#include <memory>
#include <string_view>
#include "core/Camera.hpp"
#include "core/Gamepad.hpp"
#include "gpu/Context.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/Skybox.hpp"
#include "audio/SoundEngine.hpp"
#include "scene/Scene.hpp"
#include "scene/AssetManager.hpp"
#include "scene/FastGltfLoader.hpp"
#include "scene/Quadtree.hpp"
#include "scene/RayPicker.hpp"

namespace bgl::app {

/**
 * @class Application
 * @brief Main engine application runtime coordinating rendering, physics, audio, and inputs.
 */
class Application {
public:
    Application();
    ~Application();

    core::Result<void> Initialize(uint32_t width = 1280, uint32_t height = 720);
    void Update(float deltaTime);
    void Render();
    void Shutdown();

    void ToggleBoundingBoxes() noexcept { showBoundingBoxes_ = !showBoundingBoxes_; }
    void ToggleQuadtreeBounds() noexcept { showQuadtreeBounds_ = !showQuadtreeBounds_; }
    void ToggleSkybox() noexcept { skybox_.Toggle(); }

    [[nodiscard]] bool IsBoundingBoxesEnabled() const noexcept { return showBoundingBoxes_; }
    [[nodiscard]] bool IsQuadtreeBoundsEnabled() const noexcept { return showQuadtreeBounds_; }
    [[nodiscard]] bool IsSkyboxEnabled() const noexcept { return skybox_.IsEnabled(); }

    void OnFileDrop(std::string_view filename, std::span<const uint8_t> data);
    [[nodiscard]] bool IsRunning() const noexcept { return running_; }
    void RequestClose() noexcept { running_ = false; }

    [[nodiscard]] core::Camera& GetCamera() noexcept { return camera_; }
    [[nodiscard]] core::Gamepad& GetGamepad() noexcept { return gamepad_; }
    [[nodiscard]] scene::AssetManager& GetAssetManager() noexcept { return assetManager_; }
    [[nodiscard]] audio::SoundEngine& GetSoundEngine() noexcept { return soundEngine_; }

private:
    bool running_ = false;
    bool showBoundingBoxes_ = true;
    bool showQuadtreeBounds_ = false;

    core::Camera camera_;
    core::Gamepad gamepad_;
    gpu::Context gpuContext_;
    gpu::PipelineManager pipelineManager_;
    gpu::Skybox skybox_;
    audio::SoundEngine soundEngine_;
    void* nativeWindow_ = nullptr;
    scene::AssetManager assetManager_;
    std::unique_ptr<scene::FastGltfLoader> gltfLoader_;
    std::shared_ptr<scene::Scene> activeScene_;
    std::unique_ptr<scene::Quadtree> quadtree_;
};

} // namespace bgl::app

