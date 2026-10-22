/**
 * @file miniaudio.h
 * @brief Standalone minimal miniaudio compatibility header for bgl::audio
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ma_result {
    MA_SUCCESS = 0,
    MA_ERROR = -1,
    MA_INVALID_ARGS = -2,
} ma_result;

typedef struct ma_engine ma_engine;
typedef struct ma_sound ma_sound;

typedef struct ma_engine_config {
    uint32_t channels;
    uint32_t sampleRate;
} ma_engine_config;

typedef struct ma_vec3f {
    float x;
    float y;
    float z;
} ma_vec3f;

struct ma_engine {
    bool isInitialized;
    ma_vec3f listenerPos;
    ma_vec3f listenerDir;
};

struct ma_sound {
    ma_engine* pEngine;
    bool isPlaying;
    bool isLooping;
    float volume;
    ma_vec3f position;
};

inline ma_engine_config ma_engine_config_init(void) {
    ma_engine_config cfg;
    cfg.channels = 2;
    cfg.sampleRate = 48000;
    return cfg;
}

inline ma_result ma_engine_init(const ma_engine_config* pConfig, ma_engine* pEngine) {
    (void)pConfig;
    if (!pEngine) return MA_INVALID_ARGS;
    pEngine->isInitialized = true;
    pEngine->listenerPos = (ma_vec3f){0.0f, 0.0f, 0.0f};
    pEngine->listenerDir = (ma_vec3f){0.0f, 0.0f, -1.0f};
    return MA_SUCCESS;
}

inline void ma_engine_uninit(ma_engine* pEngine) {
    if (pEngine) {
        pEngine->isInitialized = false;
    }
}

inline void ma_engine_listener_set_position(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z) {
    (void)listenerIndex;
    if (pEngine) {
        pEngine->listenerPos = (ma_vec3f){x, y, z};
    }
}

inline void ma_engine_listener_set_direction(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z) {
    (void)listenerIndex;
    if (pEngine) {
        pEngine->listenerDir = (ma_vec3f){x, y, z};
    }
}

inline ma_result ma_sound_init_from_file(ma_engine* pEngine, const char* pFilePath, uint32_t flags, void* pGroup, void* pDoneFence, ma_sound* pSound) {
    (void)pFilePath; (void)flags; (void)pGroup; (void)pDoneFence;
    if (!pEngine || !pSound) return MA_INVALID_ARGS;
    pSound->pEngine = pEngine;
    pSound->isPlaying = false;
    pSound->isLooping = false;
    pSound->volume = 1.0f;
    pSound->position = (ma_vec3f){0.0f, 0.0f, 0.0f};
    return MA_SUCCESS;
}

inline void ma_sound_uninit(ma_sound* pSound) {
    if (pSound) {
        pSound->isPlaying = false;
    }
}

inline ma_result ma_sound_start(ma_sound* pSound) {
    if (!pSound) return MA_INVALID_ARGS;
    pSound->isPlaying = true;
    return MA_SUCCESS;
}

inline ma_result ma_sound_stop(ma_sound* pSound) {
    if (!pSound) return MA_INVALID_ARGS;
    pSound->isPlaying = false;
    return MA_SUCCESS;
}

inline void ma_sound_set_looping(ma_sound* pSound, bool isLooping) {
    if (pSound) pSound->isLooping = isLooping;
}

inline void ma_sound_set_volume(ma_sound* pSound, float volume) {
    if (pSound) pSound->volume = volume;
}

inline void ma_sound_set_position(ma_sound* pSound, float x, float y, float z) {
    if (pSound) pSound->position = (ma_vec3f){x, y, z};
}

#ifdef __cplusplus
}
#endif

