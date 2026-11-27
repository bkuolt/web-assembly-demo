/**
 * @file EngineImpl.hpp
 * @brief Internal Pimpl implementation of bgl::Engine backed by engine::EngineCore.
 */
#ifndef BGL_ENGINE_ENGINEIMPL_HPP
#define BGL_ENGINE_ENGINEIMPL_HPP

#include "bgl/Engine.hpp"
#include "engine/EngineCore.hpp"
#include "ui/ImmediateUI.hpp"
#include <functional>

namespace bgl {

struct Engine::Impl final : engine::EngineCore {
    friend class Engine;
    EngineConfig config_{};
    RenderSettings settings_{};
    RenderSettings applied_{};
    bool settingsApplied_{false};
    ui::ImmediateUI gui_{};
    ui::UiInputState guiInput_{};
    std::function<void(uint32_t, uint32_t)> resizeCb_{};
    std::function<void(std::string_view)> dropCb_{};
    bool guiFrameOpen_{false};
    bool guiWantsMouse_{false};

    void ApplySettings();
    bool PollEvents();
    void Update(float deltaTime);
    void BeginGui();
    void EndGui();
    void RenderFrame();
    void Shutdown() override;

protected:
    void OnPrepareOverlay() override;
    void OnRenderOverlay(WGPURenderPassEncoder swapchainPass) override;
};

} // namespace bgl

#endif // BGL_ENGINE_ENGINEIMPL_HPP
