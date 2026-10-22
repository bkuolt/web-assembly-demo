/**
 * @file Gamepad.hpp
 * @brief Zero-GLFW Gamepad input handler with 0.12f radial deadzone.
 */
#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace bgl::core {

/**
 * @struct GamepadState
 * @brief Normalized state of dual-analog gamepad axes and action buttons.
 */
struct GamepadState {
    glm::vec2 leftStick{0.0f};  // Pan
    glm::vec2 rightStick{0.0f}; // Orbit
    float leftTrigger{0.0f};    // Zoom Out
    float rightTrigger{0.0f};   // Zoom In
    bool buttonA{false};        // Ray-Picking
    bool buttonB{false};
    bool buttonX{false};
    bool buttonY{false};        // Bounding-Box Toggle
    bool connected{false};
};

/**
 * @class Gamepad
 * @brief Polling abstraction for Gamepad API (Wasm HTML5 & Native SDL2).
 */
class Gamepad {
public:
    static constexpr float kDeadzone{0.12f};

    Gamepad() = default;
    void Update() noexcept;

    [[nodiscard]] const GamepadState& GetState() const noexcept { return state_; }

    /**
     * @brief Apply radial deadzone filtering to raw 2D analog stick inputs.
     */
    [[nodiscard]] static glm::vec2 ApplyDeadzone(glm::vec2 rawAxis) noexcept;

    void SetRawState(const GamepadState& raw) noexcept;

private:
    GamepadState state_{};
};

} // namespace bgl::core

