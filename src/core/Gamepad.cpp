/**
 * @file Gamepad.cpp
 * @brief Implementation of radial deadzone logic and input mapping.
 */
#include "core/Gamepad.hpp"
#include <algorithm>

namespace bgl::core {

glm::vec2 Gamepad::ApplyDeadzone(glm::vec2 rawAxis) noexcept {
    float length = glm::length(rawAxis);
    if (length < kDeadzone) {
        return glm::vec2(0.0f);
    }
    // Normalized smooth remap above deadzone
    float remappedLength = std::min((length - kDeadzone) / (1.0f - kDeadzone), 1.0f);
    return (rawAxis / length) * remappedLength;
}

void Gamepad::SetRawState(const GamepadState& raw) noexcept {
    state_ = raw;
    state_.leftStick = ApplyDeadzone(raw.leftStick);
    state_.rightStick = ApplyDeadzone(raw.rightStick);
}

void Gamepad::Update() noexcept {
    // Platform-specific updates are handled via callbacks or event loops
}

} // namespace bgl::core

