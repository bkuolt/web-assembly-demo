/**
 * @file TextShaper.cpp
 * @brief Unicode text shaping via HarfBuzz and FontAtlas glyph mapping.
 */
#include "text/TextShaper.hpp"
#include <algorithm>
#include <harfbuzz/hb.h>
#include <memory>
#include <span>

namespace bgl::text {

TextLayout TextShaper::Shape(FontAtlas& fontAtlas, std::string_view text, float startX, float startY,
                             const glm::vec4& color, float scale) {
    TextLayout layout{};
    if (text.empty() || !fontAtlas.GetHarfBuzzFont()) {
        return layout;
    }

    const std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)> hbBuffer{hb_buffer_create(), &hb_buffer_destroy};
    if (!hbBuffer)
        return layout;

    const float lineHeight{fontAtlas.GetLineHeight() * scale};
    const float ascender{fontAtlas.GetAscender() * scale};

    float currentY{startY};
    float maxLineWidth{0.0f};

    size_t lineStart{0};
    while (lineStart < text.size()) {
        size_t lineEnd{text.find('\n', lineStart)};
        if (lineEnd == std::string_view::npos) {
            lineEnd = text.size();
        }

        const std::string_view line{text.substr(lineStart, lineEnd - lineStart)};
        lineStart = lineEnd + 1;

        if (!line.empty()) {
            hb_buffer_clear_contents(hbBuffer.get());
            hb_buffer_add_utf8(hbBuffer.get(), line.data(), static_cast<int>(line.size()), 0,
                               static_cast<int>(line.size()));
            hb_buffer_guess_segment_properties(hbBuffer.get());
            hb_shape(fontAtlas.GetHarfBuzzFont(), hbBuffer.get(), nullptr, 0);

            unsigned int glyphCount{0};
            const std::span<const hb_glyph_info_t> glyphInfos{hb_buffer_get_glyph_infos(hbBuffer.get(), &glyphCount),
                                                              glyphCount};
            const std::span<const hb_glyph_position_t> glyphPositions{
                hb_buffer_get_glyph_positions(hbBuffer.get(), &glyphCount), glyphCount};

            float currentX{startX};
            float baselineY{currentY + ascender};

            for (size_t i{0}; i < glyphInfos.size(); ++i) {
                const uint32_t glyphIndex{glyphInfos[i].codepoint};
                const GlyphInfo& info{fontAtlas.GetGlyph(glyphIndex)};

                const float xOffset{(static_cast<float>(glyphPositions[i].x_offset) / 64.0f) * scale};
                const float yOffset{(static_cast<float>(glyphPositions[i].y_offset) / 64.0f) * scale};
                const float xAdvance{(static_cast<float>(glyphPositions[i].x_advance) / 64.0f) * scale};
                const float yAdvance{(static_cast<float>(glyphPositions[i].y_advance) / 64.0f) * scale};

                if (info.size.x > 0.0f && info.size.y > 0.0f) {
                    const float qx0{currentX + xOffset + info.bearing.x * scale};
                    const float qy0{baselineY - yOffset - info.bearing.y * scale};
                    const float qx1{qx0 + info.size.x * scale};
                    const float qy1{qy0 + info.size.y * scale};

                    layout.quads.push_back(TextQuad{.posMin = glm::vec2{qx0, qy0},
                                                    .posMax = glm::vec2{qx1, qy1},
                                                    .uvMin = info.uvMin,
                                                    .uvMax = info.uvMax,
                                                    .color = color});
                }

                currentX += xAdvance;
                baselineY += yAdvance;
            }

            maxLineWidth = std::max(maxLineWidth, currentX - startX);
        }

        currentY += lineHeight;
    }

    layout.bounds = glm::vec2{maxLineWidth, currentY - startY};
    return layout;
}

} // namespace bgl::text
