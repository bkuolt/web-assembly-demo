/**
 * @file EmbeddedFont.hpp
 * @brief Built-in TrueType font data for zero-dependency standalone text rendering.
 */
#ifndef BGL_TEXT_EMBEDDEDFONT_HPP
#define BGL_TEXT_EMBEDDEDFONT_HPP

#include <cstdint>
#include <span>

namespace bgl::text {

/**
 * @brief Retrieves the embedded TrueType font binary data (Liberation Mono Regular).
 * @return Read-only span over the font bytes.
 */
[[nodiscard]] std::span<const uint8_t> GetEmbeddedFontData() noexcept;

} // namespace bgl::text

#endif // BGL_TEXT_EMBEDDEDFONT_HPP
