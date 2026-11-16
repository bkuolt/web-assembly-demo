/**
 * @file App.hpp
 * @brief Base class with lifecycle hooks (OnStart/OnUpdate/OnGui/...) and the BGL_RUN entry-point macro.
 */
#ifndef BGL_PUBLIC_APP_HPP
#define BGL_PUBLIC_APP_HPP

#include "bgl/Engine.hpp"
#include <cstdint>
#include <memory>
#include <string_view>

namespace bgl {

/**
 * @class App
 * @brief Derive from this class, override the hooks you need and use BGL_RUN(MyApp).
 */
class App {
public:
    virtual ~App() = default;

    /**
     * @brief Override to customize window size, title, or initial render settings before Engine initialization.
     * @return Engine startup configuration struct.
     */
    virtual EngineConfig Configure() { return {}; }

    /** @brief Called once after the engine and WebGPU context are initialized. */
    virtual void OnStart() {}

    /**
     * @brief Called once per frame with elapsed wall-clock time.
     * @param deltaTime Frame delta time in seconds.
     */
    virtual void OnUpdate(float deltaTime) { (void)deltaTime; }

    /** @brief Called once per frame inside the GUI bracket (`BeginGui()` / `EndGui()`). Use `bgl::gui::*` here. */
    virtual void OnGui() {}

    /**
     * @brief Called whenever the application window is resized.
     * @param width New viewport width in pixels.
     * @param height New viewport height in pixels.
     */
    virtual void OnResize(uint32_t width, uint32_t height) {
        (void)width;
        (void)height;
    }

    /**
     * @brief Called for each file or URL dragged onto the window.
     * @param path Sanitized local path or URL string.
     */
    virtual void OnFileDrop(std::string_view path) { (void)path; }

    /** @brief Called once before the engine shuts down cleanly. */
    virtual void OnShutdown() {}

protected:
    /**
     * @brief Returns mutable access to the owning Engine instance during lifecycle callbacks.
     * @return Reference to the owning Engine.
     */
    [[nodiscard]] Engine& GetEngine() noexcept { return *engine_; }

    /**
     * @brief Returns read-only access to the owning Engine instance during lifecycle callbacks.
     * @return Const reference to the owning Engine.
     */
    [[nodiscard]] const Engine& GetEngine() const noexcept { return *engine_; }

    /** @brief Requests clean termination of the main loop after the current frame. */
    void Quit() noexcept { engine_->Quit(); }

private:
    friend int RunApp(std::unique_ptr<App> app, int argc, char** argv);
    Engine* engine_{nullptr}; ///< Non-owning back-pointer bound by RunApp().
};

/**
 * @brief Runs the standard engine frame loop for an App subclass until exit.
 * @param app Heap-allocated application instance overriding App lifecycle hooks.
 * @param argc Command-line argument count forwarded from `main()`.
 * @param argv Command-line argument vector forwarded from `main()`.
 * @return Process exit code (`0` on clean exit, non-zero if Engine initialization fails).
 */
int RunApp(std::unique_ptr<App> app, int argc, char** argv);

} // namespace bgl

/** @brief Defines main() that runs the given App subclass. */
#define BGL_RUN(AppClass)                                                                                              \
    int main(int argc, char** argv) {                                                                                  \
        return ::bgl::RunApp(std::make_unique<AppClass>(), argc, argv);                                                \
    }

#endif // BGL_PUBLIC_APP_HPP
