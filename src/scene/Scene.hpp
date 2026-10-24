/**
 * @file Scene.hpp
 * @brief Scene graph node hierarchy, PBR material definitions, and std::pmr vertex storage with WebGPU resources.
 */
#pragma once

#include <string>
#include <vector>
#include <span>
#include <array>
#include <memory>
#include <memory_resource>
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>
#include "core/Types.hpp"
#include "scene/TextureDecoder.hpp"

namespace bgl::scene {

struct TextureGpuResource {
    WGPUTexture texture{nullptr};
    WGPUTextureView view{nullptr};
    WGPUSampler sampler{nullptr};
    uint32_t width{0};
    uint32_t height{0};
};

struct CpuImage {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba{};
    bool isCompressed{false};
    KtxTranscodeTarget compressionFormat{KtxTranscodeTarget::Rgba32};
    std::vector<DecodedMipLevel> mipLevels{};
};

/**
 * @enum LightType
 * @brief Punctual light source type from glTF 2.0 KHR_lights_punctual extension.
 */
enum class LightType : uint32_t {
    Directional = 0,
    Point       = 1,
    Spot        = 2
};

/**
 * @struct PunctualLight
 * @brief Punctual light source definition (KHR_lights_punctual).
 */
struct PunctualLight {
    std::string name{};
    LightType type{LightType::Directional};
    glm::vec3 color{1.0f, 0.98f, 0.95f};
    float intensity{1.0f};          ///< Illuminance in lux (Directional) or luminous intensity in candela (Point/Spot)
    float range{0.0f};              ///< Cutoff distance (>0) or 0 for infinite range
    float innerConeAngle{0.0f};     ///< Spot light inner cone half-angle in radians
    float outerConeAngle{0.785398f};///< Spot light outer cone half-angle in radians (default pi/4)
};

enum class AlphaMode : uint32_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

struct Material {
    std::string name{};

    // 1. Core PBR Metallic-Roughness
    glm::vec4 baseColorFactor{1.0f};
    int baseColorTextureIndex{-1};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    int metallicRoughnessTextureIndex{-1};

    // 2. Normal, Bump & Occlusion
    int normalTextureIndex{-1};
    float normalScale{1.0f};
    float bumpScale{1.0f};
    int occlusionTextureIndex{-1};
    float occlusionStrength{1.0f};

    // 3. Emissive & Extensions
    glm::vec3 emissiveFactor{0.0f};
    int emissiveTextureIndex{-1};
    float emissiveStrength{1.0f};

    // 4. Alpha & Culling
    AlphaMode alphaMode{AlphaMode::Opaque};
    float alphaCutoff{0.5f};
    bool doubleSided{false};
    bool unlit{false};

    // 5. Clearcoat (Car paint lacquer)
    float clearcoatFactor{0.0f};
    float clearcoatRoughnessFactor{0.0f};
    int clearcoatTextureIndex{-1};

    // 6. Transmission & IOR (Glass / transparency)
    float transmissionFactor{0.0f};
    int transmissionTextureIndex{-1};
    float ior{1.5f};

    // 7. Sheen (Fabric / velvet)
    glm::vec3 sheenColorFactor{0.0f};
    float sheenRoughnessFactor{0.0f};
    int sheenColorTextureIndex{-1};

    // 8. Specular (KHR_materials_specular)
    float specularFactor{1.0f};
    glm::vec3 specularColorFactor{1.0f};
    int specularTextureIndex{-1};
};

/**
 * @struct MeshPrimitive
 * @brief PMR-backed mesh primitive holding GPU vertices, indices, and WebGPU buffers.
 */
struct MeshPrimitive {
    std::pmr::vector<core::Vertex> vertices;
    std::pmr::vector<uint32_t> indices;
    core::AABB boundingBox{};
    int materialIndex{-1};

    WGPUBuffer gpuVertexBuffer{nullptr};
    WGPUBuffer gpuIndexBuffer{nullptr};
    WGPUBuffer gpuWireframeIndexBuffer{nullptr};
    uint32_t wireframeIndexCount{0};
    WGPUBuffer gpuUniformBuffer{nullptr};
    WGPUBindGroup gpuBindGroup{nullptr};
    WGPUBindGroup gpuShadowBindGroup{nullptr};

    explicit MeshPrimitive(std::pmr::memory_resource* mr = std::pmr::get_default_resource());
    MeshPrimitive(const MeshPrimitive& other);
    MeshPrimitive& operator=(const MeshPrimitive& other);
    MeshPrimitive(MeshPrimitive&& other) noexcept;
    MeshPrimitive& operator=(MeshPrimitive&& other) noexcept;
    ~MeshPrimitive();

    void ReleaseGpu() noexcept;

    /**
     * @brief Compute MikkTSpace/Lengyel tangent vectors and handedness from triangle UVs and positions.
     */
    void ComputeTangents();
};

/**
 * @class SceneNode
 * @brief Hierarchical node in the scene graph.
 */
class SceneNode {
public:
    std::string name{};
    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
    core::AABB worldAABB{};
    int lightIndex{-1}; ///< Optional index into Scene::lights (KHR_lights_punctual)
    std::vector<MeshPrimitive> primitives{};
    std::vector<std::shared_ptr<SceneNode>> children{};

    void UpdateTransforms(const glm::mat4& parentTransform = glm::mat4{1.0f});
};

/**
 * @struct Scene
 * @brief Root scene representation containing node hierarchy, materials, lights, and textures.
 */
struct Scene {
    std::vector<std::shared_ptr<SceneNode>> rootNodes{};
    std::vector<Material> materials{};
    std::vector<PunctualLight> lights{};
    std::vector<CpuImage> cpuImages{};
    std::vector<TextureGpuResource> gpuTextures{};
    core::AABB sceneAABB{};

    ~Scene();
    void Update();
};

} // namespace bgl::scene
