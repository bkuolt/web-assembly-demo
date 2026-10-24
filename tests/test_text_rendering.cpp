/**
 * @file test_text_rendering.cpp
 * @brief Unit tests for FontAtlas, FreeType rasterization, and HarfBuzz text shaping.
 */
#include <gtest/gtest.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "text/FontAtlas.hpp"
#include "text/TextShaper.hpp"
#include "text/EmbeddedFont.hpp"

using namespace bgl;

TEST(TextRenderingTest, EmbeddedFontAvailable) {
    auto data = text::GetEmbeddedFontData();
    ASSERT_FALSE(data.empty());
    EXPECT_GT(data.size(), 100000); // Liberation Mono is ~319 KB
}

TEST(TextRenderingTest, FontAtlasInitialization) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    auto res = atlas.InitializeFromMemory(data, 18, 1024);
    ASSERT_TRUE(res.has_value()) << "Failed to init FontAtlas: " << res.error();

    EXPECT_EQ(atlas.GetPixelSize(), 18u);
    EXPECT_GT(atlas.GetLineHeight(), 18.0f);
    EXPECT_GT(atlas.GetAscender(), 0.0f);
    EXPECT_LT(atlas.GetDescender(), 0.0f);
    EXPECT_NE(atlas.GetFTFace(), nullptr);
    EXPECT_NE(atlas.GetHarfBuzzFont(), nullptr);
}

TEST(TextRenderingTest, GlyphRasterization) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 20, 1024).has_value());

    // Space character (glyph index should exist, advance > 0)
    const auto& glyphA = atlas.GetGlyph(FT_Get_Char_Index(atlas.GetFTFace(), 'A'));
    EXPECT_GT(glyphA.size.x, 0.0f);
    EXPECT_GT(glyphA.size.y, 0.0f);
    EXPECT_GT(glyphA.advance, 0.0f);
    EXPECT_GT(glyphA.uvMax.x, glyphA.uvMin.x);
    EXPECT_GT(glyphA.uvMax.y, glyphA.uvMin.y);

    // Number '7'
    const auto& glyph7 = atlas.GetGlyph(FT_Get_Char_Index(atlas.GetFTFace(), '7'));
    EXPECT_GT(glyph7.size.x, 0.0f);
    EXPECT_GT(glyph7.advance, 0.0f);
}

TEST(TextRenderingTest, HarfBuzzTextShaping) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 16, 1024).has_value());

    std::string textToShape = "bgl Engine v0.6.8 [WebGPU]";
    auto layout = text::TextShaper::Shape(atlas, textToShape, 10.0f, 10.0f, glm::vec4(1.0f));

    EXPECT_FALSE(layout.quads.empty());
    EXPECT_GT(layout.bounds.x, 50.0f);
    EXPECT_GT(layout.bounds.y, 10.0f);

    for (const auto& q : layout.quads) {
        EXPECT_GT(q.posMax.x, q.posMin.x);
        EXPECT_GT(q.posMax.y, q.posMin.y);
        EXPECT_GE(q.uvMax.x, q.uvMin.x);
        EXPECT_GE(q.uvMax.y, q.uvMin.y);
    }
}

TEST(TextRenderingTest, MultiLineTextShaping) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 16, 1024).has_value());

    std::string multiLine = "Line 1: Performance 60 FPS\nLine 2: Camera [0, 0, 0]\nLine 3: Quadtree Visible";
    auto layout = text::TextShaper::Shape(atlas, multiLine, 0.0f, 0.0f);

    EXPECT_GT(layout.quads.size(), 30u);
    // Height should span at least 3 lines
    EXPECT_GE(layout.bounds.y, atlas.GetLineHeight() * 2.5f);
}

TEST(TextRenderingTest, EmptyStringReturnsZeroQuads) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 16, 1024).has_value());

    auto layout = text::TextShaper::Shape(atlas, "", 0.0f, 0.0f);
    EXPECT_TRUE(layout.quads.empty());
    EXPECT_FLOAT_EQ(layout.bounds.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.bounds.y, 0.0f);
}

TEST(TextRenderingTest, WhitespaceOnlyHandling) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 16, 1024).has_value());

    // Spaces advance cursor position without generating visible textured quads
    auto layout = text::TextShaper::Shape(atlas, "     ", 0.0f, 0.0f);
    EXPECT_TRUE(layout.quads.empty());
    EXPECT_GT(layout.bounds.x, 0.0f); // Cursor advanced
}

TEST(TextRenderingTest, UnicodeUtf8SpecialSymbols) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 18, 1024).has_value());

    // UTF-8 string with accented characters and symbols
    std::string unicodeText = "Temp: 24.5°C | Alpha: α | Beta: β";
    auto layout = text::TextShaper::Shape(atlas, unicodeText, 0.0f, 0.0f);
    EXPECT_FALSE(layout.quads.empty());
    EXPECT_GT(layout.bounds.x, 100.0f);
}

TEST(TextRenderingTest, CustomColorTintPreserved) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 16, 1024).has_value());

    const glm::vec4 customColor(0.2f, 0.8f, 1.0f, 0.75f);
    auto layout = text::TextShaper::Shape(atlas, "TintedTest", 0.0f, 0.0f, customColor);
    ASSERT_FALSE(layout.quads.empty());

    for (const auto& quad : layout.quads) {
        EXPECT_FLOAT_EQ(quad.color.r, 0.2f);
        EXPECT_FLOAT_EQ(quad.color.g, 0.8f);
        EXPECT_FLOAT_EQ(quad.color.b, 1.0f);
        EXPECT_FLOAT_EQ(quad.color.a, 0.75f);
    }
}

