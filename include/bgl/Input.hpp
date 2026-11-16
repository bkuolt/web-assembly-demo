/**
 * @file Input.hpp
 * @brief Polling-based keyboard and mouse input. Query state once per frame in App::OnUpdate().
 */
#ifndef BGL_PUBLIC_INPUT_HPP
#define BGL_PUBLIC_INPUT_HPP

#include <cstdint>
#include <glm/glm.hpp>

namespace bgl {

enum class Key : uint16_t {
    Unknown = 0,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    LeftShift,
    LeftCtrl,
    LeftAlt,
    Up,
    Down,
    Left,
    Right,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Count
};

enum class MouseButton : uint8_t { Left = 0, Middle = 1, Right = 2, Count };

/** @brief Static polling API. State is updated by Engine::PollEvents(). */
class Input {
public:
    /** @brief True while the key is held down. */
    [[nodiscard]] static bool IsKeyDown(Key key) noexcept;
    /** @brief True only in the frame the key went down. */
    [[nodiscard]] static bool IsKeyPressed(Key key) noexcept;
    /** @brief True only in the frame the key was released. */
    [[nodiscard]] static bool IsKeyReleased(Key key) noexcept;
    [[nodiscard]] static bool IsMouseDown(MouseButton button) noexcept;
    [[nodiscard]] static bool IsMousePressed(MouseButton button) noexcept;
    [[nodiscard]] static bool IsMouseReleased(MouseButton button) noexcept;
    /** @brief Mouse position in window pixels (origin top-left). */
    [[nodiscard]] static glm::vec2 MousePosition() noexcept;
    /** @brief Mouse movement since the previous frame in pixels. */
    [[nodiscard]] static glm::vec2 MouseDelta() noexcept;
    /** @brief Vertical wheel movement since the previous frame (positive = away from user). */
    [[nodiscard]] static float MouseWheel() noexcept;
};

} // namespace bgl

#endif // BGL_PUBLIC_INPUT_HPP
