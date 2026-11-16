/**
 * @file Entity.cpp
 * @brief Implementation of bgl::Entity and Engine::CreateEntity / FindEntity.
 */
#include "bgl/Entity.hpp"
#include "bgl/Engine.hpp"
#include "engine/EngineImpl.hpp"
#include "engine/EntityOps.hpp"
#include <glm/gtc/quaternion.hpp>

namespace bgl {

struct Entity::Impl {
    std::weak_ptr<scene::SceneNode> node{};
    std::weak_ptr<scene::Scene> scene{};
};

bool Entity::IsValid() const noexcept {
    return impl_ && !impl_->node.expired() && !impl_->scene.expired();
}

std::string Entity::Name() const {
    if (!impl_) {
        return {};
    }
    if (auto n = impl_->node.lock()) {
        return n->name;
    }
    return {};
}

void Entity::SetPosition(const glm::vec3& position) {
    if (!impl_) {
        return;
    }
    auto n = impl_->node.lock();
    auto s = impl_->scene.lock();
    if (!n || !s) {
        return;
    }
    n->translation = position;
    n->UpdateLocalTransform();
    s->Update();
}

glm::vec3 Entity::Position() const {
    if (!impl_) {
        return glm::vec3{0.0f};
    }
    if (auto n = impl_->node.lock()) {
        return n->translation;
    }
    return glm::vec3{0.0f};
}

void Entity::SetRotationEuler(const glm::vec3& degrees) {
    SetRotation(glm::quat(glm::radians(degrees)));
}

void Entity::SetRotation(const glm::quat& rotation) {
    if (!impl_) {
        return;
    }
    auto n = impl_->node.lock();
    auto s = impl_->scene.lock();
    if (!n || !s) {
        return;
    }
    n->rotation = glm::normalize(rotation);
    n->UpdateLocalTransform();
    s->Update();
}

glm::quat Entity::Rotation() const {
    if (!impl_) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    if (auto n = impl_->node.lock()) {
        return n->rotation;
    }
    return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
}

void Entity::SetScale(const glm::vec3& scale) {
    if (!impl_) {
        return;
    }
    auto n = impl_->node.lock();
    auto s = impl_->scene.lock();
    if (!n || !s) {
        return;
    }
    n->scale = scale;
    n->UpdateLocalTransform();
    s->Update();
}

glm::vec3 Entity::Scale() const {
    if (!impl_) {
        return glm::vec3{1.0f};
    }
    if (auto n = impl_->node.lock()) {
        return n->scale;
    }
    return glm::vec3{1.0f};
}

void Entity::SetParent(const Entity& parent) {
    if (!impl_) {
        return;
    }
    auto child = impl_->node.lock();
    auto s = impl_->scene.lock();
    if (!child || !s) {
        return;
    }
    scene::SceneNode* parentPtr = nullptr;
    if (parent.impl_) {
        auto parentScene = parent.impl_->scene.lock();
        auto parentNode = parent.impl_->node.lock();
        if (parentScene && parentScene == s && parentNode) {
            parentPtr = parentNode.get();
        }
    }
    engine::Reparent(*s, *child, parentPtr);
}

void Entity::AddLight(const LightDesc& desc) {
    if (!impl_) {
        return;
    }
    auto n = impl_->node.lock();
    auto s = impl_->scene.lock();
    if (!n || !s) {
        return;
    }
    scene::PunctualLight light{};
    light.name = n->name;
    light.type = static_cast<scene::LightType>(desc.type);
    light.color = desc.color;
    light.intensity = desc.intensity;
    light.range = desc.range;
    light.innerConeAngle = desc.innerConeAngle;
    light.outerConeAngle = desc.outerConeAngle;
    engine::AttachLight(*s, *n, light);
}

Entity Engine::CreateEntity(std::string_view name) {
    auto scene = impl_->GetScene();
    if (!scene) {
        auto emptyScene = std::make_shared<scene::Scene>();
        impl_->GetPipeline().PrepareScene(*emptyScene);
        scene = emptyScene;
        // Store into EngineCore's activeScene_ via Friend/Impl access
        impl_->activeScene_ = emptyScene;
    }
    auto node = engine::AddNode(*scene, name);
    Entity e;
    e.impl_ = std::make_shared<Entity::Impl>();
    e.impl_->node = node;
    e.impl_->scene = scene;
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
            e.impl_ = std::make_shared<Entity::Impl>();
            e.impl_->node = node;
            e.impl_->scene = scene;
            return e;
        }
    }
    return {};
}

} // namespace bgl
