/**
 * @file test_command_line.cpp
 * @brief Unit tests for command-line arguments parsing and default options.
 */
#include "app/CommandLine.hpp"
#include <array>
#include <gtest/gtest.h>
#include <span>

using namespace bgl::app;

TEST(CommandLineTest, DefaultOptions) {
    char* argv[] = {const_cast<char*>("bgl_viewer")};
    auto opt = CliOptions::Parse(1, argv);

    EXPECT_FALSE(opt.showHelp);
    EXPECT_FALSE(opt.showVersion);
    EXPECT_FALSE(opt.listFormats);
    EXPECT_FALSE(opt.testCrash);
    EXPECT_EQ(opt.width, 1280u);
    EXPECT_EQ(opt.height, 720u);
    EXPECT_EQ(opt.logLevel, "info");
    EXPECT_TRUE(opt.enableSkybox);
    EXPECT_TRUE(opt.enableBoundingBoxes);
    EXPECT_FALSE(opt.enableQuadtreeBounds);
    EXPECT_TRUE(opt.enableDebugOverlay);
    EXPECT_TRUE(opt.enableBumpMapping);
    EXPECT_TRUE(opt.enableMipmapping);
    EXPECT_TRUE(opt.enableShadows);
    EXPECT_TRUE(opt.enableReflections);
}

TEST(CommandLineTest, HelpAndVersionFlags) {
    char* argvHelp[] = {const_cast<char*>("bgl_viewer"), const_cast<char*>("--help")};
    auto optHelp = CliOptions::Parse(2, argvHelp);
    EXPECT_TRUE(optHelp.showHelp);

    char* argvVer[] = {const_cast<char*>("bgl_viewer"), const_cast<char*>("-v")};
    auto optVer = CliOptions::Parse(2, argvVer);
    EXPECT_TRUE(optVer.showVersion);

    char* argvFmt[] = {const_cast<char*>("bgl_viewer"), const_cast<char*>("--formats")};
    auto optFmt = CliOptions::Parse(2, argvFmt);
    EXPECT_TRUE(optFmt.listFormats);
}

TEST(CommandLineTest, CustomAssetAndResolution) {
    char* argv[] = {const_cast<char*>("bgl_viewer"), const_cast<char*>("-a"),   const_cast<char*>("models/scene.glb"),
                    const_cast<char*>("-w"),         const_cast<char*>("1920"), const_cast<char*>("-H"),
                    const_cast<char*>("1080"),       const_cast<char*>("-l"),   const_cast<char*>("debug")};
    auto opt = CliOptions::Parse(9, argv);

    EXPECT_EQ(opt.assetPath, "models/scene.glb");
    EXPECT_EQ(opt.width, 1920u);
    EXPECT_EQ(opt.height, 1080u);
    EXPECT_EQ(opt.logLevel, "debug");

    const std::array<const char*, 2> urlArgs{
        "bgl_viewer",
        "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Box/glTF-Binary/Box.glb"};
    const auto urlOpt{CliOptions::Parse(std::span<const char* const>{urlArgs})};
    EXPECT_EQ(urlOpt.assetPath,
              "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Box/glTF-Binary/Box.glb");
}

TEST(CommandLineTest, FeatureToggles) {
    char* argv[] = {const_cast<char*>("bgl_viewer"),       const_cast<char*>("--no-skybox"),
                    const_cast<char*>("--no-bounds"),      const_cast<char*>("--quadtree"),
                    const_cast<char*>("--no-hud"),         const_cast<char*>("--no-bump"),
                    const_cast<char*>("--no-mipmaps"),     const_cast<char*>("--no-shadows"),
                    const_cast<char*>("--no-reflections"), const_cast<char*>("--no-ao")};
    auto opt = CliOptions::Parse(10, argv);

    EXPECT_FALSE(opt.enableSkybox);
    EXPECT_FALSE(opt.enableBoundingBoxes);
    EXPECT_TRUE(opt.enableQuadtreeBounds);
    EXPECT_FALSE(opt.enableDebugOverlay);
    EXPECT_FALSE(opt.enableBumpMapping);
    EXPECT_FALSE(opt.enableMipmapping);
    EXPECT_FALSE(opt.enableShadows);
    EXPECT_FALSE(opt.enableReflections);
    EXPECT_FALSE(opt.enableAmbientOcclusion);

    const std::array<const char*, 11> argsReEnable{"bgl_viewer",    "--no-bump",    "--bump",    "--no-mipmaps",
                                                   "--mipmaps",     "--no-shadows", "--shadows", "--no-reflections",
                                                   "--reflections", "--no-ao",      "--ao"};
    auto optRe{CliOptions::Parse(std::span<const char* const>{argsReEnable})};
    EXPECT_TRUE(optRe.enableBumpMapping);
    EXPECT_TRUE(optRe.enableMipmapping);
    EXPECT_TRUE(optRe.enableShadows);
    EXPECT_TRUE(optRe.enableReflections);
    EXPECT_TRUE(optRe.enableAmbientOcclusion);
}

TEST(CommandLineTest, TestCrashSignalSelection) {
    char* argvSegv[] = {const_cast<char*>("bgl_viewer"), const_cast<char*>("--test-crash"), const_cast<char*>("segv")};
    auto optSegv = CliOptions::Parse(3, argvSegv);
    EXPECT_TRUE(optSegv.testCrash);
    EXPECT_EQ(optSegv.testCrashSignal, 0);

    char* argvAbrt[] = {const_cast<char*>("bgl_viewer"), const_cast<char*>("--test-crash"), const_cast<char*>("abrt")};
    auto optAbrt = CliOptions::Parse(3, argvAbrt);
    EXPECT_TRUE(optAbrt.testCrash);
    EXPECT_EQ(optAbrt.testCrashSignal, 1);

    char* argvFpe[] = {const_cast<char*>("bgl_viewer"), const_cast<char*>("--test-crash"), const_cast<char*>("fpe")};
    auto optFpe = CliOptions::Parse(3, argvFpe);
    EXPECT_TRUE(optFpe.testCrash);
    EXPECT_EQ(optFpe.testCrashSignal, 2);
}

TEST(CommandLineTest, HeadlessOption) {
    const std::array<const char*, 2> args{"bgl_viewer", "--headless"};
    auto opt = CliOptions::Parse(std::span<const char* const>{args});
    EXPECT_TRUE(opt.headless);
}

TEST(CommandLineTest, AdditionalFlagsAndHelpOutput) {
    const std::array<const char*, 4> args{"bgl_viewer", "--no-occlusion", "--quadtree", "--no-bounds"};
    auto opt = CliOptions::Parse(std::span<const char* const>{args});
    EXPECT_FALSE(opt.enableOcclusionCulling);
    EXPECT_TRUE(opt.enableQuadtreeBounds);
    EXPECT_FALSE(opt.enableBoundingBoxes);

    // Verify PrintHelp and PrintVersion execute cleanly
    CliOptions::PrintHelp("bgl_viewer");
    CliOptions::PrintVersion();
    SUCCEED();
}

TEST(CommandLineTest, MissingFlagValueSafety) {
    // Flag -a, -w, -H, -l at the very end of argv with no accompanying value
    const std::array<const char*, 2> argsMissingVal{"bgl_viewer", "-a"};
    auto opt = CliOptions::Parse(std::span<const char* const>{argsMissingVal});
    // Must parse safely without crashing or out-of-bounds memory access
    EXPECT_EQ(opt.width, 1280u);
    EXPECT_EQ(opt.height, 720u);

    const std::array<const char*, 2> argsMissingWidth{"bgl_viewer", "-w"};
    auto optW = CliOptions::Parse(std::span<const char* const>{argsMissingWidth});
    EXPECT_EQ(optW.width, 1280u);
}

TEST(CommandLineTest, InvalidNumericValuesFallbacks) {
    const std::array<const char*, 5> invalidArgs{"bgl_viewer", "-w", "not_a_number", "-H", "invalid_height"};
    auto opt = CliOptions::Parse(std::span<const char* const>{invalidArgs});
    // Invalid integer parses must retain default values
    EXPECT_EQ(opt.width, 1280u);
    EXPECT_EQ(opt.height, 720u);
}

TEST(CommandLineTest, FullFeatureFlagToggleRoundtrips) {
    const std::array<const char*, 9> disableArgs{"bgl_viewer",       "--no-occlusion", "--no-ao",
                                                 "--no-reflections", "--no-shadows",   "--no-mipmaps",
                                                 "--no-bump",        "--no-hud",       "--no-bounds"};
    auto optDisabled = CliOptions::Parse(std::span<const char* const>{disableArgs});
    EXPECT_FALSE(optDisabled.enableOcclusionCulling);
    EXPECT_FALSE(optDisabled.enableAmbientOcclusion);
    EXPECT_FALSE(optDisabled.enableReflections);
    EXPECT_FALSE(optDisabled.enableShadows);
    EXPECT_FALSE(optDisabled.enableMipmapping);
    EXPECT_FALSE(optDisabled.enableBumpMapping);
    EXPECT_FALSE(optDisabled.enableDebugOverlay);
    EXPECT_FALSE(optDisabled.enableBoundingBoxes);

    // Re-enable in reverse order
    const std::array<const char*, 9> enableArgs{"bgl_viewer", "--occlusion", "--ao",  "--reflections", "--shadows",
                                                "--mipmaps",  "--bump",      "--hud", "--bounds"};
    auto optEnabled = CliOptions::Parse(std::span<const char* const>{enableArgs});
    EXPECT_TRUE(optEnabled.enableOcclusionCulling);
    EXPECT_TRUE(optEnabled.enableAmbientOcclusion);
    EXPECT_TRUE(optEnabled.enableReflections);
    EXPECT_TRUE(optEnabled.enableShadows);
    EXPECT_TRUE(optEnabled.enableMipmapping);
    EXPECT_TRUE(optEnabled.enableBumpMapping);
    EXPECT_TRUE(optEnabled.enableDebugOverlay);
    EXPECT_TRUE(optEnabled.enableBoundingBoxes);
}
