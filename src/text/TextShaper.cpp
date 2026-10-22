/**
 * @file TextShaper.cpp
 * @brief Unicode text shaping via HarfBuzz and FontAtlas glyph mapping.
 */
#include "text/TextShaper.hpp"
#include <harfbuzz/hb.h>
#include <algorithm>

namespace bgl::text {

TextLayout TextShaper::Shape(FontAtlas& fontAtlas,
                            std::string_view text,
                            float startX,
                            float startY,
                            const glm::vec4& color,
                            float scale) {
    TextLayout layout;
    if (text.empty() || !fontAtlas.GetHarfBuzzFont()) {
        return layout;
    }

    hb_buffer_t* hbBuffer = hb_buffer_create();
    if (!hbBuffer) return layout;

    const float lineHeight = fontAtlas.GetLineHeight() * scale;
    const float ascender = fontAtlas.GetAscender() * scale;

    float currentY = startY;
    float maxLineWidth = 0.0f;

    size_t lineStart = 0;
    while (lineStart < text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string_view::npos) {
            lineEnd = text.size();
        }

        std::string_view line = text.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;

        if (!line.empty()) {
            hb_buffer_clear_contents(hbBuffer);
            hb_buffer_add_utf8(hbBuffer, line.data(), static_cast<int>(line.size()), 0, static_cast<int>(line.size()));
            hb_buffer_guess_segment_properties(hbBuffer);
            hb_shape(fontAtlas.GetHarfBuzzFont(), hbBuffer, nullptr, 0);

            unsigned int glyphCount = 0;
            hb_glyph_info_t* glyphInfos = hb_buffer_get_glyph_infos(hbBuffer, &glyphCount);
            hb_glyph_position_t* glyphPositions = hb_buffer_get_glyph_positions(hbBuffer, &glyphCount);

            float currentX = startX;
            float baselineY = currentY + ascender;

            for (unsigned int i = 0; i < glyphCount; ++i) {
                uint32_t glyphIndex = glyphInfos[i].codepoint;
                const GlyphInfo& info = fontAtlas.GetGlyph(glyphIndex);

                float xOffset = (static_cast<float>(glyphPositions[i].x_offset) / 64.0f) * scale;
                float yOffset = (static_cast<float>(glyphPositions[i].y_offset) / 64.0f) * scale;
                float xAdvance = (static_cast<float>(glyphPositions[i].x_advance) / 64.0f) * scale;
                float yAdvance = (static_cast<float>(glyphPositions[i].y_advance) / 64.0f) * scale;

                if (info.size.x > 0.0f && info.size.y > 0.0f) {
                    float qx0 = currentX + xOffset + info.bearing.x * scale;
                    float qy0 = baselineY - yOffset - info.bearing.y * scale;
                    float qx1 = qx0 + info.size.x * scale;
                    float qy1 = qy0 + info.size.y * scale;

                    layout.quads.push_back(TextQuad{
                        .posMin = glm::vec2(qx0, qy0),
                        .posMax = glm::vec2(qx1, qy1),
                        .uvMin = info.uvMin,
                        .uvMax = info.uvMax,
                        .color = color
                    });
                }

                currentX += xAdvance;
                baselineY += yAdvance;
            }

            maxLineWidth = std::max(maxLineWidth, currentX - startX);
        }

        currentY += lineHeight;
    }

    hb_buffer_destroy(hbBuffer);

    layout.bounds = glm::vec2(maxLineWidth, currentY - startY);
    return layout;
}

} // namespace bgl::text

