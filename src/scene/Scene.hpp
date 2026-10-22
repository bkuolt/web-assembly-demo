/**
 * @file Scene.hpp
 * @brief Scene graph node hierarchy and std::pmr vertex storage.
 */
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <memory_resource>
#include <glm/glm.hpp>
#include "core/Types.hpp"

namespace bgl::scene {

struct Material {
    std::string name;
    glm::vec4 baseColorFactor{1.0f};
    int baseColorTextureIndex = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
};

/**
 * @struct MeshPrimitive
 * @brief PMR-backed mesh primitive holding GPU vertices and indices.
 */
struct MeshPrimitive {
    std::pmr::vector<core::Vertex> vertices;
    std::pmr::vector<uint32_t> indices;
    core::AABB boundingBox;
    int materialIndex = -1;

    explicit MeshPrimitive(std::pmr::memory_resource* mr)
        : vertices(mr), indices(mr) {}
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
            // Transform primitive AABB into world space
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
 * @brief Root scene representation containing node hierarchy.
 */
struct Scene {
    std::vector<std::shared_ptr<SceneNode>> rootNodes;
    std::vector<Material> materials;
    std::vector<uint32_t> gpuTextures; // OpenGL texture IDs
    core::AABB sceneAABB;

    void Update() {
        sceneAABB = core::AABB{};
        for (auto& node : rootNodes) {
            node->UpdateTransforms();
            sceneAABB.Expand(node->worldAABB);
        }
    }
};

} // namespace bgl::scene

