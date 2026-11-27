/**
 * @file EngineCore.cpp
 * @brief Reusable engine runtime implementation: SDL2/WebGPU initialization, scene loading,
 *        picking, simulation update, and multi-pass HDR render sequence.
 */
#include "engine/EngineCore.hpp"
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Version.hpp"
#include "gpu/ShaderManager.hpp"
#include "scene/RayPicker.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <ranges>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::engine {

EngineCore::EngineCore() = default;

EngineCore::~EngineCore() = default;

core::Result<void> EngineCore::Initialize(const EngineCoreConfig& config) {
    const uint32_t width{config.width};
    const uint32_t height{config.height};
    BGL_LOG_INFO("Initializing {} v{} ({}x{})...", kEngineName, kEngineVersion, width, height);

#if defined(BGL_PLATFORM_NATIVE)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        return std::unexpected(std::string{"Failed to initialize SDL2: "} + SDL_GetError());
    }
    sdlInitialized_ = true;

    const std::string title{config.windowTitle.empty() ? std::string{kEngineName} : config.windowTitle};
    SDL_Window* window{SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        static_cast<int>(width), static_cast<int>(height),
                                        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)};
    if (!window) {
        const std::string err{SDL_GetError()};
        Shutdown();
        return std::unexpected("Failed to create SDL2 window: " + err);
    }
    nativeWindow_ = window;
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_EventState(SDL_DROPTEXT, SDL_ENABLE);
    BGL_LOG_INFO("Created native SDL2 window {}x{} (Drag & Drop enabled for files and URLs)", width, height);
#endif

    gpu::ContextConfig cfg{};
    cfg.width = width;
    cfg.height = height;
    cfg.nativeWindowHandle = nativeWindow_;

    if (auto res = gpuContext_.Initialize(cfg); !res) {
        Shutdown();
        return res;
    }

    if (auto res = pipelineManager_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(),
                                               gpuContext_.GetPreferredFormat(), gpuContext_.GetSampleCount());
        !res) {
        Shutdown();
        return res;
    }

    if (auto rtRes = pipelineManager_.InitializeDesktopRayTracing(gpuContext_.SupportsDesktopRayTracing(),
                                                                  gpuContext_.SupportsBindlessTextures(),
                                                                  gpuContext_.SupportsTimestampQueries());
        !rtRes) {
        BGL_LOG_WARN("Desktop RayTracing disabled: {}", rtRes.error());
    }

    if (auto res = skybox_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpu::Context::GetHdrFormat(),
                                      gpuContext_.GetSampleCount());
        !res) {
        BGL_LOG_WARN("Could not initialize Atmospheric Skybox: {}", res.error());
    }
    skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(), pipelineManager_.GetDefaultSampler());

    if (auto spriteRes = spriteRenderer_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(),
                                                    gpu::Context::GetHdrFormat(), gpuContext_.GetPreferredFormat(),
                                                    gpuContext_.GetSampleCount(), WGPUTextureFormat_Depth24Plus);
        !spriteRes) {
        BGL_LOG_WARN("Could not initialize SpriteRenderer: {}", spriteRes.error());
    }

    if (auto audioRes = soundEngine_.Initialize(); !audioRes) {
        BGL_LOG_WARN("3D audio engine disabled: {}", audioRes.error());
    }
    gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);

    if (!quadtree_) {
        core::AABB worldBounds;
        worldBounds.minBound = glm::vec3(-100.0f);
        worldBounds.maxBound = glm::vec3(100.0f);
        quadtree_ = std::make_unique<scene::Quadtree>(worldBounds);
    }

    loggedFirstFrame_ = false;
    initialized_ = true;
    running_ = true;
    BGL_LOG_INFO("bgl Engine initialized successfully.");
    return {};
}

void EngineCore::SyncCameraAndListener() noexcept {
    const float aspect =
        static_cast<float>(gpuContext_.GetWidth()) / std::max(1.0f, static_cast<float>(gpuContext_.GetHeight()));
    camera_.Update(aspect);

    const glm::vec3 forwardDelta{camera_.GetTarget() - camera_.GetPosition()};
    const float forwardLenSq = glm::dot(forwardDelta, forwardDelta);
    const glm::vec3 forwardDir =
        (forwardLenSq > 1e-8f) ? (forwardDelta / std::sqrt(forwardLenSq)) : glm::vec3{0.0f, 0.0f, -1.0f};
    soundEngine_.UpdateListener(camera_.GetPosition(), forwardDir);
}

void EngineCore::UpdateSimulation(float deltaTime) {
    const float safeDt = std::isfinite(deltaTime) ? std::clamp(deltaTime, 0.0005f, 0.25f) : 0.016f;
    lastDeltaTime_ = safeDt;
    elapsedTime_ += lastDeltaTime_;

    PollShaderHotReload(safeDt);

    if (activeScene_) {
        activeScene_->UpdateAnimation(safeDt);
        if (quadtree_ && activeScene_->transformVersion_ != lastQuadtreeTransformVersion_) {
            quadtree_->Refit();
            cachedQuadtreeNodeCount_ = quadtree_->GetNodeCount();
            lastQuadtreeTransformVersion_ = activeScene_->transformVersion_;
        }
    }

    pipelineManager_.AdvanceWeatherTime(safeDt);
    pipelineManager_.UpdateAutoLod(safeDt);
    if (dayNightCycleActive_) {
        SetTimeOfDay(timeOfDay_ + dayNightSpeed_ * safeDt);
    }

    SyncCameraAndListener();
}

void EngineCore::Render() {
    if (!running_) {
        spriteRenderer_.ResetFrame();
        return;
    }

    struct SpriteResetGuard {
        gpu::SpriteRenderer& renderer;
        ~SpriteResetGuard() { renderer.ResetFrame(); }
    } spriteResetGuard{spriteRenderer_};

    SyncCameraAndListener();

    if (!gpuContext_.PrepareFrame())
        return;

    glm::mat4 vp = camera_.GetViewProjectionMatrix();
    glm::vec3 camPos = camera_.GetPosition();
    glm::vec3 sunDir = sunDir_;

    if (activeScene_ && pipelineManager_.IsPathTracingActive()) {
        pipelineManager_.RenderPathTracePass(gpuContext_.GetCommandEncoder(), *activeScene_,
                                             gpuContext_.GetHdrResolveView(), vp, camPos, sunDir,
                                             gpuContext_.GetWidth(), gpuContext_.GetHeight());
        pipelineManager_.PreparePostProcessPass(gpuContext_.GetCommandEncoder(), gpuContext_.GetHdrResolveView(),
                                                nullptr, 1u, vp, camera_.GetProjectionMatrix(), camPos, sunDir,
                                                skybox_.GetRotation(), gpuContext_.GetWidth(), gpuContext_.GetHeight(),
                                                lastDeltaTime_, elapsedTime_);
        OnPrepareOverlay();
        WGPURenderPassEncoder swapchainPass = gpuContext_.BeginSwapchainPass();
        if (!swapchainPass) {
            gpuContext_.AbortFrame();
            return;
        }
        pipelineManager_.RenderTonemapPass(swapchainPass, gpuContext_.GetHdrResolveView(), vp, camPos, sunDir,
                                           skybox_.GetRotation(), gpuContext_.GetWidth(), gpuContext_.GetHeight());
        spriteRenderer_.Render2D(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());
        OnRenderOverlay(swapchainPass);
        gpuContext_.EndFrame(swapchainPass);
        pipelineManager_.PollRayTracingTimestamps();
        return;
    }

    // 0. Desktop RayTracing Pre-Pass (G-Buffer + RT Shadows + RTAO) or Directional Shadow Map Pre-Pass
    if (activeScene_) {
        pipelineManager_.EnsureOpaqueCopyTexture(gpuContext_.GetWidth(), gpuContext_.GetHeight());
        pipelineManager_.RenderRayTracingPrePass(
            gpuContext_.GetCommandEncoder(), *activeScene_, vp, camPos, sunDir, quadtree_.get(),
            camera_.GetViewMatrix(), camera_.GetProjectionMatrix(), gpuContext_.GetWidth(), gpuContext_.GetHeight(),
            camera_.GetNearPlane(), camera_.GetFarPlane());
        if (!pipelineManager_.IsRtShadowsActive()) {
            pipelineManager_.RenderShadowPass(gpuContext_.GetCommandEncoder(), *activeScene_, sunDir, quadtree_.get());
        }
    }

    WGPURenderPassEncoder hdrPass = gpuContext_.BeginFrame();
    if (!hdrPass) {
        gpuContext_.AbortFrame();
        return;
    }

    if (activeScene_) {
        // 1. Hardware 4x MSAA Opaque/Masked PBR Pass (RGBA16Float + 16x Aniso + Quadtree Frustum Culling)
        pipelineManager_.RenderOpaqueScene(hdrPass, *activeScene_, vp, camPos, wireframeMode_, quadtree_.get(),
                                           camera_.GetViewMatrix(), camera_.GetProjectionMatrix(),
                                           gpuContext_.GetWidth(), gpuContext_.GetHeight(), camera_.GetNearPlane(),
                                           camera_.GetFarPlane());
        if (!loggedFirstFrame_) {
            loggedFirstFrame_ = true;
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
    if (!transparentPass) {
        gpuContext_.AbortFrame();
        return;
    }

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

    // 5. Render depth-tested / additive HDR 3D billboards, then end HDR MSAA pass for RT reflections, outline &
    // post-process
    spriteRenderer_.Render3D(transparentPass, vp, camPos, camera_.GetViewMatrix());
    gpuContext_.FinishHdrPass(transparentPass);

    if (activeScene_) {
        pipelineManager_.RenderRayTracingPostOpaque(gpuContext_.GetCommandEncoder(), gpuContext_.GetHdrResolveView(),
                                                    vp, camPos, sunDir, gpuContext_.GetWidth(),
                                                    gpuContext_.GetHeight());
    }

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

    OnPrepareOverlay();

    // 7. Open Swapchain pass (1x), finish FXAA / Tonemap, and draw HUD + ImmediateUI crisp on top
    WGPURenderPassEncoder swapchainPass = gpuContext_.BeginSwapchainPass();
    if (!swapchainPass) {
        gpuContext_.AbortFrame();
        return;
    }

    pipelineManager_.RenderTonemapPass(swapchainPass, gpuContext_.GetHdrResolveView(),
                                       camera_.GetViewProjectionMatrix(), camera_.GetPosition(), sunDir,
                                       skybox_.GetRotation(), gpuContext_.GetWidth(), gpuContext_.GetHeight());

    spriteRenderer_.Render2D(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());
    OnRenderOverlay(swapchainPass);

    gpuContext_.EndFrame(swapchainPass);
    pipelineManager_.PollRayTracingTimestamps();
}

void EngineCore::Shutdown() {
    const bool hadActiveState = initialized_ || sdlInitialized_ || nativeWindow_ != nullptr ||
                                gpuContext_.IsInitialized() || soundEngine_.IsInitialized() || activeScene_ != nullptr;

    activeScene_.reset();
    quadtree_.reset();
    gltfLoader_.reset();
    assetManager_.Clear();
    spriteRenderer_.Shutdown();
    skybox_.Shutdown();
    pipelineManager_.Shutdown();
    soundEngine_.Shutdown();
    gpuContext_.Shutdown();
#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_) {
        SDL_DestroyWindow(nativeWindow_);
        nativeWindow_ = nullptr;
    }
    if (sdlInitialized_) {
        SDL_Quit();
        sdlInitialized_ = false;
    }
#endif
    initialized_ = false;
    running_ = false;
    loggedFirstFrame_ = false;
    if (hadActiveState) {
        BGL_LOG_INFO("bgl Engine shutdown completed cleanly.");
    }
}

void EngineCore::PollShaderHotReload(float deltaTime) {
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
    if (std::ranges::find(modified, std::string{"sprite.wgsl"}) != modified.end()) {
        if (auto res = spriteRenderer_.ReloadShader(); !res) {
            BGL_LOG_ERROR("WGSL hot-reload failed for sprite.wgsl: {}", res.error());
        } else {
            BGL_LOG_INFO("WGSL hot-reloaded: sprite.wgsl");
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

} // namespace bgl::engine
