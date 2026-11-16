/**
 * @file TextShaper.cpp
 * @brief Unicode text shaping, measurement, and word wrapping via HarfBuzz and FontAtlas.
 */
#include "text/TextShaper.hpp"
#include <algorithm>
#include <cstdint>
#include <harfbuzz/hb.h>
#include <span>

namespace bgl::text {

namespace {

[[nodiscard]] uint32_t DecodeUtf8At(std::string_view text, size_t offset) noexcept {
    if (offset >= text.size()) {
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(text[offset]);
    if (b0 < 0x80u) {
        return b0;
    }
    auto cont = [&](size_t idx) -> uint32_t { return static_cast<unsigned char>(text[idx]) & 0x3Fu; };
    if ((b0 & 0xE0u) == 0xC0u && offset + 1 < text.size()) {
        return ((b0 & 0x1Fu) << 6u) | cont(offset + 1);
    }
    if ((b0 & 0xF0u) == 0xE0u && offset + 2 < text.size()) {
        return ((b0 & 0x0Fu) << 12u) | (cont(offset + 1) << 6u) | cont(offset + 2);
    }
    if ((b0 & 0xF8u) == 0xF0u && offset + 3 < text.size()) {
        return ((b0 & 0x07u) << 18u) | (cont(offset + 1) << 12u) | (cont(offset + 2) << 6u) | cont(offset + 3);
    }
    return b0;
}

[[nodiscard]] glm::vec2 ShapeInternal(FontAtlas& fontAtlas, std::string_view text, float startX, float startY,
                                      const glm::vec4& color, float scale, std::vector<TextQuad>* outQuads) {
    if (text.empty() || !fontAtlas.GetHarfBuzzFont()) {
        return glm::vec2{0.0f};
    }

    struct HbBufferGuard {
        hb_buffer_t* handle{nullptr};
        HbBufferGuard() noexcept : handle{hb_buffer_create()} {}
        ~HbBufferGuard() {
            if (handle) {
                hb_buffer_destroy(handle);
            }
        }
        HbBufferGuard(const HbBufferGuard&) = delete;
        HbBufferGuard& operator=(const HbBufferGuard&) = delete;
    };
    thread_local HbBufferGuard hbBuffer{};
    if (!hbBuffer.handle) {
        return glm::vec2{0.0f};
    }

    const float lineHeight{fontAtlas.GetLineHeight() * scale};
    const float ascender{fontAtlas.GetAscender() * scale};
    const bool hasFallbacks = fontAtlas.HasFallbackFaces();

    float currentY{startY};
    float maxLineWidth{0.0f};

    size_t lineStart{0};
    while (lineStart < text.size()) {
        size_t lineEnd{text.find('\n', lineStart)};
        if (lineEnd == std::string_view::npos) {
            lineEnd = text.size();
        }

        std::string_view line{text.substr(lineStart, lineEnd - lineStart)};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lineStart = lineEnd + 1;

        if (!line.empty()) {
            hb_buffer_reset(hbBuffer.handle);
            hb_buffer_add_utf8(hbBuffer.handle, line.data(), static_cast<int>(line.size()), 0,
                               static_cast<int>(line.size()));
            hb_buffer_guess_segment_properties(hbBuffer.handle);
            hb_shape(fontAtlas.GetHarfBuzzFont(), hbBuffer.handle, nullptr, 0);

            unsigned int glyphCount{0};
            const std::span<const hb_glyph_info_t> glyphInfos{hb_buffer_get_glyph_infos(hbBuffer.handle, &glyphCount),
                                                              glyphCount};
            const std::span<const hb_glyph_position_t> glyphPositions{
                hb_buffer_get_glyph_positions(hbBuffer.handle, &glyphCount), glyphCount};

            float currentX{startX};
            float baselineY{currentY + ascender};

            for (size_t i{0}; i < glyphInfos.size(); ++i) {
                const uint32_t primaryGlyph{glyphInfos[i].codepoint};
                bool usedFallback = false;
                const uint32_t scalar =
                    (primaryGlyph == 0 && hasFallbacks) ? DecodeUtf8At(line, glyphInfos[i].cluster) : 0u;
                const GlyphInfo& info{fontAtlas.GetGlyphOrFallback(primaryGlyph, scalar, usedFallback)};

                const float xOffset{(static_cast<float>(glyphPositions[i].x_offset) / 64.0f) * scale};
                const float yOffset{(static_cast<float>(glyphPositions[i].y_offset) / 64.0f) * scale};
                const float xAdvance = usedFallback
                                           ? (info.advance * scale)
                                           : ((static_cast<float>(glyphPositions[i].x_advance) / 64.0f) * scale);
                const float yAdvance =
                    usedFallback ? 0.0f : ((static_cast<float>(glyphPositions[i].y_advance) / 64.0f) * scale);

                if (outQuads && info.size.x > 0.0f && info.size.y > 0.0f) {
                    const float qx0{currentX + xOffset + info.bearing.x * scale};
                    const float qy0{baselineY - yOffset - info.bearing.y * scale};
                    const float qx1{qx0 + info.size.x * scale};
                    const float qy1{qy0 + info.size.y * scale};

                    outQuads->push_back(TextQuad{.posMin = glm::vec2{qx0, qy0},
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

    return glm::vec2{maxLineWidth, currentY - startY};
}

} // namespace

TextLayout TextShaper::Shape(FontAtlas& fontAtlas, std::string_view text, float startX, float startY,
                             const glm::vec4& color, float scale) {
    TextLayout layout{};
    layout.bounds = ShapeInternal(fontAtlas, text, startX, startY, color, scale, &layout.quads);
    return layout;
}

glm::vec2 TextShaper::Measure(FontAtlas& fontAtlas, std::string_view text, float scale) {
    return ShapeInternal(fontAtlas, text, 0.0f, 0.0f, glm::vec4{1.0f}, scale, nullptr);
}

std::vector<std::string> TextShaper::WrapText(FontAtlas& fontAtlas, std::string_view text, float maxWidth,
                                              float scale) {
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines;
    }
    if (maxWidth <= 0.0f || !fontAtlas.GetHarfBuzzFont()) {
        size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find('\n', start);
            std::string_view raw =
                (end == std::string_view::npos) ? text.substr(start) : text.substr(start, end - start);
            if (!raw.empty() && raw.back() == '\r') {
                raw.remove_suffix(1);
            }
            lines.emplace_back(raw);
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return lines;
    }

    size_t paraStart = 0;
    while (paraStart <= text.size()) {
        const size_t paraEnd = text.find('\n', paraStart);
        std::string_view para =
            (paraEnd == std::string_view::npos) ? text.substr(paraStart) : text.substr(paraStart, paraEnd - paraStart);
        if (!para.empty() && para.back() == '\r') {
            para.remove_suffix(1);
        }

        if (para.empty()) {
            lines.emplace_back();
        } else {
            std::string current;
            size_t pos = 0;
            while (pos < para.size()) {
                while (pos < para.size() && (para[pos] == ' ' || para[pos] == '\t')) {
                    ++pos;
                }
                if (pos >= para.size()) {
                    break;
                }
                size_t wordEnd = pos;
                while (wordEnd < para.size() && para[wordEnd] != ' ' && para[wordEnd] != '\t') {
                    ++wordEnd;
                }
                const std::string_view word = para.substr(pos, wordEnd - pos);

                std::string candidate = current.empty() ? std::string{word} : (current + " " + std::string{word});
                if (Measure(fontAtlas, candidate, scale).x <= maxWidth) {
                    current = std::move(candidate);
                } else if (!current.empty()) {
                    lines.push_back(std::move(current));
                    current.assign(word);
                } else {
                    // Break oversized single token cleanly along UTF-8 codepoint boundaries
                    size_t cut = 0;
                    while (cut < word.size()) {
                        size_t nextCut = cut + 1;
                        while (nextCut < word.size() && (static_cast<unsigned char>(word[nextCut]) & 0xC0u) == 0x80u) {
                            ++nextCut;
                        }
                        if (Measure(fontAtlas, word.substr(0, nextCut), scale).x > maxWidth && cut > 0) {
                            lines.emplace_back(word.substr(0, cut));
                            const std::string_view rest = word.substr(cut);
                            current.assign(rest);
                            break;
                        }
                        cut = nextCut;
                    }
                    if (cut >= word.size()) {
                        current.assign(word);
                    }
                }
                pos = wordEnd;
            }
            if (!current.empty()) {
                lines.push_back(std::move(current));
            }
        }

        if (paraEnd == std::string_view::npos) {
            break;
        }
        paraStart = paraEnd + 1;
    }
    return lines;
}

} // namespace bgl::text
