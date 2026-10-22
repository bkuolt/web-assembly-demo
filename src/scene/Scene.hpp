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

/**
 * @struct MeshPrimitive
 * @brief PMR-backed mesh primitive holding GPU vertices and indices.
 */
struct MeshPrimitive {
    std::pmr::vector<core::Vertex> vertices;
    std::pmr::vector<uint32_t> indices;
    core::AABB boundingBox;

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
            worldAABB.Expand(prim.boundingBox);
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

