/**
 * @file Input.cpp
 * @brief Public bgl::Input polling API implementation.
 */
#include "bgl/Input.hpp"
#include "engine/InputState.hpp"

namespace bgl::engine {

InputState& GetInputState() noexcept {
    static InputState state;
    return state;
}

} // namespace bgl::engine

namespace bgl {

bool Input::IsKeyDown(Key key) noexcept {
    return engine::GetInputState().IsKeyDown(key);
}

bool Input::IsKeyPressed(Key key) noexcept {
    return engine::GetInputState().IsKeyPressed(key);
}

bool Input::IsKeyReleased(Key key) noexcept {
    return engine::GetInputState().IsKeyReleased(key);
}

bool Input::IsMouseDown(MouseButton button) noexcept {
    return engine::GetInputState().IsMouseDown(button);
}

bool Input::IsMousePressed(MouseButton button) noexcept {
    return engine::GetInputState().IsMousePressed(button);
}

bool Input::IsMouseReleased(MouseButton button) noexcept {
    return engine::GetInputState().IsMouseReleased(button);
}

glm::vec2 Input::MousePosition() noexcept {
    return engine::GetInputState().MousePosition();
}

glm::vec2 Input::MouseDelta() noexcept {
    return engine::GetInputState().MouseDelta();
}

float Input::MouseWheel() noexcept {
    return engine::GetInputState().MouseWheel();
}

} // namespace bgl
