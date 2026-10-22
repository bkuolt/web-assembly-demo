/**
 * @file Context.cpp
 * @brief Implementation of WebGPU / OpenGL Surface Context and frame swaps.
 */
#include "gpu/Context.hpp"
#include "core/Log.hpp"

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <GL/gl.h>
#endif

namespace bgl::gpu {

Context::Context() = default;

Context::~Context() {
#if defined(BGL_PLATFORM_NATIVE)
    if (nativeContext_) {
        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(nativeContext_));
        nativeContext_ = nullptr;
    }
#endif
    initialized_ = false;
    BGL_LOG_INFO("WebGPU / OpenGL Context cleanly shut down.");
}

core::Result<void> Context::Initialize(const ContextConfig& config) {
    width_ = config.width;
    height_ = config.height;
    nativeWindow_ = config.nativeWindowHandle;

#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_) {
        SDL_Window* window = static_cast<SDL_Window*>(nativeWindow_);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

        SDL_GLContext glContext = SDL_GL_CreateContext(window);
        if (glContext) {
            nativeContext_ = glContext;
            SDL_GL_MakeCurrent(window, glContext);
            SDL_GL_SetSwapInterval(1); // V-Sync
            BGL_LOG_INFO("Created hardware-accelerated OpenGL context for native window");
        } else {
            BGL_LOG_WARN("Could not create SDL_GLContext: {}", SDL_GetError());
        }
    }
#endif

    initialized_ = true;
    BGL_LOG_INFO("Context successfully initialized with resolution {}x{}", width_, height_);
    return {};
}

void Context::Resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
#if defined(BGL_PLATFORM_NATIVE)
    glViewport(0, 0, width, height);
#endif
    BGL_LOG_INFO("Viewport resized to {}x{}", width, height);
}

void Context::BeginFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (nativeContext_) {
        glViewport(0, 0, width_, height_);
        glClearColor(0.08f, 0.11f, 0.16f, 1.0f); // Dark slate blue background
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }
#endif
}

void Context::EndFrame() {
#if defined(BGL_PLATFORM_NATIVE)
    if (nativeWindow_ && nativeContext_) {
        SDL_GL_SwapWindow(static_cast<SDL_Window*>(nativeWindow_));
    }
#endif
}

} // namespace bgl::gpu
