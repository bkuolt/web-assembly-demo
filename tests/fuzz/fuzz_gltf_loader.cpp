/**
 * @file fuzz_gltf_loader.cpp
 * @brief libFuzzer target for robust parsing of malformed / fuzz glTF / GLB buffers.
 */
#include <cstdint>
#include <cstddef>
#include <span>
#include "scene/FastGltfLoader.hpp"
#include "scene/AssetManager.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    std::span<const uint8_t> span(data, size);

    bgl::scene::AssetManager assetMgr;
    bgl::scene::FastGltfLoader loader(assetMgr);

    // Fuzz LoadFromMemory
    (void)loader.LoadFromMemory(span, "fuzz_input.glb");

    return 0;
}

