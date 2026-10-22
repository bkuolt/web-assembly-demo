/**
 * @file Scene.cpp
 * @brief Implementation of Scene graph hierarchy transforms, WebGPU resource cleanup,
 *        and multithreaded MikkTSpace/Lengyel tangent-space basis computation.
 */
#include "scene/Scene.hpp"
#include <algorithm>
#include <cmath>
#if !defined(__EMSCRIPTEN__)
#include <thread>
#endif

namespace bgl::scene {

MeshPrimitive::MeshPrimitive(std::pmr::memory_resource* mr)
    : vertices{mr}, indices{mr} {}

MeshPrimitive::MeshPrimitive(MeshPrimitive&& other) noexcept
    : vertices{std::move(other.vertices)},
      indices{std::move(other.indices)},
      boundingBox{other.boundingBox},
      materialIndex{other.materialIndex},
      gpuVertexBuffer{other.gpuVertexBuffer},
      gpuIndexBuffer{other.gpuIndexBuffer},
      gpuUniformBuffer{other.gpuUniformBuffer},
      gpuBindGroup{other.gpuBindGroup},
      gpuShadowBindGroup{other.gpuShadowBindGroup} {
    other.gpuVertexBuffer = nullptr;
    other.gpuIndexBuffer = nullptr;
    other.gpuUniformBuffer = nullptr;
    other.gpuBindGroup = nullptr;
    other.gpuShadowBindGroup = nullptr;
}

MeshPrimitive& MeshPrimitive::operator=(MeshPrimitive&& other) noexcept {
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

MeshPrimitive::~MeshPrimitive() {
    ReleaseGpu();
}

void MeshPrimitive::ReleaseGpu() noexcept {
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

void MeshPrimitive::ComputeTangents() {
    if (vertices.empty()) return;
    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3{0.0f});
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3{0.0f});

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

            tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
            tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
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
                const glm::vec3 up{(std::abs(n.y) < 0.999f) ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f}};
                vertSpan[i].tangent = glm::vec4{glm::normalize(glm::cross(up, n)), 1.0f};
            }
        }
    };

#if !defined(__EMSCRIPTEN__)
    if (vertSpan.size() >= 8192) {
        const size_t numThreads{std::clamp<size_t>(std::thread::hardware_concurrency(), 2, 8)};
        const size_t chunkSize{(vertSpan.size() + numThreads - 1) / numThreads};
        std::vector<std::jthread> workers{};
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
}

void SceneNode::UpdateTransforms(const glm::mat4& parentTransform) {
    worldTransform = parentTransform * localTransform;
    worldAABB = core::AABB{};
    for (const auto& prim : primitives) {
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

void Scene::Update() {
    sceneAABB = core::AABB{};
    for (auto& node : rootNodes) {
        if (node) {
            node->UpdateTransforms();
            sceneAABB.Expand(node->worldAABB);
        }
    }
}

} // namespace bgl::scene
