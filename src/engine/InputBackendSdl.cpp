/**
 * @file InputBackendSdl.cpp
 * @brief Native SDL2 backend translating window/keyboard/mouse events into engine::InputState.
 */
#include "engine/InputBackend.hpp"

#if defined(BGL_PLATFORM_NATIVE)
#include "engine/InputState.hpp"
#include <SDL2/SDL.h>

namespace bgl::engine {

namespace {

[[nodiscard]] SDL_Scancode ToScancode(Key key) noexcept {
    const auto code = static_cast<uint16_t>(key);
    if (key >= Key::A && key <= Key::Z) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (code - static_cast<uint16_t>(Key::A)));
    }
    if (key >= Key::F1 && key <= Key::F12) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (code - static_cast<uint16_t>(Key::F1)));
    }
    if (key >= Key::Num1 && key <= Key::Num9) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (code - static_cast<uint16_t>(Key::Num1)));
    }
    switch (key) {
    case Key::Num0:
        return SDL_SCANCODE_0;
    case Key::Space:
        return SDL_SCANCODE_SPACE;
    case Key::Enter:
        return SDL_SCANCODE_RETURN;
    case Key::Escape:
        return SDL_SCANCODE_ESCAPE;
    case Key::Tab:
        return SDL_SCANCODE_TAB;
    case Key::Backspace:
        return SDL_SCANCODE_BACKSPACE;
    case Key::LeftShift:
        return SDL_SCANCODE_LSHIFT;
    case Key::LeftCtrl:
        return SDL_SCANCODE_LCTRL;
    case Key::LeftAlt:
        return SDL_SCANCODE_LALT;
    case Key::Up:
        return SDL_SCANCODE_UP;
    case Key::Down:
        return SDL_SCANCODE_DOWN;
    case Key::Left:
        return SDL_SCANCODE_LEFT;
    case Key::Right:
        return SDL_SCANCODE_RIGHT;
    default:
        return SDL_SCANCODE_UNKNOWN;
    }
}

[[nodiscard]] MouseButton ToMouseButton(Uint8 sdlButton) noexcept {
    switch (sdlButton) {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    default:
        return MouseButton::Count;
    }
}

} // namespace

void BeginInputFrame() noexcept {
    GetInputState().BeginFrame();
}

void FeedSdlEvent(const SDL_Event& event) noexcept {
    auto& state = GetInputState();
    if (event.type == SDL_MOUSEMOTION) {
        state.AddMouseMotion({static_cast<float>(event.motion.x), static_cast<float>(event.motion.y)},
                             {static_cast<float>(event.motion.xrel), static_cast<float>(event.motion.yrel)});
    } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
        state.SetMousePosition({static_cast<float>(event.button.x), static_cast<float>(event.button.y)});
        const MouseButton btn = ToMouseButton(event.button.button);
        if (btn != MouseButton::Count) {
            state.SetMouseButton(btn, event.type == SDL_MOUSEBUTTONDOWN);
        }
    } else if (event.type == SDL_MOUSEWHEEL) {
        const float sign = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
        state.AddWheel(static_cast<float>(event.wheel.y) * sign);
    } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        state.ClearButtons();
    }
}

void SyncKeyboardState() noexcept {
    int numKeys{0};
    const Uint8* sdlKeys = SDL_GetKeyboardState(&numKeys);
    if (!sdlKeys) {
        return;
    }
    auto& state = GetInputState();
    for (uint16_t i = 1; i < static_cast<uint16_t>(Key::Count); ++i) {
        const Key key = static_cast<Key>(i);
        const SDL_Scancode sc = ToScancode(key);
        if (sc != SDL_SCANCODE_UNKNOWN && static_cast<int>(sc) < numKeys) {
            state.SetKey(key, sdlKeys[sc] != 0);
        }
    }
}

} // namespace bgl::engine
#endif
