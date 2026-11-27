/**
 * @file Application.cpp
 * @brief Core application loop, SDL2 window management, WebGPU pipeline and scene orchestration.
 */
#include "viewer/Application.hpp"
#include "core/Log.hpp"
#include "core/MemoryTracker.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Version.hpp"
#include "gpu/ShaderManager.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <ranges>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::app {

core::Result<void> Application::Initialize(uint32_t width, uint32_t height) {
    ApplicationConfig config{};
    config.width = width;
    config.height = height;
    return Initialize(config);
}

core::Result<void> Application::Initialize(const ApplicationConfig& config) {
    engine::EngineCoreConfig coreConfig{};
    coreConfig.width = config.width;
    coreConfig.height = config.height;
    if (auto res = EngineCore::Initialize(coreConfig); !res) {
        return res;
    }

    showBoundingBoxes_ = config.enableBoundingBoxes;
    showQuadtreeBounds_ = config.enableQuadtreeBounds;

    pipelineManager_.SetBumpMappingEnabled(config.enableBumpMapping);
    pipelineManager_.SetMipmappingEnabled(config.enableMipmapping);
    pipelineManager_.SetShadowsEnabled(config.enableShadows);
    pipelineManager_.SetReflectionsEnabled(config.enableReflections);
    pipelineManager_.SetAmbientOcclusionEnabled(config.enableAmbientOcclusion);
    pipelineManager_.SetOcclusionCullingEnabled(config.enableOcclusionCulling);
    pipelineManager_.SetLodEnabled(config.enableLod);
    pipelineManager_.SetAutoLodEnabled(config.enableAutoLod);
    pipelineManager_.SetLodBias(config.lodBias);
    pipelineManager_.SetDebugViewMode(static_cast<gpu::DebugViewMode>(config.debugViewMode));
    pipelineManager_.SetWeatherMode(static_cast<gpu::WeatherMode>(config.weatherMode));
    pipelineManager_.SetWeatherIntensity(config.weatherIntensity);
    pipelineManager_.SetWindStrength(config.windStrength);
    dayNightCycleActive_ = config.enableDayNightCycle;
    SetDayNightSpeed(config.dayNightSpeed);
    SetTimeOfDay(config.timeOfDay);

    skybox_.SetEnabled(config.enableSkybox);

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

    // Resolve model asset path from config or auto-detect Sponza.glb (primary) / ToyCar.glb (fallback)
    std::string assetPathToLoad{config.assetPath};
    if (assetPathToLoad.empty()) {
        static constexpr std::array<std::string_view, 4> kSearchPaths{"assets/Sponza.glb", "../assets/Sponza.glb",
                                                                      "assets/ToyCar.glb", "../assets/ToyCar.glb"};
        const auto it = std::ranges::find_if(kSearchPaths, [](std::string_view path) {
            std::error_code ec{};
            return std::filesystem::exists(path, ec);
        });
        if (it != kSearchPaths.end()) {
            assetPathToLoad = std::string{*it};
        }
    }

    if (!assetPathToLoad.empty()) {
        if (auto loadRes = LoadSceneFromUri(assetPathToLoad); !loadRes) {
            BGL_LOG_WARN("Could not load '{}': {}", assetPathToLoad, loadRes.error());
        }
    }

    return {};
}

void Application::Update(float deltaTime) {
    if (!running_)
        return;

    const float safeDt = (std::isfinite(deltaTime) && deltaTime > 0.0f) ? deltaTime : 0.016f;
    const float clampedDelta = std::clamp(safeDt, 0.0005f, 0.25f);
    fpsSampleTimer_ += clampedDelta;
    if (fpsSampleTimer_ >= 0.06f) {
        fpsSampleTimer_ = 0.0f;
        const float instFps = std::clamp(1.0f / std::max(safeDt, 1e-4f), 1.0f, 360.0f);
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
    std::string pendingDropUri{};

#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running_ = false;
                return;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED && event.window.data1 > 0 && event.window.data2 > 0) {
                    gpuContext_.Resize(static_cast<uint32_t>(event.window.data1),
                                       static_cast<uint32_t>(event.window.data2));
                    pipelineManager_.GetPostProcessPass().InvalidateBindGroup();
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    uiInput_.mouseDown = false;
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
                const float sign = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
                wheelDelta += static_cast<float>(event.wheel.y) * sign;
            } else if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
                char* droppedStr{event.drop.file};
                if (droppedStr) {
                    const std::string sanitized{core::NetworkFetcher::SanitizeUri(droppedStr)};
                    if (!sanitized.empty()) {
                        BGL_LOG_INFO("Asset/URL dropped via OS ({}): '{}'",
                                     event.type == SDL_DROPTEXT ? "SDL_DROPTEXT" : "SDL_DROPFILE",
                                     core::SanitizeForLog(sanitized));
                        pendingDropUri = sanitized;
                    }
                    SDL_free(droppedStr);
                }
            }
        }
        if (!pendingDropUri.empty()) {
            LoadSceneFromUriAsync(pendingDropUri);
        }
    }

    const Uint8* keyState = SDL_GetKeyboardState(nullptr);
    if (keyState) {
        const float moveSpeed = std::max(camera_.GetDistance() * 1.5f, 1.0f) * clampedDelta;
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

    gamepad_.Update(clampedDelta);
    const auto& gp = gamepad_.GetState();

    if (gp.connected) {
        if (camera_.GetMode() == core::CameraMode::Fly) {
            camera_.MoveForward(-gp.smoothedMove.y * clampedDelta * 6.0f);
            camera_.MoveRight(gp.smoothedMove.x * clampedDelta * 6.0f);
        } else {
            camera_.Pan(gp.smoothedMove.x * clampedDelta * camera_.GetDistance(),
                        gp.smoothedMove.y * clampedDelta * camera_.GetDistance());
        }
        // PS5 smooth exponential response curve and stabilized pitch
        camera_.Orbit(gp.smoothedLook.x * clampedDelta * 2.8f, gp.smoothedLook.y * clampedDelta * 2.8f);
        camera_.Zoom((gp.rightTrigger - gp.leftTrigger) * clampedDelta * camera_.GetDistance() * 1.5f);
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
        camera_.Orbit(turntableSpeed_ * clampedDelta, 0.0f);
    }

    PollAsyncSceneLoad(clampedDelta);
    UpdateSimulation(clampedDelta);

    // Collect telemetry stats for the in-engine debug HUD overlay
    ui::OverlayStats& stats{cachedOverlayStats_};
    stats.deltaTime = safeDt;
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
    memTelemetryTimer_ += safeDt;
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

    const auto rtStats = pipelineManager_.GetRtPassStats();
    stats.rtSupported = pipelineManager_.IsDesktopRayTracingSupported();
    stats.rtActive = rtStats.active;
    stats.rtShadowsActive = pipelineManager_.IsRtShadowsActive();
    stats.rtAoActive = pipelineManager_.IsRtAoActive();
    stats.rtReflectionsActive = pipelineManager_.IsRtReflectionsActive();
    stats.rtPathTracingActive = rtStats.pathTracingActive;
    stats.rtBindlessActive = rtStats.bindlessActive;
    stats.rtPathSamples = rtStats.pathSamples;
    stats.rtTriangles = rtStats.sceneStats.triangleCount;
    stats.rtBlasNodes = rtStats.sceneStats.blasNodeCount;
    stats.rtGpuMs = rtStats.lastGpuMs;

    if (quadtree_) {
        stats.quadtreeNodes = cachedQuadtreeNodeCount_;
        stats.visiblePrimitives = pipelineManager_.GetVisiblePrimitiveCount();
        stats.occludedPrimitives = pipelineManager_.GetOccludedPrimitiveCount();
    } else {
        stats.quadtreeNodes = 0;
        stats.visiblePrimitives = totalPrimitives_;
        stats.occludedPrimitives = 0;
    }

    debugOverlay_.Update(safeDt, stats);
}

void Application::OnSceneActivated(std::string_view sourceUri) {
    SyncModelSelectionFromUri(sourceUri);
}

void Application::OnPrepareOverlay() {
    settingsUi_.SetBackdropTextureView(pipelineManager_.GetUiBackdropView());
}

void Application::OnRenderOverlay(WGPURenderPassEncoder swapchainPass) {
    debugOverlay_.Render(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());
    if (showSettingsUi_) {
        settingsUi_.Render(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());
    }
}

void Application::Shutdown() {
    ShutdownImpl();
}

void Application::ShutdownImpl() {
    CancelPendingSceneLoads();
    settingsUi_.Shutdown();
    debugOverlay_.Shutdown();
    EngineCore::Shutdown();
}

} // namespace bgl::app
