/**
 * @file Scene.cpp
 * @brief Implementation of Scene graph hierarchy transforms, WebGPU resource cleanup,
 *        and multithreaded MikkTSpace/Lengyel tangent-space basis computation.
 */
#include "scene/Scene.hpp"
#include "core/ThreadPool.hpp"
#include <algorithm>
#include <cmath>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace bgl::scene {

MeshPrimitive::MeshPrimitive(std::pmr::memory_resource* mr) : vertices{mr}, baseVertices{mr}, indices{mr} {}

MeshPrimitive::MeshPrimitive(const MeshPrimitive& other)
    : vertices{other.vertices}, baseVertices{other.baseVertices}, indices{other.indices},
      morphTargets{other.morphTargets}, morphDirty{other.morphDirty}, boundingBox{other.boundingBox},
      materialIndex{other.materialIndex}, indexFormat{other.indexFormat}, gpuVertexBuffer{nullptr},
      gpuIndexBuffer{nullptr}, gpuWireframeIndexBuffer{nullptr}, wireframeIndexCount{0}, gpuUniformBuffer{nullptr},
      gpuBindGroup{nullptr}, gpuShadowBindGroup{nullptr} {}

MeshPrimitive& MeshPrimitive::operator=(const MeshPrimitive& other) {
    if (this != &other) {
        ReleaseGpu();
        vertices = other.vertices;
        baseVertices = other.baseVertices;
        indices = other.indices;
        morphTargets = other.morphTargets;
        morphDirty = other.morphDirty;
        boundingBox = other.boundingBox;
        materialIndex = other.materialIndex;
        indexFormat = other.indexFormat;
        gpuVertexBuffer = nullptr;
        gpuIndexBuffer = nullptr;
        gpuWireframeIndexBuffer = nullptr;
        wireframeIndexCount = 0;
        gpuUniformBuffer = nullptr;
        gpuBindGroup = nullptr;
        gpuShadowBindGroup = nullptr;
    }
    return *this;
}

MeshPrimitive::MeshPrimitive(MeshPrimitive&& other) noexcept
    : vertices{std::move(other.vertices)}, baseVertices{std::move(other.baseVertices)},
      indices{std::move(other.indices)}, morphTargets{std::move(other.morphTargets)},
      morphDirty{std::exchange(other.morphDirty, false)}, boundingBox{other.boundingBox},
      materialIndex{other.materialIndex}, indexFormat{other.indexFormat},
      gpuVertexBuffer{std::exchange(other.gpuVertexBuffer, nullptr)},
      gpuIndexBuffer{std::exchange(other.gpuIndexBuffer, nullptr)},
      gpuWireframeIndexBuffer{std::exchange(other.gpuWireframeIndexBuffer, nullptr)},
      wireframeIndexCount{std::exchange(other.wireframeIndexCount, 0)},
      gpuUniformBuffer{std::exchange(other.gpuUniformBuffer, nullptr)},
      gpuBindGroup{std::exchange(other.gpuBindGroup, nullptr)},
      gpuShadowBindGroup{std::exchange(other.gpuShadowBindGroup, nullptr)} {}

MeshPrimitive& MeshPrimitive::operator=(MeshPrimitive&& other) noexcept {
    if (this != &other) {
        ReleaseGpu();
        vertices = std::move(other.vertices);
        baseVertices = std::move(other.baseVertices);
        indices = std::move(other.indices);
        morphTargets = std::move(other.morphTargets);
        morphDirty = std::exchange(other.morphDirty, false);
        boundingBox = other.boundingBox;
        materialIndex = other.materialIndex;
        indexFormat = other.indexFormat;
        gpuVertexBuffer = std::exchange(other.gpuVertexBuffer, nullptr);
        gpuIndexBuffer = std::exchange(other.gpuIndexBuffer, nullptr);
        gpuWireframeIndexBuffer = std::exchange(other.gpuWireframeIndexBuffer, nullptr);
        wireframeIndexCount = std::exchange(other.wireframeIndexCount, 0);
        gpuUniformBuffer = std::exchange(other.gpuUniformBuffer, nullptr);
        gpuBindGroup = std::exchange(other.gpuBindGroup, nullptr);
        gpuShadowBindGroup = std::exchange(other.gpuShadowBindGroup, nullptr);
    }
    return *this;
}

MeshPrimitive::~MeshPrimitive() {
    ReleaseGpu();
}

void MeshPrimitive::ReleaseGpu() noexcept {
    if (auto bg = std::exchange(gpuShadowBindGroup, nullptr))
        wgpuBindGroupRelease(bg);
    if (auto bg = std::exchange(gpuBindGroup, nullptr))
        wgpuBindGroupRelease(bg);
    if (auto b = std::exchange(gpuUniformBuffer, nullptr))
        wgpuBufferRelease(b);
    if (auto b = std::exchange(gpuIndexBuffer, nullptr))
        wgpuBufferRelease(b);
    if (auto b = std::exchange(gpuWireframeIndexBuffer, nullptr))
        wgpuBufferRelease(b);
    wireframeIndexCount = 0;
    if (auto b = std::exchange(gpuVertexBuffer, nullptr))
        wgpuBufferRelease(b);
}

void MeshPrimitive::ComputeTangents() {
    if (vertices.empty())
        return;
    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3{0.0f});
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3{0.0f});

    std::span<core::Vertex> vertSpan{vertices.data(), vertices.size()};
    std::span<const uint32_t> idxSpan{indices.data(), indices.size()};

    auto processTri = [&](size_t i0, size_t i1, size_t i2) {
        if (i0 >= vertSpan.size() || i1 >= vertSpan.size() || i2 >= vertSpan.size())
            return;
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

            tan1[i0] += sdir;
            tan1[i1] += sdir;
            tan1[i2] += sdir;
            tan2[i0] += tdir;
            tan2[i1] += tdir;
            tan2[i2] += tdir;
        }
    };

    if (!idxSpan.empty()) {
        for (size_t i{0}; i + 2 < idxSpan.size(); i += 3) {
            processTri(idxSpan[i], idxSpan[i + 1], idxSpan[i + 2]);
        }
    } else {
        for (size_t i{0}; i + 2 < vertSpan.size(); i += 3) {
            processTri(i, i + 1, i + 2);
        }
    }

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
                const glm::vec3 up{(std::abs(n.y) < 0.999f) ? glm::vec3{0.0f, 1.0f, 0.0f}
                                                            : glm::vec3{1.0f, 0.0f, 0.0f}};
                vertSpan[i].tangent = glm::vec4{glm::normalize(glm::cross(up, n)), 1.0f};
            }
        }
    };

#if !defined(__EMSCRIPTEN__)
    if (vertSpan.size() >= 8192) {
        core::ThreadPool::Instance().ParallelFor(0, vertSpan.size(), [&](size_t i) {
            const glm::vec3& n{vertSpan[i].normal};
            const glm::vec3& t{tan1[i]};
            glm::vec3 orthoT{t - n * glm::dot(n, t)};
            const float lenSq{glm::dot(orthoT, orthoT)};
            if (lenSq > 1e-10f) {
                orthoT *= (1.0f / std::sqrt(lenSq));
                const float w{(glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f};
                vertSpan[i].tangent = glm::vec4{orthoT, w};
            } else {
                const glm::vec3 up{(std::abs(n.y) < 0.999f) ? glm::vec3{0.0f, 1.0f, 0.0f}
                                                            : glm::vec3{1.0f, 0.0f, 0.0f}};
                vertSpan[i].tangent = glm::vec4{glm::normalize(glm::cross(up, n)), 1.0f};
            }
        });
    } else {
        orthogonalizeRange(0, vertSpan.size());
    }
#else
    orthogonalizeRange(0, vertSpan.size());
#endif
}

void MeshPrimitive::ApplyMorphTargets(std::span<const float> weights) {
    if (morphTargets.empty() || baseVertices.empty() || vertices.size() != baseVertices.size()) {
        return;
    }
    vertices = baseVertices;
    boundingBox = core::AABB{};

    const size_t numTargets = std::min(morphTargets.size(), weights.size());
    for (size_t t = 0; t < numTargets; ++t) {
        const float w = weights[t];
        if (std::abs(w) < 1e-6f)
            continue;
        const auto& target = morphTargets[t];

        for (size_t i = 0; i < vertices.size(); ++i) {
            if (i < target.positionDeltas.size()) {
                vertices[i].position += w * target.positionDeltas[i];
            }
            if (i < target.normalDeltas.size()) {
                vertices[i].normal += w * target.normalDeltas[i];
            }
            if (i < target.tangentDeltas.size()) {
                vertices[i].tangent.x += w * target.tangentDeltas[i].x;
                vertices[i].tangent.y += w * target.tangentDeltas[i].y;
                vertices[i].tangent.z += w * target.tangentDeltas[i].z;
            }
        }
    }

    for (auto& v : vertices) {
        const float len = glm::length(v.normal);
        if (len > 1e-6f) {
            v.normal /= len;
        }
        boundingBox.Expand(v.position);
    }
    morphDirty = true;
}

void SceneNode::UpdateLocalTransform() noexcept {
    const glm::mat4 tMat = glm::translate(glm::mat4(1.0f), translation);
    const glm::mat4 rMat = glm::mat4_cast(rotation);
    const glm::mat4 sMat = glm::scale(glm::mat4(1.0f), scale);
    localTransform = tMat * rMat * sMat;
}

void SceneNode::ApplyMorphWeights() {
    if (morphWeights.empty())
        return;
    for (auto& prim : GetPrimitives()) {
        if (!prim.morphTargets.empty()) {
            prim.ApplyMorphTargets(morphWeights);
        }
    }
}

void SceneNode::UpdateTransforms(const glm::mat4& parentTransform) {
    worldTransform = parentTransform * localTransform;
    worldAABB = core::AABB{};
    for (const auto& prim : GetPrimitives()) {
        if (prim.boundingBox.minBound.x <= prim.boundingBox.maxBound.x) {
            worldAABB.Expand(prim.boundingBox.Transformed(worldTransform));
        }
    }
    for (auto& child : children) {
        if (child) {
            child->UpdateTransforms(worldTransform);
            worldAABB.Expand(child->worldAABB);
        }
    }
}

Scene::~Scene() {
    for (auto& t : gpuTextures) {
        if (auto view = std::exchange(t.view, nullptr)) {
            wgpuTextureViewRelease(view);
        }
        if (auto tex = std::exchange(t.texture, nullptr)) {
            wgpuTextureRelease(tex);
        }
        t.sampler = nullptr;
    }
    gpuTextures.clear();
}

void Scene::Update() {
    sceneAABB = core::AABB{};
    for (auto& node : rootNodes) {
        if (node) {
            node->UpdateTransforms();
            sceneAABB.Expand(node->worldAABB);
        }
    }
}

void Scene::UpdateAnimation(float deltaTime) {
    if (activeAnimationIndex < 0 || static_cast<size_t>(activeAnimationIndex) >= animations.size()) {
        return;
    }
    const auto& anim = animations[activeAnimationIndex];
    if (anim.duration <= 0.0f) {
        return;
    }

    if (animationPlaying) {
        animationTime += deltaTime;
        if (animationTime > anim.duration) {
            animationTime = std::fmod(animationTime, anim.duration);
        }
    }
    EvaluateAnimation(static_cast<size_t>(activeAnimationIndex), animationTime);
}

void Scene::EvaluateAnimation(size_t animIndex, float time) {
    if (animIndex >= animations.size())
        return;
    const auto& anim = animations[animIndex];

    for (const auto& channel : anim.channels) {
        if (channel.samplerIndex >= anim.samplers.size())
            continue;
        const auto& sampler = anim.samplers[channel.samplerIndex];
        if (channel.targetNodeIndex < 0 || static_cast<size_t>(channel.targetNodeIndex) >= allNodes.size())
            continue;
        const auto& targetNode = allNodes[static_cast<size_t>(channel.targetNodeIndex)];
        if (!targetNode)
            continue;

        switch (channel.path) {
        case AnimationPath::Translation: {
            std::array<float, 3> val{};
            sampler.Sample(time, 3, channel.path, val);
            targetNode->translation = glm::vec3(val[0], val[1], val[2]);
            break;
        }
        case AnimationPath::Rotation: {
            std::array<float, 4> val{};
            sampler.Sample(time, 4, channel.path, val);
            targetNode->rotation = glm::quat(val[3], val[0], val[1], val[2]);
            break;
        }
        case AnimationPath::Scale: {
            std::array<float, 3> val{};
            sampler.Sample(time, 3, channel.path, val);
            targetNode->scale = glm::vec3(val[0], val[1], val[2]);
            break;
        }
        case AnimationPath::Weights: {
            size_t count = targetNode->morphWeights.size();
            if (count == 0 && targetNode->mesh && !targetNode->mesh->primitives.empty()) {
                count = targetNode->mesh->primitives[0].morphTargets.size();
                targetNode->morphWeights.resize(count, 0.0f);
            }
            if (count > 0) {
                if (targetNode->morphWeights.size() < count) {
                    targetNode->morphWeights.resize(count, 0.0f);
                }
                sampler.Sample(time, count, channel.path, targetNode->morphWeights);
                targetNode->ApplyMorphWeights();
            }
            break;
        }
        }
        targetNode->UpdateLocalTransform();
    }

    Update();
}

void Scene::SetAnimation(int index) noexcept {
    if (index >= 0 && static_cast<size_t>(index) < animations.size()) {
        activeAnimationIndex = index;
        animationTime = 0.0f;
        animationPlaying = true;
        EvaluateAnimation(static_cast<size_t>(activeAnimationIndex), 0.0f);
    } else {
        activeAnimationIndex = -1;
    }
}

} // namespace bgl::scene
