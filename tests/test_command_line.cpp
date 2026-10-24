/**
 * @file test_command_line.cpp
 * @brief Unit tests for command-line arguments parsing and default options.
 */
#include <gtest/gtest.h>
#include <array>
#include <span>
#include "app/CommandLine.hpp"

using namespace bgl::app;

TEST(CommandLineTest, DefaultOptions) {
    char* argv[] = { const_cast<char*>("bgl_viewer") };
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
    char* argvHelp[] = { const_cast<char*>("bgl_viewer"), const_cast<char*>("--help") };
    auto optHelp = CliOptions::Parse(2, argvHelp);
    EXPECT_TRUE(optHelp.showHelp);

    char* argvVer[] = { const_cast<char*>("bgl_viewer"), const_cast<char*>("-v") };
    auto optVer = CliOptions::Parse(2, argvVer);
    EXPECT_TRUE(optVer.showVersion);

    char* argvFmt[] = { const_cast<char*>("bgl_viewer"), const_cast<char*>("--formats") };
    auto optFmt = CliOptions::Parse(2, argvFmt);
    EXPECT_TRUE(optFmt.listFormats);
}

TEST(CommandLineTest, CustomAssetAndResolution) {
    char* argv[] = {
        const_cast<char*>("bgl_viewer"),
        const_cast<char*>("-a"), const_cast<char*>("models/scene.glb"),
        const_cast<char*>("-w"), const_cast<char*>("1920"),
        const_cast<char*>("-H"), const_cast<char*>("1080"),
        const_cast<char*>("-l"), const_cast<char*>("debug")
    };
    auto opt = CliOptions::Parse(9, argv);

    EXPECT_EQ(opt.assetPath, "models/scene.glb");
    EXPECT_EQ(opt.width, 1920u);
    EXPECT_EQ(opt.height, 1080u);
    EXPECT_EQ(opt.logLevel, "debug");

    const std::array<const char*, 2> urlArgs{
        "bgl_viewer",
        "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Box/glTF-Binary/Box.glb"
    };
    const auto urlOpt{CliOptions::Parse(std::span<const char* const>{urlArgs})};
    EXPECT_EQ(urlOpt.assetPath, "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Box/glTF-Binary/Box.glb");
}

TEST(CommandLineTest, FeatureToggles) {
    char* argv[] = {
        const_cast<char*>("bgl_viewer"),
        const_cast<char*>("--no-skybox"),
        const_cast<char*>("--no-bounds"),
        const_cast<char*>("--quadtree"),
        const_cast<char*>("--no-hud"),
        const_cast<char*>("--no-bump"),
        const_cast<char*>("--no-mipmaps"),
        const_cast<char*>("--no-shadows"),
        const_cast<char*>("--no-reflections"),
        const_cast<char*>("--no-ao")
    };
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

    const std::array<const char*, 11> argsReEnable{
        "bgl_viewer",
        "--no-bump",
        "--bump",
        "--no-mipmaps",
        "--mipmaps",
        "--no-shadows",
        "--shadows",
        "--no-reflections",
        "--reflections",
        "--no-ao",
        "--ao"
    };
    auto optRe{CliOptions::Parse(std::span<const char* const>{argsReEnable})};
    EXPECT_TRUE(optRe.enableBumpMapping);
    EXPECT_TRUE(optRe.enableMipmapping);
    EXPECT_TRUE(optRe.enableShadows);
    EXPECT_TRUE(optRe.enableReflections);
    EXPECT_TRUE(optRe.enableAmbientOcclusion);
}

TEST(CommandLineTest, TestCrashSignalSelection) {
    char* argvSegv[] = {
        const_cast<char*>("bgl_viewer"),
        const_cast<char*>("--test-crash"),
        const_cast<char*>("segv")
    };
    auto optSegv = CliOptions::Parse(3, argvSegv);
    EXPECT_TRUE(optSegv.testCrash);
    EXPECT_EQ(optSegv.testCrashSignal, 0);

    char* argvAbrt[] = {
        const_cast<char*>("bgl_viewer"),
        const_cast<char*>("--test-crash"),
        const_cast<char*>("abrt")
    };
    auto optAbrt = CliOptions::Parse(3, argvAbrt);
    EXPECT_TRUE(optAbrt.testCrash);
    EXPECT_EQ(optAbrt.testCrashSignal, 1);

    char* argvFpe[] = {
        const_cast<char*>("bgl_viewer"),
        const_cast<char*>("--test-crash"),
        const_cast<char*>("fpe")
    };
    auto optFpe = CliOptions::Parse(3, argvFpe);
    EXPECT_TRUE(optFpe.testCrash);
    EXPECT_EQ(optFpe.testCrashSignal, 2);
}

TEST(CommandLineTest, AdditionalFlagsAndHelpOutput) {
    const std::array<const char*, 4> args{
        "bgl_viewer",
        "--no-occlusion",
        "--quadtree",
        "--no-bounds"
    };
    auto opt = CliOptions::Parse(std::span<const char* const>{args});
    EXPECT_FALSE(opt.enableOcclusionCulling);
    EXPECT_TRUE(opt.enableQuadtreeBounds);
    EXPECT_FALSE(opt.enableBoundingBoxes);

    // Verify PrintHelp and PrintVersion execute cleanly
    CliOptions::PrintHelp("bgl_viewer");
    CliOptions::PrintVersion();
    SUCCEED();
}

