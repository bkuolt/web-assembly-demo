/**
 * @file Application.cpp
 * @brief Core application loop, SDL2 window management, WebGPU pipeline and scene orchestration.
 */
#include "app/Application.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include <array>
#include <filesystem>
#include <algorithm>
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

    SDL_Window* window{SDL_CreateWindow(
        std::string{kEngineName}.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(width), static_cast<int>(height),
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    )};
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

    if (auto res = pipelineManager_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpuContext_.GetPreferredFormat(), gpuContext_.GetSampleCount()); !res) {
        return res;
    }
    pipelineManager_.SetBumpMappingEnabled(config.enableBumpMapping);
    pipelineManager_.SetMipmappingEnabled(config.enableMipmapping);
    pipelineManager_.SetShadowsEnabled(config.enableShadows);
    pipelineManager_.SetReflectionsEnabled(config.enableReflections);
    pipelineManager_.SetAmbientOcclusionEnabled(config.enableAmbientOcclusion);
    pipelineManager_.SetOcclusionCullingEnabled(config.enableOcclusionCulling);

    if (auto res = skybox_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpu::Context::GetHdrFormat(), gpuContext_.GetSampleCount()); !res) {
        BGL_LOG_WARN("Could not initialize Atmospheric Skybox: {}", res.error());
    }
    skybox_.SetEnabled(config.enableSkybox);
    skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(), pipelineManager_.GetDefaultSampler());

    if (auto res = debugOverlay_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpuContext_.GetPreferredFormat(), 1); !res) {
        BGL_LOG_WARN("Could not initialize Debug Overlay: {}", res.error());
    }
    debugOverlay_.SetVisible(config.enableDebugOverlay);

    soundEngine_.Initialize();
    gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);

    // Resolve model asset path from config or auto-detect Sponza.glb (primary) / ToyCar.glb (fallback)
    std::string assetPathToLoad{config.assetPath};
    if (assetPathToLoad.empty()) {
        static constexpr std::array<std::string_view, 4> kSearchPaths{
            "assets/Sponza.glb",
            "../assets/Sponza.glb",
            "assets/ToyCar.glb",
            "../assets/ToyCar.glb"
        };
        const auto it = std::ranges::find_if(kSearchPaths, [](std::string_view path) {
            return std::filesystem::exists(path);
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

void Application::Update(float deltaTime) {
    if (!running_) return;

#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running_ = false;
                return;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    gpuContext_.Resize(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
                }
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running_ = false;
                    return;
                } else if (event.key.keysym.sym == SDLK_b) {
                    ToggleBoundingBoxes();
                    BGL_LOG_INFO("Toggled 3D Bounding Boxes & AABBs visibility: {}", showBoundingBoxes_);
                } else if (event.key.keysym.sym == SDLK_q) {
                    ToggleQuadtreeBounds();
                    BGL_LOG_INFO("Toggled 3D Quadtree Spatial Bounds visibility: {}", showQuadtreeBounds_);
                } else if (event.key.keysym.sym == SDLK_o || event.key.keysym.sym == SDLK_F3) {
                    ToggleDebugOverlay();
                    BGL_LOG_INFO("Toggled In-Engine Debug Overlay visibility: {}", debugOverlay_.IsVisible());
                } else if (event.key.keysym.sym == SDLK_v) {
                    ToggleBumpMapping();
                    BGL_LOG_INFO("Toggled Bump & Normal Mapping: {}", pipelineManager_.IsBumpMappingEnabled());
                } else if (event.key.keysym.sym == SDLK_l) {
                    ToggleMipmapping();
                    BGL_LOG_INFO("Toggled Texture Mipmapping (16x Anisotropic Trilinear vs Mip-0): {}", pipelineManager_.IsMipmappingEnabled());
                } else if (event.key.keysym.sym == SDLK_m) {
                    ToggleShadows();
                    BGL_LOG_INFO("Toggled Directional Shadow Mapping (PCF): {}", pipelineManager_.IsShadowsEnabled());
                } else if (event.key.keysym.sym == SDLK_r) {
                    ToggleReflections();
                    BGL_LOG_INFO("Toggled Environment Cubemap Reflections (IBL): {}", pipelineManager_.IsReflectionsEnabled());
                } else if (event.key.keysym.sym == SDLK_u) {
                    ToggleAmbientOcclusion();
                    BGL_LOG_INFO("Toggled Ambient Occlusion & Micro-Cavities: {}", pipelineManager_.IsAmbientOcclusionEnabled());
                } else if (event.key.keysym.sym == SDLK_c) {
                    ToggleOcclusionCulling();
                    BGL_LOG_INFO("Toggled Quadtree Hierarchical Occlusion Culling: {}", pipelineManager_.IsOcclusionCullingEnabled());
                } else if (event.key.keysym.sym == SDLK_t) {
                    CycleTonemapper();
                    BGL_LOG_INFO("Switched Tonemapper: {}", pipelineManager_.GetToneMapOperatorName(pipelineManager_.GetToneMapOperator()));
                } else if (event.key.keysym.sym == SDLK_e) {
                    CycleEnvironmentPreset();
                    BGL_LOG_INFO("Switched Environment Preset: {}", pipelineManager_.GetEnvironmentPresetName(pipelineManager_.GetEnvironmentPreset()));
                } else if (event.key.keysym.sym == SDLK_k) {
                    CycleSkyboxMode();
                    BGL_LOG_INFO("Switched Skybox Mode: {}", skybox_.GetModeName(skybox_.GetMode()));
                } else if (event.key.keysym.sym == SDLK_p) {
                    ToggleParallaxOcclusion();
                    BGL_LOG_INFO("Toggled Parallax Occlusion Mapping (POM): {}", pipelineManager_.IsParallaxEnabled());
                } else if (event.key.keysym.sym == SDLK_j) {
                    ToggleLensFlare();
                    BGL_LOG_INFO("Toggled Directional Sun Lens Flare: {}", pipelineManager_.IsLensFlareEnabled());
                } else if (event.key.keysym.sym == SDLK_g) {
                    pipelineManager_.ToggleSSAO();
                    BGL_LOG_INFO("Toggled Screen-Space Ambient Occlusion (SSAO/GTAO): {}", pipelineManager_.IsSsaoEnabled());
                } else if (event.key.keysym.sym == SDLK_f) {
                    pipelineManager_.ToggleBloom();
                    BGL_LOG_INFO("Toggled Emissive HDR Bloom: {}", pipelineManager_.IsBloomEnabled());
                } else if (event.key.keysym.sym == SDLK_PLUS || event.key.keysym.sym == SDLK_KP_PLUS || event.key.keysym.sym == SDLK_EQUALS) {
                    if (event.key.keysym.mod & KMOD_SHIFT) {
                        AdjustExposure(0.15f);
                        BGL_LOG_INFO("Exposure increased: {:.2f}", pipelineManager_.GetExposure());
                    } else {
                        const float zoomStep = camera_.GetDistance() * 0.15f;
                        camera_.Zoom(zoomStep);
                    }
                } else if (event.key.keysym.sym == SDLK_MINUS || event.key.keysym.sym == SDLK_KP_MINUS) {
                    if (event.key.keysym.mod & KMOD_SHIFT) {
                        AdjustExposure(-0.15f);
                        BGL_LOG_INFO("Exposure decreased: {:.2f}", pipelineManager_.GetExposure());
                    } else {
                        const float zoomStep = camera_.GetDistance() * 0.15f;
                        camera_.Zoom(-zoomStep);
                    }
                } else if (event.key.keysym.sym == SDLK_LEFTBRACKET) {
                    RotateEnvironment(-0.0872665f); // -5 degrees
                } else if (event.key.keysym.sym == SDLK_RIGHTBRACKET) {
                    RotateEnvironment(0.0872665f);  // +5 degrees
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       (event.button.button == SDL_BUTTON_MIDDLE ||
                        (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2))) {
                if (activeScene_ && gpuContext_.GetWidth() > 0 && gpuContext_.GetHeight() > 0) {
                    const float ndcX{(2.0f * static_cast<float>(event.button.x)) / static_cast<float>(gpuContext_.GetWidth()) - 1.0f};
                    const float ndcY{1.0f - (2.0f * static_cast<float>(event.button.y)) / static_cast<float>(gpuContext_.GetHeight())};
                    const glm::mat4 invVP{glm::inverse(camera_.GetViewProjectionMatrix())};
                    const glm::vec4 nearPt{invVP * glm::vec4{ndcX, ndcY, 0.0f, 1.0f}};
                    const glm::vec4 farPt{invVP * glm::vec4{ndcX, ndcY, 1.0f, 1.0f}};
                    const glm::vec3 worldNear{ glm::vec3{nearPt} / nearPt.w };
                    const glm::vec3 worldFar{ glm::vec3{farPt} / farPt.w };
                    const core::Ray pickRay{
                        .origin = camera_.GetPosition(),
                        .direction = glm::normalize(worldFar - worldNear)
                    };
                    if (auto hit = scene::RayPicker::Pick(pickRay, activeScene_->rootNodes)) {
                        BGL_LOG_INFO("[RayPicker] Picked Node '{}' (Triangle #{}) at distance {:.3f}m, worldPos=[{:.3f}, {:.3f}, {:.3f}]",
                                     hit->hitNode ? hit->hitNode->name : "Unnamed",
                                     hit->triangleIndex, hit->distance,
                                     hit->hitPoint.x, hit->hitPoint.y, hit->hitPoint.z);
                    }
                }
            } else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
                camera_.Orbit(event.motion.xrel * 0.005f, -event.motion.yrel * 0.005f);
            } else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_RMASK)) {
                camera_.Pan(-event.motion.xrel * 0.0005f * camera_.GetDistance(), event.motion.yrel * 0.0005f * camera_.GetDistance());
            } else if (event.type == SDL_MOUSEWHEEL) {
                float zoomStep = camera_.GetDistance() * 0.12f;
                camera_.Zoom(event.wheel.y * zoomStep);
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

    gamepad_.Update();
    const auto& gp = gamepad_.GetState();

    if (gp.connected) {
        camera_.Pan(gp.leftStick.x * deltaTime * camera_.GetDistance(), gp.leftStick.y * deltaTime * camera_.GetDistance());
        camera_.Orbit(gp.rightStick.x * deltaTime * 2.0f, gp.rightStick.y * deltaTime * 2.0f);
        camera_.Zoom((gp.rightTrigger - gp.leftTrigger) * deltaTime * camera_.GetDistance());
    }

    float aspect = static_cast<float>(gpuContext_.GetWidth()) / std::max(1.0f, static_cast<float>(gpuContext_.GetHeight()));
    camera_.Update(aspect);
    soundEngine_.UpdateListener(camera_.GetPosition(), glm::normalize(camera_.GetTarget() - camera_.GetPosition()));

    // Collect telemetry stats for the in-engine debug HUD overlay
    ui::OverlayStats stats;
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
    if (!running_) return;

    if (!gpuContext_.PrepareFrame()) return;

    glm::mat4 vp = camera_.GetViewProjectionMatrix();
    glm::vec3 camPos = camera_.GetPosition();
    glm::vec3 sunDir = glm::normalize(glm::vec3(0.35f, 1.0f, 0.25f));

    // 0. Directional Shadow Map Depth Pre-Pass (before main 4x MSAA color render pass)
    if (activeScene_) {
        pipelineManager_.EnsureOpaqueCopyTexture(gpuContext_.GetWidth(), gpuContext_.GetHeight());
        pipelineManager_.RenderShadowPass(gpuContext_.GetCommandEncoder(), *activeScene_, sunDir, quadtree_.get());
    }

    WGPURenderPassEncoder hdrPass = gpuContext_.BeginFrame();
    if (!hdrPass) return;

    if (activeScene_) {
        // 1. Hardware 4x MSAA PBR Scene Rendering (RGBA16Float + 16x Aniso + Quadtree Frustum Culling)
        pipelineManager_.RenderScene(
            hdrPass,
            *activeScene_,
            vp,
            camPos,
            false,
            quadtree_.get(),
            camera_.GetViewMatrix(),
            camera_.GetProjectionMatrix(),
            gpuContext_.GetWidth(),
            gpuContext_.GetHeight(),
            camera_.GetNearPlane(),
            camera_.GetFarPlane()
        );
    }

    // 2. Procedural Atmospheric Skybox (far-plane depth Z=1.0 test against geometry, zero overdraw)
    skybox_.Render(hdrPass, vp, camPos, sunDir);

    if (activeScene_) {
        // 3. Batch & Flush 3D Wireframe Bounding Boxes (Gold/Cyan) and Quadtree Bounds (Emerald) in a single draw call
        if (showBoundingBoxes_) {
            pipelineManager_.BatchSceneAABBs(*activeScene_, true, true);
        }
        if (showQuadtreeBounds_ && quadtree_) {
            pipelineManager_.BatchQuadtreeBounds(*quadtree_);
        }
        if (showBoundingBoxes_ || (showQuadtreeBounds_ && quadtree_)) {
            pipelineManager_.FlushAABBLines(hdrPass, vp);
        }
    }

    // 4. Resolve HDR RGBA16F (MSAA -> 1x) and begin Swapchain Post-Processing Pass (Surface format)
    WGPURenderPassEncoder swapchainPass = gpuContext_.BeginPostProcessPass(hdrPass);
    if (!swapchainPass) return;

    // 5. Fullscreen HDR Tonemapping (Khronos PBR Neutral / AgX / ACES) + Real-time Sun Lens Flares + Exposure
    pipelineManager_.RenderTonemapPass(
        swapchainPass,
        gpuContext_.GetHdrResolveView(),
        camera_.GetViewProjectionMatrix(),
        camera_.GetPosition(),
        sunDir,
        skybox_.GetRotation(),
        gpuContext_.GetWidth(),
        gpuContext_.GetHeight()
    );

    // 6. In-Engine 2D Debug Overlay (FPS, HarfBuzz text, telemetry HUD) onto swapchain
    debugOverlay_.Render(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());

    gpuContext_.EndFrame(swapchainPass);
}

void Application::ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri) {
    if (!scene) return;
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

    BGL_LOG_INFO("Loading glTF/GLB asset from {} '{}'...",
                 core::NetworkFetcher::IsUrl(sanitizedUri) ? "URL" : "path", sanitizedUri);
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
    if (running_ || nativeWindow_) {
        activeScene_.reset();
        quadtree_.reset();
        gltfLoader_.reset();
        skybox_.Shutdown();
        pipelineManager_.Shutdown();
        soundEngine_.Shutdown();
        gpuContext_.Shutdown();
#if defined(BGL_PLATFORM_NATIVE)
        if (nativeWindow_) {
            SDL_DestroyWindow(static_cast<SDL_Window*>(nativeWindow_));
            nativeWindow_ = nullptr;
            SDL_Quit();
        }
#endif
        running_ = false;
        BGL_LOG_INFO("bgl Engine shutdown completed cleanly.");
    }
}

void Application::CacheSceneMetrics() {
    totalPrimitives_ = 0;
    totalVertices_ = 0;
    totalIndices_ = 0;
    if (!activeScene_) return;

    size_t& primCount{totalPrimitives_};
    size_t& vertCount{totalVertices_};
    size_t& idxCount{totalIndices_};
    auto countStats = [&](this auto&& self, const scene::SceneNode& node) -> void {
        for (const auto& prim : node.primitives) {
            primCount++;
            vertCount += prim.vertices.size();
            idxCount += prim.indices.size();
        }
        for (const auto& child : node.children) {
            if (child) self(*child);
        }
    };

    for (const auto& rootNode : activeScene_->rootNodes) {
        if (rootNode) countStats(*rootNode);
    }
    BGL_LOG_INFO("Scene '{}' metrics cached: {} primitives, {} vertices, {} indices",
                 currentModelName_, totalPrimitives_, totalVertices_, totalIndices_);
}

} // namespace bgl::app
