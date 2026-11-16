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

#include "text/TextRenderer.hpp"

TEST(FontLibraryTest, DiscoverInspectFindAndEmbeddedAliasWork) {
    bgl::text::FontLibrary::ClearCache();
    const std::array<std::filesystem::path, 1> extraDirs{"assets/fonts"};
    const auto catalog = bgl::text::FontLibrary::Discover(extraDirs);
    ASSERT_FALSE(catalog.empty());

    const auto inspectRes = bgl::text::FontLibrary::InspectFile("assets/fonts/LiberationMono-Regular.ttf");
    ASSERT_TRUE(inspectRes.has_value()) << inspectRes.error();
    ASSERT_FALSE(inspectRes->empty());
    EXPECT_EQ(inspectRes->front().family, "Liberation Mono");
    EXPECT_TRUE(inspectRes->front().isMonospace);
    EXPECT_FALSE(inspectRes->front().isEmbedded);

    const auto byPath = bgl::text::FontLibrary::Find("assets/fonts/LiberationMono-Regular.ttf");
    ASSERT_TRUE(byPath.has_value()) << byPath.error();
    EXPECT_EQ(byPath->family, "Liberation Mono");

    const auto byFamily = bgl::text::FontLibrary::Find("Liberation Mono");
    ASSERT_TRUE(byFamily.has_value()) << byFamily.error();

    const auto embedded = bgl::text::FontLibrary::Find("embedded");
    ASSERT_TRUE(embedded.has_value()) << embedded.error();
    EXPECT_TRUE(embedded->isEmbedded);

    const auto missing = bgl::text::FontLibrary::Find("NonExistentFontFamily_XYZ_9999");
    EXPECT_FALSE(missing.has_value());

    bgl::text::FontAtlas queryAtlas;
    EXPECT_FALSE(
        queryAtlas.InitializeWithQuery(bgl::text::FontQuery{.nameOrPath = "NonExistentFont_XYZ"}, 20, 512, false, false)
            .has_value());
    EXPECT_TRUE(
        queryAtlas.InitializeWithQuery(bgl::text::FontQuery{.nameOrPath = "NonExistentFont_XYZ"}, 20, 512, false, true)
            .has_value());
    EXPECT_FALSE(queryAtlas.GetFamilyName().empty());
}

TEST(TextShaperTest, MeasureAndWordWrappingProduceValidBounds) {
    bgl::text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromFile("assets/fonts/LiberationMono-Regular.ttf", 24, 1024, true).has_value());

    EXPECT_FLOAT_EQ(bgl::text::TextShaper::Measure(atlas, "").x, 0.0f);
    EXPECT_FLOAT_EQ(bgl::text::TextShaper::Measure(atlas, "").y, 0.0f);

    const glm::vec2 singleMeasure = bgl::text::TextShaper::Measure(atlas, "Hello World", 0.75f);
    const bgl::text::TextLayout shaped =
        bgl::text::TextShaper::Shape(atlas, "Hello World", 0.0f, 0.0f, glm::vec4{1.0f}, 0.75f);
    EXPECT_NEAR(singleMeasure.x, shaped.bounds.x, 0.05f);
    EXPECT_NEAR(singleMeasure.y, shaped.bounds.y, 0.05f);

    const glm::vec2 twoLineMeasure = bgl::text::TextShaper::Measure(atlas, "Line One\nLine Two", 0.75f);
    EXPECT_GT(twoLineMeasure.y, singleMeasure.y * 1.5f);

    const auto wrapped = bgl::text::TextShaper::WrapText(
        atlas, "The quick brown fox jumps over the lazy dog\n\nSecond paragraph here.", 110.0f, 0.65f);
    EXPECT_GE(wrapped.size(), 4u);

    const auto embeddedBytes = bgl::text::GetEmbeddedFontData();
    EXPECT_TRUE(atlas.AddFallbackFromMemory(embeddedBytes).has_value());
    EXPECT_TRUE(atlas.AddFallbackFromMemory(embeddedBytes).has_value());
    EXPECT_TRUE(atlas.AddFallbackFromMemory(embeddedBytes).has_value());
    EXPECT_TRUE(atlas.AddFallbackFromMemory(embeddedBytes).has_value());
    EXPECT_FALSE(atlas.AddFallbackFromMemory(embeddedBytes).has_value());
}

TEST(TextRendererTest, MeasureTextAndAlignedDrawWithoutGpu) {
    bgl::text::TextRenderer renderer;
    EXPECT_FLOAT_EQ(renderer.MeasureText("Hello").x, 0.0f);
    renderer.DrawTextAligned("SafeNoAtlas", 50.0f, 25.0f, 0.5f);
    EXPECT_EQ(renderer.GetQueuedVertexCount(), 0u);

    bgl::text::FontAtlas atlas;
    ASSERT_TRUE(atlas.InitializeFromMemory(bgl::text::GetEmbeddedFontData(), 24, 512, false).has_value());
    renderer.SetFontAtlas(atlas);
    const glm::vec2 sz = renderer.MeasureText("Hello Engine");
    EXPECT_GT(sz.x, 10.0f);
    EXPECT_GT(sz.y, 10.0f);

    renderer.DrawTextAligned("Centered", 100.0f, 50.0f, 0.5f);
    EXPECT_GT(renderer.GetQueuedVertexCount(), 0u);
    renderer.Shutdown();
    EXPECT_EQ(renderer.GetQueuedVertexCount(), 0u);
}

TEST(ImmediateUIHoverAndTooltipTest, OptionalHoverEffectsTooltipsAndSparklineCrosshair) {
    bgl::ui::ImmediateUI ui;
    ui.Style().enableHoverEffects = true;

    const std::array<float, 5> samples{10.0f, 20.0f, 15.0f, 30.0f, 25.0f};

    // Frame 1: hovered button with hover effects enabled & tooltip queued
    bgl::ui::UiInputState hoverInput{};
    hoverInput.mouseX = 100.0f;
    hoverInput.mouseY = 82.0f;
    ui.BeginFrame(hoverInput, 800, 600, 0.05f);
    ui.BeginWindow("HoverWin", 10.0f, 10.0f, 260.0f);
    ui.Label("Static Info");
    ui.Separator();
    EXPECT_FALSE(ui.Button("ActionBtn"));
    EXPECT_TRUE(ui.IsItemHovered());
    ui.Tooltip("Helpful button description");
    ui.ProgressBar(0.45f, "Progress");
    ui.Sparkline("FPS", samples, 0.0f, 40.0f, 40.0f);
    ui.EndWindow();
    ui.EndFrame();
    const size_t hoveredVerts = ui.GetVertexCount();

    // Hovering over the sparkline directly activates crosshair indicator vertices
    bgl::ui::ImmediateUI uiSparkHover;
    uiSparkHover.Style().enableHoverEffects = true;
    const size_t sparkNoHoverVerts = [&]() {
        bgl::ui::UiInputState in{};
        uiSparkHover.BeginFrame(in, 800, 600, 0.1f);
        uiSparkHover.BeginWindow("SparkWin", 10.0f, 10.0f, 260.0f);
        uiSparkHover.Sparkline("FPS", samples, 0.0f, 40.0f, 40.0f);
        uiSparkHover.EndWindow();
        uiSparkHover.EndFrame();
        return uiSparkHover.GetVertexCount();
    }();
    bgl::ui::UiInputState sparkIn{};
    sparkIn.mouseX = 120.0f;
    sparkIn.mouseY = 72.0f;
    uiSparkHover.BeginFrame(sparkIn, 800, 600, 0.1f);
    uiSparkHover.BeginWindow("SparkWin", 10.0f, 10.0f, 260.0f);
    uiSparkHover.Sparkline("FPS", samples, 0.0f, 40.0f, 40.0f);
    EXPECT_TRUE(uiSparkHover.IsItemHovered());
    uiSparkHover.EndWindow();
    uiSparkHover.EndFrame();
    EXPECT_GT(uiSparkHover.GetVertexCount(), sparkNoHoverVerts);

    // PushHoverEnabled(false) suppresses hover glow without suppressing hover detection
    bgl::ui::ImmediateUI uiSuppressed;
    uiSuppressed.Style().enableHoverEffects = true;
    uiSuppressed.BeginFrame(hoverInput, 800, 600, 0.05f);
    uiSuppressed.BeginWindow("HoverWin", 10.0f, 10.0f, 260.0f);
    uiSuppressed.PushHoverEnabled(false);
    EXPECT_FALSE(uiSuppressed.AreHoverEffectsEnabled());
    uiSuppressed.Label("Static Info");
    uiSuppressed.Separator();
    EXPECT_FALSE(uiSuppressed.Button("ActionBtn"));
    EXPECT_TRUE(uiSuppressed.IsItemHovered());
    uiSuppressed.ProgressBar(0.45f, "Progress");
    uiSuppressed.Sparkline("FPS", samples, 0.0f, 40.0f, 40.0f);
    uiSuppressed.PopHoverEnabled();
    EXPECT_TRUE(uiSuppressed.AreHoverEffectsEnabled());
    uiSuppressed.EndWindow();
    uiSuppressed.EndFrame();
    EXPECT_LT(uiSuppressed.GetVertexCount(), hoveredVerts);
}

TEST(ImmediateUIMultiFontTest, LoadAndSwitchFontsSplitsDrawBatches) {
    bgl::ui::ImmediateUI ui;
    ASSERT_TRUE(ui.SetDefaultFont("embedded", 24.0f));
    EXPECT_TRUE(ui.LoadFont("mono18", "assets/fonts/LiberationMono-Regular.ttf", 18.0f).has_value());
    EXPECT_FALSE(ui.LoadFont("missing", "NonExistentFont_XYZ_404", 20.0f).has_value());

    bgl::ui::UiInputState in{};
    ui.BeginFrame(in, 800, 600, 0.016f);
    ui.BeginWindow("FontTest", 10.0f, 10.0f, 260.0f);
    EXPECT_EQ(ui.GetActiveFontName(), "default");
    ui.Label("Default Line");
    EXPECT_TRUE(ui.PushFont("mono18"));
    EXPECT_EQ(ui.GetActiveFontName(), "mono18");
    ui.Label("Secondary Font Line");
    ui.PopFont();
    EXPECT_EQ(ui.GetActiveFontName(), "default");
    EXPECT_FALSE(ui.PushFont("unknown_slot"));
    ui.Label("Back To Default");
    ui.EndWindow();
    ui.EndFrame();

    EXPECT_GE(ui.GetDrawCommandCount(), 3u);
    EXPECT_GT(ui.GetVertexCount(), 0u);
}

TEST(ImmediateUIModalAndLoadingTest, QueuedModalBlocksBackgroundAndInvokesCallback) {
    bgl::ui::ImmediateUI ui;
    ASSERT_TRUE(ui.SetDefaultFont("embedded", 24.0f));

    int resultBtn = -1;
    constexpr std::array<std::string_view, 2> kButtons{"Cancel", "OK"};
    ui.ShowMessageBox("confirm_modal", "Confirm Action", "Apply setting changes?", bgl::ui::MessageBoxType::Confirm,
                      kButtons, [&resultBtn](int idx) { resultBtn = idx; });
    EXPECT_TRUE(ui.HasOpenModal());

    // Frame 1: click directly on background button behind modal -> blocked
    bgl::ui::UiInputState bgClick{};
    bgClick.mouseX = 100.0f;
    bgClick.mouseY = 54.0f;
    bgClick.mouseReleased = true;
    bool bgPressed = false;
    ui.BeginFrame(bgClick, 800, 600, 0.016f);
    ui.BeginWindow("BackgroundWin", 10.0f, 10.0f, 260.0f);
    if (ui.Button("BgButton")) {
        bgPressed = true;
    }
    ui.EndWindow();
    ui.EndFrame();
    EXPECT_FALSE(bgPressed);
    EXPECT_TRUE(ui.WantsMouse());
    EXPECT_TRUE(ui.HasOpenModal());

    // Compute exact "OK" button location from lastItemRect_ (cardRect)
    const bgl::ui::UiRect card = ui.GetLastItemRect();
    const float okX = card.x + card.w - 18.0f - 64.0f;
    const float okY = card.y + card.h - 18.0f - 13.0f + 2.0f;

    bgl::ui::UiInputState modalPress{};
    modalPress.mouseX = okX;
    modalPress.mouseY = okY;
    modalPress.mouseDown = true;
    ui.BeginFrame(modalPress, 800, 600, 0.016f);
    ui.EndFrame();

    bgl::ui::UiInputState modalRelease{};
    modalRelease.mouseX = okX;
    modalRelease.mouseY = okY;
    modalRelease.mouseReleased = true;
    ui.BeginFrame(modalRelease, 800, 600, 0.016f);
    ui.EndFrame();

    EXPECT_EQ(resultBtn, 1);
    EXPECT_FALSE(ui.HasOpenModal());
}

TEST(ImmediateUIModalAndLoadingTest, LoadingScreenImmediateCancelAndPersistentStateWork) {
    bgl::ui::ImmediateUI ui;
    ASSERT_TRUE(ui.SetDefaultFont("embedded", 24.0f));

    ui.ShowLoadingScreen("Loading World", -1.0f, "Streaming shaders...");
    EXPECT_TRUE(ui.HasOpenModal());
    ui.BeginFrame(bgl::ui::UiInputState{}, 800, 600, 0.1f);
    ui.EndFrame();
    EXPECT_TRUE(ui.WantsMouse());
    EXPECT_GT(ui.GetVertexCount(), 0u);
    ui.HideLoadingScreen();
    EXPECT_FALSE(ui.HasOpenModal());

    // Immediate cancellable loading screen: click Cancel button
    // Viewport 800x600 with 1 status line -> cardH=142, cardY=229, Cancel center (400, 343)
    bgl::ui::UiInputState pressIn{};
    pressIn.mouseX = 400.0f;
    pressIn.mouseY = 343.0f;
    pressIn.mouseDown = true;
    ui.BeginFrame(pressIn, 800, 600, 0.016f);
    EXPECT_FALSE(ui.DrawLoadingScreen("Downloading Asset", 0.5f, "Mesh data...", false, true));
    ui.EndFrame();

    bgl::ui::UiInputState releaseIn{};
    releaseIn.mouseX = 400.0f;
    releaseIn.mouseY = 343.0f;
    releaseIn.mouseReleased = true;
    ui.BeginFrame(releaseIn, 800, 600, 0.016f);
    EXPECT_TRUE(ui.DrawLoadingScreen("Downloading Asset", 0.5f, "Mesh data...", false, true));
    ui.EndFrame();
}

TEST(ImmediateUILayoutAndControlsTest, ContainerPanelGroupToggleBoxAndOptionsWorkWithHover) {
    bgl::ui::ImmediateUI ui;
    ASSERT_TRUE(ui.SetDefaultFont("embedded", 24.0f));
    ui.Style().enableHoverEffects = true;

    bool toggleState = false;
    int radioMode = 0;
    size_t qualityIdx = 0;
    size_t segIdx = 1;
    const std::array<std::string_view, 3> qualityOpts{"Low", "Medium", "High"};
    const std::array<const char*, 3> segOpts{"Lit", "Wire", "Norm"};

    // Frame 1: Standalone panel + horizontal container + group + toggle box + options
    bgl::ui::UiInputState idleIn{};
    ui.BeginFrame(idleIn, 1024, 768, 0.016f);

    // Standalone panel at (20, 20)
    EXPECT_TRUE(bgl::ui::UI::BeginPanel("SidePanel", 20.0f, 20.0f, 280.0f, 0.0f, "Inspector"));

    // Horizontal 2-column container
    bgl::ui::ContainerOptions hOpts{};
    hOpts.layout = bgl::ui::ContainerLayout::Horizontal;
    hOpts.columns = 2;
    hOpts.showBackground = true;
    hOpts.showBorder = true;
    hOpts.hoverable = true;
    bgl::ui::UI::BeginContainer("BtnRow", hOpts);
    bgl::ui::UI::Button("LeftBtn");
    const bgl::ui::UiRect leftRect = ui.GetLastItemRect();
    bgl::ui::UI::Button("RightBtn");
    const bgl::ui::UiRect rightRect = ui.GetLastItemRect();
    EXPECT_GT(rightRect.x, leftRect.x);
    EXPECT_NEAR(rightRect.y, leftRect.y, 0.1f);
    bgl::ui::UI::EndContainer();

    // Group + ToggleBox + Options
    bgl::ui::UI::BeginGroup("Display", true);
    EXPECT_FALSE(bgl::ui::UI::ToggleBox("Bloom", toggleState, "Enable HDR post-processing bloom"));
    const bgl::ui::UiRect toggleRect = ui.GetLastItemRect();
    EXPECT_FALSE(ui.Option("Perspective", radioMode, 0));
    EXPECT_FALSE(ui.Option("Orthographic", radioMode, 1));
    const bgl::ui::UiRect orthoRect = ui.GetLastItemRect();
    bgl::ui::UI::EndGroup();

    EXPECT_FALSE(bgl::ui::UI::OptionGroup("Quality", qualityOpts, qualityIdx, false));
    const bgl::ui::UiRect qualGroupRect = ui.GetLastItemRect();
    EXPECT_FALSE(bgl::ui::UI::OptionGroup("Shading", std::span<const char* const>{segOpts}, segIdx, true));
    const bgl::ui::UiRect segRect = ui.GetLastItemRect();

    bgl::ui::UI::EndPanel();
    const bgl::ui::UiRect panelRect = ui.GetLastItemRect();
    ui.EndFrame();

    EXPECT_GT(ui.GetVertexCount(), 0u);
    EXPECT_GT(panelRect.h, 120.0f);
    EXPECT_FALSE(ui.WantsMouse());

    // Frame 2: Click ToggleBox -> flips toggleState and sets IsItemHovered / WantsMouse
    bgl::ui::UiInputState clickToggle{};
    clickToggle.mouseX = toggleRect.x + toggleRect.w * 0.5f;
    clickToggle.mouseY = toggleRect.y + toggleRect.h * 0.5f;
    clickToggle.mouseReleased = true;
    ui.BeginFrame(clickToggle, 1024, 768, 0.05f);
    ui.BeginPanel("SidePanel", 20.0f, 20.0f, 280.0f, 0.0f, "Inspector");
    ui.BeginContainer("BtnRow", hOpts);
    ui.Button("LeftBtn");
    ui.Button("RightBtn");
    ui.EndContainer();
    ui.BeginGroup("Display", true);
    EXPECT_TRUE(ui.ToggleBox("Bloom", toggleState, "Enable HDR post-processing bloom"));
    EXPECT_TRUE(ui.IsItemHovered());
    ui.Tooltip("Toggles bloom pass");
    EXPECT_TRUE(toggleState);
    ui.EndGroup();
    ui.EndPanel();
    ui.EndFrame();
    EXPECT_TRUE(ui.WantsMouse());

    // Frame 3: Click Orthographic Option -> updates radioMode to 1
    bgl::ui::UiInputState clickOpt{};
    clickOpt.mouseX = orthoRect.x + 20.0f;
    clickOpt.mouseY = orthoRect.y + orthoRect.h * 0.5f;
    clickOpt.mouseReleased = true;
    ui.BeginFrame(clickOpt, 1024, 768, 0.05f);
    ui.BeginPanel("SidePanel", 20.0f, 20.0f, 280.0f, 0.0f, "Inspector");
    ui.BeginContainer("BtnRow", hOpts);
    ui.Button("LeftBtn");
    ui.Button("RightBtn");
    ui.EndContainer();
    ui.BeginGroup("Display", true);
    ui.ToggleBox("Bloom", toggleState, "Enable HDR post-processing bloom");
    ui.Option("Perspective", radioMode, 0);
    EXPECT_TRUE(ui.Option("Orthographic", radioMode, 1));
    EXPECT_EQ(radioMode, 1);
    ui.EndGroup();
    ui.EndPanel();
    ui.EndFrame();

    // Frame 4: Click first segment in horizontal OptionGroup ("Lit") -> segIdx becomes 0
    bgl::ui::UiInputState clickSeg{};
    clickSeg.mouseX = segRect.x + 15.0f;
    clickSeg.mouseY = segRect.y + segRect.h * 0.5f;
    clickSeg.mouseReleased = true;
    ui.BeginFrame(clickSeg, 1024, 768, 0.05f);
    ui.BeginPanel("SidePanel", 20.0f, 20.0f, 280.0f, 0.0f, "Inspector");
    ui.BeginContainer("BtnRow", hOpts);
    ui.Button("LeftBtn");
    ui.Button("RightBtn");
    ui.EndContainer();
    ui.BeginGroup("Display", true);
    ui.ToggleBox("Bloom", toggleState, "Enable HDR post-processing bloom");
    ui.Option("Perspective", radioMode, 0);
    ui.Option("Orthographic", radioMode, 1);
    ui.EndGroup();
    ui.OptionGroup("Quality", qualityOpts, qualityIdx, false);
    EXPECT_TRUE(ui.OptionGroup("Shading", std::span<const char* const>{segOpts}, segIdx, true));
    EXPECT_EQ(segIdx, 0u);
    EXPECT_TRUE(ui.IsItemHovered());
    ui.EndPanel();
    ui.EndFrame();

    // Verify vertical OptionGroup bounding rect is non-trivial and hover suppression works cleanly
    EXPECT_GT(qualGroupRect.h, 50.0f);
    bgl::ui::ImmediateUI uiNoHover;
    uiNoHover.Style().enableHoverEffects = false;
    uiNoHover.BeginFrame(bgl::ui::UiInputState{}, 1024, 768, 0.05f);
    uiNoHover.BeginPanel("SidePanel", 20.0f, 20.0f, 280.0f, 0.0f, "Inspector");
    uiNoHover.OptionGroup("Shading", std::span<const char* const>{segOpts}, segIdx, true);
    const bgl::ui::UiRect noHoverSegRect = uiNoHover.GetLastItemRect();
    uiNoHover.EndPanel();
    uiNoHover.EndFrame();

    bgl::ui::UiInputState hoverNoFx{};
    hoverNoFx.mouseX = noHoverSegRect.x + 15.0f;
    hoverNoFx.mouseY = noHoverSegRect.y + noHoverSegRect.h * 0.5f;
    uiNoHover.BeginFrame(hoverNoFx, 1024, 768, 0.05f);
    uiNoHover.BeginPanel("SidePanel", 20.0f, 20.0f, 280.0f, 0.0f, "Inspector");
    uiNoHover.OptionGroup("Shading", std::span<const char* const>{segOpts}, segIdx, true);
    EXPECT_TRUE(uiNoHover.IsItemHovered());
    uiNoHover.EndPanel();
    uiNoHover.EndFrame();
}
