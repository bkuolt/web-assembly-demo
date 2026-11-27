/**
 * @file Scene.cpp
 * @brief Scene graph hierarchy transforms, WebGPU resource cleanup, and node/light queries.
 */
#include "scene/Scene.hpp"
#include "core/MemoryTracker.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace bgl::scene {

namespace {

void ReleaseTrackedGpuBuffer(WGPUBuffer& buf, uint64_t& bytes) noexcept {
    if (auto b = std::exchange(buf, nullptr)) {
        if (bytes > 0) {
            core::MemoryTracker::Instance().TrackBufferFree(bytes);
        }
        wgpuBufferRelease(b);
    }
    bytes = 0;
}

} // namespace

MeshPrimitive::MeshPrimitive(std::pmr::memory_resource* mr) : vertices{mr}, baseVertices{mr}, indices{mr} {}

MeshPrimitive::MeshPrimitive(const MeshPrimitive& other)
    : vertices{other.vertices}, baseVertices{other.baseVertices}, indices{other.indices}, lodIndices{other.lodIndices},
      lodLevels{other.lodLevels}, morphTargets{other.morphTargets}, morphDirty{other.morphDirty},
      boundingBox{other.boundingBox}, materialIndex{other.materialIndex}, indexFormat{other.indexFormat},
      gpuVertexBuffer{nullptr}, gpuIndexBuffer{nullptr}, gpuWireframeIndexBuffer{nullptr}, gpuVertexBufferSize{0},
      gpuIndexBufferSize{0}, gpuWireframeIndexBufferSize{0}, wireframeIndexCount{0}, gpuUniformBuffer{nullptr},
      gpuUniformBufferSize{0}, gpuMorphDeltaBuffer{nullptr}, gpuMorphDeltaBufferSize{0}, gpuMorphTargetCount{0},
      gpuMorphVertexCount{0}, gpuBindGroup{nullptr} {}

MeshPrimitive& MeshPrimitive::operator=(const MeshPrimitive& other) {
    if (this != &other) {
        ReleaseGpu();
        vertices = other.vertices;
        baseVertices = other.baseVertices;
        indices = other.indices;
        lodIndices = other.lodIndices;
        lodLevels = other.lodLevels;
        morphTargets = other.morphTargets;
        morphDirty = other.morphDirty;
        boundingBox = other.boundingBox;
        materialIndex = other.materialIndex;
        indexFormat = other.indexFormat;
        gpuVertexBuffer = nullptr;
        gpuIndexBuffer = nullptr;
        gpuWireframeIndexBuffer = nullptr;
        gpuVertexBufferSize = 0;
        gpuIndexBufferSize = 0;
        gpuWireframeIndexBufferSize = 0;
        wireframeIndexCount = 0;
        gpuUniformBuffer = nullptr;
        gpuUniformBufferSize = 0;
        gpuMorphDeltaBuffer = nullptr;
        gpuMorphDeltaBufferSize = 0;
        gpuMorphTargetCount = 0;
        gpuMorphVertexCount = 0;
        gpuBindGroup = nullptr;
    }
    return *this;
}

MeshPrimitive::MeshPrimitive(MeshPrimitive&& other) noexcept
    : vertices{std::move(other.vertices)}, baseVertices{std::move(other.baseVertices)},
      indices{std::move(other.indices)}, lodIndices{std::move(other.lodIndices)}, lodLevels{other.lodLevels},
      morphTargets{std::move(other.morphTargets)}, morphDirty{std::exchange(other.morphDirty, false)},
      boundingBox{other.boundingBox}, materialIndex{other.materialIndex}, indexFormat{other.indexFormat},
      gpuVertexBuffer{std::exchange(other.gpuVertexBuffer, nullptr)},
      gpuIndexBuffer{std::exchange(other.gpuIndexBuffer, nullptr)},
      gpuWireframeIndexBuffer{std::exchange(other.gpuWireframeIndexBuffer, nullptr)},
      gpuVertexBufferSize{std::exchange(other.gpuVertexBufferSize, 0)},
      gpuIndexBufferSize{std::exchange(other.gpuIndexBufferSize, 0)},
      gpuWireframeIndexBufferSize{std::exchange(other.gpuWireframeIndexBufferSize, 0)},
      wireframeIndexCount{std::exchange(other.wireframeIndexCount, 0)},
      gpuUniformBuffer{std::exchange(other.gpuUniformBuffer, nullptr)},
      gpuUniformBufferSize{std::exchange(other.gpuUniformBufferSize, 0)},
      gpuMorphDeltaBuffer{std::exchange(other.gpuMorphDeltaBuffer, nullptr)},
      gpuMorphDeltaBufferSize{std::exchange(other.gpuMorphDeltaBufferSize, 0)},
      gpuMorphTargetCount{std::exchange(other.gpuMorphTargetCount, 0)},
      gpuMorphVertexCount{std::exchange(other.gpuMorphVertexCount, 0)},
      gpuBindGroup{std::exchange(other.gpuBindGroup, nullptr)} {}

MeshPrimitive& MeshPrimitive::operator=(MeshPrimitive&& other) noexcept {
    if (this != &other) {
        ReleaseGpu();
        vertices = std::move(other.vertices);
        baseVertices = std::move(other.baseVertices);
        indices = std::move(other.indices);
        lodIndices = std::move(other.lodIndices);
        lodLevels = other.lodLevels;
        morphTargets = std::move(other.morphTargets);
        morphDirty = std::exchange(other.morphDirty, false);
        boundingBox = other.boundingBox;
        materialIndex = other.materialIndex;
        indexFormat = other.indexFormat;
        gpuVertexBuffer = std::exchange(other.gpuVertexBuffer, nullptr);
        gpuIndexBuffer = std::exchange(other.gpuIndexBuffer, nullptr);
        gpuWireframeIndexBuffer = std::exchange(other.gpuWireframeIndexBuffer, nullptr);
        gpuVertexBufferSize = std::exchange(other.gpuVertexBufferSize, 0);
        gpuIndexBufferSize = std::exchange(other.gpuIndexBufferSize, 0);
        gpuWireframeIndexBufferSize = std::exchange(other.gpuWireframeIndexBufferSize, 0);
        wireframeIndexCount = std::exchange(other.wireframeIndexCount, 0);
        gpuUniformBuffer = std::exchange(other.gpuUniformBuffer, nullptr);
        gpuUniformBufferSize = std::exchange(other.gpuUniformBufferSize, 0);
        gpuMorphDeltaBuffer = std::exchange(other.gpuMorphDeltaBuffer, nullptr);
        gpuMorphDeltaBufferSize = std::exchange(other.gpuMorphDeltaBufferSize, 0);
        gpuMorphTargetCount = std::exchange(other.gpuMorphTargetCount, 0);
        gpuMorphVertexCount = std::exchange(other.gpuMorphVertexCount, 0);
        gpuBindGroup = std::exchange(other.gpuBindGroup, nullptr);
    }
    return *this;
}

MeshPrimitive::~MeshPrimitive() {
    ReleaseGpu();
}

void MeshPrimitive::ReleaseGpu() noexcept {
    if (auto bg = std::exchange(gpuBindGroup, nullptr)) {
        wgpuBindGroupRelease(bg);
    }
    ReleaseTrackedGpuBuffer(gpuMorphDeltaBuffer, gpuMorphDeltaBufferSize);
    gpuMorphTargetCount = 0;
    gpuMorphVertexCount = 0;
    ReleaseTrackedGpuBuffer(gpuUniformBuffer, gpuUniformBufferSize);
    ReleaseTrackedGpuBuffer(gpuIndexBuffer, gpuIndexBufferSize);
    ReleaseTrackedGpuBuffer(gpuWireframeIndexBuffer, gpuWireframeIndexBufferSize);
    wireframeIndexCount = 0;
    ReleaseTrackedGpuBuffer(gpuVertexBuffer, gpuVertexBufferSize);
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
        if (!prim.morphTargets.empty() && prim.gpuMorphDeltaBuffer == nullptr) {
            prim.ApplyMorphTargets(morphWeights);
        }
    }
}

namespace {

constexpr size_t kMaxSceneHierarchyDepth{256u};

void UpdateTransformsImpl(SceneNode& node, const glm::mat4& parentTransform,
                          std::unordered_set<const SceneNode*>& visited, size_t depth) {
    if (depth > kMaxSceneHierarchyDepth || !visited.insert(&node).second) {
        return;
    }
    node.worldTransform = parentTransform * node.localTransform;
    node.worldAABB = core::AABB{};
    for (const auto& prim : node.GetPrimitives()) {
        if (prim.boundingBox.IsValid()) {
            if (node.instanceTransforms.empty()) {
                node.worldAABB.Expand(prim.boundingBox.Transformed(node.worldTransform));
            } else {
                for (const glm::mat4& instMat : node.instanceTransforms) {
                    node.worldAABB.Expand(prim.boundingBox.Transformed(node.worldTransform * instMat));
                }
            }
        }
    }
    for (auto& child : node.children) {
        if (child) {
            UpdateTransformsImpl(*child, node.worldTransform, visited, depth + 1u);
            node.worldAABB.Expand(child->worldAABB);
        }
    }
}

} // namespace

void SceneNode::UpdateTransforms(const glm::mat4& parentTransform) {
    std::unordered_set<const SceneNode*> visited{};
    UpdateTransformsImpl(*this, parentTransform, visited, 0u);
}

Scene::~Scene() {
    for (auto& n : allNodes) {
        if (n) {
            n->children.clear();
        }
    }
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
    std::unordered_set<const SceneNode*> visited{};
    for (auto& node : rootNodes) {
        if (node) {
            UpdateTransformsImpl(*node, glm::mat4{1.0f}, visited, 0u);
            sceneAABB.Expand(node->worldAABB);
        }
    }
    ++transformVersion_;
}

void Scene::UpdateAnimation(float deltaTime) {
    if (activeAnimationIndex < 0 || static_cast<size_t>(activeAnimationIndex) >= animations.size()) {
        return;
    }
    const auto& anim = animations[activeAnimationIndex];
    if (anim.duration <= 0.0f || !std::isfinite(anim.duration)) {
        return;
    }

    if (animationPlaying && std::isfinite(deltaTime)) {
        animationTime += std::max(0.0f, deltaTime);
        animationTime = std::fmod(animationTime, anim.duration);
        if (!std::isfinite(animationTime)) {
            animationTime = 0.0f;
        } else if (animationTime < 0.0f) {
            animationTime += anim.duration;
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
            std::array<float, 3> val{targetNode->translation.x, targetNode->translation.y, targetNode->translation.z};
            sampler.Sample(time, 3, channel.path, val);
            if (std::isfinite(val[0]) && std::isfinite(val[1]) && std::isfinite(val[2])) {
                targetNode->translation = glm::vec3(val[0], val[1], val[2]);
            }
            break;
        }
        case AnimationPath::Rotation: {
            std::array<float, 4> val{targetNode->rotation.x, targetNode->rotation.y, targetNode->rotation.z,
                                     targetNode->rotation.w};
            sampler.Sample(time, 4, channel.path, val);
            if (std::isfinite(val[0]) && std::isfinite(val[1]) && std::isfinite(val[2]) && std::isfinite(val[3])) {
                const glm::quat rawQ(val[3], val[0], val[1], val[2]);
                const float lenSq = glm::dot(rawQ, rawQ);
                targetNode->rotation = (lenSq > 1e-10f) ? glm::normalize(rawQ) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            }
            break;
        }
        case AnimationPath::Scale: {
            std::array<float, 3> val{targetNode->scale.x, targetNode->scale.y, targetNode->scale.z};
            sampler.Sample(time, 3, channel.path, val);
            if (std::isfinite(val[0]) && std::isfinite(val[1]) && std::isfinite(val[2])) {
                targetNode->scale = glm::vec3(val[0], val[1], val[2]);
            }
            break;
        }
        case AnimationPath::Weights: {
            size_t count = targetNode->morphWeights.size();
            if (count == 0 && targetNode->mesh) {
                for (const auto& p : targetNode->mesh->primitives) {
                    count = std::max(count, p.morphTargets.size());
                }
                if (count > 0) {
                    targetNode->morphWeights.resize(count, 0.0f);
                }
            }
            if (count > 0 && !sampler.timestamps.empty()) {
                const size_t keyFactor = (sampler.interpolation == AnimationInterpolation::CubicSpline) ? 3u : 1u;
                const size_t denom = sampler.timestamps.size() * keyFactor;
                const size_t samplerStride = (denom > 0 && (sampler.keyframeValues.size() % denom) == 0)
                                                 ? (sampler.keyframeValues.size() / denom)
                                                 : count;
                if (samplerStride > 0) {
                    if (samplerStride == targetNode->morphWeights.size()) {
                        sampler.Sample(time, samplerStride, channel.path, targetNode->morphWeights);
                    } else {
                        std::vector<float> tmpWeights(samplerStride, 0.0f);
                        sampler.Sample(time, samplerStride, channel.path, tmpWeights);
                        const size_t copyN = std::min(samplerStride, targetNode->morphWeights.size());
                        std::copy_n(tmpWeights.data(), copyN, targetNode->morphWeights.data());
                    }
                    targetNode->ApplyMorphWeights();
                }
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

Skin::Skin(const Skin& other)
    : name{other.name}, inverseBindMatrices{other.inverseBindMatrices},
      skeletonRootNodeIndex{other.skeletonRootNodeIndex}, joints{other.joints}, gpuJointBuffer{nullptr},
      gpuJointBufferSize{0}, gpuBindGroup{nullptr}, primaryGpuNode{nullptr}, gpuDevice_{nullptr},
      gpuBindGroupLayout_{nullptr} {}

Skin& Skin::operator=(const Skin& other) {
    if (this != &other) {
        ReleaseGpu();
        name = other.name;
        inverseBindMatrices = other.inverseBindMatrices;
        skeletonRootNodeIndex = other.skeletonRootNodeIndex;
        joints = other.joints;
        gpuJointBufferSize = 0;
        cachedJointMatrices.clear();
        cachedFrameId = 0;
        cachedUploadedFrameId = 0;
        cachedMeshNode = nullptr;
        cachedJointCount = 0;
        primaryGpuNode = nullptr;
    }
    return *this;
}

Skin::Skin(Skin&& other) noexcept
    : name{std::move(other.name)}, inverseBindMatrices{std::move(other.inverseBindMatrices)},
      skeletonRootNodeIndex{std::exchange(other.skeletonRootNodeIndex, -1)}, joints{std::move(other.joints)},
      gpuJointBuffer{std::exchange(other.gpuJointBuffer, nullptr)},
      gpuJointBufferSize{std::exchange(other.gpuJointBufferSize, 0)},
      gpuBindGroup{std::exchange(other.gpuBindGroup, nullptr)},
      cachedJointMatrices{std::move(other.cachedJointMatrices)}, cachedFrameId{std::exchange(other.cachedFrameId, 0)},
      cachedUploadedFrameId{std::exchange(other.cachedUploadedFrameId, 0)},
      cachedMeshNode{std::exchange(other.cachedMeshNode, nullptr)},
      cachedJointCount{std::exchange(other.cachedJointCount, 0)},
      primaryGpuNode{std::exchange(other.primaryGpuNode, nullptr)},
      extraNodeGpuBindings_{std::move(other.extraNodeGpuBindings_)},
      gpuDevice_{std::exchange(other.gpuDevice_, nullptr)},
      gpuBindGroupLayout_{std::exchange(other.gpuBindGroupLayout_, nullptr)} {
    other.extraNodeGpuBindings_.clear();
}

Skin& Skin::operator=(Skin&& other) noexcept {
    if (this != &other) {
        ReleaseGpu();
        name = std::move(other.name);
        inverseBindMatrices = std::move(other.inverseBindMatrices);
        skeletonRootNodeIndex = std::exchange(other.skeletonRootNodeIndex, -1);
        joints = std::move(other.joints);
        gpuJointBuffer = std::exchange(other.gpuJointBuffer, nullptr);
        gpuJointBufferSize = std::exchange(other.gpuJointBufferSize, 0);
        gpuBindGroup = std::exchange(other.gpuBindGroup, nullptr);
        cachedJointMatrices = std::move(other.cachedJointMatrices);
        cachedFrameId = std::exchange(other.cachedFrameId, 0);
        cachedUploadedFrameId = std::exchange(other.cachedUploadedFrameId, 0);
        cachedMeshNode = std::exchange(other.cachedMeshNode, nullptr);
        cachedJointCount = std::exchange(other.cachedJointCount, 0);
        primaryGpuNode = std::exchange(other.primaryGpuNode, nullptr);
        extraNodeGpuBindings_ = std::move(other.extraNodeGpuBindings_);
        other.extraNodeGpuBindings_.clear();
        gpuDevice_ = std::exchange(other.gpuDevice_, nullptr);
        gpuBindGroupLayout_ = std::exchange(other.gpuBindGroupLayout_, nullptr);
    }
    return *this;
}

Skin::~Skin() {
    ReleaseGpu();
}

void Skin::ReleaseGpu() noexcept {
    if (auto bg = std::exchange(gpuBindGroup, nullptr)) {
        wgpuBindGroupRelease(bg);
    }
    ReleaseTrackedGpuBuffer(gpuJointBuffer, gpuJointBufferSize);
    for (auto& [nodePtr, slot] : extraNodeGpuBindings_) {
        if (auto bg = std::exchange(slot.bindGroup, nullptr)) {
            wgpuBindGroupRelease(bg);
        }
        ReleaseTrackedGpuBuffer(slot.jointBuffer, slot.jointBufferSize);
    }
    extraNodeGpuBindings_.clear();
    primaryGpuNode = nullptr;
    cachedFrameId = 0;
    cachedUploadedFrameId = 0;
    cachedMeshNode = nullptr;
    gpuDevice_ = nullptr;
    gpuBindGroupLayout_ = nullptr;
}

void Skin::ResolveNodeGpuBinding(const SceneNode* meshNode, uint64_t frameId, WGPUBuffer& outBuffer,
                                 WGPUBindGroup& outBindGroup, bool& outNeedsUpload) const noexcept {
    outBuffer = nullptr;
    outBindGroup = nullptr;
    outNeedsUpload = false;
    if (joints.empty() || !gpuJointBuffer || !gpuBindGroup) {
        return;
    }
    if (!meshNode || primaryGpuNode == nullptr || primaryGpuNode == meshNode) {
        if (meshNode) {
            primaryGpuNode = meshNode;
        }
        outBuffer = gpuJointBuffer;
        outBindGroup = gpuBindGroup;
        outNeedsUpload = (frameId == 0 || cachedUploadedFrameId != frameId);
        return;
    }

    auto it = extraNodeGpuBindings_.find(meshNode);
    if (it == extraNodeGpuBindings_.end() && gpuDevice_ && gpuBindGroupLayout_) {
        const size_t jointCount{std::max<size_t>(1, joints.size())};
        const uint64_t bufSize{static_cast<uint64_t>(jointCount) * sizeof(glm::mat4)};
        WGPUBufferDescriptor bufDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
        bufDesc.size = bufSize;
        bufDesc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        bufDesc.label = WGPUStringView{"SkinNodeJointMatrices_SSBO", 26};
        if (WGPUBuffer buf{wgpuDeviceCreateBuffer(gpuDevice_, &bufDesc)}) {
            core::MemoryTracker::Instance().TrackBufferAlloc(bufSize);
            WGPUBindGroupEntry bgEntry = WGPU_BIND_GROUP_ENTRY_INIT;
            bgEntry.binding = 0;
            bgEntry.buffer = buf;
            bgEntry.offset = 0;
            bgEntry.size = bufSize;

            WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgDesc.layout = gpuBindGroupLayout_;
            bgDesc.entryCount = 1;
            bgDesc.entries = &bgEntry;
            bgDesc.label = WGPUStringView{"SkinNodeStorage_BindGroup", 25};
            if (WGPUBindGroup bg{wgpuDeviceCreateBindGroup(gpuDevice_, &bgDesc)}) {
                it = extraNodeGpuBindings_
                         .emplace(meshNode, NodeGpuBinding{.jointBuffer = buf,
                                                           .jointBufferSize = bufSize,
                                                           .bindGroup = bg,
                                                           .uploadedFrameId = 0})
                         .first;
            } else {
                uint64_t freeSize{bufSize};
                ReleaseTrackedGpuBuffer(buf, freeSize);
            }
        }
    }

    if (it != extraNodeGpuBindings_.end() && it->second.jointBuffer && it->second.bindGroup) {
        outBuffer = it->second.jointBuffer;
        outBindGroup = it->second.bindGroup;
        outNeedsUpload = (frameId == 0 || it->second.uploadedFrameId != frameId);
    }
}

void Skin::MarkNodeGpuUploaded(uint64_t frameId, const SceneNode* meshNode) const noexcept {
    if (!meshNode || primaryGpuNode == meshNode) {
        cachedUploadedFrameId = frameId;
        cachedMeshNode = meshNode;
        return;
    }
    if (auto it = extraNodeGpuBindings_.find(meshNode); it != extraNodeGpuBindings_.end()) {
        it->second.uploadedFrameId = frameId;
    }
}

uint32_t Skin::ComputeJointMatrices(const glm::mat4& meshNodeWorldTransform,
                                    std::span<const std::shared_ptr<SceneNode>> allNodes,
                                    std::span<glm::mat4> outJointMatrices) const noexcept {
    const uint32_t count =
        std::min(static_cast<uint32_t>(joints.size()), static_cast<uint32_t>(outJointMatrices.size()));
    const glm::mat4 invWorld = glm::inverse(meshNodeWorldTransform);
    for (uint32_t j = 0; j < count; ++j) {
        const int jointNodeIdx = joints[j];
        glm::mat4 jointWorld{1.0f};
        if (jointNodeIdx >= 0 && static_cast<size_t>(jointNodeIdx) < allNodes.size()) {
            if (const auto& jointNode = allNodes[static_cast<size_t>(jointNodeIdx)]) {
                jointWorld = jointNode->worldTransform;
            }
        }
        const glm::mat4 ibm = (j < inverseBindMatrices.size()) ? inverseBindMatrices[j] : glm::mat4{1.0f};
        outJointMatrices[j] = invWorld * jointWorld * ibm;
    }
    return count;
}

std::span<const glm::mat4>
Skin::GetOrComputeJointMatrices(uint64_t frameId, const SceneNode* meshNode, const glm::mat4& meshNodeWorldTransform,
                                std::span<const std::shared_ptr<SceneNode>> allNodes) const noexcept {
    if (frameId != 0 && cachedFrameId == frameId && cachedMeshNode == meshNode && !cachedJointMatrices.empty()) {
        return std::span<const glm::mat4>{cachedJointMatrices.data(), cachedJointCount};
    }
    const size_t count = std::max<size_t>(1, joints.size());
    if (cachedJointMatrices.size() < count) {
        cachedJointMatrices.resize(count, glm::mat4{1.0f});
    }
    cachedJointCount = ComputeJointMatrices(meshNodeWorldTransform, allNodes, cachedJointMatrices);
    cachedFrameId = frameId;
    cachedMeshNode = meshNode;
    return std::span<const glm::mat4>{cachedJointMatrices.data(), cachedJointCount};
}

} // namespace bgl::scene
