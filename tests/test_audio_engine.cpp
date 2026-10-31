/**
 * @file test_audio_engine.cpp
 * @brief Unit tests for 3D spatial audio SoundEngine lifecycle and emitters.
 */
#include "audio/SoundEngine.hpp"
#include <gtest/gtest.h>

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
    if (engine.Initialize().has_value()) {
        const auto res = engine.PlaySound3D("missing", "non_existent_file_path_12345.wav", glm::vec3(0.0f, 0.0f, 0.0f));
        EXPECT_FALSE(res.has_value());
        EXPECT_TRUE(res.error().find("Failed to load") != std::string::npos);
        engine.Shutdown();
    }
}

TEST(SoundEngineTest, MultipleStopsOnNonExistentSound) {
    SoundEngine engine;
    if (engine.Initialize().has_value()) {
        EXPECT_NO_THROW({
            engine.StopSound("unknown_1");
            engine.StopSound("unknown_2");
            engine.SetEmitterPosition("unknown_1", glm::vec3(10.0f));
        });
        engine.Shutdown();
    }
}

TEST(SoundEngineTest, IdempotentShutdown) {
    SoundEngine engine;
    if (engine.Initialize().has_value()) {
        EXPECT_TRUE(engine.IsInitialized());
        engine.Shutdown();
        EXPECT_FALSE(engine.IsInitialized());
        // Second shutdown must be a safe no-op
        EXPECT_NO_THROW(engine.Shutdown());
        EXPECT_FALSE(engine.IsInitialized());
    }
}
