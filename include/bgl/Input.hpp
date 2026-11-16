/**
 * @file Input.hpp
 * @brief Polling-based keyboard and mouse input. Query state once per frame in App::OnUpdate().
 */
#ifndef BGL_PUBLIC_INPUT_HPP
#define BGL_PUBLIC_INPUT_HPP

#include <cstdint>
#include <glm/glm.hpp>

namespace bgl {

/** @brief Physical keyboard key codes supported by `bgl::Input`. */
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
    Count ///< Total enumerator sentinel (not a valid key).
};

/** @brief Physical mouse buttons recognized by `bgl::Input`. */
enum class MouseButton : uint8_t { Left = 0, Middle = 1, Right = 2, Count };

/** @brief Static polling API updated every frame by `Engine::PollEvents()`. */
class Input {
public:
    /**
     * @brief Checks whether a key is currently held down.
     * @param key Key identifier to query.
     * @return `true` while the key remains pressed.
     */
    [[nodiscard]] static bool IsKeyDown(Key key) noexcept;

    /**
     * @brief Checks whether a key transitioned from up to down during the current frame.
     * @param key Key identifier to query.
     * @return `true` only on the frame the key was pressed.
     */
    [[nodiscard]] static bool IsKeyPressed(Key key) noexcept;

    /**
     * @brief Checks whether a key transitioned from down to up during the current frame.
     * @param key Key identifier to query.
     * @return `true` only on the frame the key was released.
     */
    [[nodiscard]] static bool IsKeyReleased(Key key) noexcept;

    /**
     * @brief Checks whether a mouse button is currently held down.
     * @param button Mouse button to query.
     * @return `true` while the button remains pressed.
     */
    [[nodiscard]] static bool IsMouseDown(MouseButton button) noexcept;

    /**
     * @brief Checks whether a mouse button was pressed during the current frame.
     * @param button Mouse button to query.
     * @return `true` only on the frame the button went down.
     */
    [[nodiscard]] static bool IsMousePressed(MouseButton button) noexcept;

    /**
     * @brief Checks whether a mouse button was released during the current frame.
     * @param button Mouse button to query.
     * @return `true` only on the frame the button was released.
     */
    [[nodiscard]] static bool IsMouseReleased(MouseButton button) noexcept;

    /**
     * @brief Returns cursor coordinates in window pixels (origin at top-left corner).
     * @return 2D screen-space position `{x, y}`.
     */
    [[nodiscard]] static glm::vec2 MousePosition() noexcept;

    /**
     * @brief Returns cumulative cursor movement during the current frame in pixels.
     * @return Relative `{dx, dy}` cursor displacement.
     */
    [[nodiscard]] static glm::vec2 MouseDelta() noexcept;

    /**
     * @brief Returns vertical scroll wheel delta for the current frame.
     * @return Positive values indicate scrolling away from the user.
     */
    [[nodiscard]] static float MouseWheel() noexcept;
};

} // namespace bgl

#endif // BGL_PUBLIC_INPUT_HPP
