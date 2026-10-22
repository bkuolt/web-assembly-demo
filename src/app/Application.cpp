/**
 * @file Application.cpp
 * @brief Core application loop, SDL2 window management, WebGPU pipeline and scene orchestration.
 */
#include "app/Application.hpp"
#include "core/NetworkFetcher.hpp"
#include "core/Log.hpp"
#include <filesystem>
#include <algorithm>

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
    BGL_LOG_INFO("Initializing WebAssembly an WebGPU Demo v0.7.0 ({}x{})...", width, height);

    showBoundingBoxes_ = config.enableBoundingBoxes;
    showQuadtreeBounds_ = config.enableQuadtreeBounds;

#if defined(BGL_PLATFORM_NATIVE)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        return std::unexpected(std::string{"Failed to initialize SDL2: "} + SDL_GetError());
    }

    SDL_Window* window{SDL_CreateWindow(
        "WebAssembly an WebGPU Demo",
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
    pipelineManager_.SetOcclusionCullingEnabled(config.enableOcclusionCulling);

    if (auto res = skybox_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpuContext_.GetPreferredFormat(), gpuContext_.GetSampleCount()); !res) {
        BGL_LOG_WARN("Could not initialize Atmospheric Skybox: {}", res.error());
    }
    skybox_.SetEnabled(config.enableSkybox);

    if (auto res = debugOverlay_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpuContext_.GetPreferredFormat(), gpuContext_.GetSampleCount()); !res) {
        BGL_LOG_WARN("Could not initialize Debug Overlay: {}", res.error());
    }
    debugOverlay_.SetVisible(config.enableDebugOverlay);

    soundEngine_.Initialize();
    gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);

    // Resolve model asset path from config or auto-detect Sponza.glb (primary) / ToyCar.glb (fallback)
    std::string assetPathToLoad = config.assetPath;
    if (assetPathToLoad.empty()) {
        const std::vector<std::string> searchPaths = {
            "assets/Sponza.glb",
            "../assets/Sponza.glb",
            "assets/ToyCar.glb",
            "../assets/ToyCar.glb"
        };
        for (const auto& path : searchPaths) {
            if (std::filesystem::exists(path)) {
                assetPathToLoad = path;
                break;
            }
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
                } else if (event.key.keysym.sym == SDLK_k) {
                    ToggleSkybox();
                    BGL_LOG_INFO("Toggled Atmospheric Skybox visibility: {}", skybox_.IsEnabled());
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
                } else if (event.key.keysym.sym == SDLK_c) {
                    ToggleOcclusionCulling();
                    BGL_LOG_INFO("Toggled Quadtree Hierarchical Occlusion Culling: {}", pipelineManager_.IsOcclusionCullingEnabled());
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
        pipelineManager_.RenderShadowPass(gpuContext_.GetCommandEncoder(), *activeScene_, sunDir, quadtree_.get());
    }

    WGPURenderPassEncoder pass = gpuContext_.BeginFrame();
    if (!pass) return;

    // 1. Procedural Atmospheric Skybox (far-plane depth clamped behind geometry)
    skybox_.Render(pass, vp, camPos, sunDir);

    if (activeScene_) {
        // 2. Hardware 4x MSAA PBR Scene Rendering (16x Aniso + Quadtree Frustum Culling)
        pipelineManager_.RenderScene(pass, *activeScene_, vp, camPos, false, quadtree_.get());

        // 3. 3D Wireframe Bounding Boxes / AABBs (Gold for scene, Cyan for primitives)
        if (showBoundingBoxes_) {
            pipelineManager_.RenderSceneAABBs(pass, *activeScene_, vp, true, true);
        }

        // 4. 3D Quadtree Spatial Partition Bounds (Emerald green for ground quadrants)
        if (showQuadtreeBounds_ && quadtree_) {
            pipelineManager_.RenderQuadtreeBounds(pass, *quadtree_, vp);
        }
    }

    // 5. In-Engine 2D Debug Overlay (FPS, HarfBuzz text, telemetry HUD)
    debugOverlay_.Render(pass, gpuContext_.GetWidth(), gpuContext_.GetHeight());

    gpuContext_.EndFrame(pass);
}

void Application::ActivateScene(std::shared_ptr<scene::Scene> scene, std::string_view sourceUri) {
    if (!scene) return;
    activeScene_ = std::move(scene);
    currentModelName_ = core::NetworkFetcher::ExtractDisplayName(sourceUri);
    CacheSceneMetrics();
    pipelineManager_.PrepareScene(*activeScene_);

    const glm::vec3 center{activeScene_->sceneAABB.Center()};
    const float radius{glm::length(activeScene_->sceneAABB.Extents()) * 0.5f};

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

core::Result<void> Application::LoadSceneFromUri(std::string_view uri) {
    const std::string sanitizedUri{core::NetworkFetcher::SanitizeUri(uri)};
    if (sanitizedUri.empty()) {
        return std::unexpected("Cannot load empty asset URI");
    }
    if (!gltfLoader_) {
        gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);
    }

    BGL_LOG_INFO("Loading glTF/GLB asset from {} '{}'...",
                 core::NetworkFetcher::IsUrl(sanitizedUri) ? "URL" : "path", sanitizedUri);
    auto sceneRes = gltfLoader_->LoadFromFile(sanitizedUri);
    if (!sceneRes) {
        return std::unexpected(sceneRes.error());
    }
    ActivateScene(*sceneRes, sanitizedUri);
    return {};
}

void Application::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped buffer '{}' ({} bytes)", filename, data.size());
    if (!gltfLoader_) {
        gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);
    }
    auto res = gltfLoader_->LoadFromMemory(data, filename);
    if (res) {
        ActivateScene(*res, filename);
        BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", filename);
    } else {
        BGL_LOG_ERROR("Failed to load dropped glTF buffer '{}': {}", filename, res.error());
    }
}

void Application::Shutdown() {
    if (running_ || nativeWindow_) {
        soundEngine_.Shutdown();
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

    auto countStats = [&](auto& self, const scene::SceneNode& node) -> void {
        for (const auto& prim : node.primitives) {
            totalPrimitives_++;
            totalVertices_ += prim.vertices.size();
            totalIndices_ += prim.indices.size();
        }
        for (const auto& child : node.children) {
            if (child) self(self, *child);
        }
    };

    for (const auto& rootNode : activeScene_->rootNodes) {
        if (rootNode) countStats(countStats, *rootNode);
    }
    BGL_LOG_INFO("Scene '{}' metrics cached: {} primitives, {} vertices, {} indices",
                 currentModelName_, totalPrimitives_, totalVertices_, totalIndices_);
}

} // namespace bgl::app
