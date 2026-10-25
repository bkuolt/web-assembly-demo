/**
 * @file Animation.hpp
 * @brief glTF 2.0 Animation, Skeletal Skinning, and Morph Target data structures and evaluators.
 */
#pragma once

#include "core/Types.hpp"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bgl::scene {

class SceneNode;

/**
 * @enum AnimationInterpolation
 * @brief glTF 2.0 animation sampler interpolation modes.
 */
enum class AnimationInterpolation : uint8_t { Linear = 0, Step = 1, CubicSpline = 2 };

/**
 * @enum AnimationPath
 * @brief Target node property driven by an animation channel.
 */
enum class AnimationPath : uint8_t { Translation = 0, Rotation = 1, Scale = 2, Weights = 3 };

/**
 * @struct AnimationSampler
 * @brief Keyframe sequence with input timestamps and output values.
 */
struct AnimationSampler {
    std::vector<float> timestamps{};
    std::vector<float> keyframeValues{};
    AnimationInterpolation interpolation{AnimationInterpolation::Linear};

    [[nodiscard]] constexpr float GetStartTime() const noexcept {
        return timestamps.empty() ? 0.0f : timestamps.front();
    }

    [[nodiscard]] constexpr float GetEndTime() const noexcept { return timestamps.empty() ? 0.0f : timestamps.back(); }

    /**
     * @brief Evaluates interpolated values at time t.
     * @param t Target animation timestamp.
     * @param stride Component count per value (3 for VEC3, 4 for VEC4/Quat, M for Weights).
     * @param path Target property path.
     * @param outValues Destination buffer for the interpolated floats.
     */
    void Sample(float t, size_t stride, AnimationPath path, std::span<float> outValues) const;
};

/**
 * @struct AnimationChannel
 * @brief Binds a sampler to a target node property.
 */
struct AnimationChannel {
    size_t samplerIndex{0};
    int targetNodeIndex{-1};
    AnimationPath path{AnimationPath::Translation};
};

/**
 * @struct Animation
 * @brief Named collection of animation channels and samplers.
 */
struct Animation {
    std::string name{};
    std::vector<AnimationChannel> channels{};
    std::vector<AnimationSampler> samplers{};
    float duration{0.0f};

    constexpr void CalculateDuration() noexcept {
        duration = 0.0f;
        for (const auto& sampler : samplers) {
            duration = std::max(duration, sampler.GetEndTime());
        }
    }
};

/**
 * @struct Skin
 * @brief Skeletal skin binding joints and inverse bind matrices.
 */
struct Skin {
    std::string name{};
    std::vector<glm::mat4> inverseBindMatrices{};
    int skeletonRootNodeIndex{-1};
    std::vector<int> joints{};

    /**
     * @brief Computes skinning joint matrices for a mesh instance:
     *        jointMatrix[i] = inverse(meshNodeWorldTransform) * jointNodeWorldTransform[i] * inverseBindMatrix[i]
     * @param meshNodeWorldTransform World transform matrix of the node referencing this skin.
     * @param allNodes Global scene node list to lookup joint node transforms.
     * @param outJointMatrices Destination span of matrices (typically 128 elements).
     * @return Number of joint matrices written.
     */
    [[nodiscard]] uint32_t ComputeJointMatrices(const glm::mat4& meshNodeWorldTransform,
                                                std::span<const std::shared_ptr<SceneNode>> allNodes,
                                                std::span<glm::mat4> outJointMatrices) const noexcept;
};

/**
 * @struct MorphTarget
 * @brief Delta attributes for morph target blending.
 */
struct MorphTarget {
    std::vector<glm::vec3> positionDeltas{};
    std::vector<glm::vec3> normalDeltas{};
    std::vector<glm::vec3> tangentDeltas{};
};

} // namespace bgl::scene
