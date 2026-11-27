/**
 * @file Engine.hpp
 * @brief Public facade of the bgl Engine: window, rendering, model loading, camera and settings.
 */
#ifndef BGL_PUBLIC_ENGINE_HPP
#define BGL_PUBLIC_ENGINE_HPP

#include "bgl/Entity.hpp"
#include "bgl/Input.hpp"
#include "bgl/Settings.hpp"
#include "bgl/Sprite.hpp"
#include "bgl/Types.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace bgl {

/**
 * @struct EngineConfig
 * @brief Startup parameters passed to `Engine::Create()` or returned from `App::Configure()`.
 */
struct EngineConfig {
    uint32_t width{1280};            ///< Initial window / viewport width in pixels.
    uint32_t height{720};            ///< Initial window / viewport height in pixels.
    std::string title{"bgl Engine"}; ///< Native window title bar text (unused in browser builds).
    RenderSettings render{};         ///< Initial rendering and post-processing configuration.
    bool defaultCameraControls{
        true}; ///< LMB orbit, RMB pan, wheel zoom, WASD/arrow movement when GUI doesn't capture input.
    bool loadDroppedFiles{true}; ///< Automatically load dragged-and-dropped `.glb`/`.gltf` files or URLs.
};

/**
 * @class Engine
 * @brief Owns the window, WebGPU device, renderer, camera, and active scene. Instantiate with `Engine::Create()`.
 */
class Engine {
public:
    /**
     * @brief Creates the native/WASM window, initializes WebGPU, and configures pipelines.
     * @param config Window dimensions, title, and initial render settings.
     * @return Owning handle on success, or a human-readable error string (e.g. missing GPU adapter) on failure.
     */
    [[nodiscard]] static Result<std::unique_ptr<Engine>> Create(const EngineConfig& config = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /**
     * @brief Pumps pending window, keyboard, mouse, and file-drop events for the current frame.
     * @return `false` if the user closed the window or `Quit()` was called; `true` otherwise.
     */
    [[nodiscard]] bool PollEvents();

    /**
     * @brief Applies pending RenderSettings diffs and advances animation, camera, skybox, and telemetry.
     * @param deltaTime Elapsed wall-clock time since the previous frame in seconds.
     */
    void Update(float deltaTime);

    /** @brief Encodes and presents one HDR + post-processed + GUI frame. */
    void Render();

    /**
     * @brief Checks whether the engine loop is still active.
     * @return `true` until `Quit()` is called or the user closes the window.
     */
    [[nodiscard]] bool IsRunning() const noexcept;

    /** @brief Signals the engine loop to terminate cleanly after the current frame. */
    void Quit() noexcept;

    /**
     * @brief Loads a glTF 2.0/2.1 or GLB asset from a file path, `file://` URI, or `http(s)://` URL.
     * @param pathOrUrl Filesystem path, `file://`, `data:`, or HTTP/HTTPS resource URI.
     * @return Empty Result on success, or an error description string on failure.
     */
    [[nodiscard]] Result<void> LoadModel(std::string_view pathOrUrl);

    /**
     * @brief Loads a glTF 2.0/2.1 or GLB asset from an in-memory byte buffer and makes it the active scene.
     * @param data Contiguous view of raw `.glb` or `.gltf` bytes.
     * @param name Virtual filename or base URI used for logging and resolving external relative assets.
     * @return Empty Result on success, or an error description string on failure.
     */
    [[nodiscard]] Result<void> LoadModel(std::span<const uint8_t> data, std::string_view name = "memory://memory.glb");

    /**
     * @brief Creates an empty entity in the active scene (creates an empty scene automatically if none is loaded).
     * @param name Debug name assigned to the created scene node.
     * @return Handle to the newly created Entity.
     */
    Entity CreateEntity(std::string_view name);

    /**
     * @brief Finds the first entity matching `name` in the active scene.
     * @param name Exact node name to search for.
     * @return Valid Entity handle if found, or an invalid handle otherwise.
     */
    [[nodiscard]] Entity FindEntity(std::string_view name);

    /**
     * @brief Returns mutable access to the interactive orbit/fly camera.
     * @return Reference to the active Camera.
     */
    [[nodiscard]] Camera& GetCamera() noexcept;

    /**
     * @brief Returns read-only access to the interactive orbit/fly camera.
     * @return Const reference to the active Camera.
     */
    [[nodiscard]] const Camera& GetCamera() const noexcept;

    /** @brief Moves the camera target and distance so the whole active scene fits comfortably in view. */
    void FrameAll() noexcept;

    /**
     * @brief Returns mutable render settings; modifications are diffed and applied on the next `Update()`.
     * @return Reference to the live RenderSettings struct.
     */
    [[nodiscard]] RenderSettings& Settings() noexcept;

    /**
     * @brief Returns read-only view of the current render settings.
     * @return Const reference to the current RenderSettings struct.
     */
    [[nodiscard]] const RenderSettings& Settings() const noexcept;

    /**
     * @brief Returns the current drawable viewport width in pixels.
     * @return Viewport width in pixels.
     */
    [[nodiscard]] uint32_t Width() const noexcept;

    /**
     * @brief Returns the current drawable viewport height in pixels.
     * @return Viewport height in pixels.
     */
    [[nodiscard]] uint32_t Height() const noexcept;

    /**
     * @brief Registers a callback invoked whenever the window or swapchain resizes.
     * @param callback Invoked with the new viewport width and height in pixels.
     */
    void SetResizeCallback(std::function<void(uint32_t width, uint32_t height)> callback);

    /**
     * @brief Registers a callback invoked for each dropped file or URL (in addition to auto-loading if enabled).
     * @param callback Invoked with the sanitized path or URL string.
     */
    void SetFileDropCallback(std::function<void(std::string_view path)> callback);

    /** @brief Opens the per-frame immediate-mode GUI bracket (called automatically around `App::OnGui()`). */
    void BeginGui();

    /** @brief Closes the per-frame immediate-mode GUI bracket (called automatically around `App::OnGui()`). */
    void EndGui();

    /**
     * @brief Loads a 2D texture (PNG, JPEG, WebP, or KTX2) from path/URL into a `SpriteTexture`.
     * @param pathOrUrl Filesystem path, `file://`, `data:`, or HTTP/HTTPS resource URI.
     * @param nearestFilter Uses crisp nearest-neighbor pixel filtering instead of bilinear filtering when true.
     * @return Shared texture handle on success, or a human-readable error string.
     */
    [[nodiscard]] Result<std::shared_ptr<SpriteTexture>> LoadSpriteTexture(std::string_view pathOrUrl,
                                                                           bool nearestFilter = false);

    /**
     * @brief Decodes raw PNG, JPEG, WebP, or KTX2 image bytes into a `SpriteTexture`.
     * @param data Contiguous view of raw encoded image bytes.
     * @param formatHint Optional filename, extension, or MIME type hint for format sniffing.
     * @param nearestFilter Uses crisp nearest-neighbor pixel filtering instead of bilinear filtering when true.
     * @return Shared texture handle on success, or a human-readable error string.
     */
    [[nodiscard]] Result<std::shared_ptr<SpriteTexture>>
    LoadSpriteTexture(std::span<const uint8_t> data, std::string_view formatHint = {}, bool nearestFilter = false);

    /**
     * @brief Enqueues a 2D screen-space `Sprite` (or `AnimatedSprite`) for rendering in the current frame.
     * @param sprite Sprite descriptor copied into the frame batch.
     */
    void DrawSprite(const Sprite& sprite);

    /**
     * @brief Enqueues a 3D camera-facing `Billboard` (or `AnimatedBillboard`) for rendering in the HDR transparent
     * pass.
     * @param billboard Billboard descriptor copied into the frame batch.
     */
    void DrawBillboard(const Billboard& billboard);

    struct Impl;

private:
    Engine();
    std::unique_ptr<Impl> impl_{}; ///< Internal Pimpl state inheriting `engine::EngineCore`.
};

} // namespace bgl

#endif // BGL_PUBLIC_ENGINE_HPP
