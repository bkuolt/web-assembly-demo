/**
 * @file Sprite.hpp
 * @brief Public 2D Sprite, AnimatedSprite, SpriteClip, and 3D Billboard types for `bgl::Engine`.
 */
#ifndef BGL_PUBLIC_SPRITE_HPP
#define BGL_PUBLIC_SPRITE_HPP

#include "bgl/Types.hpp"
#include "core/Sprite.hpp"

namespace bgl {

/// Alpha/additive compositing mode for 2D sprites and 3D HDR billboards.
using SpriteBlend = core::SpriteBlendMode;
/// Orientation behavior for camera-facing 3D billboards.
using BillboardMode = core::BillboardMode;
/// Animation playback mode (`Loop`, `Once`, `PingPong`).
using SpritePlayback = core::SpritePlaybackMode;
/// Single atlas sub-rectangle UV frame with display duration.
using SpriteFrame = core::SpriteFrame;
/// Named sequence of sprite-sheet frames and playback mode.
using SpriteClip = core::SpriteClip;
/// CPU RGBA texture handle uploaded lazily to WebGPU.
using SpriteTexture = core::SpriteTexture;
/// Reusable clip animation controller.
using SpriteAnimator = core::SpriteAnimator;
/// 2D screen-space quad sprite rendered in the crisp swapchain pass.
using Sprite = core::Sprite;
/// 2D sprite paired with a `SpriteAnimator` controller.
using AnimatedSprite = core::AnimatedSprite;
/// 3D world-space camera-facing quad rendered in the HDR transparent pass.
using Billboard = core::Billboard;
/// 3D billboard paired with a `SpriteAnimator` controller.
using AnimatedBillboard = core::AnimatedBillboard;

} // namespace bgl

#endif // BGL_PUBLIC_SPRITE_HPP
