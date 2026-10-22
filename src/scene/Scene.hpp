/**
 * @file Scene.hpp
 * @brief Scene graph node hierarchy and std::pmr vertex storage with WebGPU resources.
 */
#pragma once

#include <string>
#include <vector>
#include <span>
#include <array>
#include <memory>
#include <memory_resource>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>
#include "core/Types.hpp"

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
<<<<<<< HEAD
    int normalTextureIndex{-1};
    float normalScale{1.0f};
    float bumpScale{1.0f};
    int occlusionTextureIndex{-1};
    float occlusionStrength{1.0f};
=======
    int normalTextureIndex = -1;
    float normalScale = 1.0f;
    float bumpScale = 1.0f;
    int occlusionTextureIndex = -1;
    float occlusionStrength = 1.0f;
>>>>>>> c3c4b27404f82cd2dc023292e0a9a6a3492607ab

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

<<<<<<< HEAD
    WGPUBuffer gpuVertexBuffer{nullptr};
    WGPUBuffer gpuIndexBuffer{nullptr};
    WGPUBuffer gpuUniformBuffer{nullptr};
    WGPUBindGroup gpuBindGroup{nullptr};
    WGPUBindGroup gpuShadowBindGroup{nullptr};
=======
    WGPUBuffer gpuVertexBuffer = nullptr;
    WGPUBuffer gpuIndexBuffer = nullptr;
    WGPUBuffer gpuUniformBuffer = nullptr;
    WGPUBindGroup gpuBindGroup = nullptr;
    WGPUBindGroup gpuShadowBindGroup = nullptr;
>>>>>>> c3c4b27404f82cd2dc023292e0a9a6a3492607ab

    explicit MeshPrimitive(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : vertices{mr}, indices{mr} {}

    MeshPrimitive(MeshPrimitive&& other) noexcept
<<<<<<< HEAD
        : vertices{std::move(other.vertices)},
          indices{std::move(other.indices)},
          boundingBox{other.boundingBox},
          materialIndex{other.materialIndex},
          gpuVertexBuffer{other.gpuVertexBuffer},
          gpuIndexBuffer{other.gpuIndexBuffer},
          gpuUniformBuffer{other.gpuUniformBuffer},
          gpuBindGroup{other.gpuBindGroup},
          gpuShadowBindGroup{other.gpuShadowBindGroup} {
=======
        : vertices(std::move(other.vertices)),
          indices(std::move(other.indices)),
          boundingBox(other.boundingBox),
          materialIndex(other.materialIndex),
          gpuVertexBuffer(other.gpuVertexBuffer),
          gpuIndexBuffer(other.gpuIndexBuffer),
          gpuUniformBuffer(other.gpuUniformBuffer),
          gpuBindGroup(other.gpuBindGroup),
          gpuShadowBindGroup(other.gpuShadowBindGroup) {
>>>>>>> c3c4b27404f82cd2dc023292e0a9a6a3492607ab
        other.gpuVertexBuffer = nullptr;
        other.gpuIndexBuffer = nullptr;
        other.gpuUniformBuffer = nullptr;
        other.gpuBindGroup = nullptr;
        other.gpuShadowBindGroup = nullptr;
    }

    MeshPrimitive& operator=(MeshPrimitive&& other) noexcept {
        if (this != &other) {
            ReleaseGpu();
            vertices = std::move(other.vertices);
            indices = std::move(other.indices);
            boundingBox = other.boundingBox;
            materialIndex = other.materialIndex;
            gpuVertexBuffer = other.gpuVertexBuffer;
            gpuIndexBuffer = other.gpuIndexBuffer;
            gpuUniformBuffer = other.gpuUniformBuffer;
            gpuBindGroup = other.gpuBindGroup;
            gpuShadowBindGroup = other.gpuShadowBindGroup;
            other.gpuVertexBuffer = nullptr;
            other.gpuIndexBuffer = nullptr;
            other.gpuUniformBuffer = nullptr;
            other.gpuBindGroup = nullptr;
            other.gpuShadowBindGroup = nullptr;
        }
        return *this;
    }

    ~MeshPrimitive() {
        ReleaseGpu();
    }

    void ReleaseGpu() {
        if (gpuShadowBindGroup) {
            wgpuBindGroupRelease(gpuShadowBindGroup);
            gpuShadowBindGroup = nullptr;
        }
        if (gpuBindGroup) {
            wgpuBindGroupRelease(gpuBindGroup);
            gpuBindGroup = nullptr;
        }
        if (gpuUniformBuffer) {
            wgpuBufferRelease(gpuUniformBuffer);
            gpuUniformBuffer = nullptr;
        }
        if (gpuIndexBuffer) {
            wgpuBufferRelease(gpuIndexBuffer);
            gpuIndexBuffer = nullptr;
        }
        if (gpuVertexBuffer) {
            wgpuBufferRelease(gpuVertexBuffer);
            gpuVertexBuffer = nullptr;
        }
    }

    /**
     * @brief Compute MikkTSpace/Lengyel tangent vectors and handedness from triangle UVs and positions.
     */
    void ComputeTangents() {
        if (vertices.empty()) return;
        std::vector<glm::vec3> tan1(vertices.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> tan2(vertices.size(), glm::vec3(0.0f));

<<<<<<< HEAD
        std::span<core::Vertex> vertSpan{vertices.data(), vertices.size()};
        std::span<const uint32_t> idxSpan{indices.data(), indices.size()};

        auto processTri = [&](size_t i0, size_t i1, size_t i2) {
            if (i0 >= vertSpan.size() || i1 >= vertSpan.size() || i2 >= vertSpan.size()) return;
            const glm::vec3& p0{vertSpan[i0].position};
            const glm::vec3& p1{vertSpan[i1].position};
            const glm::vec3& p2{vertSpan[i2].position};

            const glm::vec2& uv0{vertSpan[i0].uv};
            const glm::vec2& uv1{vertSpan[i1].uv};
            const glm::vec2& uv2{vertSpan[i2].uv};

            const glm::vec3 e1{p1 - p0};
            const glm::vec3 e2{p2 - p0};
            const glm::vec2 duv1{uv1 - uv0};
            const glm::vec2 duv2{uv2 - uv0};

            const float det{duv1.x * duv2.y - duv2.x * duv1.y};
            if (std::abs(det) > 1e-8f) {
                const float r{1.0f / det};
                const glm::vec3 sdir{(e1 * duv2.y - e2 * duv1.y) * r};
                const glm::vec3 tdir{(e2 * duv1.x - e1 * duv2.x) * r};
=======
        auto processTri = [&](size_t i0, size_t i1, size_t i2) {
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) return;
            const glm::vec3& p0 = vertices[i0].position;
            const glm::vec3& p1 = vertices[i1].position;
            const glm::vec3& p2 = vertices[i2].position;

            const glm::vec2& uv0 = vertices[i0].uv;
            const glm::vec2& uv1 = vertices[i1].uv;
            const glm::vec2& uv2 = vertices[i2].uv;

            glm::vec3 e1 = p1 - p0;
            glm::vec3 e2 = p2 - p0;
            glm::vec2 duv1 = uv1 - uv0;
            glm::vec2 duv2 = uv2 - uv0;

            float det = duv1.x * duv2.y - duv2.x * duv1.y;
            if (std::abs(det) > 1e-8f) {
                float r = 1.0f / det;
                glm::vec3 sdir = (e1 * duv2.y - e2 * duv1.y) * r;
                glm::vec3 tdir = (e2 * duv1.x - e1 * duv2.x) * r;
>>>>>>> c3c4b27404f82cd2dc023292e0a9a6a3492607ab

                tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
                tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
            }
        };

<<<<<<< HEAD
        if (!idxSpan.empty()) {
            for (size_t i{0}; i + 2 < idxSpan.size(); i += 3) {
                processTri(idxSpan[i], idxSpan[i + 1], idxSpan[i + 2]);
            }
        } else {
            for (size_t i{0}; i + 2 < vertSpan.size(); i += 3) {
=======
        if (!indices.empty()) {
            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                processTri(indices[i], indices[i + 1], indices[i + 2]);
            }
        } else {
            for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
>>>>>>> c3c4b27404f82cd2dc023292e0a9a6a3492607ab
                processTri(i, i + 1, i + 2);
            }
        }

<<<<<<< HEAD
        auto orthogonalizeRange = [&](size_t beginIdx, size_t endIdx) {
            for (size_t i{beginIdx}; i < endIdx; ++i) {
                const glm::vec3& n{vertSpan[i].normal};
                const glm::vec3& t{tan1[i]};
                glm::vec3 orthoT{t - n * glm::dot(n, t)};
                const float lenSq{glm::dot(orthoT, orthoT)};
                if (lenSq > 1e-10f) {
                    orthoT *= (1.0f / std::sqrt(lenSq));
                    const float w{(glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f};
                    vertSpan[i].tangent = glm::vec4{orthoT, w};
                } else {
                    const glm::vec3 up{(std::abs(n.y) < 0.999f) ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f}};
                    vertSpan[i].tangent = glm::vec4{glm::normalize(glm::cross(up, n)), 1.0f};
                }
            }
        };

#if !defined(__EMSCRIPTEN__)
        if (vertSpan.size() >= 8192) {
            const size_t numThreads{std::clamp<size_t>(std::thread::hardware_concurrency(), 2, 8)};
            const size_t chunkSize{(vertSpan.size() + numThreads - 1) / numThreads};
            std::vector<std::jthread> workers;
            workers.reserve(numThreads);
            for (size_t t{0}; t < numThreads; ++t) {
                const size_t start{t * chunkSize};
                const size_t end{std::min(vertSpan.size(), start + chunkSize)};
                if (start < end) {
                    workers.emplace_back(orthogonalizeRange, start, end);
                }
            }
        } else {
            orthogonalizeRange(0, vertSpan.size());
        }
#else
        orthogonalizeRange(0, vertSpan.size());
#endif
=======
        for (size_t i = 0; i < vertices.size(); ++i) {
            const glm::vec3& n = vertices[i].normal;
            const glm::vec3& t = tan1[i];
            glm::vec3 orthoT = t - n * glm::dot(n, t);
            float lenSq = glm::dot(orthoT, orthoT);
            if (lenSq > 1e-10f) {
                orthoT *= (1.0f / std::sqrt(lenSq));
                float w = (glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;
                vertices[i].tangent = glm::vec4(orthoT, w);
            } else {
                glm::vec3 up = (std::abs(n.y) < 0.999f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
                vertices[i].tangent = glm::vec4(glm::normalize(glm::cross(up, n)), 1.0f);
            }
        }
>>>>>>> c3c4b27404f82cd2dc023292e0a9a6a3492607ab
    }
};

/**
 * @class SceneNode
 * @brief Hierarchical node in the scene graph.
 */
class SceneNode {
public:
    std::string name;
    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
    core::AABB worldAABB;
    std::vector<MeshPrimitive> primitives;
    std::vector<std::shared_ptr<SceneNode>> children;

    void UpdateTransforms(const glm::mat4& parentTransform = glm::mat4(1.0f)) {
        worldTransform = parentTransform * localTransform;
        worldAABB = core::AABB{};
        for (const auto& prim : primitives) {
            if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
                glm::vec3 c = prim.boundingBox.Center();
                glm::vec3 e = prim.boundingBox.Extents();
                glm::vec3 corners[8] = {
                    c + glm::vec3(-e.x, -e.y, -e.z),
                    c + glm::vec3( e.x, -e.y, -e.z),
                    c + glm::vec3(-e.x,  e.y, -e.z),
                    c + glm::vec3( e.x,  e.y, -e.z),
                    c + glm::vec3(-e.x, -e.y,  e.z),
                    c + glm::vec3( e.x, -e.y,  e.z),
                    c + glm::vec3(-e.x,  e.y,  e.z),
                    c + glm::vec3( e.x,  e.y,  e.z)
                };
                for (const auto& corner : corners) {
                    glm::vec4 worldCorner = worldTransform * glm::vec4(corner, 1.0f);
                    worldAABB.Expand(glm::vec3(worldCorner));
                }
            }
        }
        for (auto& child : children) {
            child->UpdateTransforms(worldTransform);
            worldAABB.Expand(child->worldAABB);
        }
    }
};

/**
 * @struct Scene
 * @brief Root scene representation containing node hierarchy and textures.
 */
struct Scene {
    std::vector<std::shared_ptr<SceneNode>> rootNodes;
    std::vector<Material> materials;
    std::vector<CpuImage> cpuImages;
    std::vector<TextureGpuResource> gpuTextures;
    core::AABB sceneAABB;

    ~Scene() {
        for (auto& t : gpuTextures) {
            if (t.view) {
                wgpuTextureViewRelease(t.view);
                t.view = nullptr;
            }
            if (t.texture) {
                wgpuTextureRelease(t.texture);
                t.texture = nullptr;
            }
        }
        gpuTextures.clear();
    }

    void Update() {
        sceneAABB = core::AABB{};
        for (auto& node : rootNodes) {
            node->UpdateTransforms();
            sceneAABB.Expand(node->worldAABB);
        }
    }
};

} // namespace bgl::scene
