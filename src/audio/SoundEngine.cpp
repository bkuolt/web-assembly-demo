/**
 * @file SoundEngine.cpp
 * @brief Implementation of miniaudio 3D spatial audio wrapper.
 */
#define MINIAUDIO_IMPLEMENTATION
#include "audio/SoundEngine.hpp"
#include "core/Log.hpp"
#include <cmath>
#include <filesystem>
#include <system_error>

namespace bgl::audio {

SoundEngine::SoundEngine() = default;

SoundEngine::~SoundEngine() {
    Shutdown();
}

core::Result<void> SoundEngine::Initialize() {
    if (initialized_) {
        return {};
    }
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
            if (sound) {
                ma_sound_stop(sound.get());
                ma_sound_uninit(sound.get());
            }
        }
        activeSounds_.clear();
        ma_engine_uninit(&engine_);
        initialized_ = false;
        BGL_LOG_INFO("3D Spatial SoundEngine shutdown completed.");
    }
}

void SoundEngine::UpdateListener(const glm::vec3& position, const glm::vec3& forward) {
    if (!initialized_)
        return;
    auto isFiniteVec = [](const glm::vec3& v) noexcept {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    const glm::vec3 safePos = isFiniteVec(position) ? position : glm::vec3{0.0f};
    const float fwdLenSq = glm::dot(forward, forward);
    const glm::vec3 safeFwd =
        (isFiniteVec(forward) && fwdLenSq > 1e-10f) ? (forward / std::sqrt(fwdLenSq)) : glm::vec3{0.0f, 0.0f, -1.0f};
    ma_engine_listener_set_position(&engine_, 0, safePos.x, safePos.y, safePos.z);
    ma_engine_listener_set_direction(&engine_, 0, safeFwd.x, safeFwd.y, safeFwd.z);
}

core::Result<void> SoundEngine::PlaySound3D(std::string_view soundId, std::string_view filePath,
                                            const glm::vec3& position, bool loop) {
    if (!initialized_)
        return std::unexpected("SoundEngine not initialized");

    const std::string id{soundId};
    const std::string path{filePath};

    // Guard missing/non-regular paths before miniaudio 0.11.21 calls ma_resource_manager_data_buffer_node_acquire
    // (which has an upstream UAF reading pDataBufferNode->isDataOwnedByResourceManager after ma_free on open error).
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec) || ec) {
        return std::unexpected("Failed to load sound file: " + path);
    }

    if (activeSounds_.size() >= 16) {
        for (auto it = activeSounds_.begin(); it != activeSounds_.end();) {
            if (it->second && !ma_sound_is_looping(it->second.get()) && !ma_sound_is_playing(it->second.get())) {
                ma_sound_uninit(it->second.get());
                it = activeSounds_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (auto existingIt = activeSounds_.find(soundId); existingIt != activeSounds_.end()) {
        if (existingIt->second) {
            ma_sound_stop(existingIt->second.get());
            ma_sound_uninit(existingIt->second.get());
        }
        activeSounds_.erase(existingIt);
    }

    auto sound = std::make_unique<ma_sound>();
    const ma_result res{ma_sound_init_from_file(&engine_, path.c_str(), 0, nullptr, nullptr, sound.get())};
    if (res != MA_SUCCESS) {
        return std::unexpected("Failed to load sound file: " + std::string{filePath});
    }

    auto sanitizePos = [](const glm::vec3& p) noexcept {
        return (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) ? p : glm::vec3{0.0f};
    };
    const glm::vec3 safePos = sanitizePos(position);
    ma_sound_set_position(sound.get(), safePos.x, safePos.y, safePos.z);
    ma_sound_set_looping(sound.get(), loop ? MA_TRUE : MA_FALSE);
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        ma_sound_uninit(sound.get());
        return std::unexpected("Failed to start sound playback: " + std::string{filePath});
    }

    activeSounds_[id] = std::move(sound);
    BGL_LOG_INFO("Playing 3D sound [{}] from '{}' (loop: {})", id, filePath, loop);
    return {};
}

void SoundEngine::SetEmitterPosition(std::string_view soundId, const glm::vec3& position) {
    if (!initialized_ || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return;
    auto it = activeSounds_.find(soundId);
    if (it != activeSounds_.end() && it->second) {
        ma_sound_set_position(it->second.get(), position.x, position.y, position.z);
    }
}

void SoundEngine::StopSound(std::string_view soundId) {
    if (!initialized_)
        return;
    auto it = activeSounds_.find(soundId);
    if (it != activeSounds_.end()) {
        if (it->second) {
            ma_sound_stop(it->second.get());
            ma_sound_uninit(it->second.get());
        }
        activeSounds_.erase(it);
    }
}

} // namespace bgl::audio
