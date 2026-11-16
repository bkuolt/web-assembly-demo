/**
 * @file Types.hpp
 * @brief Fundamental public types of the bgl Engine API.
 */
#ifndef BGL_PUBLIC_TYPES_HPP
#define BGL_PUBLIC_TYPES_HPP

#include "core/Camera.hpp"
#include "core/Log.hpp"
#include "core/Types.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <span>

namespace bgl {

/** @brief Error-as-value result type returned by fallible public engine operations (`std::expected<T, std::string>`).
 */
template <typename T>
using Result = core::Result<T>;

/** @brief Navigation mode of the 3D scene camera (`Orbit` or `Fly`). */
using CameraMode = core::CameraMode;

/** @brief Interactive 3D camera supporting orbit, pan, zoom, fly navigation, frustum extraction, and viewpoints. */
using Camera = core::Camera;

} // namespace bgl

#endif // BGL_PUBLIC_TYPES_HPP
