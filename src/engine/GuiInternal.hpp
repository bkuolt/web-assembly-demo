/**
 * @file GuiInternal.hpp
 * @brief Internal thread-local active ImmediateUI context for bgl::gui.
 */
#ifndef BGL_ENGINE_GUIINTERNAL_HPP
#define BGL_ENGINE_GUIINTERNAL_HPP

#include <cstdint>

namespace bgl::ui {
class ImmediateUI;
} // namespace bgl::ui

namespace bgl::engine {

inline thread_local ui::ImmediateUI* g_currentGui{nullptr};
inline thread_local ui::ImmediateUI* g_registeredEngineGui{nullptr};
inline thread_local uint32_t g_currentViewportWidth{1280};

[[nodiscard]] inline ui::ImmediateUI* ActiveOrRegisteredGui() noexcept {
    return g_currentGui ? g_currentGui : g_registeredEngineGui;
}

} // namespace bgl::engine

#endif // BGL_ENGINE_GUIINTERNAL_HPP
