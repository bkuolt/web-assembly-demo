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
#include "ui/DebugOverlay.hpp"

namespace bgl::app {

/**
 * @struct ApplicationConfig
 * @brief Startup parameters and initial rendering states.
 */
struct ApplicationConfig {
    uint32_t width{1280};
    uint32_t height{720};
    std::string assetPath{};
    bool enableSkybox{true};
    bool enableBoundingBoxes{true};
    bool enableQuadtreeBounds{false};
    bool enableDebugOverlay{true};
    bool enableBumpMapping{true};
    bool enableMipmapping{true};
    bool enableShadows{true};
    bool enableReflections{true};
    bool enableOcclusionCulling{true};
};

/**
 * @class Application
 * @brief Main engine application runtime coordinating rendering, physics, audio, and inputs.
 */
class Application {
public:
    Application();
    ~Application();

    core::Result<void> Initialize(const ApplicationConfig& config = {});
    core::Result<void> Initialize(uint32_t width, uint32_t height);
    void Update(float deltaTime);
    void Render();
    void Shutdown();

    void ToggleBoundingBoxes() noexcept { showBoundingBoxes_ = !showBoundingBoxes_; }
    void ToggleQuadtreeBounds() noexcept { showQuadtreeBounds_ = !showQuadtreeBounds_; }
    void ToggleSkybox() noexcept { skybox_.Toggle(); }
    void ToggleDebugOverlay() noexcept { debugOverlay_.Toggle(); }
    void ToggleBumpMapping() noexcept { pipelineManager_.ToggleBumpMapping(); }
    void ToggleMipmapping() { pipelineManager_.ToggleMipmapping(activeScene_.get()); }
    void ToggleShadows() noexcept { pipelineManager_.ToggleShadows(); }
    void ToggleReflections() noexcept { pipelineManager_.ToggleReflections(); }
    void ToggleOcclusionCulling() noexcept { pipelineManager_.ToggleOcclusionCulling(); }

    [[nodiscard]] bool IsBoundingBoxesEnabled() const noexcept { return showBoundingBoxes_; }
    [[nodiscard]] bool IsQuadtreeBoundsEnabled() const noexcept { return showQuadtreeBounds_; }
    [[nodiscard]] bool IsSkyboxEnabled() const noexcept { return skybox_.IsEnabled(); }
    [[nodiscard]] bool IsDebugOverlayEnabled() const noexcept { return debugOverlay_.IsVisible(); }
    [[nodiscard]] bool IsBumpMappingEnabled() const noexcept { return pipelineManager_.IsBumpMappingEnabled(); }
    [[nodiscard]] bool IsMipmappingEnabled() const noexcept { return pipelineManager_.IsMipmappingEnabled(); }
    [[nodiscard]] bool IsShadowsEnabled() const noexcept { return pipelineManager_.IsShadowsEnabled(); }
    [[nodiscard]] bool IsReflectionsEnabled() const noexcept { return pipelineManager_.IsReflectionsEnabled(); }
    [[nodiscard]] bool IsOcclusionCullingEnabled() const noexcept { return pipelineManager_.IsOcclusionCullingEnabled(); }

    void OnFileDrop(std::string_view filename, std::span<const uint8_t> data);
    core::Result<void> LoadScene(std::span<const uint8_t> data, std::string_view baseUri);
    core::Result<void> LoadSceneFromUri(std::string_view uri);
    [[nodiscard]] bool IsRunning() const noexcept { return running_; }
    void RequestClose() noexcept { running_ = false; }

    [[nodiscard]] core::Camera& GetCamera() noexcept { return camera_; }
    [[nodiscard]] core::Gamepad& GetGamepad() noexcept { return gamepad_; }
    [[nodiscard]] scene::AssetManager& GetAssetManager() noexcept { return assetManager_; }
    [[nodiscard]] audio::SoundEngine& GetSoundEngine() noexcept { return soundEngine_; }
    [[nodiscard]] ui::DebugOverlay& GetDebugOverlay() noexcept { return debugOverlay_; }

private:
    void ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri);
    void CacheSceneMetrics();

    bool running_{false};
    bool showBoundingBoxes_{true};
    bool showQuadtreeBounds_{false};

    core::Camera camera_{};
    core::Gamepad gamepad_{};
    gpu::Context gpuContext_{};
    gpu::PipelineManager pipelineManager_{};
    gpu::Skybox skybox_{};
    ui::DebugOverlay debugOverlay_{};
    audio::SoundEngine soundEngine_{};
    void* nativeWindow_{nullptr};
    scene::AssetManager assetManager_{};
    std::unique_ptr<scene::FastGltfLoader> gltfLoader_{nullptr};
    std::shared_ptr<scene::Scene> activeScene_{nullptr};
    std::unique_ptr<scene::Quadtree> quadtree_{nullptr};

    std::string currentModelName_{"None"};
    size_t totalPrimitives_{0};
    size_t totalVertices_{0};
    size_t totalIndices_{0};
    size_t cachedQuadtreeNodeCount_{0};
};

} // namespace bgl::app

