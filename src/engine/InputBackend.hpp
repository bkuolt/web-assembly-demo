/**
 * @file InputBackend.hpp
 * @brief Internal platform input backend hooks feeding engine::InputState.
 */
#pragma once

#if defined(BGL_PLATFORM_NATIVE)
union SDL_Event;

namespace bgl::engine {

void BeginInputFrame() noexcept;
void FeedSdlEvent(const SDL_Event& event) noexcept;
void SyncKeyboardState() noexcept;

} // namespace bgl::engine
#endif
