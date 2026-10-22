/**
 * @file Application.cpp
 * @brief Core application loop, SDL2 window management, WebGPU pipeline and scene orchestration.
 */
#include "app/Application.hpp"
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
    core::InitLogging();
    BGL_LOG_INFO("Initializing bgl Engine v0.6.2...");

#if defined(BGL_PLATFORM_NATIVE)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        return std::unexpected(std::string("Failed to initialize SDL2: ") + SDL_GetError());
    }

    SDL_Window* window = SDL_CreateWindow(
        "bgl_viewer - C++23 WebGPU & glTF 2.1",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(width), static_cast<int>(height),
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (window) {
        nativeWindow_ = window;
        BGL_LOG_INFO("Created native SDL2 window {}x{}", width, height);
    }
#endif

    gpu::ContextConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.nativeWindowHandle = nativeWindow_;

    if (auto res = gpuContext_.Initialize(cfg); !res) {
        return res;
    }

    if (auto res = pipelineManager_.Initialize(gpuContext_.GetDevice(), gpuContext_.GetQueue(), gpuContext_.GetPreferredFormat()); !res) {
        return res;
    }

    soundEngine_.Initialize();
    gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);

    // Auto-load ToyCar.glb if present
    std::string toyCarPath = "";
    if (std::filesystem::exists("assets/ToyCar.glb")) {
        toyCarPath = "assets/ToyCar.glb";
    } else if (std::filesystem::exists("../assets/ToyCar.glb")) {
        toyCarPath = "../assets/ToyCar.glb";
    }

    if (!toyCarPath.empty()) {
        BGL_LOG_INFO("Found default asset '{}', loading scene...", toyCarPath);
        auto sceneRes = gltfLoader_->LoadFromFile(toyCarPath);
        if (sceneRes) {
            activeScene_ = *sceneRes;
            pipelineManager_.PrepareScene(*activeScene_);

            // Frame camera around ToyCar world AABB
            glm::vec3 center = activeScene_->sceneAABB.Center();
            float radius = glm::length(activeScene_->sceneAABB.Extents()) * 0.5f;
            float targetDistance = std::max(radius * 3.0f, 0.15f);
            camera_ = core::Camera(center, targetDistance);
            camera_.Orbit(0.8f, 0.4f); // 3/4 isometric perspective
            BGL_LOG_INFO("ToyCar.glb loaded: center=[{:.4f}, {:.4f}, {:.4f}], radius={:.4f}, camDistance={:.4f}",
                         center.x, center.y, center.z, radius, targetDistance);
        } else {
            BGL_LOG_WARN("Could not load {}: {}", toyCarPath, sceneRes.error());
        }
    }

    core::AABB worldBounds;
    worldBounds.minBound = glm::vec3(-100.0f);
    worldBounds.maxBound = glm::vec3(100.0f);
    quadtree_ = std::make_unique<scene::Quadtree>(worldBounds);
    if (activeScene_) {
        for (const auto& rootNode : activeScene_->rootNodes) {
            quadtree_->Insert(rootNode);
        }
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
                    BGL_LOG_INFO("Toggled Bounding Box visibility: {}", showBoundingBoxes_);
                }
            } else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
                camera_.Orbit(event.motion.xrel * 0.005f, -event.motion.yrel * 0.005f);
            } else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_RMASK)) {
                camera_.Pan(-event.motion.xrel * 0.0005f * camera_.GetDistance(), event.motion.yrel * 0.0005f * camera_.GetDistance());
            } else if (event.type == SDL_MOUSEWHEEL) {
                float zoomStep = camera_.GetDistance() * 0.12f;
                camera_.Zoom(event.wheel.y * zoomStep);
            } else if (event.type == SDL_DROPFILE) {
                char* droppedFile = event.drop.file;
                if (droppedFile) {
                    BGL_LOG_INFO("File dropped via OS: {}", droppedFile);
                    auto sceneRes = gltfLoader_->LoadFromFile(droppedFile);
                    if (sceneRes) {
                        activeScene_ = *sceneRes;
                        pipelineManager_.PrepareScene(*activeScene_);
                        glm::vec3 center = activeScene_->sceneAABB.Center();
                        float radius = glm::length(activeScene_->sceneAABB.Extents()) * 0.5f;
                        float targetDistance = std::max(radius * 3.0f, 0.15f);
                        camera_ = core::Camera(center, targetDistance);
                        camera_.Orbit(0.8f, 0.4f);
                        BGL_LOG_INFO("Successfully loaded dropped model: {}", droppedFile);
                    }
                    SDL_free(droppedFile);
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

    camera_.Update(16.0f / 9.0f);
    soundEngine_.UpdateListener(camera_.GetPosition(), glm::normalize(camera_.GetTarget() - camera_.GetPosition()));
}

void Application::Render() {
    if (!running_) return;

    WGPURenderPassEncoder pass = gpuContext_.BeginFrame();
    if (!pass) return;

    glm::mat4 vp = camera_.GetViewProjectionMatrix();
    skybox_.Render(vp);

    if (activeScene_) {
        pipelineManager_.RenderScene(pass, *activeScene_, vp, camera_.GetPosition(), false);

        if (showBoundingBoxes_) {
            pipelineManager_.RenderScene(pass, *activeScene_, vp, camera_.GetPosition(), true);
        }
    }

    gpuContext_.EndFrame(pass);
}

void Application::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped file '{}' ({} bytes)", filename, data.size());
    if (filename.ends_with(".glb") || filename.ends_with(".gltf")) {
        auto res = gltfLoader_->LoadFromMemory(data, filename);
        if (res) {
            activeScene_ = *res;
            pipelineManager_.PrepareScene(*activeScene_);
            glm::vec3 center = activeScene_->sceneAABB.Center();
            float radius = glm::length(activeScene_->sceneAABB.Extents()) * 0.5f;
            float targetDistance = std::max(radius * 3.0f, 0.15f);
            camera_ = core::Camera(center, targetDistance);
            camera_.Orbit(0.8f, 0.4f);
            BGL_LOG_INFO("Successfully activated dropped glTF model '{}'", filename);
        } else {
            BGL_LOG_ERROR("Failed to load dropped glTF file: {}", res.error());
        }
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

} // namespace bgl::app
