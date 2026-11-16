/**
 * @file EntityOps.cpp
 * @brief Pure CPU scene-graph operations for node creation, reparenting, and light attachment.
 */
#include "engine/EntityOps.hpp"
#include <algorithm>

namespace bgl::engine {

namespace {

void DetachFromTree(scene::Scene& scene, const scene::SceneNode* target) {
    auto eraseMatch = [target](std::vector<std::shared_ptr<scene::SceneNode>>& list) {
        std::erase_if(list, [target](const std::shared_ptr<scene::SceneNode>& n) { return n.get() == target; });
    };
    eraseMatch(scene.rootNodes);
    for (auto& candidate : scene.allNodes) {
        if (candidate) {
            eraseMatch(candidate->children);
        }
    }
}

bool IsDescendantOrSelf(const scene::SceneNode* ancestor, const scene::SceneNode* node) {
    if (!ancestor || !node) {
        return false;
    }
    if (ancestor == node) {
        return true;
    }
    for (const auto& child : ancestor->children) {
        if (IsDescendantOrSelf(child.get(), node)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::shared_ptr<scene::SceneNode> AddNode(scene::Scene& scene, std::string_view name) {
    auto node = std::make_shared<scene::SceneNode>();
    node->name = std::string{name};
    node->nodeIndex = static_cast<int>(scene.allNodes.size());
    scene.allNodes.push_back(node);
    scene.rootNodes.push_back(node);
    scene.Update();
    return node;
}

void Reparent(scene::Scene& scene, scene::SceneNode& child, scene::SceneNode* newParent) {
    if (newParent && IsDescendantOrSelf(&child, newParent)) {
        return;
    }

    std::shared_ptr<scene::SceneNode> childPtr{};
    for (const auto& n : scene.allNodes) {
        if (n.get() == &child) {
            childPtr = n;
            break;
        }
    }
    if (!childPtr) {
        return;
    }

    DetachFromTree(scene, &child);
    if (newParent) {
        newParent->children.push_back(std::move(childPtr));
    } else {
        scene.rootNodes.push_back(std::move(childPtr));
    }
    scene.Update();
}

void AttachLight(scene::Scene& scene, scene::SceneNode& node, const scene::PunctualLight& light) {
    if (node.lightIndex >= 0 && static_cast<size_t>(node.lightIndex) < scene.lights.size()) {
        scene.lights[static_cast<size_t>(node.lightIndex)] = light;
        return;
    }
    scene.lights.push_back(light);
    node.lightIndex = static_cast<int>(scene.lights.size() - 1);
}

} // namespace bgl::engine
