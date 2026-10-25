/**
 * @file EmbeddedFont.hpp
 * @brief Built-in TrueType font data for zero-dependency standalone text rendering.
 */
#pragma once

#include <cstdint>
#include <span>

namespace bgl::text {

/**
 * @brief Retrieves the embedded TrueType font binary data (Liberation Mono Regular).
 * @return Read-only span over the font bytes.
 */
[[nodiscard]] std::span<const uint8_t> GetEmbeddedFontData() noexcept;

} // namespace bgl::text
