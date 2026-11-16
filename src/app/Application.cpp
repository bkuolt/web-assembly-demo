/**
 * @file Application.cpp
 * @brief Core application loop, SDL2 window management, WebGPU pipeline and scene orchestration.
 */
#include "app/Application.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Version.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <ranges>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::app {

Application::Application() = default;

Application::~Application() {
    Shutdown();
}

core::Result<void> Application::Initialize(uint32_t width, uint32_t height) {
    ApplicationConfig config{};
    config.width = width;
    config.height = height;
    return Initialize(config);
}

core::Result<void> Application::Initialize(const ApplicationConfig& config) {
    const uint32_t width{config.width};
    const uint32_t height{config.height};
    BGL_LOG_INFO("Initializing {} v{} ({}x{})...", kEngineName, kEngineVersion, width, height);

    showBoundingBoxes_ = config.enableBoundingBoxes;
    showQuadtreeBounds_ = config.enableQuadtreeBounds;

#if defined(BGL_PLATFORM_NATIVE)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        return std::unexpected(std::string{"Failed to initialize SDL2: "} + SDL_GetError());
    }

    SDL_Window* window{SDL_CreateWindow(std::string{kEngineName}.c_str(), SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, static_cast<int>(width), static_cast<int>(height),
                                        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)};
    if (window) {
        nativeWindow_ = window;
        SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
        SDL_EventState(SDL_DROPTEXT, SDL_ENABLE);
        BGL_LOG_INFO("Created native SDL2 window {}x{} (Drag & Drop enabled for files and URLs)", width, height);
    }
#endif

    gpu::ContextConfig cfg{};
    cfg.width = width;
    cfg.height = height;
    cfg.nativeWindowHandle = nativeWindow_;

    if (auto res = gpuContext_.Initialize(cfg); !res) {
        return res;
    }

    if (auto res = pipelineManager_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(),
                                               gpuContext_.GetPreferredFormat(), gpuContext_.GetSampleCount());
        !res) {
        return res;
    }
    pipelineManager_.SetBumpMappingEnabled(config.enableBumpMapping);
    pipelineManager_.SetMipmappingEnabled(config.enableMipmapping);
    pipelineManager_.SetShadowsEnabled(config.enableShadows);
    pipelineManager_.SetReflectionsEnabled(config.enableReflections);
    pipelineManager_.SetAmbientOcclusionEnabled(config.enableAmbientOcclusion);
    pipelineManager_.SetOcclusionCullingEnabled(config.enableOcclusionCulling);
    pipelineManager_.SetLodEnabled(config.enableLod);
    pipelineManager_.SetAutoLodEnabled(config.enableAutoLod);
    pipelineManager_.SetLodBias(config.lodBias);
    pipelineManager_.SetDebugViewMode(config.debugViewMode);
    pipelineManager_.SetWeatherMode(config.weatherMode);
    pipelineManager_.SetWeatherIntensity(config.weatherIntensity);
    pipelineManager_.SetWindStrength(config.windStrength);
    dayNightCycleActive_ = config.enableDayNightCycle;
    SetDayNightSpeed(config.dayNightSpeed);
    SetTimeOfDay(config.timeOfDay);

    if (auto res = skybox_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpu::Context::GetHdrFormat(),
                                      gpuContext_.GetSampleCount());
        !res) {
        BGL_LOG_WARN("Could not initialize Atmospheric Skybox: {}", res.error());
    }
    skybox_.SetEnabled(config.enableSkybox);
    skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(), pipelineManager_.GetDefaultSampler());

    if (auto res = debugOverlay_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(),
                                            gpuContext_.GetPreferredFormat(), 1);
        !res) {
        BGL_LOG_WARN("Could not initialize Debug Overlay: {}", res.error());
    }
    debugOverlay_.SetVisible(config.enableDebugOverlay);

    if (auto res = settingsUi_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(),
                                          gpuContext_.GetPreferredFormat(), 1);
        !res) {
        BGL_LOG_WARN("Could not initialize ImmediateUI: {}", res.error());
    }
    switch (scene::TextureDecoder::GetDefaultKtxTarget()) {
    case scene::KtxTranscodeTarget::Bc7:
        selectedKtxOption_ = 1;
        break;
    case scene::KtxTranscodeTarget::Astc4x4:
        selectedKtxOption_ = 2;
        break;
    case scene::KtxTranscodeTarget::Etc2Rgba:
        selectedKtxOption_ = 3;
        break;
    case scene::KtxTranscodeTarget::Rgba32:
    default:
        selectedKtxOption_ = 4;
        break;
    }

    if (auto audioRes = soundEngine_.Initialize(); !audioRes) {
        BGL_LOG_WARN("3D audio engine disabled: {}", audioRes.error());
    }
    gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);

    // Resolve model asset path from config or auto-detect Sponza.glb (primary) / ToyCar.glb (fallback)
    std::string assetPathToLoad{config.assetPath};
    if (assetPathToLoad.empty()) {
        static constexpr std::array<std::string_view, 4> kSearchPaths{"assets/Sponza.glb", "../assets/Sponza.glb",
                                                                      "assets/ToyCar.glb", "../assets/ToyCar.glb"};
        const auto it =
            std::ranges::find_if(kSearchPaths, [](std::string_view path) { return std::filesystem::exists(path); });
        if (it != kSearchPaths.end()) {
            assetPathToLoad = std::string{*it};
        }
    }

    if (!assetPathToLoad.empty()) {
        if (auto loadRes = LoadSceneFromUri(assetPathToLoad); !loadRes) {
            BGL_LOG_WARN("Could not load '{}': {}", assetPathToLoad, loadRes.error());
        }
    }

    if (!quadtree_) {
        core::AABB worldBounds;
        worldBounds.minBound = glm::vec3(-100.0f);
        worldBounds.maxBound = glm::vec3(100.0f);
        quadtree_ = std::make_unique<scene::Quadtree>(worldBounds);
    }

    running_ = true;
    BGL_LOG_INFO("bgl Engine initialized successfully.");
    return {};
}

#if defined(BGL_PLATFORM_NATIVE)
void Application::HandleKeyDown(int32_t keySym, uint16_t keyMod) {
    if (keySym >= SDLK_1 && keySym <= SDLK_9) {
        const size_t slot = static_cast<size_t>(keySym - SDLK_1);
        if ((keyMod & KMOD_CTRL) != 0) {
            camera_.SaveBookmark(slot);
            BGL_LOG_INFO("Saved camera bookmark to slot #{}", slot + 1);
        } else if (camera_.RestoreBookmark(slot)) {
            BGL_LOG_INFO("Restored camera bookmark from slot #{}", slot + 1);
        }
        return;
    }

    switch (keySym) {
    case SDLK_ESCAPE:
        running_ = false;
        return;
    case SDLK_F1:
        wireframeMode_ = !wireframeMode_;
        BGL_LOG_INFO("Toggled Wireframe Overlay: {}", wireframeMode_ ? "ON" : "OFF");
        break;
    case SDLK_F2:
        ToggleSettingsUI();
        BGL_LOG_INFO("Toggled VRAM & Compression Settings UI: {}", showSettingsUi_ ? "ON" : "OFF");
        break;
    case SDLK_o:
    case SDLK_F3: {
        ToggleDebugOverlay();
        const char* modeStr = (debugOverlay_.GetMode() == ui::OverlayMode::Full)      ? "Full"
                              : (debugOverlay_.GetMode() == ui::OverlayMode::Minimal) ? "Minimal"
                                                                                      : "Hidden";
        BGL_LOG_INFO("Toggled In-Engine Debug Overlay mode: {}", modeStr);
        break;
    }
    case SDLK_b:
        ToggleBoundingBoxes();
        BGL_LOG_INFO("Toggled 3D Bounding Boxes & AABBs visibility: {}", showBoundingBoxes_);
        break;
    case SDLK_q:
        ToggleQuadtreeBounds();
        BGL_LOG_INFO("Toggled 3D Quadtree Spatial Bounds visibility: {}", showQuadtreeBounds_);
        break;
    case SDLK_v:
        ToggleBumpMapping();
        BGL_LOG_INFO("Toggled Bump & Normal Mapping: {}", pipelineManager_.IsBumpMappingEnabled());
        break;
    case SDLK_l:
        ToggleMipmapping();
        BGL_LOG_INFO("Toggled Texture Mipmapping (16x Anisotropic Trilinear vs Mip-0): {}",
                     pipelineManager_.IsMipmappingEnabled());
        break;
    case SDLK_m:
        ToggleShadows();
        BGL_LOG_INFO("Toggled Directional Shadow Mapping (PCF): {}", pipelineManager_.IsShadowsEnabled());
        break;
    case SDLK_r:
        ToggleReflections();
        BGL_LOG_INFO("Toggled Environment Cubemap Reflections (IBL): {}", pipelineManager_.IsReflectionsEnabled());
        break;
    case SDLK_u:
        ToggleAmbientOcclusion();
        BGL_LOG_INFO("Toggled Ambient Occlusion & Micro-Cavities: {}", pipelineManager_.IsAmbientOcclusionEnabled());
        break;
    case SDLK_h:
        ToggleOcclusionCulling();
        BGL_LOG_INFO("Toggled Quadtree Hierarchical Occlusion Culling (Hi-Z): {}",
                     pipelineManager_.IsOcclusionCullingEnabled());
        break;
    case SDLK_x:
        CycleTonemapper();
        BGL_LOG_INFO("Switched Tonemapper: {}",
                     pipelineManager_.GetToneMapOperatorName(pipelineManager_.GetToneMapOperator()));
        break;
    case SDLK_z:
        pipelineManager_.ToggleBloom();
        BGL_LOG_INFO("Toggled Emissive HDR Bloom: {}", pipelineManager_.IsBloomEnabled());
        break;
    case SDLK_e:
        CycleEnvironmentPreset();
        BGL_LOG_INFO("Switched Environment Preset: {}",
                     pipelineManager_.GetEnvironmentPresetName(pipelineManager_.GetEnvironmentPreset()));
        break;
    case SDLK_k:
        CycleSkyboxMode();
        BGL_LOG_INFO("Switched Skybox Mode: {}", skybox_.GetModeName(skybox_.GetMode()));
        break;
    case SDLK_p:
        ToggleParallaxOcclusion();
        BGL_LOG_INFO("Toggled Parallax Occlusion Mapping (POM): {}", pipelineManager_.IsParallaxEnabled());
        break;
    case SDLK_j:
        ToggleLensFlare();
        BGL_LOG_INFO("Toggled Directional Sun Lens Flare: {}", pipelineManager_.IsLensFlareEnabled());
        break;
    case SDLK_g:
        pipelineManager_.ToggleSSAO();
        BGL_LOG_INFO("Toggled Screen-Space Ambient Occlusion (SSAO/GTAO): {}", pipelineManager_.IsSsaoEnabled());
        break;
    case SDLK_y:
        CycleWeatherMode();
        BGL_LOG_INFO("Switched Weather Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F4:
        ToggleRain();
        BGL_LOG_INFO("Toggled Rain Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F5:
        ToggleSnow();
        BGL_LOG_INFO("Toggled Snow Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F6:
        ToggleFog();
        BGL_LOG_INFO("Toggled Fog Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_F7:
        ToggleSmoke();
        BGL_LOG_INFO("Toggled Smoke Shader Effect: {}",
                     pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
        break;
    case SDLK_i:
    case SDLK_F8:
        ToggleDayNightCycle();
        BGL_LOG_INFO("Toggled Automatic Day/Night Cycle: {} (Time: {:02d}:{:02d})", dayNightCycleActive_ ? "ON" : "OFF",
                     static_cast<int>(timeOfDay_), static_cast<int>((timeOfDay_ - std::floor(timeOfDay_)) * 60.0f));
        break;
    case SDLK_n:
    case SDLK_F9:
        CycleDebugViewMode();
        BGL_LOG_INFO("Switched Diagnostic Debug View: {}",
                     pipelineManager_.GetDebugViewModeName(pipelineManager_.GetDebugViewMode()));
        break;
    case SDLK_F10:
        ToggleLodHeatmap();
        BGL_LOG_INFO("Toggled LOD Heatmap View: {}",
                     pipelineManager_.GetDebugViewModeName(pipelineManager_.GetDebugViewMode()));
        break;
    case SDLK_F11:
        ToggleLod();
        BGL_LOG_INFO("Toggled Multi-Level Mesh & Shader LOD System: {}",
                     pipelineManager_.IsLodEnabled() ? "ON" : "OFF");
        break;
    case SDLK_F12:
        ToggleAutoLod();
        BGL_LOG_INFO("Toggled Adaptive 60 FPS Auto-LOD: {} (LOD Bias: {:.2f}x)",
                     pipelineManager_.IsAutoLodEnabled() ? "ON" : "OFF", pipelineManager_.GetLodBias());
        break;
    case SDLK_SEMICOLON:
        SetLodBias(pipelineManager_.GetLodBias() - 0.15f);
        BGL_LOG_INFO("LOD Bias decreased: {:.2f}x", pipelineManager_.GetLodBias());
        break;
    case SDLK_QUOTE:
        SetLodBias(pipelineManager_.GetLodBias() + 0.15f);
        BGL_LOG_INFO("LOD Bias increased: {:.2f}x", pipelineManager_.GetLodBias());
        break;
    case SDLK_COMMA:
        SetTimeOfDay(timeOfDay_ - 0.5f);
        BGL_LOG_INFO("Time of Day stepped back: {:02d}:{:02d}", static_cast<int>(timeOfDay_),
                     static_cast<int>((timeOfDay_ - std::floor(timeOfDay_)) * 60.0f));
        break;
    case SDLK_PERIOD:
        SetTimeOfDay(timeOfDay_ + 0.5f);
        BGL_LOG_INFO("Time of Day stepped forward: {:02d}:{:02d}", static_cast<int>(timeOfDay_),
                     static_cast<int>((timeOfDay_ - std::floor(timeOfDay_)) * 60.0f));
        break;
    case SDLK_f:
        FocusSelection();
        BGL_LOG_INFO("Framed camera on selection / scene bounds");
        break;
    case SDLK_c:
        ToggleCameraMode();
        BGL_LOG_INFO("Switched camera mode: {}",
                     camera_.GetMode() == core::CameraMode::Fly ? "First-Person Fly" : "Orbit");
        break;
    case SDLK_t:
        ToggleTurntable();
        BGL_LOG_INFO("Turntable autorotation: {}", turntableActive_ ? "ON" : "OFF");
        break;
    case SDLK_PLUS:
    case SDLK_KP_PLUS:
    case SDLK_EQUALS:
        if ((keyMod & KMOD_SHIFT) != 0) {
            AdjustExposure(0.15f);
            BGL_LOG_INFO("Exposure increased: {:.2f}", pipelineManager_.GetExposure());
        } else {
            camera_.Zoom(camera_.GetDistance() * 0.15f);
        }
        break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        if ((keyMod & KMOD_SHIFT) != 0) {
            AdjustExposure(-0.15f);
            BGL_LOG_INFO("Exposure decreased: {:.2f}", pipelineManager_.GetExposure());
        } else {
            camera_.Zoom(-camera_.GetDistance() * 0.15f);
        }
        break;
    case SDLK_LEFTBRACKET:
        RotateEnvironment(-0.0872665f); // -5 degrees
        break;
    case SDLK_RIGHTBRACKET:
        RotateEnvironment(0.0872665f); // +5 degrees
        break;
    default:
        break;
    }
}
#endif

void Application::Update(float deltaTime) {
    if (!running_)
        return;

    lastDeltaTime_ = std::clamp(deltaTime, 0.0005f, 0.25f);
    elapsedTime_ += lastDeltaTime_;
    fpsSampleTimer_ += lastDeltaTime_;
    if (fpsSampleTimer_ >= 0.06f) {
        fpsSampleTimer_ = 0.0f;
        const float instFps = std::clamp(1.0f / std::max(deltaTime, 1e-4f), 1.0f, 360.0f);
        if (fpsHistoryCount_ < fpsHistory_.size()) {
            fpsHistory_[fpsHistoryCount_++] = instFps;
        } else {
            std::copy(fpsHistory_.begin() + 1, fpsHistory_.end(), fpsHistory_.begin());
            fpsHistory_.back() = instFps;
        }
    }

    uiInput_.mousePressed = false;
    uiInput_.mouseReleased = false;
    uiInput_.wheelDelta = 0.0f;
    bool pendingPick = false;
    float pickX = 0.0f;
    float pickY = 0.0f;
    float orbitDx = 0.0f;
    float orbitDy = 0.0f;
    float panDx = 0.0f;
    float panDy = 0.0f;
    float wheelDelta = 0.0f;

#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running_ = false;
                return;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    gpuContext_.Resize(static_cast<uint32_t>(event.window.data1),
                                       static_cast<uint32_t>(event.window.data2));
                }
            } else if (event.type == SDL_KEYDOWN) {
                HandleKeyDown(event.key.keysym.sym, event.key.keysym.mod);
                if (!running_) {
                    return;
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                uiInput_.mouseX = static_cast<float>(event.button.x);
                uiInput_.mouseY = static_cast<float>(event.button.y);
                if (event.button.button == SDL_BUTTON_LEFT) {
                    uiInput_.mouseDown = true;
                    uiInput_.mousePressed = true;
                }
                if (event.button.button == SDL_BUTTON_MIDDLE ||
                    (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2)) {
                    pendingPick = true;
                    pickX = static_cast<float>(event.button.x);
                    pickY = static_cast<float>(event.button.y);
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                uiInput_.mouseX = static_cast<float>(event.button.x);
                uiInput_.mouseY = static_cast<float>(event.button.y);
                if (event.button.button == SDL_BUTTON_LEFT) {
                    uiInput_.mouseDown = false;
                    uiInput_.mouseReleased = true;
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                uiInput_.mouseX = static_cast<float>(event.motion.x);
                uiInput_.mouseY = static_cast<float>(event.motion.y);
                if (event.motion.state & SDL_BUTTON_LMASK) {
                    orbitDx += static_cast<float>(event.motion.xrel);
                    orbitDy += static_cast<float>(event.motion.yrel);
                } else if (event.motion.state & SDL_BUTTON_RMASK) {
                    panDx += static_cast<float>(event.motion.xrel);
                    panDy += static_cast<float>(event.motion.yrel);
                }
            } else if (event.type == SDL_MOUSEWHEEL) {
                wheelDelta += static_cast<float>(event.wheel.y);
            } else if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
                char* droppedStr{event.drop.file};
                if (droppedStr) {
                    const std::string sanitized{core::NetworkFetcher::SanitizeUri(droppedStr)};
                    BGL_LOG_INFO("Asset/URL dropped via OS ({}): '{}'",
                                 event.type == SDL_DROPTEXT ? "SDL_DROPTEXT" : "SDL_DROPFILE", sanitized);
                    if (auto loadRes = LoadSceneFromUri(sanitized); !loadRes) {
                        BGL_LOG_ERROR("Failed to load dropped asset/URL '{}': {}", sanitized, loadRes.error());
                    }
                    SDL_free(droppedStr);
                }
            }
        }
    }

    const Uint8* keyState = SDL_GetKeyboardState(nullptr);
    if (keyState) {
        const float moveSpeed = std::max(camera_.GetDistance() * 1.5f, 1.0f) * deltaTime;
        if (keyState[SDL_SCANCODE_W] || keyState[SDL_SCANCODE_UP]) {
            camera_.MoveForward(moveSpeed);
        }
        if (keyState[SDL_SCANCODE_S] || keyState[SDL_SCANCODE_DOWN]) {
            camera_.MoveForward(-moveSpeed);
        }
        if (keyState[SDL_SCANCODE_A] || keyState[SDL_SCANCODE_LEFT]) {
            camera_.MoveRight(-moveSpeed);
        }
        if (keyState[SDL_SCANCODE_D] || keyState[SDL_SCANCODE_RIGHT]) {
            camera_.MoveRight(moveSpeed);
        }
    }
#endif

    gamepad_.Update(deltaTime);
    const auto& gp = gamepad_.GetState();

    if (gp.connected) {
        if (camera_.GetMode() == core::CameraMode::Fly) {
            camera_.MoveForward(-gp.smoothedMove.y * deltaTime * 6.0f);
            camera_.MoveRight(gp.smoothedMove.x * deltaTime * 6.0f);
        } else {
            camera_.Pan(gp.smoothedMove.x * deltaTime * camera_.GetDistance(),
                        gp.smoothedMove.y * deltaTime * camera_.GetDistance());
        }
        // PS5 smooth exponential response curve and stabilized pitch
        camera_.Orbit(gp.smoothedLook.x * deltaTime * 2.8f, gp.smoothedLook.y * deltaTime * 2.8f);
        camera_.Zoom((gp.rightTrigger - gp.leftTrigger) * deltaTime * camera_.GetDistance() * 1.5f);
        if (gp.buttonY && !prevGamepadY_) {
            ToggleBoundingBoxes();
        }
        if (gp.buttonX && !prevGamepadX_) {
            FocusSelection();
        }
        prevGamepadY_ = gp.buttonY;
        prevGamepadX_ = gp.buttonX;
    } else {
        prevGamepadY_ = false;
        prevGamepadX_ = false;
    }

    if (turntableActive_) {
        camera_.Orbit(turntableSpeed_ * deltaTime, 0.0f);
    }

    PollShaderHotReload(deltaTime);

    if (activeScene_) {
        activeScene_->UpdateAnimation(deltaTime);
        if (quadtree_ && activeScene_->transformVersion_ != lastQuadtreeTransformVersion_) {
            quadtree_->Refit();
            cachedQuadtreeNodeCount_ = quadtree_->GetNodeCount();
            lastQuadtreeTransformVersion_ = activeScene_->transformVersion_;
        }
    }

    pipelineManager_.AdvanceWeatherTime(deltaTime);
    pipelineManager_.UpdateAutoLod(deltaTime);
    if (dayNightCycleActive_) {
        SetTimeOfDay(timeOfDay_ + dayNightSpeed_ * deltaTime);
    }

    float aspect =
        static_cast<float>(gpuContext_.GetWidth()) / std::max(1.0f, static_cast<float>(gpuContext_.GetHeight()));
    camera_.Update(aspect);
    soundEngine_.UpdateListener(camera_.GetPosition(), glm::normalize(camera_.GetTarget() - camera_.GetPosition()));

    // Collect telemetry stats for the in-engine debug HUD overlay
    ui::OverlayStats& stats{cachedOverlayStats_};
    stats.deltaTime = deltaTime;
    stats.cameraPos = camera_.GetPosition();
    stats.cameraTarget = camera_.GetTarget();
    stats.cameraDistance = camera_.GetDistance();
    stats.sceneName = currentModelName_;
    stats.primitiveCount = totalPrimitives_;
    stats.vertexCount = totalVertices_;
    stats.indexCount = totalIndices_;
    stats.boundingBoxesEnabled = showBoundingBoxes_;
    stats.quadtreeBoundsEnabled = showQuadtreeBounds_;
    stats.skyboxEnabled = skybox_.IsEnabled();
    stats.bumpMappingEnabled = pipelineManager_.IsBumpMappingEnabled();
    stats.mipmappingEnabled = pipelineManager_.IsMipmappingEnabled();
    stats.shadowsEnabled = pipelineManager_.IsShadowsEnabled();
    stats.reflectionsEnabled = pipelineManager_.IsReflectionsEnabled();
    stats.ambientOcclusionEnabled = pipelineManager_.IsAmbientOcclusionEnabled();
    stats.occlusionCullingEnabled = pipelineManager_.IsOcclusionCullingEnabled();
    stats.lodEnabled = pipelineManager_.IsLodEnabled();
    stats.autoLodEnabled = pipelineManager_.IsAutoLodEnabled();
    stats.lodBias = pipelineManager_.GetLodBias();
    stats.renderedTriangles = pipelineManager_.GetRenderedTriangleCount();
    stats.totalVisibleTriangles = pipelineManager_.GetTotalVisibleTriangleCount();
    stats.debugViewName.assign(pipelineManager_.GetDebugViewModeName(pipelineManager_.GetDebugViewMode()));
    stats.weatherModeName.assign(pipelineManager_.GetWeatherModeName(pipelineManager_.GetWeatherMode()));
    stats.timeOfDay = timeOfDay_;
    stats.dayNightCycleEnabled = dayNightCycleActive_;

    // Memory and Gamepad telemetry
    memTelemetryTimer_ += deltaTime;
    if (memTelemetryTimer_ >= 0.25f) {
        memTelemetryTimer_ = 0.0f;
        cachedMemTelemetry_ = core::MemoryTracker::Instance().Query();
    }
    const auto& memTelemetry = cachedMemTelemetry_;
    stats.sampleCount = gpuContext_.GetSampleCount();
    stats.residentRamBytes = memTelemetry.residentRamBytes;
    size_t vramBytes = memTelemetry.TotalGpuBytes();
    size_t textureVramBytes = 0;
    if (activeScene_) {
        for (const auto& tex : activeScene_->gpuTextures) {
            const size_t texBytes = (tex.vramBytes > 0)
                                        ? tex.vramBytes
                                        : static_cast<size_t>(static_cast<double>(tex.width) *
                                                              static_cast<double>(tex.height) * 4.0 * 1.33);
            textureVramBytes += texBytes;
        }
        vramBytes += textureVramBytes;
    }
    // Offscreen HDR + Depth render targets allocated in gpu::Context are not registered via MemoryTracker;
    // ShadowPass depth, geometry/skin/morph/ring/instance/UI buffers are already counted in TotalGpuBytes().
    const size_t pxCount = static_cast<size_t>(gpuContext_.GetWidth()) * static_cast<size_t>(gpuContext_.GetHeight());
    const size_t msaaSamples = static_cast<size_t>(std::max<uint32_t>(1u, gpuContext_.GetSampleCount()));
    vramBytes += pxCount * 8u; // HDR single-sampled resolve target (RGBA16Float)
    if (msaaSamples > 1u) {
        vramBytes += pxCount * 8u * msaaSamples; // HDR MSAA color target
    }
    vramBytes += pxCount * 4u * msaaSamples; // Depth24Plus target
    stats.vramBytes = vramBytes;
    lastVramBytes_ = vramBytes;
    lastTextureVramBytes_ = textureVramBytes;

    // Immediate Mode Settings UI & Mouse/Wheel Event Consumption (blocks 3D camera/picking when interacting with UI)
    uiInput_.wheelDelta = wheelDelta;
    UpdateSettingsUI();
    uiInput_.wheelDelta = 0.0f;
    if (!settingsUi_.WantsMouse()) {
        if (pendingPick) {
            PickNodeAt(pickX, pickY, static_cast<float>(gpuContext_.GetWidth()),
                       static_cast<float>(gpuContext_.GetHeight()));
        }
        if (orbitDx != 0.0f || orbitDy != 0.0f) {
            camera_.Orbit(orbitDx * 0.005f, -orbitDy * 0.005f);
        }
        if (panDx != 0.0f || panDy != 0.0f) {
            camera_.Pan(-panDx * 0.0005f * camera_.GetDistance(), panDy * 0.0005f * camera_.GetDistance());
        }
        if (wheelDelta != 0.0f) {
            camera_.Zoom(wheelDelta * camera_.GetDistance() * 0.12f);
        }
    }

    stats.gamepadConnected = gp.connected;
    stats.isDualSense = gp.isDualSense;
    stats.gamepadName.assign(gp.deviceName);

    if (quadtree_) {
        stats.quadtreeNodes = cachedQuadtreeNodeCount_;
        stats.visiblePrimitives = pipelineManager_.GetVisiblePrimitiveCount();
        stats.occludedPrimitives = pipelineManager_.GetOccludedPrimitiveCount();
    } else {
        stats.quadtreeNodes = 0;
        stats.visiblePrimitives = totalPrimitives_;
        stats.occludedPrimitives = 0;
    }

    debugOverlay_.Update(deltaTime, stats);
}

void Application::Render() {
    if (!running_)
        return;

    if (!gpuContext_.PrepareFrame())
        return;

    glm::mat4 vp = camera_.GetViewProjectionMatrix();
    glm::vec3 camPos = camera_.GetPosition();
    glm::vec3 sunDir = sunDir_;

    // 0. Directional Shadow Map Depth Pre-Pass (before main 4x MSAA color render pass)
    if (activeScene_) {
        pipelineManager_.EnsureOpaqueCopyTexture(gpuContext_.GetWidth(), gpuContext_.GetHeight());
        pipelineManager_.RenderShadowPass(gpuContext_.GetCommandEncoder(), *activeScene_, sunDir, quadtree_.get());
    }

    WGPURenderPassEncoder hdrPass = gpuContext_.BeginFrame();
    if (!hdrPass)
        return;

    if (activeScene_) {
        // 1. Hardware 4x MSAA Opaque/Masked PBR Pass (RGBA16Float + 16x Aniso + Quadtree Frustum Culling)
        pipelineManager_.RenderOpaqueScene(hdrPass, *activeScene_, vp, camPos, wireframeMode_, quadtree_.get(),
                                           camera_.GetViewMatrix(), camera_.GetProjectionMatrix(),
                                           gpuContext_.GetWidth(), gpuContext_.GetHeight(), camera_.GetNearPlane(),
                                           camera_.GetFarPlane());
        static bool loggedFirstFrame = false;
        if (!loggedFirstFrame) {
            loggedFirstFrame = true;
            BGL_LOG_DEBUG("Frame 1 Render Diagnostic: {} visible primitives rendered ({} occluded by Hi-Z)",
                          pipelineManager_.GetVisiblePrimitiveCount(), pipelineManager_.GetOccludedPrimitiveCount());
        }
    }

    // 2. Procedural Atmospheric Skybox (rendered before opaque snapshot so glass refracts the skybox)
    skybox_.Render(hdrPass, vp, camPos, sunDir, pipelineManager_.GetSolarElevationFactor(),
                   static_cast<uint32_t>(pipelineManager_.GetWeatherMode()), pipelineManager_.GetWeatherIntensity());

    // 3. Snapshot Opaque + Skybox HDR Resolve -> OpaqueColorCopyTexture, generate roughness-aware frosted mips,
    //    and render Transparent/Transmissive Pass
    gpuContext_.SnapshotOpaqueToCopy(hdrPass, pipelineManager_.GetOpaqueColorCopyTexture(),
                                     pipelineManager_.GetOpaqueCopyWidth(), pipelineManager_.GetOpaqueCopyHeight());
    pipelineManager_.GenerateOpaqueCopyMips(gpuContext_.GetCommandEncoder());
    WGPURenderPassEncoder transparentPass = gpuContext_.OpenTransparentPass();
    if (!transparentPass)
        return;

    if (activeScene_) {
        pipelineManager_.RenderTransparentScene(transparentPass, *activeScene_, vp, wireframeMode_);

        // 4. Batch & Flush 3D Wireframe Bounding Boxes (Gold/Cyan) and Quadtree Bounds (Emerald) in a single draw call
        if (showBoundingBoxes_) {
            pipelineManager_.BatchSceneAABBs(*activeScene_, true, true);
        }
        if (showQuadtreeBounds_ && quadtree_) {
            pipelineManager_.BatchQuadtreeBounds(*quadtree_);
        }
        if (selectedNode_ && selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
            // Highlight selected node with a vibrant gold bounding outline
            pipelineManager_.AppendAABBLines(selectedNode_->worldAABB, glm::vec4{1.0f, 0.72f, 0.05f, 1.0f});
        }
        if (showBoundingBoxes_ || (showQuadtreeBounds_ && quadtree_) || selectedNode_) {
            pipelineManager_.FlushAABBLines(transparentPass, vp);
        }
    }

    // 5. End HDR MSAA pass so resolved HDR color + depth can be read by Selection Outline & Post-Processing
    gpuContext_.FinishHdrPass(transparentPass);

    if (activeScene_ && selectedNode_) {
        pipelineManager_.RenderSelectionOutline(gpuContext_.GetCommandEncoder(), *activeScene_, selectedNode_.get(), vp,
                                                gpuContext_.GetHdrResolveView(), gpuContext_.GetWidth(),
                                                gpuContext_.GetHeight());
    }

    // 6. Execute pre-swapchain GPU passes (Depth Linearize, GTAO, 5-Mip Bloom + UI Backdrop, Auto-Exposure,
    // Tonemap->LDR)
    pipelineManager_.PreparePostProcessPass(
        gpuContext_.GetCommandEncoder(), gpuContext_.GetHdrResolveView(), gpuContext_.GetDepthReadView(),
        gpuContext_.GetSampleCount(), vp, camera_.GetProjectionMatrix(), camPos, sunDir, skybox_.GetRotation(),
        gpuContext_.GetWidth(), gpuContext_.GetHeight(), lastDeltaTime_, elapsedTime_);

    settingsUi_.SetBackdropTextureView(pipelineManager_.GetUiBackdropView());

    // 7. Open Swapchain pass (1x), finish FXAA / Tonemap, and draw HUD + ImmediateUI crisp on top
    WGPURenderPassEncoder swapchainPass = gpuContext_.BeginSwapchainPass();
    if (!swapchainPass)
        return;

    pipelineManager_.RenderTonemapPass(swapchainPass, gpuContext_.GetHdrResolveView(),
                                       camera_.GetViewProjectionMatrix(), camera_.GetPosition(), sunDir,
                                       skybox_.GetRotation(), gpuContext_.GetWidth(), gpuContext_.GetHeight());

    debugOverlay_.Render(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());
    if (showSettingsUi_) {
        settingsUi_.Render(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());
    }

    gpuContext_.EndFrame(swapchainPass);
}

void Application::ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri) {
    if (!scene)
        return;
    activeScene_ = std::move(scene);
    currentModelName_ = core::NetworkFetcher::ExtractDisplayName(sourceUri);
    CacheSceneMetrics();
    pipelineManager_.PrepareScene(*activeScene_);

    const glm::vec3 center{activeScene_->sceneAABB.Center()};
    const float radius{glm::length(activeScene_->sceneAABB.Extents())};

    if (sourceUri.find("Sponza") != std::string_view::npos || sourceUri.find("sponza") != std::string_view::npos) {
        const glm::vec3 courtyardCenter{center.x, 2.0f, center.z};
        constexpr float camDistance{10.0f};
        camera_ = core::Camera{courtyardCenter, camDistance};
        camera_.Orbit(1.2f, 0.15f);
        BGL_LOG_INFO("Sponza scene activated: courtyardCenter=[{:.2f}, {:.2f}, {:.2f}], camDistance={:.2f}",
                     courtyardCenter.x, courtyardCenter.y, courtyardCenter.z, camDistance);
    } else {
        const float targetDistance{std::max(radius * 2.5f, 0.15f)};
        camera_ = core::Camera{center, targetDistance};
        camera_.Orbit(0.8f, 0.4f);
        BGL_LOG_INFO("Scene '{}' activated: center=[{:.4f}, {:.4f}, {:.4f}], radius={:.4f}, camDistance={:.4f}",
                     currentModelName_, center.x, center.y, center.z, radius, targetDistance);
    }

    core::AABB worldBounds{activeScene_->sceneAABB};
    const glm::vec3 margin{glm::max(glm::vec3{10.0f}, worldBounds.Extents() * 0.1f)};
    worldBounds.minBound -= margin;
    worldBounds.maxBound += margin;
    quadtree_ = std::make_unique<scene::Quadtree>(worldBounds);
    quadtree_->InsertNodes(activeScene_->rootNodes);
    cachedQuadtreeNodeCount_ = quadtree_->GetNodeCount();
    lastQuadtreeTransformVersion_ = activeScene_->transformVersion_;

    if (!activeScene_->animations.empty()) {
        activeScene_->SetAnimation(0);
        BGL_LOG_INFO("Activated default glTF animation 0 ('{}', duration: {:.2f}s)",
                     activeScene_->animations[0].name.empty() ? "unnamed" : activeScene_->animations[0].name,
                     activeScene_->animations[0].duration);
    }
}

core::Result<void> Application::LoadScene(std::span<const uint8_t> data, std::string_view baseUri) {
    if (data.empty()) {
        return std::unexpected("Cannot load glTF scene from empty byte buffer");
    }
    if (!gltfLoader_) {
        gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);
    }
    auto sceneRes = gltfLoader_->LoadFromMemory(data, baseUri);
    if (!sceneRes) {
        return std::unexpected(sceneRes.error());
    }
    ActivateScene(*sceneRes, baseUri);
    return {};
}

core::Result<void> Application::LoadSceneFromUri(std::string_view uri) {
    const std::string sanitizedUri{core::NetworkFetcher::SanitizeUri(uri)};
    if (sanitizedUri.empty()) {
        return std::unexpected("Cannot load empty asset URI");
    }

    BGL_LOG_INFO("Loading glTF/GLB asset from {} '{}'...", core::NetworkFetcher::IsUrl(sanitizedUri) ? "URL" : "path",
                 sanitizedUri);
    auto fetchRes = core::NetworkFetcher::FetchSync(sanitizedUri);
    if (!fetchRes) {
        return std::unexpected("Failed to fetch glTF asset '" + sanitizedUri + "': " + fetchRes.error());
    }
    return LoadScene(*fetchRes, sanitizedUri);
}

void Application::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped buffer '{}' ({} bytes)", filename, data.size());
    if (auto res = LoadScene(data, filename); res) {
        BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", filename);
    } else {
        BGL_LOG_ERROR("Failed to load dropped glTF buffer '{}': {}", filename, res.error());
    }
}

void Application::Shutdown() {
    activeScene_.reset();
    quadtree_.reset();
    gltfLoader_.reset();
    assetManager_.Clear();
    settingsUi_.Shutdown();
    skybox_.Shutdown();
    pipelineManager_.Shutdown();
    soundEngine_.Shutdown();
    gpuContext_.Shutdown();
#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_) {
        SDL_DestroyWindow(nativeWindow_);
        nativeWindow_ = nullptr;
        SDL_Quit();
    }
#endif
    running_ = false;
    BGL_LOG_INFO("bgl Engine shutdown completed cleanly.");
}

void Application::CacheSceneMetrics() {
    totalPrimitives_ = 0;
    totalVertices_ = 0;
    totalIndices_ = 0;
    if (!activeScene_)
        return;

    size_t& primCount{totalPrimitives_};
    size_t& vertCount{totalVertices_};
    size_t& idxCount{totalIndices_};
    auto countStats = [&](this auto&& self, const scene::SceneNode& node) -> void {
        for (const auto& prim : node.GetPrimitives()) {
            primCount++;
            vertCount += prim.vertices.size();
            idxCount += prim.indices.size();
        }
        for (const auto& child : node.children) {
            if (child)
                self(*child);
        }
    };

    for (const auto& rootNode : activeScene_->rootNodes) {
        if (rootNode)
            countStats(*rootNode);
    }
    BGL_LOG_INFO("Scene '{}' metrics cached: {} primitives, {} vertices, {} indices", currentModelName_,
                 totalPrimitives_, totalVertices_, totalIndices_);
}

void Application::FocusSelection() noexcept {
    if (selectedNode_ && selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
        camera_.FocusOnBounds(selectedNode_->worldAABB);
    } else if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void Application::FrameAll() noexcept {
    if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void Application::SelectNodeByIndex(int index) noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->allNodes.size()) {
        ClearSelection();
        return;
    }
    SelectNode(activeScene_->allNodes[index], index);
}

void Application::SelectNode(std::shared_ptr<scene::SceneNode> node, int index) noexcept {
    selectedNode_ = std::move(node);
    selectedNodeIndex_ = index;
    if (index < 0 && node && activeScene_) {
        for (size_t i = 0; i < activeScene_->allNodes.size(); ++i) {
            if (activeScene_->allNodes[i] == node) {
                selectedNodeIndex_ = static_cast<int>(i);
                break;
            }
        }
    }
    if (selectedNode_) {
        if (selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
            const float distToNode = glm::length(camera_.GetPosition() - selectedNode_->worldAABB.Center());
            pipelineManager_.GetPostProcessPass().SetDoFFocusDistance(distToNode);
        }
        BGL_LOG_INFO("Selected Node #{} '{}' (AABB min=[{:.2f}, {:.2f}, {:.2f}], max=[{:.2f}, {:.2f}, {:.2f}])",
                     selectedNodeIndex_, selectedNode_->name, selectedNode_->worldAABB.minBound.x,
                     selectedNode_->worldAABB.minBound.y, selectedNode_->worldAABB.minBound.z,
                     selectedNode_->worldAABB.maxBound.x, selectedNode_->worldAABB.maxBound.y,
                     selectedNode_->worldAABB.maxBound.z);
    }
}

int Application::PickNodeAt(float screenX, float screenY, float screenWidth, float screenHeight) {
    if (!activeScene_ || screenWidth <= 0.0f || screenHeight <= 0.0f) {
        return -1;
    }
    const core::Ray pickRay = camera_.ScreenPointToRay(screenX, screenY, screenWidth, screenHeight);
    if (auto hit = scene::RayPicker::Pick(pickRay, activeScene_->rootNodes)) {
        if (hit->hitNode) {
            SelectNode(hit->hitNode, hit->hitNode->nodeIndex);
            if (std::filesystem::exists("assets/audio/click.wav")) {
                if (auto audioRes = soundEngine_.PlaySound3D("pick", "assets/audio/click.wav", hit->hitPoint);
                    !audioRes) {
                    BGL_LOG_DEBUG("Audio pick cue skipped: {}", audioRes.error());
                }
            }
            BGL_LOG_INFO("[RayPicker] Hit Node '{}' (index #{}, distance {:.3f}m)", hit->hitNode->name,
                         hit->hitNode->nodeIndex, hit->distance);
            return hit->hitNode->nodeIndex;
        }
    }
    ClearSelection();
    return -1;
}

void Application::SelectCamera(int cameraIndex) noexcept {
    if (!activeScene_ || cameraIndex < 0 || static_cast<size_t>(cameraIndex) >= activeScene_->cameras.size()) {
        activeCameraIndex_ = -1;
        BGL_LOG_INFO("Switched to default interactive orbit/fly camera");
        return;
    }
    activeCameraIndex_ = cameraIndex;
    const auto& sc = activeScene_->cameras[cameraIndex];
    camera_.SetFromTransform(sc.worldTransform, sc.yfov, sc.znear, sc.zfar);
    BGL_LOG_INFO("Activated glTF camera #{}: '{}' (fovY={:.1f}deg, near={:.2f}, far={:.2f})", cameraIndex, sc.name,
                 glm::degrees(sc.yfov), sc.znear, sc.zfar);
}

void Application::CycleCamera() noexcept {
    if (!activeScene_ || activeScene_->cameras.empty())
        return;
    int next = activeCameraIndex_ + 1;
    if (static_cast<size_t>(next) >= activeScene_->cameras.size()) {
        next = -1;
    }
    SelectCamera(next);
}

int Application::GetCameraCount() const noexcept {
    return activeScene_ ? static_cast<int>(activeScene_->cameras.size()) : 0;
}

std::string_view Application::GetCameraName(int index) const noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->cameras.size()) {
        return "";
    }
    return activeScene_->cameras[index].name;
}

void Application::SetTimeOfDay(float hour) noexcept {
    float wrapped = std::fmod(hour, 24.0f);
    if (wrapped < 0.0f) {
        wrapped += 24.0f;
    }
    timeOfDay_ = wrapped;
    pipelineManager_.SetTimeOfDay(timeOfDay_);

    // Diurnal cycle: sunrise ~6:00, noon 12:00 (peak elevation), sunset ~18:00, midnight 0:00 (moon peak)
    const float solarSin = -std::cos((timeOfDay_ / 24.0f) * 2.0f * std::numbers::pi_v<float>);
    const float activeElev = (solarSin >= 0.0f) ? std::asin(std::clamp(solarSin, 0.0f, 1.0f))
                                                : std::asin(std::clamp(-solarSin * 0.75f, 0.0f, 1.0f));
    const float azimuth = (timeOfDay_ / 24.0f) * 2.0f * std::numbers::pi_v<float>;
    SetSunAzimuthElevation(azimuth, std::max(activeElev, 0.06f));
}

void Application::SetSunAzimuthElevation(float azimuthRad, float elevationRad) noexcept {
    sunAzimuth_ = azimuthRad;
    sunElevation_ = std::clamp(elevationRad, 0.02f, 1.57f);
    UpdateSunDirection();
}

void Application::UpdateSunDirection() noexcept {
    const float cosEl = std::cos(sunElevation_);
    sunDir_ = glm::normalize(
        glm::vec3{cosEl * std::sin(sunAzimuth_), std::sin(sunElevation_), cosEl * std::cos(sunAzimuth_)});
}

void Application::SetKtxCompressionTarget(scene::KtxTranscodeTarget target) {
    if (!activeScene_) {
        return;
    }
    scene::KtxTranscodeTarget resolved = target;
    const bool unsupported = (resolved == scene::KtxTranscodeTarget::Bc7 && !gpuContext_.SupportsBC()) ||
                             (resolved == scene::KtxTranscodeTarget::Astc4x4 && !gpuContext_.SupportsASTC()) ||
                             (resolved == scene::KtxTranscodeTarget::Etc2Rgba && !gpuContext_.SupportsETC2());
    if (unsupported) {
        resolved = scene::KtxTranscodeTarget::Rgba32;
    }
    pipelineManager_.RetranscodeKtxTextures(*activeScene_, resolved);
}

void Application::UpdateSettingsUI() {
    int winW = 1280;
    int winH = 720;
    if (nativeWindow_) {
        SDL_GetWindowSize(nativeWindow_, &winW, &winH);
    }
    const uint32_t vpW = static_cast<uint32_t>(std::max(1, winW));
    const uint32_t vpH = static_cast<uint32_t>(std::max(1, winH));
    settingsUi_.BeginFrame(uiInput_, vpW, vpH);

    if (!showSettingsUi_) {
        settingsUi_.EndFrame();
        return;
    }

    // Apply live UI accent theme before building widgets
    settingsUi_.Style().accent = glm::vec4{uiAccentColor_, 1.0f};
    settingsUi_.Style().sparklineFill = glm::vec4{uiAccentColor_, 0.24f};

    // World-to-screen floating badge anchored on selected 3D node
    if (showSelectionAnchors_ && selectedNode_ &&
        selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
        const glm::vec3 center = selectedNode_->worldAABB.Center();
        const glm::vec4 clip = camera_.GetViewProjectionMatrix() * glm::vec4{center, 1.0f};
        if (clip.w > 0.001f) {
            const glm::vec3 ndc = glm::vec3{clip} / clip.w;
            if (ndc.z >= 0.0f && ndc.z <= 1.0f && ndc.x >= -0.95f && ndc.x <= 0.95f && ndc.y >= -0.95f &&
                ndc.y <= 0.95f) {
                const float sx = (ndc.x * 0.5f + 0.5f) * static_cast<float>(vpW);
                const float sy = (0.5f - ndc.y * 0.5f) * static_cast<float>(vpH);
                const float dist = glm::length(camera_.GetPosition() - center);
                const std::string nodeTitle = selectedNode_->name.empty() ? "Selected" : selectedNode_->name;
                settingsUi_.DrawFloatingBadge(sx + 8.0f, sy - 24.0f, std::format("{} ({:.1f}m)", nodeTitle, dist),
                                              glm::vec4{1.0f, 0.72f, 0.12f, 0.95f});
            }
        }
    }

    settingsUi_.BeginWindow("bgl-Engine Studio", std::max(16.0f, static_cast<float>(vpW) - 356.0f), 14.0f, 340.0f);

    const double totalMb = static_cast<double>(lastVramBytes_) / (1024.0 * 1024.0);
    const double texMb = static_cast<double>(lastTextureVramBytes_) / (1024.0 * 1024.0);
    bgl::ui::UI::Label(std::format("VRAM: {:.1f} MB (Tex: {:.1f} MB)", totalMb, texMb),
                       glm::vec4{0.72f, 0.86f, 0.98f, 1.0f});
    if (fpsHistoryCount_ >= 2) {
        bgl::ui::UI::Sparkline("Frame Rate (FPS)", std::span<const float>{fpsHistory_.data(), fpsHistoryCount_}, 0.0f,
                               0.0f, 34.0f, glm::vec4{uiAccentColor_, 1.0f});
    }

    bgl::ui::UI::Separator();

    // 1. Scene & Camera Section
    if (bgl::ui::UI::CollapsingHeader("Camera & Scene", true)) {
        static constexpr const char* kViewModeNames[] = {
            "Lit PBR", "LOD Heatmap", "Normals", "Metal/Rough/AO", "UV Checker", "Clay + Wireframe", "Toon / Cel",
        };
        size_t viewIdx = static_cast<size_t>(std::to_underlying(pipelineManager_.GetDebugViewMode()));
        if (bgl::ui::UI::Dropdown("Shading View", kViewModeNames, viewIdx)) {
            pipelineManager_.SetDebugViewMode(static_cast<gpu::DebugViewMode>(viewIdx));
        }

        if (bgl::ui::UI::Button("Frame All / Reset View")) {
            FrameAll();
        }
        bool turntable = turntableActive_;
        if (bgl::ui::UI::Checkbox("Auto Turntable", turntable)) {
            turntableActive_ = turntable;
        }
        bool wf = wireframeMode_;
        if (bgl::ui::UI::Checkbox("Wireframe Overlay [F1]", wf)) {
            wireframeMode_ = wf;
        }
        bool aabbs = showBoundingBoxes_;
        if (bgl::ui::UI::Checkbox("Bounding Boxes [B]", aabbs)) {
            showBoundingBoxes_ = aabbs;
        }
        bool anchors = showSelectionAnchors_;
        if (bgl::ui::UI::Checkbox("3D Selection Badge", anchors)) {
            showSelectionAnchors_ = anchors;
        }

        if (activeScene_ && !activeScene_->allNodes.empty()) {
            bgl::ui::UI::Label("Scene Nodes (Scroll):", settingsUi_.Style().textMuted);
            bgl::ui::UI::BeginScrollArea("scene_hierarchy_scroll", 104.0f);
            size_t shownCount = 0;
            for (size_t ni = 0; ni < activeScene_->allNodes.size() && shownCount < 48; ++ni) {
                const auto& node = activeScene_->allNodes[ni];
                if (!node || node->GetPrimitives().empty()) {
                    continue;
                }
                ++shownCount;
                const bool isSel = (selectedNodeIndex_ == static_cast<int>(ni));
                const std::string btnText =
                    std::format("{}{} [{}]", isSel ? "* " : "", node->name.empty() ? "MeshNode" : node->name, ni);
                if (bgl::ui::UI::Button(btnText)) {
                    SelectNodeByIndex(static_cast<int>(ni));
                }
            }
            bgl::ui::UI::EndScrollArea();
        }
    }

    // 2. Post-Processing & Cinema Lens Section
    if (bgl::ui::UI::CollapsingHeader("Post-Processing & Lens", true)) {
        auto& pp = pipelineManager_.GetPostProcessPass();

        static constexpr const char* kTonemapNames[] = {"Khronos PBR Neutral", "AgX Cinema", "ACES Filmic"};
        size_t tmIdx = static_cast<size_t>(std::to_underlying(pp.GetToneMapOperator()));
        if (bgl::ui::UI::Dropdown("Tone Mapper", kTonemapNames, tmIdx)) {
            pp.SetToneMapOperator(static_cast<gpu::ToneMapOperator>(tmIdx));
        }

        static constexpr const char* kGradingNames[] = {"Neutral", "Warm Cinema", "Cool Sci-Fi", "Bleach Bypass",
                                                        "Noir"};
        size_t gradeIdx = static_cast<size_t>(std::to_underlying(pp.GetColorGradingPreset()));
        if (bgl::ui::UI::Dropdown("Color Grading", kGradingNames, gradeIdx)) {
            pp.SetColorGradingPreset(static_cast<gpu::ColorGradingPreset>(gradeIdx));
        }

        float exposure = pp.GetExposure();
        if (bgl::ui::UI::SliderFloat("Exposure (EV)", exposure, 0.2f, 4.0f, "{:.2f}")) {
            pp.SetExposure(exposure);
        }
        bool autoExp = pp.IsAutoExposureEnabled();
        if (bgl::ui::UI::Checkbox("GPU Auto-Exposure", autoExp)) {
            pp.SetAutoExposureEnabled(autoExp);
        }
        bool fxaa = pp.IsFxaaEnabled();
        if (bgl::ui::UI::Checkbox("FXAA Edge AA", fxaa)) {
            pp.SetFxaaEnabled(fxaa);
        }

        bool bloom = pp.IsBloomEnabled();
        if (bgl::ui::UI::Checkbox("5-Mip HDR Bloom [Z]", bloom)) {
            pp.SetBloomEnabled(bloom);
        }
        if (bloom) {
            float bloomInt = pp.GetBloomIntensity();
            if (bgl::ui::UI::SliderFloat("Bloom Intensity", bloomInt, 0.0f, 2.5f, "{:.2f}")) {
                pp.SetBloomIntensity(bloomInt);
            }
        }

        bool gtao = pp.IsSsaoEnabled();
        if (bgl::ui::UI::Checkbox("Depth GTAO Occlusion [G]", gtao)) {
            pp.SetSsaoEnabled(gtao);
        }
        if (gtao) {
            float gtaoInt = pp.GetSsaoIntensity();
            if (bgl::ui::UI::SliderFloat("GTAO Strength", gtaoInt, 0.0f, 1.5f, "{:.2f}")) {
                pp.SetSsaoIntensity(gtaoInt);
            }
        }

        bool ssr = pp.IsSsrEnabled();
        if (bgl::ui::UI::Checkbox("Screen-Space Reflections", ssr)) {
            pp.SetSsrEnabled(ssr);
        }
        bool rays = pp.IsGodRaysEnabled();
        if (bgl::ui::UI::Checkbox("Volumetric Sun God Rays", rays)) {
            pp.SetGodRaysEnabled(rays);
        }
        bool dof = pp.IsDoFEnabled();
        if (bgl::ui::UI::Checkbox("Bokeh Depth of Field", dof)) {
            pp.SetDoFEnabled(dof);
        }
        if (dof) {
            float fDist = pp.GetDoFFocusDistance();
            if (bgl::ui::UI::SliderFloat("DoF Focus Dist", fDist, 0.5f, 40.0f, "{:.1f}m")) {
                pp.SetDoFFocusDistance(fDist);
            }
            float fRange = pp.GetDoFFocusRange();
            if (bgl::ui::UI::SliderFloat("DoF Range", fRange, 1.0f, 20.0f, "{:.1f}m")) {
                pp.SetDoFFocusRange(fRange);
            }
        }

        float vign = pp.GetVignetteIntensity();
        if (bgl::ui::UI::SliderFloat("Vignette", vign, 0.0f, 1.0f, "{:.2f}")) {
            pp.SetVignetteIntensity(vign);
        }
    }

    // 3. Environment & Weather Section
    if (bgl::ui::UI::CollapsingHeader("Atmosphere & Weather", false)) {
        static constexpr const char* kEnvNames[] = {"Mediterranean Courtyard", "Sunset Studio", "Overcast Atelier",
                                                    "Night Neon"};
        size_t envIdx =
            std::min(static_cast<size_t>(std::to_underlying(pipelineManager_.GetEnvironmentPreset())), size_t{3});
        if (bgl::ui::UI::Dropdown("Environment Preset", kEnvNames, envIdx)) {
            pipelineManager_.SetEnvironmentPreset(static_cast<gpu::EnvironmentPreset>(envIdx), activeScene_.get());
        }

        float tod = timeOfDay_;
        if (bgl::ui::UI::SliderFloat("Time of Day", tod, 0.0f, 24.0f, "{:.1f}h")) {
            SetTimeOfDay(tod);
        }
        bool dn = dayNightCycleActive_;
        if (bgl::ui::UI::Checkbox("Animate Sun Cycle", dn)) {
            dayNightCycleActive_ = dn;
        }

        static constexpr const char* kWeatherNames[] = {"Clear Sky", "Rain + Puddles", "Snowfall", "Dense Fog",
                                                        "Volumetric Smoke"};
        size_t wIdx = static_cast<size_t>(std::to_underlying(pipelineManager_.GetWeatherMode()));
        if (bgl::ui::UI::Dropdown("Weather Mode", kWeatherNames, wIdx)) {
            pipelineManager_.SetWeatherMode(static_cast<gpu::WeatherMode>(wIdx));
        }
        float wInt = pipelineManager_.GetWeatherIntensity();
        if (bgl::ui::UI::SliderFloat("Weather Intensity", wInt, 0.0f, 1.5f, "{:.2f}")) {
            pipelineManager_.SetWeatherIntensity(wInt);
        }
    }

    // 4. Engine, Texture Compression & Theme
    if (bgl::ui::UI::CollapsingHeader("Engine & UI Theme", false)) {
        static constexpr const char* kKtxOptions[] = {
            "Auto (Hardware)", "BC7 (Desktop)", "ASTC 4x4 (Mobile)", "ETC2 RGBA", "RGBA32 (Uncompressed)",
        };
        if (bgl::ui::UI::Dropdown("KTX2 Compression", kKtxOptions, selectedKtxOption_)) {
            scene::KtxTranscodeTarget target = scene::KtxTranscodeTarget::Rgba32;
            switch (selectedKtxOption_) {
            case 0:
                if (gpuContext_.SupportsBC()) {
                    target = scene::KtxTranscodeTarget::Bc7;
                } else if (gpuContext_.SupportsASTC()) {
                    target = scene::KtxTranscodeTarget::Astc4x4;
                } else if (gpuContext_.SupportsETC2()) {
                    target = scene::KtxTranscodeTarget::Etc2Rgba;
                }
                break;
            case 1:
                target = scene::KtxTranscodeTarget::Bc7;
                break;
            case 2:
                target = scene::KtxTranscodeTarget::Astc4x4;
                break;
            case 3:
                target = scene::KtxTranscodeTarget::Etc2Rgba;
                break;
            case 4:
            default:
                target = scene::KtxTranscodeTarget::Rgba32;
                break;
            }
            SetKtxCompressionTarget(target);
        }

        bool mipmaps = pipelineManager_.IsMipmappingEnabled();
        if (bgl::ui::UI::Checkbox("Anisotropic Mipmapping", mipmaps)) {
            ToggleMipmapping();
        }
        bool shadows = pipelineManager_.IsShadowsEnabled();
        if (bgl::ui::UI::Checkbox("Soft Poisson Shadows", shadows)) {
            ToggleShadows();
        }
        bgl::ui::UI::ColorEdit3("UI Accent Color", uiAccentColor_);
    }

    settingsUi_.EndWindow();
    settingsUi_.EndFrame();
}

void Application::PollShaderHotReload(float deltaTime) {
#if defined(BGL_PLATFORM_NATIVE)
    hotReloadPollTimer_ += deltaTime;
    if (hotReloadPollTimer_ < 0.25f) {
        return;
    }
    hotReloadPollTimer_ = 0.0f;

    const auto modified = gpu::ShaderManager::Instance().PollModifiedShaders();
    if (modified.empty()) {
        return;
    }

    if (std::ranges::find(modified, std::string{"skybox.wgsl"}) != modified.end()) {
        if (auto res = skybox_.ReloadShader(); !res) {
            BGL_LOG_ERROR("WGSL hot-reload failed for skybox.wgsl: {}", res.error());
        } else {
            BGL_LOG_INFO("WGSL hot-reloaded: skybox.wgsl");
        }
    }
    if (auto res = pipelineManager_.ReloadShaders(modified); !res) {
        BGL_LOG_ERROR("WGSL hot-reload failed for pipeline shaders: {}", res.error());
    } else {
        BGL_LOG_INFO("WGSL hot-reloaded pipeline shaders cleanly.");
    }
#else
    (void)deltaTime;
#endif
}

} // namespace bgl::app
