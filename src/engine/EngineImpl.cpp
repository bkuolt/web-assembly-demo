/**
 * @file EngineImpl.cpp
 * @brief Implementation of bgl::Engine::Impl: settings diffing, SDL event pumping, camera controls, and GUI bracket.
 */
#include "engine/EngineImpl.hpp"
#include "core/Log.hpp"
#include "core/NetworkFetcher.hpp"
#include "engine/GuiInternal.hpp"
#include "engine/InputBackend.hpp"
#include <algorithm>
#include <string>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl {

void Engine::Impl::ApplySettings() {
    const bool force = !settingsApplied_;
    auto changed = [&](const auto& a, const auto& b) { return force || a != b; };
    const RenderSettings& n = settings_;
    const RenderSettings& p = applied_;

    if (changed(n.shadows, p.shadows))
        pipelineManager_.SetShadowsEnabled(n.shadows);
    if (changed(n.reflections, p.reflections))
        pipelineManager_.SetReflectionsEnabled(n.reflections);
    if (changed(n.bumpMapping, p.bumpMapping))
        pipelineManager_.SetBumpMappingEnabled(n.bumpMapping);
    if (changed(n.parallaxOcclusion, p.parallaxOcclusion))
        pipelineManager_.SetParallaxEnabled(n.parallaxOcclusion);
    if (changed(n.parallaxScale, p.parallaxScale))
        pipelineManager_.SetParallaxScale(n.parallaxScale);
    if (changed(n.mipmaps, p.mipmaps))
        pipelineManager_.SetMipmappingEnabled(n.mipmaps, activeScene_.get());
    if (changed(n.materialAmbientOcclusion, p.materialAmbientOcclusion))
        pipelineManager_.SetAmbientOcclusionEnabled(n.materialAmbientOcclusion);
    if (changed(n.materialAoIntensity, p.materialAoIntensity))
        pipelineManager_.SetAoIntensity(n.materialAoIntensity);
    if (changed(n.environment, p.environment)) {
        pipelineManager_.SetEnvironmentPreset(static_cast<gpu::EnvironmentPreset>(n.environment), activeScene_.get());
        skybox_.SetEnvironmentCubemap(pipelineManager_.GetEnvironmentCubemapView(),
                                      pipelineManager_.GetDefaultSampler());
    }
    if (changed(n.environmentRotationRad, p.environmentRotationRad)) {
        pipelineManager_.SetEnvironmentRotation(n.environmentRotationRad);
        skybox_.SetRotation(n.environmentRotationRad);
    }
    if (changed(n.timeOfDayHours, p.timeOfDayHours))
        SetTimeOfDay(n.timeOfDayHours);
    if (changed(n.skybox, p.skybox))
        skybox_.SetEnabled(n.skybox);
    if (changed(n.skyboxStyle, p.skyboxStyle))
        skybox_.SetMode(static_cast<gpu::SkyboxMode>(n.skyboxStyle));
    if (changed(n.toneMap, p.toneMap))
        pipelineManager_.SetToneMapOperator(static_cast<gpu::ToneMapOperator>(n.toneMap));
    if (changed(n.exposure, p.exposure))
        pipelineManager_.SetExposure(n.exposure);
    if (changed(n.bloom, p.bloom))
        pipelineManager_.SetBloomEnabled(n.bloom);
    if (changed(n.bloomIntensity, p.bloomIntensity))
        pipelineManager_.SetBloomIntensity(n.bloomIntensity);
    if (changed(n.bloomThreshold, p.bloomThreshold))
        pipelineManager_.SetBloomThreshold(n.bloomThreshold);
    if (changed(n.ssao, p.ssao))
        pipelineManager_.SetSsaoEnabled(n.ssao);
    if (changed(n.ssaoIntensity, p.ssaoIntensity))
        pipelineManager_.SetSsaoIntensity(n.ssaoIntensity);
    if (changed(n.ssaoRadius, p.ssaoRadius))
        pipelineManager_.SetSsaoRadius(n.ssaoRadius);
    if (changed(n.lensFlare, p.lensFlare))
        pipelineManager_.SetLensFlareEnabled(n.lensFlare);
    if (changed(n.lensFlareIntensity, p.lensFlareIntensity))
        pipelineManager_.SetLensFlareIntensity(n.lensFlareIntensity);
    if (changed(n.occlusionCulling, p.occlusionCulling))
        pipelineManager_.SetOcclusionCullingEnabled(n.occlusionCulling);
    if (changed(n.lod, p.lod))
        pipelineManager_.SetLodEnabled(n.lod);
    if (changed(n.autoLod, p.autoLod))
        pipelineManager_.SetAutoLodEnabled(n.autoLod);
    if (changed(n.lodBias, p.lodBias))
        pipelineManager_.SetLodBias(n.lodBias);
    if (changed(n.weather, p.weather))
        pipelineManager_.SetWeatherMode(static_cast<gpu::WeatherMode>(n.weather));
    if (changed(n.weatherIntensity, p.weatherIntensity))
        pipelineManager_.SetWeatherIntensity(n.weatherIntensity);
    if (changed(n.windStrength, p.windStrength))
        pipelineManager_.SetWindStrength(n.windStrength);
    if (changed(n.debugView, p.debugView))
        pipelineManager_.SetDebugViewMode(static_cast<gpu::DebugViewMode>(n.debugView));

    applied_ = settings_;
    settingsApplied_ = true;
}

bool Engine::Impl::PollEvents() {
    if (!IsRunning()) {
        return false;
    }

#if defined(BGL_PLATFORM_NATIVE)
    engine::BeginInputFrame();
    guiInput_.mousePressed = false;
    guiInput_.mouseReleased = false;
    guiInput_.wheelDelta = 0.0f;

    if (nativeWindow_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            engine::FeedSdlEvent(event);

            if (event.type == SDL_QUIT) {
                RequestClose();
                return false;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    const auto w = static_cast<uint32_t>(std::max(1, static_cast<int>(event.window.data1)));
                    const auto h = static_cast<uint32_t>(std::max(1, static_cast<int>(event.window.data2)));
                    gpuContext_.Resize(w, h);
                    if (resizeCb_) {
                        resizeCb_(w, h);
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                guiInput_.mouseX = static_cast<float>(event.button.x);
                guiInput_.mouseY = static_cast<float>(event.button.y);
                if (event.button.button == SDL_BUTTON_LEFT) {
                    guiInput_.mouseDown = true;
                    guiInput_.mousePressed = true;
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                guiInput_.mouseX = static_cast<float>(event.button.x);
                guiInput_.mouseY = static_cast<float>(event.button.y);
                if (event.button.button == SDL_BUTTON_LEFT) {
                    guiInput_.mouseDown = false;
                    guiInput_.mouseReleased = true;
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                guiInput_.mouseX = static_cast<float>(event.motion.x);
                guiInput_.mouseY = static_cast<float>(event.motion.y);
            } else if (event.type == SDL_MOUSEWHEEL) {
                guiInput_.wheelDelta += static_cast<float>(event.wheel.y);
            } else if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
                char* droppedStr{event.drop.file};
                if (droppedStr) {
                    const std::string sanitized{core::NetworkFetcher::SanitizeUri(droppedStr)};
                    if (!sanitized.empty()) {
                        if (dropCb_) {
                            dropCb_(sanitized);
                        }
                        if (config_.loadDroppedFiles) {
                            if (auto loadRes = LoadSceneFromUri(sanitized); !loadRes) {
                                BGL_LOG_ERROR("Failed to load dropped asset/URL '{}': {}", sanitized, loadRes.error());
                            }
                        }
                    }
                    SDL_free(droppedStr);
                }
            }
        }
    }

    engine::SyncKeyboardState();
#endif

    return IsRunning();
}

void Engine::Impl::Update(float deltaTime) {
    if (!IsRunning()) {
        return;
    }

    ApplySettings();

    const float safeDt = std::clamp(deltaTime, 0.0f, 0.25f);
    lastDeltaTime_ = safeDt;
    if (config_.defaultCameraControls) {
        const float dist = camera_.GetDistance();
        if (!guiWantsMouse_) {
            const glm::vec2 delta = Input::MouseDelta();
            if (Input::IsMouseDown(MouseButton::Left) && (delta.x != 0.0f || delta.y != 0.0f)) {
                camera_.Orbit(delta.x * 0.005f, -delta.y * 0.005f);
            } else if (Input::IsMouseDown(MouseButton::Right) && (delta.x != 0.0f || delta.y != 0.0f)) {
                camera_.Pan(-delta.x * 0.0005f * dist, delta.y * 0.0005f * dist);
            }
            if (const float wheel = Input::MouseWheel(); wheel != 0.0f) {
                camera_.Zoom(wheel * dist * 0.12f);
            }
        }

        const float moveSpeed = std::max(dist * 1.5f, 1.0f) * safeDt;
        if (Input::IsKeyDown(Key::W) || Input::IsKeyDown(Key::Up)) {
            camera_.MoveForward(moveSpeed);
        }
        if (Input::IsKeyDown(Key::S) || Input::IsKeyDown(Key::Down)) {
            camera_.MoveForward(-moveSpeed);
        }
        if (Input::IsKeyDown(Key::A) || Input::IsKeyDown(Key::Left)) {
            camera_.MoveRight(-moveSpeed);
        }
        if (Input::IsKeyDown(Key::D) || Input::IsKeyDown(Key::Right)) {
            camera_.MoveRight(moveSpeed);
        }
    }

    UpdateSimulation(safeDt);
}

void Engine::Impl::BeginGui() {
    if (guiFrameOpen_) {
        return;
    }
    const uint32_t w = std::max<uint32_t>(1u, gpuContext_.GetWidth());
    const uint32_t h = std::max<uint32_t>(1u, gpuContext_.GetHeight());
    gui_.BeginFrame(guiInput_, w, h, lastDeltaTime_);
    guiInput_.mousePressed = false;
    guiInput_.mouseReleased = false;
    guiInput_.wheelDelta = 0.0f;
    engine::g_currentGui = &gui_;
    engine::g_currentViewportWidth = w;
    guiFrameOpen_ = true;
}

void Engine::Impl::EndGui() {
    if (!guiFrameOpen_) {
        return;
    }
    engine::g_currentGui = nullptr;
    gui_.EndFrame();
    guiWantsMouse_ = gui_.WantsMouse();
    guiFrameOpen_ = false;
}

void Engine::Impl::OnPrepareOverlay() {
    gui_.SetBackdropTextureView(pipelineManager_.GetUiBackdropView());
}

void Engine::Impl::OnRenderOverlay(WGPURenderPassEncoder swapchainPass) {
    gui_.Render(swapchainPass, gpuContext_.GetWidth(), gpuContext_.GetHeight());
}

void Engine::Impl::Render() {
    ApplySettings();
    EngineCore::Render();
}

void Engine::Impl::Shutdown() {
    if (guiFrameOpen_) {
        engine::g_currentGui = nullptr;
        guiFrameOpen_ = false;
    }
    if (engine::g_registeredEngineGui == &gui_) {
        engine::g_registeredEngineGui = nullptr;
    }
    gui_.Shutdown();
    EngineCore::Shutdown();
}

} // namespace bgl
