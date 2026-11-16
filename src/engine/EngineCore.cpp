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

    const std::string title{config.windowTitle.empty() ? std::string{kEngineName} : config.windowTitle};
    SDL_Window* window{SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        static_cast<int>(width), static_cast<int>(height),
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

    if (auto res = skybox_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpu::Context::GetHdrFormat(),
                                      gpuContext_.GetSampleCount());
        !res) {
        BGL_LOG_WARN("Could not initialize Atmospheric Skybox: {}", res.error());
    }
    skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(), pipelineManager_.GetDefaultSampler());

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

    running_ = true;
    BGL_LOG_INFO("bgl Engine initialized successfully.");
    return {};
}

void EngineCore::UpdateSimulation(float deltaTime) {
    lastDeltaTime_ = std::clamp(deltaTime, 0.0005f, 0.25f);
    elapsedTime_ += lastDeltaTime_;

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
}

void EngineCore::Render() {
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

    OnPrepareOverlay();

    // 7. Open Swapchain pass (1x), finish FXAA / Tonemap, and draw HUD + ImmediateUI crisp on top
    WGPURenderPassEncoder swapchainPass = gpuContext_.BeginSwapchainPass();
    if (!swapchainPass)
        return;

    pipelineManager_.RenderTonemapPass(swapchainPass, gpuContext_.GetHdrResolveView(),
                                       camera_.GetViewProjectionMatrix(), camera_.GetPosition(), sunDir,
                                       skybox_.GetRotation(), gpuContext_.GetWidth(), gpuContext_.GetHeight());

    OnRenderOverlay(swapchainPass);

    gpuContext_.EndFrame(swapchainPass);
}

void EngineCore::Shutdown() {
    activeScene_.reset();
    quadtree_.reset();
    gltfLoader_.reset();
    assetManager_.Clear();
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

void EngineCore::ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri) {
    if (!scene) {
        return;
    }
    ClearSelection();
    activeCameraIndex_ = -1;
    activeScene_ = std::move(scene);
    currentModelName_ = core::NetworkFetcher::ExtractDisplayName(sourceUri);
    OnSceneActivated(sourceUri);
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

core::Result<void> EngineCore::LoadScene(std::span<const uint8_t> data, std::string_view baseUri) {
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

core::Result<void> EngineCore::LoadSceneFromUri(std::string_view uri) {
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

void EngineCore::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped buffer '{}' ({} bytes)", filename, data.size());
    if (auto res = LoadScene(data, filename); res) {
        BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", filename);
    } else {
        BGL_LOG_ERROR("Failed to load dropped glTF buffer '{}': {}", filename, res.error());
    }
}

void EngineCore::CacheSceneMetrics() {
    totalPrimitives_ = 0;
    totalVertices_ = 0;
    totalIndices_ = 0;
    if (!activeScene_) {
        return;
    }

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
            if (child) {
                self(*child);
            }
        }
    };

    for (const auto& rootNode : activeScene_->rootNodes) {
        if (rootNode) {
            countStats(*rootNode);
        }
    }
    BGL_LOG_INFO("Scene '{}' metrics cached: {} primitives, {} vertices, {} indices", currentModelName_,
                 totalPrimitives_, totalVertices_, totalIndices_);
}

void EngineCore::FocusSelection() noexcept {
    if (selectedNode_ && selectedNode_->worldAABB.minBound.x <= selectedNode_->worldAABB.maxBound.x) {
        camera_.FocusOnBounds(selectedNode_->worldAABB);
    } else if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void EngineCore::FrameAll() noexcept {
    if (activeScene_) {
        camera_.FocusOnBounds(activeScene_->sceneAABB);
    }
}

void EngineCore::SelectNodeByIndex(int index) noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->allNodes.size()) {
        ClearSelection();
        return;
    }
    SelectNode(activeScene_->allNodes[index], index);
}

void EngineCore::SelectNode(std::shared_ptr<scene::SceneNode> node, int index) noexcept {
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

int EngineCore::PickNodeAt(float screenX, float screenY, float screenWidth, float screenHeight) {
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

void EngineCore::SelectCamera(int cameraIndex) noexcept {
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

void EngineCore::CycleCamera() noexcept {
    if (!activeScene_ || activeScene_->cameras.empty()) {
        return;
    }
    int next = activeCameraIndex_ + 1;
    if (static_cast<size_t>(next) >= activeScene_->cameras.size()) {
        next = -1;
    }
    SelectCamera(next);
}

int EngineCore::GetCameraCount() const noexcept {
    return activeScene_ ? static_cast<int>(activeScene_->cameras.size()) : 0;
}

std::string_view EngineCore::GetCameraName(int index) const noexcept {
    if (!activeScene_ || index < 0 || static_cast<size_t>(index) >= activeScene_->cameras.size()) {
        return "";
    }
    return activeScene_->cameras[index].name;
}

void EngineCore::SetTimeOfDay(float hour) noexcept {
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

void EngineCore::SetSunAzimuthElevation(float azimuthRad, float elevationRad) noexcept {
    sunAzimuth_ = azimuthRad;
    sunElevation_ = std::clamp(elevationRad, 0.02f, 1.57f);
    UpdateSunDirection();
}

void EngineCore::UpdateSunDirection() noexcept {
    const float cosEl = std::cos(sunElevation_);
    sunDir_ = glm::normalize(
        glm::vec3{cosEl * std::sin(sunAzimuth_), std::sin(sunElevation_), cosEl * std::cos(sunAzimuth_)});
}

void EngineCore::SetKtxCompressionTarget(scene::KtxTranscodeTarget target) {
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
