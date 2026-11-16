/**
 * @file Gui.hpp
 * @brief Immediate-mode debug UI. Call only inside App::OnGui().
 */
#ifndef BGL_PUBLIC_GUI_HPP
#define BGL_PUBLIC_GUI_HPP

#include <string_view>

namespace bgl::gui {

/**
 * @brief Opens a draggable immediate-mode window anchored near the top-right corner.
 * @param title Window header title string.
 * @note Every call to `Begin()` must be matched with `End()`.
 */
void Begin(std::string_view title);

/** @brief Closes the current window opened by `Begin()`. */
void End();

/**
 * @brief Renders a single line of static text inside the active window.
 * @param text UTF-8 label content to shape and display.
 */
void Label(std::string_view text);

/**
 * @brief Renders an interactive push button.
 * @param label Button caption text.
 * @return `true` in the frame the button was clicked.
 */
bool Button(std::string_view label);

/**
 * @brief Renders a boolean toggle checkbox.
 * @param label Checkbox caption text.
 * @param[in,out] value Boolean state modified when clicked.
 * @return `true` when `value` changed during this frame.
 */
bool Checkbox(std::string_view label, bool& value);

/**
 * @brief Renders a draggable floating-point range slider.
 * @param label Slider caption text.
 * @param[in,out] value Scalar value updated by dragging.
 * @param minValue Minimum clamped slider bound.
 * @param maxValue Maximum clamped slider bound.
 * @return `true` when `value` changed during this frame.
 */
bool Slider(std::string_view label, float& value, float minValue, float maxValue);

} // namespace bgl::gui

#endif // BGL_PUBLIC_GUI_HPP
