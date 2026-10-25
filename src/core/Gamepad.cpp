/**
 * @file Gamepad.cpp
 * @brief Implementation of radial deadzone logic, PS5 DualSense detection, and response curves.
 */
#include "core/Gamepad.hpp"
#include <algorithm>
#include <cmath>

#if defined(BGL_PLATFORM_NATIVE)
#include <SDL2/SDL.h>
#endif

namespace bgl::core {

Gamepad::~Gamepad() {
#if defined(BGL_PLATFORM_NATIVE)
    if (activeController_) {
        SDL_GameControllerClose(activeController_);
        activeController_ = nullptr;
    }
#endif
}

Gamepad::Gamepad(Gamepad&& other) noexcept
    : state_{std::move(other.state_)}
#if defined(BGL_PLATFORM_NATIVE)
      ,
      activeController_{other.activeController_}
#endif
{
#if defined(BGL_PLATFORM_NATIVE)
    other.activeController_ = nullptr;
#endif
}

Gamepad& Gamepad::operator=(Gamepad&& other) noexcept {
    if (this != &other) {
#if defined(BGL_PLATFORM_NATIVE)
        if (activeController_) {
            SDL_GameControllerClose(activeController_);
        }
        activeController_ = other.activeController_;
        other.activeController_ = nullptr;
#endif
        state_ = std::move(other.state_);
    }
    return *this;
}

glm::vec2 Gamepad::ApplyDeadzone(glm::vec2 rawAxis) noexcept {
    const float length{glm::length(rawAxis)};
    if (length < kDeadzone) {
        return glm::vec2{0.0f};
    }
    // Normalized smooth remap above deadzone
    const float remappedLength{std::min((length - kDeadzone) / (1.0f - kDeadzone), 1.0f)};
    return (rawAxis / length) * remappedLength;
}

glm::vec2 Gamepad::ApplyPs5ResponseCurve(glm::vec2 stick, float exponent, float pitchFactor) noexcept {
    const float mag = glm::length(stick);
    if (mag <= 1e-4f) {
        return glm::vec2{0.0f};
    }
    const float curvedMag = std::pow(std::min(mag, 1.0f), exponent);
    glm::vec2 dir = stick / mag;
    dir.y *= pitchFactor;
    return dir * curvedMag;
}

void Gamepad::SetRawState(const GamepadState& raw, float deltaTime) noexcept {
    state_ = raw;
    state_.leftStick = ApplyDeadzone(raw.leftStick);
    state_.rightStick = ApplyDeadzone(raw.rightStick);
    state_.curvedLook = ApplyPs5ResponseCurve(state_.rightStick);

    const float dt = std::clamp(deltaTime, 0.001f, 0.1f);
    const float blend = 1.0f - std::exp(-20.0f * dt);
    state_.smoothedMove = glm::mix(state_.smoothedMove, state_.leftStick, blend);
    state_.smoothedLook = glm::mix(state_.smoothedLook, state_.curvedLook, blend);
}

void Gamepad::Rumble(float lowFreq, float highFreq, uint32_t durationMs) noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    if (activeController_) {
        const Uint16 low = static_cast<Uint16>(std::clamp(lowFreq, 0.0f, 1.0f) * 65535.0f);
        const Uint16 high = static_cast<Uint16>(std::clamp(highFreq, 0.0f, 1.0f) * 65535.0f);
        SDL_GameControllerRumble(activeController_, low, high, durationMs);
    }
#else
    (void)lowFreq;
    (void)highFreq;
    (void)durationMs;
#endif
}

void Gamepad::Update(float deltaTime) noexcept {
#if defined(BGL_PLATFORM_NATIVE)
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        state_.connected = false;
        return;
    }

    if (activeController_ && !SDL_GameControllerGetAttached(activeController_)) {
        SDL_GameControllerClose(activeController_);
        activeController_ = nullptr;
        state_.connected = false;
        state_.deviceName = "None";
        state_.isDualSense = false;
    }

    if (!activeController_) {
        const int numJoysticks{SDL_NumJoysticks()};
        for (int i{0}; i < numJoysticks; ++i) {
            if (!SDL_IsGameController(i))
                continue;
            activeController_ = SDL_GameControllerOpen(i);
            if (activeController_)
                break;
        }
    }

    if (!activeController_) {
        state_.connected = false;
        return;
    }

    constexpr float kAxisScale{1.0f / 32767.0f};
    GamepadState raw{};
    raw.connected = true;

    const char* name = SDL_GameControllerName(activeController_);
    raw.deviceName = name ? name : "Gamepad Controller";
    const uint16_t vendor = SDL_GameControllerGetVendor(activeController_);
    const uint16_t product = SDL_GameControllerGetProduct(activeController_);

    // PS5 DualSense (0x054c / 0x0ce6) and DualSense Edge (0x0df2)
    raw.isDualSense = (vendor == 0x054c && (product == 0x0ce6 || product == 0x0df2));
    if (!raw.isDualSense && name) {
        const std::string sName = name;
        if (sName.find("DualSense") != std::string::npos || sName.find("PS5") != std::string::npos ||
            (vendor == 0x054c && sName.find("Wireless Controller") != std::string::npos)) {
            raw.isDualSense = true;
        }
    }

    raw.leftStick.x = std::clamp(
        static_cast<float>(SDL_GameControllerGetAxis(activeController_, SDL_CONTROLLER_AXIS_LEFTX)) * kAxisScale, -1.0f,
        1.0f);
    raw.leftStick.y = std::clamp(
        static_cast<float>(SDL_GameControllerGetAxis(activeController_, SDL_CONTROLLER_AXIS_LEFTY)) * kAxisScale, -1.0f,
        1.0f);
    raw.rightStick.x = std::clamp(
        static_cast<float>(SDL_GameControllerGetAxis(activeController_, SDL_CONTROLLER_AXIS_RIGHTX)) * kAxisScale,
        -1.0f, 1.0f);
    raw.rightStick.y = std::clamp(
        static_cast<float>(SDL_GameControllerGetAxis(activeController_, SDL_CONTROLLER_AXIS_RIGHTY)) * kAxisScale,
        -1.0f, 1.0f);
    raw.leftTrigger = std::clamp(
        static_cast<float>(SDL_GameControllerGetAxis(activeController_, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) * kAxisScale,
        0.0f, 1.0f);
    raw.rightTrigger = std::clamp(
        static_cast<float>(SDL_GameControllerGetAxis(activeController_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) * kAxisScale,
        0.0f, 1.0f);
    raw.buttonA = SDL_GameControllerGetButton(activeController_, SDL_CONTROLLER_BUTTON_A) != 0;
    raw.buttonB = SDL_GameControllerGetButton(activeController_, SDL_CONTROLLER_BUTTON_B) != 0;
    raw.buttonX = SDL_GameControllerGetButton(activeController_, SDL_CONTROLLER_BUTTON_X) != 0;
    raw.buttonY = SDL_GameControllerGetButton(activeController_, SDL_CONTROLLER_BUTTON_Y) != 0;

    SetRawState(raw, deltaTime);
#else
    (void)deltaTime;
#endif
}

} // namespace bgl::core
