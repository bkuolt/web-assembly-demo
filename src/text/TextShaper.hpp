/**
 * @file TextShaper.hpp
 * @brief Unicode text shaping via HarfBuzz and FontAtlas glyph mapping.
 */
#pragma once

#include <string_view>
#include <vector>
#include <glm/glm.hpp>
#include "text/FontAtlas.hpp"

namespace bgl::text {

/**
 * @struct TextQuad
 * @brief 2D Quad representing a single shaped glyph or solid rect to be rendered.
 */
struct TextQuad {
    glm::vec2 posMin{0.0f}; ///< Top-left corner in screen pixel coordinates
    glm::vec2 posMax{0.0f}; ///< Bottom-right corner in screen pixel coordinates
    glm::vec2 uvMin{0.0f};  ///< Top-left UV coordinates in font atlas
    glm::vec2 uvMax{0.0f};  ///< Bottom-right UV coordinates in font atlas
    glm::vec4 color{1.0f};  ///< Quad RGBA tint
};

/**
 * @struct TextLayout
 * @brief Output of text shaping containing layout quads and text bounding box.
 */
struct TextLayout {
    std::vector<TextQuad> quads;
    glm::vec2 bounds{0.0f}; ///< Bounding size (width, height in pixels)
};

/**
 * @class TextShaper
 * @brief High-level Unicode text shaper orchestrating HarfBuzz font shaping and multi-line layouts.
 */
class TextShaper {
public:
    TextShaper() = default;
    ~TextShaper() = default;

    /**
     * @brief Shapes a UTF-8 string into positioned glyph quads ready for GPU vertex buffer generation.
     * @param fontAtlas The active font atlas containing glyph rasterizations and metrics.
     * @param text UTF-8 string (may contain multi-line newlines `\n`).
     * @param startX Starting X coordinate on screen in pixels.
     * @param startY Starting Y coordinate on screen in pixels (top of first line).
     * @param color RGBA color tint.
     * @param scale Uniform scale factor (default 1.0f).
     * @return Shaped TextLayout with quads and dimensions.
     */
    [[nodiscard]] static TextLayout Shape(FontAtlas& fontAtlas,
                                          std::string_view text,
                                          float startX,
                                          float startY,
                                          const glm::vec4& color = glm::vec4(1.0f),
                                          float scale = 1.0f);
};

} // namespace bgl::text

