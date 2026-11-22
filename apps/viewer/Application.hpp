/**
 * @file Application.hpp
 * @brief Application lifecycle, event loop, UI-Bridge, and drag-and-drop file ingestion.
 */
#ifndef BGL_VIEWER_APPLICATION_HPP
#define BGL_VIEWER_APPLICATION_HPP

#include "core/MemoryTracker.hpp"
#include "engine/EngineCore.hpp"
#include "ui/DebugOverlay.hpp"
#include "ui/ImmediateUI.hpp"
#include "viewer/ApplicationConfig.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgl::app {

/**
 * @struct ModelPresetEntry
 * @brief Descriptor for a built-in local or remote glTF/GLB sample model in the viewer dropdown.
 */
struct ModelPresetEntry {
    const char* label{nullptr};
    const char* localRelPath{nullptr};
    const char* remoteUrl{nullptr};
};

/**
 * @class Application
 * @brief Main engine application runtime coordinating rendering, physics, audio, and inputs.
 */
class Application : public engine::EngineCore {
public:
    Application();
    ~Application() override;

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
     * @brief Gracefully releases WebGPU device resources, pipelines, and native window handles.
     */
    void Shutdown();

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

    /** @brief Sets atmospheric weather mode (None / Rain / Snow). */
    void SetWeatherMode(gpu::WeatherMode mode) noexcept { pipelineManager_.SetWeatherMode(mode); }

    /** @brief Cycles atmospheric weather mode (None -> Rain -> Snow -> None). */
    void CycleWeatherMode() noexcept { pipelineManager_.CycleWeatherMode(); }

    /** @brief Toggles rain storm simulation on/off. */
    void ToggleRain() noexcept { pipelineManager_.ToggleRain(); }

    /** @brief Toggles snowfall simulation on/off. */
    void ToggleSnow() noexcept { pipelineManager_.ToggleSnow(); }

    /** @brief Toggles volumetric height/distance fog simulation on/off. */
    void ToggleFog() noexcept { pipelineManager_.ToggleFog(); }

    /** @brief Toggles volumetric rising smoke plumes simulation on/off. */
    void ToggleSmoke() noexcept { pipelineManager_.ToggleSmoke(); }

    /** @brief Queries current atmospheric weather mode. */
    [[nodiscard]] gpu::WeatherMode GetWeatherMode() const noexcept { return pipelineManager_.GetWeatherMode(); }

    /** @brief Sets weather precipitation, wetness glaze, and snow depth intensity. */
    void SetWeatherIntensity(float val) noexcept { pipelineManager_.SetWeatherIntensity(val); }

    /** @brief Sets lateral wind direction and strength for precipitation. */
    void SetWindStrength(float val) noexcept { pipelineManager_.SetWindStrength(val); }

    /** @brief Toggles spatial Quadtree hierarchical occlusion culling. */
    void ToggleOcclusionCulling() noexcept { pipelineManager_.ToggleOcclusionCulling(); }

    /** @brief Multi-level Mesh & Shader LOD and Diagnostic Debug View controls. */
    void ToggleLod() noexcept { pipelineManager_.ToggleLod(); }
    void SetLodEnabled(bool enabled) noexcept { pipelineManager_.SetLodEnabled(enabled); }
    [[nodiscard]] bool IsLodEnabled() const noexcept { return pipelineManager_.IsLodEnabled(); }

    void ToggleAutoLod() noexcept { pipelineManager_.ToggleAutoLod(); }
    void SetAutoLodEnabled(bool enabled) noexcept { pipelineManager_.SetAutoLodEnabled(enabled); }
    [[nodiscard]] bool IsAutoLodEnabled() const noexcept { return pipelineManager_.IsAutoLodEnabled(); }

    void SetLodBias(float bias) noexcept { pipelineManager_.SetLodBias(bias); }
    [[nodiscard]] float GetLodBias() const noexcept { return pipelineManager_.GetLodBias(); }

    void SetDebugViewMode(gpu::DebugViewMode mode) noexcept { pipelineManager_.SetDebugViewMode(mode); }
    void CycleDebugViewMode() noexcept { pipelineManager_.CycleDebugViewMode(); }
    void ToggleLodHeatmap() noexcept { pipelineManager_.ToggleLodHeatmap(); }
    [[nodiscard]] gpu::DebugViewMode GetDebugViewMode() const noexcept { return pipelineManager_.GetDebugViewMode(); }

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
    void OnFileDrop(std::string_view filename, std::span<const uint8_t> data) override;

    /**
     * @brief Asynchronously fetches, parses, and prepares a glTF/GLB model while reporting live progress.
     * @param uri Local path or remote URL to the glTF/GLB model.
     * @param displayLabel Optional user-facing label shown in the progress banner.
     */
    void LoadSceneFromUriAsync(std::string_view uri, std::string_view displayLabel = {});

    /**
     * @brief Returns the built-in native model dropdown catalog (local assets + Khronos glTF Sample Assets).
     */
    [[nodiscard]] static std::span<const ModelPresetEntry> GetBuiltinModelPresets() noexcept;

    /**
     * @brief Resolves the preferred local path (`assets/` or `web/assets/`) or remote HTTPS URL for a preset.
     */
    [[nodiscard]] static std::string ResolveModelPresetUri(const ModelPresetEntry& preset);

    [[nodiscard]] bool IsLoadingScene() const noexcept;
    [[nodiscard]] float GetLoadingProgress() const noexcept;
    [[nodiscard]] std::string GetLoadingStatus() const;

    [[nodiscard]] ui::DebugOverlay& GetDebugOverlay() noexcept { return debugOverlay_; }
    [[nodiscard]] ui::ImmediateUI& GetImmediateUI() noexcept { return settingsUi_; }

    void ToggleSettingsUI() noexcept { showSettingsUi_ = !showSettingsUi_; }
    [[nodiscard]] bool IsSettingsUIVisible() const noexcept { return showSettingsUi_; }

protected:
    void OnSceneActivated(std::string_view sourceUri) override;
    void OnPrepareOverlay() override;
    void OnRenderOverlay(WGPURenderPassEncoder swapchainPass) override;

private:
    struct AsyncLoadTask;

    void UpdateSettingsUI();
    void DrawSelectionFloatingBadge(uint32_t vpW, uint32_t vpH);
    void DrawModelSelectorSection(bool showLoadingUi);
    void DrawCameraSceneSection();
    void DrawRayTracingSection();
    void DrawPostProcessSection();
    void DrawAtmosphereWeatherSection();
    void DrawEngineThemeSection();
    void PollAsyncSceneLoad(float deltaTime);
    void RetireActiveLoadAsync() noexcept;
    void CancelPendingSceneLoads() noexcept;
    void SyncModelSelectionFromUri(std::string_view sourceUri);
    void SetLoadingSucceededState();
    void SetLoadingFailedState(std::string_view titleName, std::string_view errorMsg, bool revertSelection);
#if defined(BGL_PLATFORM_NATIVE)
    void HandleKeyDown(int32_t keySym, uint16_t keyMod);
#endif

    ui::DebugOverlay debugOverlay_{};
    ui::ImmediateUI settingsUi_{};
    ui::UiInputState uiInput_{};
    bool prevGamepadY_{false};
    bool prevGamepadX_{false};
    bool showSettingsUi_{true};
    size_t selectedKtxOption_{0};
    size_t lastVramBytes_{0};
    size_t lastTextureVramBytes_{0};

    // Camera Navigation & Turntable
    bool turntableActive_{false};
    float turntableSpeed_{0.35f}; // rad/s

    ui::OverlayStats cachedOverlayStats_{};
    core::MemoryTelemetry cachedMemTelemetry_{};
    float memTelemetryTimer_{0.25f};
    float fpsSampleTimer_{0.0f};
    std::array<float, 48> fpsHistory_{};
    size_t fpsHistoryCount_{0};
    bool showSelectionAnchors_{true};
    glm::vec3 uiAccentColor_{0.24f, 0.62f, 0.96f};
    size_t selectedUiFontIdx_{0};

    size_t selectedModelOption_{0};
    size_t lastCommittedModelOption_{0};
    std::string customModelLabel_{};
    float loadingTargetProgress_{0.0f};
    float loadingVisualProgress_{0.0f};
    float loadingBannerTimer_{0.0f};
    bool loadingHasError_{false};
    std::string loadingTitle_{};
    std::string loadingStatus_{};
    std::unique_ptr<AsyncLoadTask> activeLoad_{};
    std::vector<std::unique_ptr<AsyncLoadTask>> cancellingLoads_{};
};

} // namespace bgl::app

#endif // BGL_VIEWER_APPLICATION_HPP
