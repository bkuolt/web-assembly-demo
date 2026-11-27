/**
 * @file test_audio_engine.cpp
 * @brief Unit tests for 3D spatial audio SoundEngine lifecycle and emitters.
 */
#include "audio/SoundEngine.hpp"
#include <gtest/gtest.h>
#include <limits>

using namespace bgl::audio;

TEST(SoundEngineTest, LifecycleAndState) {
    SoundEngine engine;
    EXPECT_FALSE(engine.IsInitialized());

    const auto res = engine.Initialize();
    if (res.has_value()) {
        EXPECT_TRUE(engine.IsInitialized());
        engine.UpdateListener(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
        engine.SetEmitterPosition("test_sound", glm::vec3(5.0f, 0.0f, 5.0f));
        engine.StopSound("test_sound");
        engine.Shutdown();
        EXPECT_FALSE(engine.IsInitialized());
    } else {
        EXPECT_FALSE(engine.IsInitialized());
    }
}

TEST(SoundEngineTest, UninitializedErrorHandling) {
    SoundEngine engine;
    EXPECT_FALSE(engine.IsInitialized());

    const auto playRes = engine.PlaySound3D("ambient", "assets/sound.wav", glm::vec3(0.0f));
    EXPECT_FALSE(playRes.has_value());
    EXPECT_TRUE(playRes.error().find("not initialized") != std::string::npos);

    EXPECT_NO_THROW({
        engine.UpdateListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        engine.SetEmitterPosition("sound1", glm::vec3(1.0f, 2.0f, 3.0f));
        engine.StopSound("sound1");
        engine.Shutdown();
    });
}

TEST(SoundEngineTest, NonExistentSoundFileError) {
    SoundEngine engine;
    if (!engine.Initialize().has_value()) {
        GTEST_SKIP() << "Audio playback device unavailable in headless environment";
    }
    const auto res = engine.PlaySound3D("missing", "non_existent_file_path_12345.wav", glm::vec3(0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(res.has_value());
    EXPECT_TRUE(res.error().find("Failed to load") != std::string::npos);
    engine.Shutdown();
}

TEST(SoundEngineTest, MultipleStopsOnNonExistentSound) {
    SoundEngine engine;
    if (!engine.Initialize().has_value()) {
        GTEST_SKIP() << "Audio playback device unavailable in headless environment";
    }
    EXPECT_NO_THROW({
        engine.StopSound("unknown_1");
        engine.StopSound("unknown_2");
        engine.SetEmitterPosition("unknown_1", glm::vec3(10.0f));
    });
    engine.Shutdown();
}

TEST(SoundEngineTest, IdempotentShutdown) {
    SoundEngine engine;
    if (!engine.Initialize().has_value()) {
        GTEST_SKIP() << "Audio playback device unavailable in headless environment";
    }
    EXPECT_TRUE(engine.IsInitialized());
    engine.Shutdown();
    EXPECT_FALSE(engine.IsInitialized());
    // Second shutdown must be a safe no-op
    EXPECT_NO_THROW(engine.Shutdown());
    EXPECT_FALSE(engine.IsInitialized());
}

TEST(SoundEngineTest, DoubleInitializeIsIdempotent) {
    SoundEngine engine;
    if (!engine.Initialize().has_value()) {
        GTEST_SKIP() << "Audio playback device unavailable in headless environment";
    }
    EXPECT_TRUE(engine.IsInitialized());
    EXPECT_TRUE(engine.Initialize().has_value());
    EXPECT_TRUE(engine.IsInitialized());
    engine.Shutdown();
    EXPECT_FALSE(engine.IsInitialized());
}

TEST(SoundEngineTest, NonFiniteSpatialVectorsAndPostShutdownCallsAreSafe) {
    SoundEngine engine;
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    (void)engine.Initialize();
    EXPECT_NO_THROW({
        engine.UpdateListener(glm::vec3(qnan, inf, -inf), glm::vec3(0.0f, qnan, -1.0f));
        engine.SetEmitterPosition("", glm::vec3(qnan, 0.0f, inf));
        engine.StopSound("");
    });
    engine.Shutdown();
    EXPECT_FALSE(engine.IsInitialized());

    // After Shutdown(), PlaySound3D must fail cleanly with initialization error
    const auto res = engine.PlaySound3D("post_shutdown", "assets/sound.wav", glm::vec3(0.0f));
    EXPECT_FALSE(res.has_value());
}
