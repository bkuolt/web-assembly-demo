/**
 * @file SoundEngine.cpp
 * @brief Implementation of miniaudio 3D spatial audio wrapper.
 */
#include "audio/SoundEngine.hpp"
#include "core/Log.hpp"

namespace bgl::audio {

SoundEngine::SoundEngine() = default;

SoundEngine::~SoundEngine() {
    Shutdown();
}

core::Result<void> SoundEngine::Initialize() {
    ma_engine_config config = ma_engine_config_init();
    ma_result result = ma_engine_init(&config, &engine_);
    if (result != MA_SUCCESS) {
        return std::unexpected("Failed to initialize miniaudio engine");
    }
    initialized_ = true;
    BGL_LOG_INFO("3D Spatial SoundEngine initialized successfully.");
    return {};
}

void SoundEngine::Shutdown() {
    if (initialized_) {
        for (auto& [id, sound] : activeSounds_) {
            ma_sound_uninit(&sound);
        }
        activeSounds_.clear();
        ma_engine_uninit(&engine_);
        initialized_ = false;
        BGL_LOG_INFO("3D Spatial SoundEngine shutdown completed.");
    }
}

void SoundEngine::UpdateListener(const glm::vec3& position, const glm::vec3& forward) {
    if (!initialized_) return;
    ma_engine_listener_set_position(&engine_, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&engine_, 0, forward.x, forward.y, forward.z);
}

core::Result<void> SoundEngine::PlaySound3D(std::string_view soundId, std::string_view filePath, const glm::vec3& position, bool loop) {
    if (!initialized_) return std::unexpected("SoundEngine not initialized");

    std::string id(soundId);
    ma_sound sound;
    ma_result res = ma_sound_init_from_file(&engine_, filePath.data(), 0, nullptr, nullptr, &sound);
    if (res != MA_SUCCESS) {
        return std::unexpected("Failed to load sound file: " + std::string(filePath));
    }

    ma_sound_set_position(&sound, position.x, position.y, position.z);
    ma_sound_set_looping(&sound, loop);
    ma_sound_start(&sound);

    activeSounds_[id] = sound;
    BGL_LOG_INFO("Playing 3D sound [{}] from '{}' (loop: {})", id, filePath, loop);
    return {};
}

void SoundEngine::SetEmitterPosition(std::string_view soundId, const glm::vec3& position) {
    auto it = activeSounds_.find(std::string(soundId));
    if (it != activeSounds_.end()) {
        ma_sound_set_position(&it->second, position.x, position.y, position.z);
    }
}

void SoundEngine::StopSound(std::string_view soundId) {
    auto it = activeSounds_.find(std::string(soundId));
    if (it != activeSounds_.end()) {
        ma_sound_stop(&it->second);
        ma_sound_uninit(&it->second);
        activeSounds_.erase(it);
    }
}

} // namespace bgl::audio

