/**
 * @file Gamepad.cpp
 * @brief Implementation of radial deadzone logic and input mapping.
 */
#include "core/Gamepad.hpp"
#include <algorithm>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::core {

glm::vec2 Gamepad::ApplyDeadzone(glm::vec2 rawAxis) noexcept {
    const float length{glm::length(rawAxis)};
    if (length < kDeadzone) {
        return glm::vec2{0.0f};
    }
    // Normalized smooth remap above deadzone
    const float remappedLength{std::min((length - kDeadzone) / (1.0f - kDeadzone), 1.0f)};
    return (rawAxis / length) * remappedLength;
}

void Gamepad::SetRawState(const GamepadState& raw) noexcept {
    state_ = raw;
    state_.leftStick = ApplyDeadzone(raw.leftStick);
    state_.rightStick = ApplyDeadzone(raw.rightStick);
}

void Gamepad::Update() noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        return;
    }
    const int numJoysticks{SDL_NumJoysticks()};
    for (int i{0}; i < numJoysticks; ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController* pad{SDL_GameControllerOpen(i)};
        if (!pad) continue;

        constexpr float kAxisScale{1.0f / 32767.0f};
        GamepadState raw{};
        raw.connected = true;
        raw.leftStick.x  = std::clamp(static_cast<float>(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX))  * kAxisScale, -1.0f, 1.0f);
        raw.leftStick.y  = std::clamp(static_cast<float>(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY))  * kAxisScale, -1.0f, 1.0f);
        raw.rightStick.x = std::clamp(static_cast<float>(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX)) * kAxisScale, -1.0f, 1.0f);
        raw.rightStick.y = std::clamp(static_cast<float>(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY)) * kAxisScale, -1.0f, 1.0f);
        raw.leftTrigger  = std::clamp(static_cast<float>(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT))  * kAxisScale, 0.0f, 1.0f);
        raw.rightTrigger = std::clamp(static_cast<float>(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) * kAxisScale, 0.0f, 1.0f);
        raw.buttonA = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
        raw.buttonB = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
        raw.buttonX = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
        raw.buttonY = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
        SDL_GameControllerClose(pad);
        SetRawState(raw);
        return;
    }
#endif
}

} // namespace bgl::core

