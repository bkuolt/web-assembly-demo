/**
 * @file Gamepad.hpp
 * @brief Zero-GLFW Gamepad input handler with PS5 DualSense detection and exponential response curves.
 */
#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cstdint>

#if defined(BGL_PLATFORM_NATIVE)
struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;
#endif

namespace bgl::core {

/**
 * @struct GamepadState
 * @brief Normalized state of dual-analog gamepad axes, smoothed inputs, and action buttons.
 */
struct GamepadState {
    glm::vec2 leftStick{0.0f};      // Raw deadzoned Move / Pan
    glm::vec2 rightStick{0.0f};     // Raw deadzoned Orbit / Look
    glm::vec2 curvedLook{0.0f};     // PS5 exponential response curve applied
    glm::vec2 smoothedMove{0.0f};   // Dynamically smoothed translation
    glm::vec2 smoothedLook{0.0f};   // Dynamically smoothed look/orbit
    float leftTrigger{0.0f};        // Zoom Out
    float rightTrigger{0.0f};       // Zoom In
    bool buttonA{false};            // Ray-Picking / Confirm
    bool buttonB{false};            // Back
    bool buttonX{false};            // Focus
    bool buttonY{false};            // Bounding-Box Toggle
    bool connected{false};
    bool isDualSense{false};        // PS5 DualSense / DualSense Edge controller
    std::string deviceName{"None"};
};

/**
 * @class Gamepad
 * @brief Polling abstraction for Gamepad API (Wasm HTML5 & Native SDL2) with PS5 DualSense feel.
 */
class Gamepad {
public:
    static constexpr float kDeadzone{0.12f};

    Gamepad() = default;
    ~Gamepad();

    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;
    Gamepad(Gamepad&&) noexcept;
    Gamepad& operator=(Gamepad&&) noexcept;

    /**
     * @brief Polls connected gamepad state and advances smoothing filters.
     * @param deltaTime Elapsed frame time in seconds.
     */
    void Update(float deltaTime = 0.016f) noexcept;

    [[nodiscard]] const GamepadState& GetState() const noexcept { return state_; }

    /**
     * @brief Apply radial deadzone filtering to raw 2D analog stick inputs.
     */
    [[nodiscard]] static glm::vec2 ApplyDeadzone(glm::vec2 rawAxis) noexcept;

    /**
     * @brief Applies modern console (PS5) exponential response curve with stabilized pitch.
     * @param stick Deadzoned 2D stick vector.
     * @param exponent Sensitivity power curve (default 2.1f for fine precision + fast turning).
     * @param pitchFactor Vertical sensitivity damping (default 0.70f for horizon stability).
     */
    [[nodiscard]] static glm::vec2 ApplyPs5ResponseCurve(glm::vec2 stick, float exponent = 2.1f, float pitchFactor = 0.70f) noexcept;

    /**
     * @brief Delivers a haptic vibration pulse to the controller.
     */
    void Rumble(float lowFreq, float highFreq, uint32_t durationMs = 80) noexcept;

    void SetRawState(const GamepadState& raw, float deltaTime = 0.016f) noexcept;

private:
    GamepadState state_{};
#if defined(BGL_PLATFORM_NATIVE)
    SDL_GameController* activeController_{nullptr};
#endif
};

} // namespace bgl::core

