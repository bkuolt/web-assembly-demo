/**
 * @file App.hpp
 * @brief Base class with lifecycle hooks (OnStart/OnUpdate/OnGui/...) and the BGL_RUN entry-point macro.
 */
#pragma once

#include "bgl/Engine.hpp"
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

    /** @brief Override to change window size, title or initial render settings. */
    virtual EngineConfig Configure() { return {}; }
    /** @brief Called once after the engine was created. */
    virtual void OnStart() {}
    /** @brief Called once per frame with the elapsed time in seconds. */
    virtual void OnUpdate(float deltaTime) { (void)deltaTime; }
    /** @brief Called once per frame between BeginGui/EndGui. Use bgl::gui::* here. */
    virtual void OnGui() {}
    virtual void OnResize(uint32_t width, uint32_t height) {
        (void)width;
        (void)height;
    }
    virtual void OnFileDrop(std::string_view path) { (void)path; }
    /** @brief Called once before the engine is destroyed. */
    virtual void OnShutdown() {}

protected:
    [[nodiscard]] Engine& GetEngine() noexcept { return *engine_; }
    void Quit() noexcept { engine_->Quit(); }

private:
    friend int RunApp(std::unique_ptr<App> app, int argc, char** argv);
    Engine* engine_{nullptr};
};

/** @brief Runs the main loop (native). Returns the process exit code. */
int RunApp(std::unique_ptr<App> app, int argc, char** argv);

} // namespace bgl

/** @brief Defines main() that runs the given App subclass. */
#define BGL_RUN(AppClass)                                                                                              \
    int main(int argc, char** argv) {                                                                                  \
        return ::bgl::RunApp(std::make_unique<AppClass>(), argc, argv);                                                \
    }
