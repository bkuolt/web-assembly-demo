/**
 * @file Entity.cpp
 * @brief Implementation of bgl::Entity and Engine::CreateEntity / FindEntity.
 */
#include "bgl/Entity.hpp"
#include "bgl/Engine.hpp"
#include "engine/EngineImpl.hpp"
#include "engine/EntityOps.hpp"
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace bgl {

struct Entity::Impl {
    std::weak_ptr<scene::SceneNode> node{};
    std::weak_ptr<scene::Scene> scene{};
};

namespace {

[[nodiscard]] bool IsFiniteVec3(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

template <typename Fn>
void WithNodeAndScene(const std::shared_ptr<Entity::Impl>& impl, Fn&& fn) {
    if (!impl) {
        return;
    }
    auto node = impl->node.lock();
    auto scene = impl->scene.lock();
    if (!node || !scene) {
        return;
    }
    fn(*node, *scene);
}

void CommitTransform(scene::SceneNode& node, scene::Scene& scene) {
    node.UpdateLocalTransform();
    scene.Update();
}

std::shared_ptr<Entity::Impl> MakeEntityImpl(const std::shared_ptr<scene::SceneNode>& node,
                                             const std::shared_ptr<scene::Scene>& scene) {
    auto impl = std::make_shared<Entity::Impl>();
    impl->node = node;
    impl->scene = scene;
    return impl;
}

} // namespace

bool Entity::IsValid() const noexcept {
    return impl_ && !impl_->node.expired() && !impl_->scene.expired();
}

bool Entity::operator==(const Entity& other) const noexcept {
    const auto nodeA = impl_ ? impl_->node.lock() : nullptr;
    const auto nodeB = other.impl_ ? other.impl_->node.lock() : nullptr;
    const auto sceneA = impl_ ? impl_->scene.lock() : nullptr;
    const auto sceneB = other.impl_ ? other.impl_->scene.lock() : nullptr;
    const bool validA = (nodeA != nullptr && sceneA != nullptr);
    const bool validB = (nodeB != nullptr && sceneB != nullptr);
    if (!validA && !validB) {
        return true;
    }
    return validA && validB && nodeA == nodeB && sceneA == sceneB;
}

std::string Entity::Name() const {
    if (impl_ && !impl_->scene.expired()) {
        if (auto n = impl_->node.lock()) {
            return n->name;
        }
    }
    return {};
}

void Entity::SetPosition(const glm::vec3& position) {
    if (!IsFiniteVec3(position)) {
        return;
    }
    WithNodeAndScene(impl_, [&](scene::SceneNode& node, scene::Scene& scene) {
        node.translation = position;
        CommitTransform(node, scene);
    });
}

glm::vec3 Entity::Position() const {
    if (impl_ && !impl_->scene.expired()) {
        if (auto n = impl_->node.lock()) {
            return n->translation;
        }
    }
    return glm::vec3{0.0f};
}

void Entity::SetRotationEuler(const glm::vec3& degrees) {
    if (!IsFiniteVec3(degrees)) {
        return;
    }
    SetRotation(glm::quat(glm::radians(degrees)));
}

void Entity::SetRotation(const glm::quat& rotation) {
    WithNodeAndScene(impl_, [&](scene::SceneNode& node, scene::Scene& scene) {
        const float lenSq = glm::dot(rotation, rotation);
        node.rotation =
            (std::isfinite(lenSq) && lenSq > 1e-8f) ? glm::normalize(rotation) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        CommitTransform(node, scene);
    });
}

glm::quat Entity::Rotation() const {
    if (impl_ && !impl_->scene.expired()) {
        if (auto n = impl_->node.lock()) {
            return n->rotation;
        }
    }
    return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
}

void Entity::SetScale(const glm::vec3& scale) {
    if (!IsFiniteVec3(scale)) {
        return;
    }
    WithNodeAndScene(impl_, [&](scene::SceneNode& node, scene::Scene& scene) {
        node.scale = scale;
        CommitTransform(node, scene);
    });
}

glm::vec3 Entity::Scale() const {
    if (impl_ && !impl_->scene.expired()) {
        if (auto n = impl_->node.lock()) {
            return n->scale;
        }
    }
    return glm::vec3{1.0f};
}

void Entity::SetParent(const Entity& parent) {
    WithNodeAndScene(impl_, [&](scene::SceneNode& child, scene::Scene& scene) {
        scene::SceneNode* parentPtr = nullptr;
        if (parent.impl_) {
            auto parentScene = parent.impl_->scene.lock();
            auto parentNode = parent.impl_->node.lock();
            if (!parentScene || !parentNode || parentScene.get() != &scene) {
                return;
            }
            parentPtr = parentNode.get();
        }
        engine::Reparent(scene, child, parentPtr);
    });
}

void Entity::AddLight(const LightDesc& desc) {
    WithNodeAndScene(impl_, [&](scene::SceneNode& node, scene::Scene& scene) {
        scene::PunctualLight light{};
        light.name = node.name;
        light.type = static_cast<scene::LightType>(desc.type);
        light.color = IsFiniteVec3(desc.color) ? glm::max(desc.color, glm::vec3{0.0f}) : glm::vec3{1.0f};
        light.intensity = std::isfinite(desc.intensity) ? std::max(desc.intensity, 0.0f) : 1.0f;
        light.range = (std::isfinite(desc.range) && desc.range >= 0.0f) ? desc.range : 0.0f;
        light.innerConeAngle = std::isfinite(desc.innerConeAngle) ? std::clamp(desc.innerConeAngle, 0.0f, 1.57f) : 0.0f;
        light.outerConeAngle = std::isfinite(desc.outerConeAngle)
                                   ? std::clamp(desc.outerConeAngle, light.innerConeAngle, 1.5707f)
                                   : 0.7854f;
        engine::AttachLight(scene, node, light);
    });
}

Entity Engine::CreateEntity(std::string_view name) {
    auto scene = impl_->GetScene();
    if (!scene) {
        auto emptyScene = std::make_shared<scene::Scene>();
        impl_->ActivateScene(emptyScene, "Scene");
        scene = impl_->GetScene();
    }
    Entity e;
    e.impl_ = MakeEntityImpl(engine::AddNode(*scene, name), scene);
    return e;
}

Entity Engine::FindEntity(std::string_view name) {
    const auto& scene = impl_->GetScene();
    if (!scene) {
        return {};
    }
    for (const auto& node : scene->allNodes) {
        if (node && node->name == name) {
            Entity e;
            e.impl_ = MakeEntityImpl(node, scene);
            return e;
        }
    }
    return {};
}

} // namespace bgl
