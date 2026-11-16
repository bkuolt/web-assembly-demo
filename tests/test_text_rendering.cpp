/**
 * @file test_text_rendering.cpp
 * @brief Unit tests for FontAtlas, FreeType rasterization, and HarfBuzz text shaping.
 */
#include <ft2build.h>
#include <gtest/gtest.h>
#include FT_FREETYPE_H
#include "text/EmbeddedFont.hpp"
#include "text/FontAtlas.hpp"
#include "text/TextShaper.hpp"
#include "ui/ImmediateUI.hpp"
#include <array>

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

TEST(TextRenderingTest, SdfFontAtlasInitializationAndGlyphRasterization) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    auto res = atlas.InitializeFromMemory(data, 24, 1024, true);
    ASSERT_TRUE(res.has_value()) << (res.has_value() ? "" : res.error());
    EXPECT_TRUE(atlas.IsSdf());
    EXPECT_EQ(atlas.GetPixelSize(), 24u);

    const uint32_t glyphIdx = FT_Get_Char_Index(atlas.GetFTFace(), 'A');
    const auto& glyph = atlas.GetGlyph(glyphIdx);
    EXPECT_GT(glyph.size.x, 0.0f);
    EXPECT_GT(glyph.size.y, 0.0f);
    EXPECT_GT(glyph.uvMax.x, glyph.uvMin.x);
    EXPECT_GT(glyph.uvMax.y, glyph.uvMin.y);
}

TEST(ImmediateUITest, EmbeddedShaderAndWidgetsAndMouseConsumption) {
    const std::string_view shaderSrc = ui::ImmediateUI::GetEmbeddedShaderSource();
    EXPECT_FALSE(shaderSrc.empty());
    EXPECT_NE(shaderSrc.find("UiUniforms"), std::string_view::npos);
    EXPECT_NE(shaderSrc.find("isSdf"), std::string_view::npos);
    EXPECT_NE(shaderSrc.find("fwidth"), std::string_view::npos);

    ui::ImmediateUI imgui;
    bool checkboxState = false;
    size_t dropdownIdx = 0;
    constexpr std::array<const char*, 3> options = {"Auto", "BC7", "ASTC"};

    // Layout inside BeginWindow("VRAM Settings", 10, 10, 300):
    //   Label:    y = 48 .. 72
    //   Button:   y = 72 .. 96   (center y = 84)
    //   Checkbox: y = 102 .. 126 (center y = 114)
    //   Dropdown: label at 132, box y = 149 .. 173 (center y = 161),
    //             popup y = 175 .. 247 (opt 0: 175..199, opt 1: 199..223 -> center y = 211)

    // Frame 1: click Button at (100, 84)
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 84.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("VRAM Settings", 10.0f, 10.0f, 300.0f);
    ui::UI::Label("VRAM: 12.5 MB");
    EXPECT_TRUE(ui::UI::Button("Apply"));
    EXPECT_FALSE(ui::UI::Checkbox("Mipmaps", checkboxState));
    EXPECT_FALSE(ui::UI::Dropdown("Compression", options, dropdownIdx));
    imgui.EndWindow();
    imgui.EndFrame();

    EXPECT_TRUE(imgui.WantsMouse());
    EXPECT_GT(imgui.GetVertexCount(), 0u);
    EXPECT_GT(imgui.GetIndexCount(), 0u);

    // Frame 2: click Checkbox at (100, 114)
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 114.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("VRAM Settings", 10.0f, 10.0f, 300.0f);
    ui::UI::Label("VRAM: 12.5 MB");
    EXPECT_FALSE(ui::UI::Button("Apply"));
    EXPECT_TRUE(ui::UI::Checkbox("Mipmaps", checkboxState));
    EXPECT_FALSE(ui::UI::Dropdown("Compression", options, dropdownIdx));
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_TRUE(checkboxState);

    // Frame 3: open Dropdown anchor at (100, 161)
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 161.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("VRAM Settings", 10.0f, 10.0f, 300.0f);
    ui::UI::Label("VRAM: 12.5 MB");
    EXPECT_FALSE(ui::UI::Button("Apply"));
    EXPECT_FALSE(ui::UI::Checkbox("Mipmaps", checkboxState));
    EXPECT_FALSE(ui::UI::Dropdown("Compression", options, dropdownIdx));
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_TRUE(imgui.WantsMouse());

    // Frame 4: click second dropdown option ("BC7") at (100, 211) inside popup overlay
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 211.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("VRAM Settings", 10.0f, 10.0f, 300.0f);
    ui::UI::Label("VRAM: 12.5 MB");
    EXPECT_FALSE(ui::UI::Button("Apply"));
    EXPECT_FALSE(ui::UI::Checkbox("Mipmaps", checkboxState));
    EXPECT_TRUE(ui::UI::Dropdown("Compression", options, dropdownIdx));
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_EQ(dropdownIdx, 1u);

    // Frame 5: mouse outside window and no widgets -> PMR arena reset leaves 0 verts and WantsMouse() == false
    imgui.BeginFrame(ui::UiInputState{.mouseX = 900.0f, .mouseY = 500.0f}, 1280, 720);
    imgui.EndFrame();
    EXPECT_EQ(imgui.GetVertexCount(), 0u);
    EXPECT_EQ(imgui.GetIndexCount(), 0u);
    EXPECT_FALSE(imgui.WantsMouse());
    EXPECT_EQ(ui::UI::GetContext(), nullptr);
}

TEST(TextRenderingTest, CrlfLineEndingsShapeWithoutExtraCrGlyph) {
    auto data = text::GetEmbeddedFontData();
    text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(data, 16, 1024).has_value());

    const auto lfLayout = text::TextShaper::Shape(atlas, "Hello\nWorld", 0.0f, 0.0f);
    const auto crlfLayout = text::TextShaper::Shape(atlas, "Hello\r\nWorld", 0.0f, 0.0f);
    EXPECT_EQ(lfLayout.quads.size(), crlfLayout.quads.size());
    EXPECT_FLOAT_EQ(lfLayout.bounds.x, crlfLayout.bounds.x);
    EXPECT_FLOAT_EQ(lfLayout.bounds.y, crlfLayout.bounds.y);
}

TEST(ImmediateUIAdvancedWidgetsTest, ShaderSourceIncludesSdfAndGlassFeatures) {
    const std::string_view src = ui::ImmediateUI::GetEmbeddedShaderSource();
    EXPECT_NE(src.find("sdRoundedBox"), std::string_view::npos);
    EXPECT_NE(src.find("hsv2rgb"), std::string_view::npos);
    EXPECT_NE(src.find("backdropTexture"), std::string_view::npos);
}

TEST(ImmediateUIAdvancedWidgetsTest, HsvConversionRoundtripAccurate) {
    const glm::vec3 samples[] = {
        {1.0f, 0.0f, 0.0f},
        {0.18f, 0.65f, 0.92f},
        {0.85f, 0.72f, 0.25f},
        {0.4f, 0.4f, 0.4f},
    };
    for (const glm::vec3& rgb : samples) {
        const glm::vec3 hsv = ui::ImmediateUI::RgbToHsv(rgb);
        const glm::vec3 back = ui::ImmediateUI::HsvToRgb(hsv);
        EXPECT_NEAR(back.r, rgb.r, 1e-4f);
        EXPECT_NEAR(back.g, rgb.g, 1e-4f);
        EXPECT_NEAR(back.b, rgb.b, 1e-4f);
    }
}

TEST(ImmediateUIAdvancedWidgetsTest, SliderHeaderSparklineScrollAndWindowDragWork) {
    ui::ImmediateUI imgui;
    float sliderVal = 0.0f;
    glm::vec3 pickedColor{0.2f, 0.6f, 0.9f};
    const std::array<float, 6> fpsHistory{58.0f, 60.0f, 61.5f, 59.0f, 62.0f, 60.5f};

    // Frame 1: Initial layout (single unclipped batch when scroll area is unused)
    imgui.BeginFrame(ui::UiInputState{}, 1280, 720);
    imgui.BeginWindow("Controls", 20.0f, 20.0f, 300.0f);
    // Header at y = 20..48; cursorY starts at 58
    EXPECT_TRUE(ui::UI::CollapsingHeader("Bloom Settings", true));         // y = 58..82, next cursor = 88
    EXPECT_FALSE(ui::UI::SliderFloat("Threshold", sliderVal, 0.0f, 2.0f)); // label 88..105, track 105..121
    ui::UI::Separator();
    ui::UI::Sparkline("FPS", fpsHistory, 0.0f, 120.0f, 36.0f);
    EXPECT_FALSE(ui::UI::ColorEdit3("Tint", pickedColor));
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_EQ(imgui.GetDrawCommandCount(), 1u);
    EXPECT_FLOAT_EQ(imgui.GetLastWindowRect().x, 20.0f);
    EXPECT_FLOAT_EQ(imgui.GetLastWindowRect().y, 20.0f);

    // Frame 2: Press on slider track around 75% width (x = 30 + 280 * 0.75 = 240, y = 113)
    imgui.BeginFrame(ui::UiInputState{.mouseX = 240.0f, .mouseY = 113.0f, .mouseDown = true, .mousePressed = true},
                     1280, 720);
    imgui.BeginWindow("Controls", 20.0f, 20.0f, 300.0f);
    EXPECT_TRUE(ui::UI::CollapsingHeader("Bloom Settings", true));
    EXPECT_TRUE(ui::UI::SliderFloat("Threshold", sliderVal, 0.0f, 2.0f));
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_GT(sliderVal, 1.3f);
    EXPECT_TRUE(imgui.WantsMouse());

    // Release slider
    imgui.BeginFrame(ui::UiInputState{.mouseX = 240.0f, .mouseY = 113.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("Controls", 20.0f, 20.0f, 300.0f);
    EXPECT_TRUE(ui::UI::CollapsingHeader("Bloom Settings", true));
    ui::UI::SliderFloat("Threshold", sliderVal, 0.0f, 2.0f);
    imgui.EndWindow();
    imgui.EndFrame();

    // Frame 3: Toggle collapsing header closed by clicking (100, 70)
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 70.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("Controls", 20.0f, 20.0f, 300.0f);
    const bool openAfterClick = ui::UI::CollapsingHeader("Bloom Settings", true);
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_FALSE(openAfterClick);

    // Frame 4 & 5: Drag window title bar from (100, 34) to (220, 94) (+120, +60)
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 34.0f, .mouseDown = true, .mousePressed = true}, 1280,
                     720);
    imgui.BeginWindow("Controls", 20.0f, 20.0f, 300.0f);
    imgui.EndWindow();
    imgui.EndFrame();

    imgui.BeginFrame(ui::UiInputState{.mouseX = 220.0f, .mouseY = 94.0f, .mouseDown = true}, 1280, 720);
    imgui.BeginWindow("Controls", 20.0f, 20.0f, 300.0f);
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_NEAR(imgui.GetLastWindowRect().x, 140.0f, 0.5f);
    EXPECT_NEAR(imgui.GetLastWindowRect().y, 80.0f, 0.5f);

    // Release drag and test ScrollArea scissor splitting
    imgui.BeginFrame(ui::UiInputState{.mouseX = 900.0f, .mouseY = 500.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("Scene List", 20.0f, 20.0f, 280.0f);
    ui::UI::BeginScrollArea("nodes", 80.0f);
    for (int i = 0; i < 8; ++i) {
        ui::UI::Label("Node Item");
    }
    ui::UI::EndScrollArea();
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_GE(imgui.GetDrawCommandCount(), 2u);
}

TEST(ImmediateUIAdvancedWidgetsTest, ColorEdit3PopupOpeningAndHsvDragUpdatesRgb) {
    ui::ImmediateUI imgui;
    glm::vec3 color{1.0f, 0.0f, 0.0f}; // pure red

    // Frame 1: Render ColorEdit3 at window (20, 20), width 260 -> cursor at (30, 58), contentW = 240.
    // Swatch rect: x = 30 + 240 - 48 = 222..270, y = 58 + 3 = 61..79 (center 246, 70)
    imgui.BeginFrame(ui::UiInputState{}, 1280, 720);
    imgui.BeginWindow("Palette", 20.0f, 20.0f, 260.0f);
    EXPECT_FALSE(ui::UI::ColorEdit3("Tint", color));
    imgui.EndWindow();
    imgui.EndFrame();

    // Click swatch at (246, 70) to open deferred HSV popup overlay
    imgui.BeginFrame(ui::UiInputState{.mouseX = 246.0f, .mouseY = 70.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("Palette", 20.0f, 20.0f, 260.0f);
    ui::UI::ColorEdit3("Tint", color);
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_TRUE(imgui.WantsMouse());

    // Popup placed below swatch: popW = 24+132+18 = 174, popH = 16+132+22 = 170, popX = 270-174 = 96, popY = 83
    // SV box at (104..236, 91..223); Hue bar at (244..262, 91..223)
    // Click Hue bar midpoint (~0.5 = cyan hue!) at (253, 157)
    imgui.BeginFrame(ui::UiInputState{.mouseX = 253.0f, .mouseY = 157.0f, .mouseDown = true, .mousePressed = true},
                     1280, 720);
    imgui.BeginWindow("Palette", 20.0f, 20.0f, 260.0f);
    const bool hueChanged = ui::UI::ColorEdit3("Tint", color);
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_TRUE(hueChanged);
    EXPECT_GT(color.g, 0.5f);
    EXPECT_GT(color.b, 0.5f);
    EXPECT_LT(color.r, 0.25f);
}

TEST(ImmediateUIAdvancedWidgetsTest, ScrollableDropdownAndProgressBarAndLoadingBannerWorkCleanly) {
    ui::ImmediateUI imgui;
    std::array<const char*, 20> manyModels = {
        "ToyCar (5.1 MB)",         "Sponza (52.6 MB)",        "RobotExpressive (0.5 MB)", "Soldier (2.1 MB)",
        "DamagedHelmet (3.6 MB)",  "AntiqueCamera (16.7 MB)", "BoomBox (10.1 MB)",        "WaterBottle (8.6 MB)",
        "Shoe (7.5 MB)",           "Avocado (7.7 MB)",        "BarramundiFish (11.9 MB)", "Lantern (1.2 MB)",
        "Corset (0.3 MB)",         "Fox (0.16 MB)",           "CesiumMan (0.42 MB)",      "BrainStem (3.05 MB)",
        "ClearCoatTest (0.25 MB)", "MorphTest (0.05 MB)",     "Duck (0.11 MB)",           "ExtraCustomModel (1.0 MB)",
    };
    size_t selectedModel = 0;
    bool blockedCheck = false;

    // Frame 1: click anchor at (100, 77) inside BeginWindow(10, 10, 320) to open scrollable dropdown
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 77.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("Model Browser", 10.0f, 10.0f, 320.0f);
    EXPECT_FALSE(ui::UI::Dropdown("glTF Model", manyModels, selectedModel));
    ui::UI::ProgressBar(0.5f, "Downloading ToyCar.glb: 2.5 / 5.1 MB (50%)");
    imgui.EndWindow();
    imgui.DrawLoadingBanner("Loading ToyCar...", 0.5f, "Decoding textures...", false, 1.0f);
    imgui.EndFrame();
    EXPECT_TRUE(imgui.WantsMouse());
    EXPECT_GE(imgui.GetDrawCommandCount(), 2u);

    // Frame 2: wheel down inside popup (91..355) to scroll to bottom items
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 180.0f, .wheelDelta = -5.0f}, 1280, 720);
    imgui.BeginWindow("Model Browser", 10.0f, 10.0f, 320.0f);
    EXPECT_FALSE(ui::UI::Dropdown("glTF Model", manyModels, selectedModel));
    ui::UI::ProgressBar(0.75f, "Building meshes...");
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_TRUE(imgui.WantsMouse());

    // Frame 3: click last scrolled item at (100, 343); verify checkboxes underneath do not trigger on popup close
    imgui.BeginFrame(ui::UiInputState{.mouseX = 100.0f, .mouseY = 343.0f, .mouseReleased = true}, 1280, 720);
    imgui.BeginWindow("Model Browser", 10.0f, 10.0f, 320.0f);
    const bool picked = ui::UI::Dropdown("glTF Model", manyModels, selectedModel);
    ui::UI::ProgressBar(1.0f, "Ready");
    for (int i = 0; i < 10; ++i) {
        ui::UI::Checkbox("Underneath", blockedCheck);
    }
    imgui.EndWindow();
    imgui.EndFrame();
    EXPECT_TRUE(picked);
    EXPECT_EQ(selectedModel, 19u);
    EXPECT_FALSE(blockedCheck);

    // Frame 4: unclipped ProgressBar + LoadingBanner preserve single draw call
    imgui.BeginFrame(ui::UiInputState{}, 1280, 720);
    imgui.BeginWindow("Progress Only", 10.0f, 10.0f, 320.0f);
    ui::UI::ProgressBar(0.42f, "Loading...");
    imgui.EndWindow();
    imgui.DrawLoadingBanner("Loaded!", 1.0f, "Ready", false, 0.8f);
    imgui.EndFrame();
    EXPECT_EQ(imgui.GetDrawCommandCount(), 1u);
}
