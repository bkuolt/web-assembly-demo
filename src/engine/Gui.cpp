/**
 * @file Gui.cpp
 * @brief Public bgl::gui immediate-mode helpers delegating to ui::ImmediateUI.
 */
#include "bgl/Gui.hpp"
#include "engine/GuiInternal.hpp"
#include "ui/ImmediateUI.hpp"
#include <algorithm>

namespace bgl::gui {

void Begin(std::string_view title) {
    if (!engine::g_currentGui) {
        return;
    }
    const float x = std::max(16.0f, static_cast<float>(engine::g_currentViewportWidth) - 356.0f);
    engine::g_currentGui->BeginWindow(title, x, 14.0f, 340.0f);
}

void End() {
    if (!engine::g_currentGui) {
        return;
    }
    engine::g_currentGui->EndWindow();
}

void Label(std::string_view text) {
    if (!engine::g_currentGui) {
        return;
    }
    engine::g_currentGui->Label(text);
}

bool Button(std::string_view label) {
    if (!engine::g_currentGui) {
        return false;
    }
    return engine::g_currentGui->Button(label);
}

bool Checkbox(std::string_view label, bool& value) {
    if (!engine::g_currentGui) {
        return false;
    }
    return engine::g_currentGui->Checkbox(label, value);
}

bool Slider(std::string_view label, float& value, float minValue, float maxValue) {
    if (!engine::g_currentGui) {
        return false;
    }
    return engine::g_currentGui->SliderFloat(label, value, minValue, maxValue, "{:.2f}");
}

} // namespace bgl::gui
