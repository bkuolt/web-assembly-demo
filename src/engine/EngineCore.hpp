/**
 * @file EngineCore.hpp
 * @brief Reusable engine runtime: native window, WebGPU context, scene loading, picking, sun/time-of-day
 *        and the full HDR frame render sequence. Contains no viewer UI or hotkeys.
 */
#pragma once

#include "audio/SoundEngine.hpp"
#include "core/Camera.hpp"
#include "core/Gamepad.hpp"
#include "core/Types.hpp"
#include "gpu/Context.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/Skybox.hpp"
#include "scene/AssetManager.hpp"
#include "scene/FastGltfLoader.hpp"
#include "scene/Quadtree.hpp"
#include "scene/RayPicker.hpp"
#include "scene/Scene.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

struct SDL_Window;

namespace bgl::engine {

/**
 * @struct EngineCoreConfig
 * @brief Minimal startup parameters of the engine runtime.
 */
struct EngineCoreConfig {
    uint32_t width{1280};
    uint32_t height{720};
    std::string windowTitle{}; ///< Empty = bgl::kEngineName
};

/**
 * @class EngineCore
 * @brief Window + GPU + scene + render-sequence runtime shared by the viewer and the public bgl::Engine.
 */
class EngineCore {
public:
    EngineCore();
    virtual ~EngineCore();
    EngineCore(const EngineCore&) = delete;
    EngineCore& operator=(const EngineCore&) = delete;

    /** @brief Creates window (native), WebGPU context, pipelines, skybox, audio, glTF loader. Does NOT load a scene. */
    core::Result<void> Initialize(const EngineCoreConfig& config);

    /** @brief Advances engine simulation: shader hot-reload, animation, quadtree refit, weather, auto-LOD, day/night,
     * camera matrices, audio listener. */
    void UpdateSimulation(float deltaTime);

    /** @brief Records and submits all rendering passes (shadow map, HDR clustered PBR, post-process, tonemap, overlay
     * hook). */
    void Render();

    /** @brief Releases GPU resources and the native window. Safe to call twice. */
    void Shutdown();

    /** @brief Toggles rendering of 3D scene and mesh primitive bounding boxes. */
    void ToggleBoundingBoxes() noexcept { showBoundingBoxes_ = !showBoundingBoxes_; }

    /** @brief Toggles rendering of spatial Quadtree ground partition cells. */
    void ToggleQuadtreeBounds() noexcept { showQuadtreeBounds_ = !showQuadtreeBounds_; }

    /** @brief Selects active glTF animation by index (-1 to disable). */
    void SetAnimation(int index) noexcept {
        if (activeScene_)
            activeScene_->SetAnimation(index);
    }

    /** @brief Returns currently playing animation index or -1. */
    [[nodiscard]] int GetActiveAnimationIndex() const noexcept {
        return activeScene_ ? activeScene_->activeAnimationIndex : -1;
    }

    /** @brief Returns total number of animations loaded in active scene. */
    [[nodiscard]] size_t GetAnimationCount() const noexcept {
        return activeScene_ ? activeScene_->animations.size() : 0;
    }

    /** @brief Sets animation playback state (playing / paused). */
    void SetAnimationPlaying(bool playing) noexcept {
        if (activeScene_)
            activeScene_->animationPlaying = playing;
    }

    /** @brief Queries whether animation playback is currently active. */
    [[nodiscard]] bool IsAnimationPlaying() const noexcept {
        return activeScene_ ? activeScene_->animationPlaying : false;
    }

    /** @brief Focuses camera target and distance onto current selection or scene. */
    void FocusSelection() noexcept;

    /** @brief Frames entire scene AABB. */
    void FrameAll() noexcept;

    /** @brief Selects scene node by index (-1 clears selection). */
    void SelectNodeByIndex(int index) noexcept;
    void SelectNode(std::shared_ptr<scene::SceneNode> node, int index = -1) noexcept;
    void ClearSelection() noexcept {
        selectedNode_ = nullptr;
        selectedNodeIndex_ = -1;
    }
    [[nodiscard]] std::shared_ptr<scene::SceneNode> GetSelectedNode() const noexcept { return selectedNode_; }
    [[nodiscard]] int GetSelectedNodeIndex() const noexcept { return selectedNodeIndex_; }

    /**
     * @brief Ray-pick object under screen coordinate and select it.
     * @return Selected nodeIndex, or -1 if no node was hit.
     */
    int PickNodeAt(float screenX, float screenY, float screenWidth, float screenHeight);

    /** @brief Switches to a glTF camera by index (-1 for default interactive camera). */
    void SelectCamera(int cameraIndex) noexcept;
    void CycleCamera() noexcept;
    [[nodiscard]] int GetCameraCount() const noexcept;
    [[nodiscard]] std::string_view GetCameraName(int index) const noexcept;
    [[nodiscard]] int GetActiveCameraIndex() const noexcept { return activeCameraIndex_; }

    /** @brief Sun position and diurnal cycle live controls. */
    void SetTimeOfDay(float hour) noexcept;
    [[nodiscard]] float GetTimeOfDay() const noexcept { return timeOfDay_; }
    void ToggleDayNightCycle() noexcept { dayNightCycleActive_ = !dayNightCycleActive_; }
    void SetDayNightCycle(bool active) noexcept { dayNightCycleActive_ = active; }
    [[nodiscard]] bool IsDayNightCycleActive() const noexcept { return dayNightCycleActive_; }
    void SetDayNightSpeed(float hoursPerSec) noexcept { dayNightSpeed_ = std::clamp(hoursPerSec, 0.05f, 12.0f); }
    [[nodiscard]] float GetDayNightSpeed() const noexcept { return dayNightSpeed_; }
    void SetSunAzimuthElevation(float azimuthRad, float elevationRad) noexcept;
    [[nodiscard]] float GetSunAzimuth() const noexcept { return sunAzimuth_; }
    [[nodiscard]] float GetSunElevation() const noexcept { return sunElevation_; }
    [[nodiscard]] glm::vec3 GetSunDirection() const noexcept { return sunDir_; }
    void UpdateSunDirection() noexcept;

    [[nodiscard]] bool IsBoundingBoxesEnabled() const noexcept { return showBoundingBoxes_; }
    [[nodiscard]] bool IsQuadtreeBoundsEnabled() const noexcept { return showQuadtreeBounds_; }

    /**
     * @brief Ingests raw file bytes dropped directly onto the viewer window.
     * @param filename Target file name or path.
     * @param data Byte span containing file contents.
     */
    virtual void OnFileDrop(std::string_view filename, std::span<const uint8_t> data);

    /**
     * @brief Parses and activates a 3D scene from an in-memory glTF / GLB buffer.
     * @param data Byte view over binary glTF / GLB asset data.
     * @param baseUri Base directory or URL for resolving external buffers/textures.
     * @return Result indicating success or error string.
     */
    core::Result<void> LoadScene(std::span<const uint8_t> data, std::string_view baseUri);

    /**
     * @brief Loads and activates a 3D model from a local file path, `file://` URI, or `http://`/`https://` URL.
     * @param uri Local path or remote URL to the glTF/GLB model.
     * @return Result indicating success or error string.
     */
    core::Result<void> LoadSceneFromUri(std::string_view uri);

    [[nodiscard]] bool IsRunning() const noexcept { return running_; }
    void RequestClose() noexcept { running_ = false; }

    [[nodiscard]] core::Camera& GetCamera() noexcept { return camera_; }
    [[nodiscard]] core::Gamepad& GetGamepad() noexcept { return gamepad_; }
    [[nodiscard]] scene::AssetManager& GetAssetManager() noexcept { return assetManager_; }
    [[nodiscard]] audio::SoundEngine& GetSoundEngine() noexcept { return soundEngine_; }

    /** @brief Switches active KTX2 texture transcoding target at runtime and re-uploads loaded KTX2 assets. */
    void SetKtxCompressionTarget(scene::KtxTranscodeTarget target);

    // Internal accessors for advanced users (viewer, EngineImpl). Not part of the public bgl/ API.
    [[nodiscard]] gpu::Context& GetContext() noexcept { return gpuContext_; }
    [[nodiscard]] gpu::PipelineManager& GetPipeline() noexcept { return pipelineManager_; }
    [[nodiscard]] gpu::Skybox& GetSkybox() noexcept { return skybox_; }
    [[nodiscard]] const std::shared_ptr<scene::Scene>& GetScene() const noexcept { return activeScene_; }

protected:
    /** @brief Called inside ActivateScene() after currentModelName_ is set. Override for UI sync hooks. */
    virtual void OnSceneActivated(std::string_view sourceUri) { (void)sourceUri; }
    /** @brief Called inside Render() right before the swapchain pass is opened. Override to prepare overlay resources.
     */
    virtual void OnPrepareOverlay() {}
    /** @brief Called inside Render() after tonemapping, with the swapchain pass still open. Override to draw HUD/UI. */
    virtual void OnRenderOverlay(WGPURenderPassEncoder swapchainPass) { (void)swapchainPass; }

    void ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri);
    void CacheSceneMetrics();
    void PollShaderHotReload(float deltaTime);

    bool running_{false};
    bool showBoundingBoxes_{true};
    bool showQuadtreeBounds_{false};
    bool wireframeMode_{false};

    core::Camera camera_{};
    core::Gamepad gamepad_{};
    gpu::Context gpuContext_{};
    gpu::PipelineManager pipelineManager_{};
    gpu::Skybox skybox_{};
    audio::SoundEngine soundEngine_{};
    SDL_Window* nativeWindow_{nullptr};
    scene::AssetManager assetManager_{};
    std::unique_ptr<scene::FastGltfLoader> gltfLoader_{nullptr};
    std::shared_ptr<scene::Scene> activeScene_{nullptr};
    std::unique_ptr<scene::Quadtree> quadtree_{nullptr};

    // Selection & Interactivity
    std::shared_ptr<scene::SceneNode> selectedNode_{nullptr};
    int selectedNodeIndex_{-1};

    // Camera Navigation
    int activeCameraIndex_{-1};

    // Sun & Time-of-Day
    float timeOfDay_{14.0f}; // 14:00 (2 PM)
    bool dayNightCycleActive_{false};
    float dayNightSpeed_{1.0f}; // hours per second
    float sunAzimuth_{0.8f};
    float sunElevation_{0.95f};
    glm::vec3 sunDir_{0.35f, 1.0f, 0.25f};

    std::string currentModelName_{"None"};
    size_t totalPrimitives_{0};
    size_t totalVertices_{0};
    size_t totalIndices_{0};
    size_t cachedQuadtreeNodeCount_{0};
    uint64_t lastQuadtreeTransformVersion_{0};
    float hotReloadPollTimer_{0.0f};
    float lastDeltaTime_{0.016f};
    float elapsedTime_{0.0f};
};

} // namespace bgl::engine
