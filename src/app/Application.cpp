/**
 * @file Application.cpp
 * @brief Implementation of Application runtime loop.
 */
#include "app/Application.hpp"
#include "core/Log.hpp"

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::app {

Application::Application() = default;

Application::~Application() {
    Shutdown();
}

core::Result<void> Application::Initialize(uint32_t width, uint32_t height) {
    core::InitLogging("info");
    BGL_LOG_INFO("Initializing bgl Engine v0.6.0...");

#if defined(BGL_PLATFORM_NATIVE)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        BGL_LOG_WARN("SDL_Init warning: {}", SDL_GetError());
    } else {
        SDL_Window* window = SDL_CreateWindow(
            "bgl_viewer - C++23 WebGPU & glTF 2.1",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            static_cast<int>(width), static_cast<int>(height),
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
        );
        if (window) {
            nativeWindow_ = window;
            BGL_LOG_INFO("Created native SDL2 window {}x{}", width, height);
        } else {
            BGL_LOG_WARN("Could not create SDL window: {}", SDL_GetError());
        }
    }
#endif

    gpu::ContextConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.nativeWindowHandle = nativeWindow_;
    if (auto res = gpuContext_.Initialize(cfg); !res) {
        return res;
    }

    if (auto res = pipelineManager_.Initialize(); !res) {
        return res;
    }

    soundEngine_.Initialize();
    gltfLoader_ = std::make_unique<scene::FastGltfLoader>(assetManager_);

    core::AABB worldBounds;
    worldBounds.minBound = glm::vec3(-100.0f);
    worldBounds.maxBound = glm::vec3(100.0f);
    quadtree_ = std::make_unique<scene::Quadtree>(worldBounds);

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
                camera_.Pan(-event.motion.xrel * 0.01f, event.motion.yrel * 0.01f);
            } else if (event.type == SDL_MOUSEWHEEL) {
                camera_.Zoom(event.wheel.y * 0.5f);
            } else if (event.type == SDL_DROPFILE) {
                char* droppedFile = event.drop.file;
                if (droppedFile) {
                    BGL_LOG_INFO("File dropped via OS: {}", droppedFile);
                    SDL_free(droppedFile);
                }
            }
        }
    }
#endif

    gamepad_.Update();
    const auto& gp = gamepad_.GetState();

    if (gp.connected) {
        camera_.Pan(gp.leftStick.x * deltaTime * 5.0f, gp.leftStick.y * deltaTime * 5.0f);
        camera_.Orbit(gp.rightStick.x * deltaTime * 2.0f, gp.rightStick.y * deltaTime * 2.0f);
        camera_.Zoom((gp.rightTrigger - gp.leftTrigger) * deltaTime * 8.0f);
    }

    camera_.Update(16.0f / 9.0f);
    soundEngine_.UpdateListener(camera_.GetPosition(), glm::normalize(camera_.GetTarget() - camera_.GetPosition()));
}

void Application::Render() {
    if (!running_) return;

    glm::mat4 vp = camera_.GetViewProjectionMatrix();
    skybox_.Render(vp);

    pipelineManager_.BindPipeline(gpu::RenderAlphaMode::Opaque);

    if (showBoundingBoxes_) {
        pipelineManager_.BindWireframePipeline();
    }
}

void Application::OnFileDrop(std::string_view filename, std::span<const uint8_t> data) {
    BGL_LOG_INFO("Ingesting drag-and-dropped file '{}' ({} bytes)", filename, data.size());
    if (filename.ends_with(".glb") || filename.ends_with(".gltf")) {
        auto res = gltfLoader_->LoadFromMemory(data, filename);
        if (res) {
            activeScene_ = *res;
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

