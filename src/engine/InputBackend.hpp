/**
 * @file InputBackend.hpp
 * @brief Internal platform input backend hooks feeding engine::InputState.
 */
#ifndef BGL_ENGINE_INPUTBACKEND_HPP
#define BGL_ENGINE_INPUTBACKEND_HPP

#if defined(BGL_PLATFORM_NATIVE)
union SDL_Event;

namespace bgl::engine {

void BeginInputFrame() noexcept;
void FeedSdlEvent(const SDL_Event& event) noexcept;
void SyncKeyboardState() noexcept;

} // namespace bgl::engine
#endif

#endif // BGL_ENGINE_INPUTBACKEND_HPP
