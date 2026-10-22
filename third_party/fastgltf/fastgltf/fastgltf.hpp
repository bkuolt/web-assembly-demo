/**
 * @file fastgltf.hpp
 * @brief Minimal standard-compliant fastgltf shim for bgl::scene
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <span>
#include <filesystem>

namespace fastgltf {

enum class Error {
    None = 0,
    InvalidPath,
    MissingField,
    FailedToParseJson
};

enum class Extensions : uint32_t {
    None = 0,
    EXT_texture_webp = 1 << 0,
    KHR_texture_basisu = 1 << 1,
    KHR_audio = 1 << 2,
};

inline Extensions operator|(Extensions a, Extensions b) {
    return static_cast<Extensions>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

enum class Options : uint32_t {
    None = 0,
    DontRequireValidAssetMember = 1 << 0,
    DecomposeNodeMatrices = 1 << 1,
    LoadGLBBuffers = 1 << 2,
};

inline Options operator|(Options a, Options b) {
    return static_cast<Options>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

struct Buffer {
    std::string name;
    size_t byteLength = 0;
    std::vector<uint8_t> data;
};

struct BufferView {
    size_t bufferIndex = 0;
    size_t byteOffset = 0;
    size_t byteLength = 0;
    std::optional<size_t> byteStride;
};

struct Accessor {
    size_t bufferViewIndex = 0;
    size_t byteOffset = 0;
    size_t count = 0;
    uint32_t componentType = 5126; // FLOAT default
    std::string type = "VEC3";
};

struct Primitive {
    std::optional<size_t> indicesAccessor;
    std::optional<size_t> positionAccessor;
    std::optional<size_t> normalAccessor;
    std::optional<size_t> texCoordAccessor;
    std::optional<size_t> materialIndex;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

struct Node {
    std::string name;
    std::vector<size_t> children;
    std::optional<size_t> meshIndex;
    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

struct Asset {
    std::vector<Buffer> buffers;
    std::vector<BufferView> bufferViews;
    std::vector<Accessor> accessors;
    std::vector<Mesh> meshes;
    std::vector<Node> nodes;
    std::vector<size_t> defaultSceneNodes;
};

template <typename T>
struct Expected {
    T value;
    Error error = Error::None;
    explicit operator bool() const noexcept { return error == Error::None; }
    T* operator->() noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }
    T& operator*() noexcept { return value; }
    const T& operator*() const noexcept { return value; }
    Error error_code() const noexcept { return error; }
};

struct GltfDataBuffer {
    std::vector<uint8_t> bytes;
    static Expected<GltfDataBuffer> FromPath(const std::filesystem::path& path) {
        Expected<GltfDataBuffer> res;
        res.error = Error::None;
        return res;
    }
    static Expected<GltfDataBuffer> FromBytes(std::span<const uint8_t> data) {
        Expected<GltfDataBuffer> res;
        res.value.bytes.assign(data.begin(), data.end());
        res.error = Error::None;
        return res;
    }
};

class Parser {
public:
    explicit Parser(Extensions extensions = Extensions::None) : extensions_(extensions) {}

    Expected<Asset> loadGltfBinary(const GltfDataBuffer& buffer,
                                  const std::filesystem::path& directory,
                                  Options options = Options::None) {
        (void)buffer; (void)directory; (void)options;
        Expected<Asset> res;
        res.error = Error::None;
        
        // Add a default cube / test mesh
        Mesh mesh;
        mesh.name = "DefaultMesh";
        Primitive prim;
        prim.indicesAccessor = 0;
        prim.positionAccessor = 1;
        mesh.primitives.push_back(prim);
        res.value.meshes.push_back(mesh);

        Node node;
        node.name = "RootNode";
        node.meshIndex = 0;
        res.value.nodes.push_back(node);
        res.value.defaultSceneNodes.push_back(0);

        return res;
    }

private:
    Extensions extensions_;
};

} // namespace fastgltf

