/**
 * @file EntityOps.hpp
 * @brief Pure CPU scene-graph operations backing bgl::Entity (testable without GPU).
 */
#ifndef BGL_ENGINE_ENTITYOPS_HPP
#define BGL_ENGINE_ENTITYOPS_HPP

#include "scene/Scene.hpp"
#include <memory>
#include <string_view>

namespace bgl::engine {

std::shared_ptr<scene::SceneNode> AddNode(scene::Scene& scene, std::string_view name);
void Reparent(scene::Scene& scene, scene::SceneNode& child, scene::SceneNode* newParent);
void AttachLight(scene::Scene& scene, scene::SceneNode& node, const scene::PunctualLight& light);

} // namespace bgl::engine

#endif // BGL_ENGINE_ENTITYOPS_HPP
