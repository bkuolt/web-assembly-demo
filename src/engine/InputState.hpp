/**
 * @file InputState.hpp
 * @brief Internal frame-buffered keyboard and mouse state backing bgl::Input.
 */
#pragma once

#include "bgl/Input.hpp"
#include <array>
#include <cstddef>
#include <glm/glm.hpp>

namespace bgl::engine {

class InputState {
public:
    /** @brief Call once per frame BEFORE feeding new events. */
    void BeginFrame() noexcept {
        previousKeys_ = keys_;
        previousMouse_ = mouse_;
        mouseDelta_ = glm::vec2{0.0f};
        wheel_ = 0.0f;
    }

    void SetKey(Key key, bool down) noexcept {
        if (const auto i = Index(key); i < keys_.size()) {
            keys_[i] = down;
        }
    }

    void SetMouseButton(MouseButton button, bool down) noexcept {
        if (const auto i = static_cast<size_t>(button); i < mouse_.size()) {
            mouse_[i] = down;
        }
    }

    void AddMouseMotion(glm::vec2 position, glm::vec2 delta) noexcept {
        mousePosition_ = position;
        mouseDelta_ += delta;
    }

    void SetMousePosition(glm::vec2 position) noexcept { mousePosition_ = position; }

    void AddWheel(float y) noexcept { wheel_ += y; }

    [[nodiscard]] bool IsKeyDown(Key k) const noexcept { return Get(keys_, Index(k)); }
    [[nodiscard]] bool IsKeyPressed(Key k) const noexcept {
        return Get(keys_, Index(k)) && !Get(previousKeys_, Index(k));
    }
    [[nodiscard]] bool IsKeyReleased(Key k) const noexcept {
        return !Get(keys_, Index(k)) && Get(previousKeys_, Index(k));
    }
    [[nodiscard]] bool IsMouseDown(MouseButton b) const noexcept { return Get(mouse_, static_cast<size_t>(b)); }
    [[nodiscard]] bool IsMousePressed(MouseButton b) const noexcept {
        return Get(mouse_, static_cast<size_t>(b)) && !Get(previousMouse_, static_cast<size_t>(b));
    }
    [[nodiscard]] bool IsMouseReleased(MouseButton b) const noexcept {
        return !Get(mouse_, static_cast<size_t>(b)) && Get(previousMouse_, static_cast<size_t>(b));
    }
    [[nodiscard]] glm::vec2 MousePosition() const noexcept { return mousePosition_; }
    [[nodiscard]] glm::vec2 MouseDelta() const noexcept { return mouseDelta_; }
    [[nodiscard]] float MouseWheel() const noexcept { return wheel_; }

private:
    static constexpr size_t kKeyCount = static_cast<size_t>(Key::Count);
    static constexpr size_t kMouseCount = static_cast<size_t>(MouseButton::Count);

    [[nodiscard]] static size_t Index(Key k) noexcept { return static_cast<size_t>(k); }

    template <size_t N>
    [[nodiscard]] static bool Get(const std::array<bool, N>& a, size_t i) noexcept {
        return i < N && a[i];
    }

    std::array<bool, kKeyCount> keys_{};
    std::array<bool, kKeyCount> previousKeys_{};
    std::array<bool, kMouseCount> mouse_{};
    std::array<bool, kMouseCount> previousMouse_{};
    glm::vec2 mousePosition_{0.0f};
    glm::vec2 mouseDelta_{0.0f};
    float wheel_{0.0f};
};

/** @brief Process-wide input state read by bgl::Input and written by the engine event pump. */
[[nodiscard]] InputState& GetInputState() noexcept;

} // namespace bgl::engine
