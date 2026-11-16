/**
 * @file Types.hpp
 * @brief Fundamental public types of the bgl Engine API.
 */
#pragma once

#include "core/Log.hpp"
#include "core/Types.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace bgl {

/** @brief Error-as-value result type used by all fallible public functions (std::expected<T, std::string>). */
template <typename T>
using Result = core::Result<T>;

} // namespace bgl
