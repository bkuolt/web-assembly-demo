/**
 * @file SoundEngine.hpp
 * @brief 3D Spatial Audio Engine using miniaudio and glTF KHR_audio integration.
 */
#ifndef BGL_AUDIO_SOUNDENGINE_HPP
#define BGL_AUDIO_SOUNDENGINE_HPP

#include "core/Types.hpp"
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#if defined(BGL_ENABLE_AUDIO)
#include <miniaudio.h>
#endif
#include <string>
#include <string_view>
#include <unordered_map>

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
    core::Result<void> PlaySound3D(std::string_view soundId, std::string_view filePath, const glm::vec3& position,
                                   bool loop = false);

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
    struct TransparentStringHash {
        using is_transparent = void;
        [[nodiscard]] size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
    };

    bool initialized_{false};
#if defined(BGL_ENABLE_AUDIO)
    ma_engine engine_{};
    std::unordered_map<std::string, std::unique_ptr<ma_sound>, TransparentStringHash, std::equal_to<>> activeSounds_{};
#endif
};

} // namespace bgl::audio

#endif // BGL_AUDIO_SOUNDENGINE_HPP
