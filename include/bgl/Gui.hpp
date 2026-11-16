/**
 * @file Gui.hpp
 * @brief Immediate-mode debug UI. Call only inside App::OnGui().
 */
#ifndef BGL_PUBLIC_GUI_HPP
#define BGL_PUBLIC_GUI_HPP

#include <string_view>

namespace bgl::gui {

/** @brief Opens a window at the top-right corner. Must be paired with End(). */
void Begin(std::string_view title);
/** @brief Closes the current window opened by Begin(). */
void End();
/** @brief Renders a text label. */
void Label(std::string_view text);
/** @brief Returns true in the frame the button was clicked. */
bool Button(std::string_view label);
/** @brief Returns true when the value changed. */
bool Checkbox(std::string_view label, bool& value);
/** @brief Returns true when the value changed. */
bool Slider(std::string_view label, float& value, float minValue, float maxValue);

} // namespace bgl::gui

#endif // BGL_PUBLIC_GUI_HPP
