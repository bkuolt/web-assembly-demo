/**
 * @file Engine.hpp
 * @brief Public facade of the bgl Engine: window, rendering, model loading, camera and settings.
 */
#pragma once

#include "bgl/Input.hpp"
#include "bgl/Settings.hpp"
#include "bgl/Types.hpp"
#include "core/Camera.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bgl {

/** @brief Startup parameters of Engine::Create(). */
struct EngineConfig {
    uint32_t width{1280};
    uint32_t height{720};
    std::string title{"bgl Engine"};
    RenderSettings render{};
    bool defaultCameraControls{true}; ///< LMB orbit, RMB pan, wheel zoom, WASD/arrow keys move
    bool loadDroppedFiles{true};      ///< Dropping a .glb/.gltf onto the window loads it
};

/**
 * @class Engine
 * @brief Owns window, GPU device, renderer and the active scene. Create with Engine::Create().
 */
class Engine {
public:
    /** @brief Creates window and renderer. Returns an error string (e.g. no GPU adapter) on failure. */
    [[nodiscard]] static Result<std::unique_ptr<Engine>> Create(const EngineConfig& config = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /** @brief Pumps window/input events. Returns false when the window was closed or Quit() was called. */
    bool PollEvents();
    /** @brief Advances simulation (animation, camera, sky). Call once per frame. */
    void Update(float deltaTime);
    /** @brief Renders one frame (including gui, see bgl/Gui.hpp). */
    void Render();

    [[nodiscard]] bool IsRunning() const noexcept;
    void Quit() noexcept;

    /** @brief Loads a glTF/GLB from a path, file:// URI or http(s):// URL and makes it the active scene. */
    Result<void> LoadModel(std::string_view pathOrUrl);

    /** @brief The orbit/fly camera. */
    [[nodiscard]] core::Camera& GetCamera() noexcept;
    /** @brief Moves the camera so the whole scene is visible. */
    void FrameAll() noexcept;

    /** @brief Mutable settings; changes are applied automatically at the start of the next Update(). */
    [[nodiscard]] RenderSettings& Settings() noexcept;

    [[nodiscard]] uint32_t Width() const noexcept;
    [[nodiscard]] uint32_t Height() const noexcept;

    /** @brief Called when the window is resized. */
    void SetResizeCallback(std::function<void(uint32_t width, uint32_t height)> callback);
    /** @brief Called for every dropped file/URL (in addition to the automatic load if loadDroppedFiles is set). */
    void SetFileDropCallback(std::function<void(std::string_view path)> callback);

    /** @brief Opens the per-frame immediate-mode GUI bracket (used by App). */
    void BeginGui();
    /** @brief Closes the per-frame immediate-mode GUI bracket (used by App). */
    void EndGui();

    struct Impl;
    [[nodiscard]] Impl& GetImpl() noexcept { return *impl_; }

private:
    Engine();
    std::unique_ptr<Impl> impl_;
};

} // namespace bgl
