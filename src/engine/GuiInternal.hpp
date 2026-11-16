/**
 * @file GuiInternal.hpp
 * @brief Internal thread-local active ImmediateUI context for bgl::gui.
 */
#pragma once

#include <cstdint>

namespace bgl::ui {
class ImmediateUI;
} // namespace bgl::ui

namespace bgl::engine {

inline thread_local ui::ImmediateUI* g_currentGui{nullptr};
inline thread_local uint32_t g_currentViewportWidth{1280};

} // namespace bgl::engine
