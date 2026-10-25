/**
 * @file Scene.hpp
 * @brief Scene graph node hierarchy, PBR material definitions, and std::pmr vertex storage with WebGPU resources.
 */
#pragma once

#include "core/Types.hpp"
#include "scene/Animation.hpp"
#include "scene/TextureDecoder.hpp"
#include <array>
#include <glm/glm.hpp>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

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
enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 };

/**
 * @struct PunctualLight
 * @brief Punctual light source definition (KHR_lights_punctual).
 */
struct PunctualLight {
    std::string name{};
    LightType type{LightType::Directional};
    glm::vec3 color{1.0f, 0.98f, 0.95f};
    float intensity{1.0f};           ///< Illuminance in lux (Directional) or luminous intensity in candela (Point/Spot)
    float range{0.0f};               ///< Cutoff distance (>0) or 0 for infinite range
    float innerConeAngle{0.0f};      ///< Spot light inner cone half-angle in radians
    float outerConeAngle{0.785398f}; ///< Spot light outer cone half-angle in radians (default pi/4)
};

enum class AlphaMode : uint32_t { Opaque = 0, Mask = 1, Blend = 2 };

/**
 * @struct SceneCamera
 * @brief Camera parameters parsed from glTF asset (perspective / orthographic).
 */
struct SceneCamera {
    std::string name{};
    bool isPerspective{true};
    float yfov{0.785398f};
    float znear{0.1f};
    float zfar{1000.0f};
    float xmag{1.0f};
    float ymag{1.0f};
    glm::mat4 worldTransform{1.0f};
    int nodeIndex{-1};
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
    glm::vec2 uvOffset{0.0f, 0.0f};
    glm::vec2 uvScale{1.0f, 1.0f};
    float uvRotation{0.0f};

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

    // 9. Volume & Transmission (KHR_materials_volume)
    float thicknessFactor{0.0f};
    int thicknessTextureIndex{-1};
    float attenuationDistance{1e9f};
    glm::vec3 attenuationColor{1.0f};
};

/**
 * @struct MeshPrimitive
 * @brief PMR-backed mesh primitive holding GPU vertices, indices, and WebGPU buffers.
 */
struct MeshPrimitive {
    std::pmr::vector<core::Vertex> vertices;
    std::pmr::vector<core::Vertex> baseVertices;
    std::pmr::vector<uint32_t> indices;
    std::vector<MorphTarget> morphTargets{};
    bool morphDirty{false};
    core::AABB boundingBox{};
    int materialIndex{-1};

    WGPUIndexFormat indexFormat{WGPUIndexFormat_Uint32};
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

    /**
     * @brief Apply morph target deltas weighted by active weights onto vertices.
     */
    void ApplyMorphTargets(std::span<const float> weights);
};

/**
 * @struct Mesh
 * @brief Shared mesh resource containing one or more mesh primitives.
 * Enables zero-copy instancing where multiple SceneNodes share the same geometry.
 */
struct Mesh {
    std::string name{};
    std::vector<MeshPrimitive> primitives{};
};

/**
 * @class SceneNode
 * @brief Hierarchical node in the scene graph.
 */
class SceneNode {
public:
    std::string name{};
    int nodeIndex{-1};
    int cameraIndex{-1};
    int lightIndex{-1}; ///< Optional index into Scene::lights (KHR_lights_punctual)
    int skinIndex{-1};  ///< Optional index into Scene::skins (Skeletal skinning)

    // Decoupled TRS local transform
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // Identity quaternion
    glm::vec3 scale{1.0f};
    std::vector<float> morphWeights{};

    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
    core::AABB worldAABB{};

    /// Shared mesh resource if instantiated from glTF meshes
    std::shared_ptr<Mesh> mesh{nullptr};

    /// Fallback primitives for standalone / procedural geometry
    std::vector<MeshPrimitive> primitives{};

    std::vector<std::shared_ptr<SceneNode>> children{};

    [[nodiscard]] std::span<const MeshPrimitive> GetPrimitives() const noexcept {
        if (mesh)
            return mesh->primitives;
        return primitives;
    }

    [[nodiscard]] std::span<MeshPrimitive> GetPrimitives() noexcept {
        if (mesh)
            return mesh->primitives;
        return primitives;
    }

    void UpdateLocalTransform() noexcept;
    void ApplyMorphWeights();
    void UpdateTransforms(const glm::mat4& parentTransform = glm::mat4{1.0f});
};

/**
 * @struct Scene
 * @brief Root scene representation containing node hierarchy, materials, lights, cameras, and textures.
 */
struct Scene {
    std::vector<std::shared_ptr<SceneNode>> rootNodes{};
    std::vector<std::shared_ptr<SceneNode>> allNodes{};
    std::vector<std::shared_ptr<Mesh>> meshes{};
    std::vector<Material> materials{};
    std::vector<PunctualLight> lights{};
    std::vector<SceneCamera> cameras{};
    std::vector<CpuImage> cpuImages{};
    std::vector<TextureGpuResource> gpuTextures{};
    std::vector<Animation> animations{};
    std::vector<Skin> skins{};
    int activeAnimationIndex{-1};
    float animationTime{0.0f};
    bool animationPlaying{true};
    core::AABB sceneAABB{};

    ~Scene();
    void Update();
    void UpdateAnimation(float deltaTime);
    void EvaluateAnimation(size_t animIndex, float time);
    void SetAnimation(int index) noexcept;
};

} // namespace bgl::scene
