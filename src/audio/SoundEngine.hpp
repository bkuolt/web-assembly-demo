/**
 * @file SoundEngine.hpp
 * @brief 3D Spatial Audio Engine using miniaudio and glTF KHR_audio integration.
 */
#pragma once

#include <miniaudio.h>
#include <string_view>
#include <string>
#include <unordered_map>
#include <memory>
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

    /**
     * @brief Initializes the miniaudio engine backend and audio device context.
     * @return Result indicating success or error string.
     */
    core::Result<void> Initialize();

    /**
     * @brief Stops all active 3D sounds and uninitializes the audio device.
     */
    void Shutdown();

    /**
     * @brief Updates spatial audio listener position and forward orientation in 3D world space.
     * @param position 3D coordinates of the listener (e.g. camera position).
     * @param forward Normalized forward vector of the listener.
     */
    void UpdateListener(const glm::vec3& position, const glm::vec3& forward);

    /**
     * @brief Plays a 3D localized sound at a specified world-space position.
     * @param soundId Unique identifier string for managing the sound emitter.
     * @param filePath Filesystem path to audio file (WAV, MP3, FLAC).
     * @param position 3D emitter position in world space.
     * @param loop Whether the sound should loop indefinitely until stopped.
     * @return Result indicating success or error string.
     */
    core::Result<void> PlaySound3D(std::string_view soundId, std::string_view filePath, const glm::vec3& position, bool loop = false);

    /**
     * @brief Updates the world-space position of an active 3D sound emitter.
     * @param soundId Identifier string of the active sound emitter.
     * @param position New 3D coordinates of the sound emitter.
     */
    void SetEmitterPosition(std::string_view soundId, const glm::vec3& position);

    /**
     * @brief Stops and releases an active sound emitter.
     * @param soundId Identifier string of the sound emitter to terminate.
     */
    void StopSound(std::string_view soundId);

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    bool initialized_{false};
    ma_engine engine_{};
    std::unordered_map<std::string, std::unique_ptr<ma_sound>> activeSounds_{};
};

} // namespace bgl::audio

