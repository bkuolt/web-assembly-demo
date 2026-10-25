/**
 * @file Application.hpp
 * @brief Application lifecycle, event loop, UI-Bridge, and drag-and-drop file ingestion.
 */
#pragma once

#include "audio/SoundEngine.hpp"
#include "core/Camera.hpp"
#include "core/Gamepad.hpp"
#include "gpu/Context.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/Skybox.hpp"
#include "scene/AssetManager.hpp"
#include "scene/FastGltfLoader.hpp"
#include "scene/Quadtree.hpp"
#include "scene/RayPicker.hpp"
#include "scene/Scene.hpp"
#include "ui/DebugOverlay.hpp"
#include <memory>
#include <string_view>

struct SDL_Window;

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
    bool enableAmbientOcclusion{true};
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

    /**
     * @brief Initializes all engine subsystems (WebGPU context, audio, pipelines, default scene).
     * @param config Application startup parameters, window geometry, and initial feature states.
     * @return Result indicating success or an error description string.
     */
    core::Result<void> Initialize(const ApplicationConfig& config = {});

    /**
     * @brief Convenience overload initializing the application with given dimensions.
     * @param width Initial viewport width in pixels.
     * @param height Initial viewport height in pixels.
     * @return Result indicating success or an error description string.
     */
    core::Result<void> Initialize(uint32_t width, uint32_t height);

    /**
     * @brief Performs per-frame physics, camera movement, gamepad polling, and HUD updates.
     * @param deltaTime Elapsed wall-clock time since previous frame in seconds.
     */
    void Update(float deltaTime);

    /**
     * @brief Records and submits all rendering passes (shadow map, HDR clustered PBR, tonemap, HUD).
     */
    void Render();

    /**
     * @brief Gracefully releases WebGPU device resources, pipelines, and native window handles.
     */
    void Shutdown();

    /** @brief Toggles rendering of 3D scene and mesh primitive bounding boxes. */
    void ToggleBoundingBoxes() noexcept { showBoundingBoxes_ = !showBoundingBoxes_; }

    /** @brief Toggles rendering of spatial Quadtree ground partition cells. */
    void ToggleQuadtreeBounds() noexcept { showQuadtreeBounds_ = !showQuadtreeBounds_; }

    /** @brief Toggles procedural atmospheric skybox rendering. */
    void ToggleSkybox() noexcept { skybox_.Toggle(); }

    /** @brief Toggles visibility of the in-engine FreeType2/HarfBuzz telemetry HUD overlay. */
    void ToggleDebugOverlay() noexcept { debugOverlay_.Toggle(); }

    /** @brief Toggles tangent-space normal and bump mapping. */
    void ToggleBumpMapping() noexcept { pipelineManager_.ToggleBumpMapping(); }

    /** @brief Toggles 16x anisotropic mipmapping vs base level 0. */
    void ToggleMipmapping() { pipelineManager_.ToggleMipmapping(activeScene_.get()); }

    /** @brief Toggles 2048x2048 PCF directional shadow mapping. */
    void ToggleShadows() noexcept { pipelineManager_.ToggleShadows(); }

    /** @brief Toggles specular cubemap IBL environment reflections. */
    void ToggleReflections() noexcept { pipelineManager_.ToggleReflections(); }

    /** @brief Toggles glTF ambient occlusion map evaluation. */
    void ToggleAmbientOcclusion() noexcept { pipelineManager_.ToggleAmbientOcclusion(); }

    /** @brief Toggles immersive Parallax Occlusion Mapping (POM) with self-shadowing. */
    void ToggleParallaxOcclusion() noexcept { pipelineManager_.ToggleParallaxOcclusion(); }

    /** @brief Toggles directional sun lens flare post-processing effect. */
    void ToggleLensFlare() noexcept { pipelineManager_.ToggleLensFlare(); }

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

    /** @brief Toggles screen-space ambient occlusion (SSAO/GTAO) post-pass. */
    void ToggleSSAO() noexcept { pipelineManager_.ToggleSSAO(); }

    /** @brief Toggles emissive HDR bloom post-processing. */
    void ToggleBloom() noexcept { pipelineManager_.ToggleBloom(); }

    /** @brief Sets ambient occlusion intensity multiplier. */
    void SetAoIntensity(float val) noexcept { pipelineManager_.SetAoIntensity(val); }

    /** @brief Sets SSAO intensity multiplier. */
    void SetSsaoIntensity(float val) noexcept { pipelineManager_.SetSsaoIntensity(val); }

    /** @brief Sets HDR Bloom intensity multiplier. */
    void SetBloomIntensity(float val) noexcept { pipelineManager_.SetBloomIntensity(val); }

    /** @brief Sets Parallax Occlusion Mapping displacement height scale. */
    void SetParallaxScale(float val) noexcept { pipelineManager_.SetParallaxScale(val); }

    /** @brief Sets directional sun lens flare intensity multiplier. */
    void SetLensFlareIntensity(float val) noexcept { pipelineManager_.SetLensFlareIntensity(val); }

    /** @brief Toggles spatial Quadtree hierarchical occlusion culling. */
    void ToggleOcclusionCulling() noexcept { pipelineManager_.ToggleOcclusionCulling(); }

    /** @brief Cycles HDR tonemapping operator (Khronos PBR Neutral -> AgX -> ACES). */
    void CycleTonemapper() noexcept { pipelineManager_.CycleToneMapOperator(); }

    /** @brief Cycles built-in HDR environment presets (Courtyard, Sunset, Atelier, Night, Custom). */
    void CycleEnvironmentPreset() noexcept {
        pipelineManager_.CycleEnvironmentPreset(activeScene_.get());
        skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(),
                                      pipelineManager_.GetDefaultSampler());
    }

    /** @brief Cycles skybox display mode (Sharp -> Blurred -> Hidden). */
    void CycleSkyboxMode() noexcept { skybox_.CycleMode(); }

    /** @brief Adjusts the current HDR exposure level by a given delta value. */
    void AdjustExposure(float delta) noexcept { pipelineManager_.SetExposure(pipelineManager_.GetExposure() + delta); }

    /** @brief Rotates the active IBL environment and skybox by radians. */
    void RotateEnvironment(float deltaRad) noexcept {
        const float newRot{pipelineManager_.GetEnvironmentRotation() + deltaRad};
        pipelineManager_.SetEnvironmentRotation(newRot);
        skybox_.SetRotation(newRot);
    }

    /** @brief Toggles turntable autorotation around active camera target. */
    void ToggleTurntable() noexcept { turntableActive_ = !turntableActive_; }
    void SetTurntable(bool active) noexcept { turntableActive_ = active; }
    [[nodiscard]] bool IsTurntableActive() const noexcept { return turntableActive_; }
    void SetTurntableSpeed(float speed) noexcept { turntableSpeed_ = speed; }
    [[nodiscard]] float GetTurntableSpeed() const noexcept { return turntableSpeed_; }

    /** @brief Toggles between Orbit and First-Person Fly camera modes. */
    void ToggleCameraMode() noexcept { camera_.ToggleMode(); }

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
    void SetSunAzimuthElevation(float azimuthRad, float elevationRad) noexcept;
    [[nodiscard]] float GetSunAzimuth() const noexcept { return sunAzimuth_; }
    [[nodiscard]] float GetSunElevation() const noexcept { return sunElevation_; }
    [[nodiscard]] glm::vec3 GetSunDirection() const noexcept { return sunDir_; }
    void UpdateSunDirection() noexcept;

    [[nodiscard]] bool IsBoundingBoxesEnabled() const noexcept { return showBoundingBoxes_; }
    [[nodiscard]] bool IsQuadtreeBoundsEnabled() const noexcept { return showQuadtreeBounds_; }
    [[nodiscard]] bool IsSkyboxEnabled() const noexcept { return skybox_.IsEnabled(); }
    [[nodiscard]] bool IsDebugOverlayEnabled() const noexcept { return debugOverlay_.IsVisible(); }
    [[nodiscard]] bool IsBumpMappingEnabled() const noexcept { return pipelineManager_.IsBumpMappingEnabled(); }
    [[nodiscard]] bool IsMipmappingEnabled() const noexcept { return pipelineManager_.IsMipmappingEnabled(); }
    [[nodiscard]] bool IsShadowsEnabled() const noexcept { return pipelineManager_.IsShadowsEnabled(); }
    [[nodiscard]] bool IsReflectionsEnabled() const noexcept { return pipelineManager_.IsReflectionsEnabled(); }
    [[nodiscard]] bool IsAmbientOcclusionEnabled() const noexcept {
        return pipelineManager_.IsAmbientOcclusionEnabled();
    }
    [[nodiscard]] bool IsOcclusionCullingEnabled() const noexcept {
        return pipelineManager_.IsOcclusionCullingEnabled();
    }

    /**
     * @brief Ingests raw file bytes dropped directly onto the viewer window.
     * @param filename Target file name or path.
     * @param data Byte span containing file contents.
     */
    void OnFileDrop(std::string_view filename, std::span<const uint8_t> data);

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
    SDL_Window* nativeWindow_{nullptr};
    scene::AssetManager assetManager_{};
    std::unique_ptr<scene::FastGltfLoader> gltfLoader_{nullptr};
    std::shared_ptr<scene::Scene> activeScene_{nullptr};
    std::unique_ptr<scene::Quadtree> quadtree_{nullptr};

    // Selection & Interactivity
    std::shared_ptr<scene::SceneNode> selectedNode_{nullptr};
    int selectedNodeIndex_{-1};

    // Camera Navigation & Turntable
    bool turntableActive_{false};
    float turntableSpeed_{0.35f}; // rad/s
    int activeCameraIndex_{-1};

    // Sun & Time-of-Day
    float timeOfDay_{14.0f}; // 14:00 (2 PM)
    float sunAzimuth_{0.8f};
    float sunElevation_{0.95f};
    glm::vec3 sunDir_{0.35f, 1.0f, 0.25f};

    std::string currentModelName_{"None"};
    size_t totalPrimitives_{0};
    size_t totalVertices_{0};
    size_t totalIndices_{0};
    size_t cachedQuadtreeNodeCount_{0};
};

} // namespace bgl::app
