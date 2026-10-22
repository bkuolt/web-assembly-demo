/**
 * @file SoundEngine.hpp
 * @brief 3D Spatial Audio Engine using miniaudio and glTF KHR_audio integration.
 */
#pragma once

#include <miniaudio.h>
#include <string_view>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include "core/Types.hpp"

namespace bgl::audio {

/**
 * @class SoundEngine
 * @brief Spatial audio playback and 3D emitter binding for scene nodes.
 */
class SoundEngine {
public:
    SoundEngine();
    ~SoundEngine();

    SoundEngine(const SoundEngine&) = delete;
    SoundEngine& operator=(const SoundEngine&) = delete;

    core::Result<void> Initialize();
    void Shutdown();

    void UpdateListener(const glm::vec3& position, const glm::vec3& forward);

    core::Result<void> PlaySound3D(std::string_view soundId, std::string_view filePath, const glm::vec3& position, bool loop = false);
    void SetEmitterPosition(std::string_view soundId, const glm::vec3& position);
    void StopSound(std::string_view soundId);

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    bool initialized_ = false;
    ma_engine engine_{};
    std::unordered_map<std::string, ma_sound> activeSounds_;
};

} // namespace bgl::audio

