/**
 * @file Animation.cpp
 * @brief Implementation of glTF 2.0 animation keyframe sampling and interpolation.
 */
#include "scene/Animation.hpp"

namespace bgl::scene {

void AnimationSampler::Sample(float t, size_t stride, AnimationPath path, std::span<float> outValues) const {
    if (timestamps.empty() || keyframeValues.empty() || outValues.size() < stride || stride == 0) {
        return;
    }

    const size_t keyframeCount = timestamps.size();

    auto copyKeyframe = [&](size_t keyframeIdx) noexcept {
        if (interpolation == AnimationInterpolation::CubicSpline) {
            // Cubic spline stores [in-tangent, value, out-tangent]; value is at offset 1 * stride
            const size_t valueOffset = (keyframeIdx * 3 + 1) * stride;
            if (valueOffset + stride <= keyframeValues.size()) {
                std::copy_n(keyframeValues.data() + valueOffset, stride, outValues.data());
            }
        } else {
            const size_t valueOffset = keyframeIdx * stride;
            if (valueOffset + stride <= keyframeValues.size()) {
                std::copy_n(keyframeValues.data() + valueOffset, stride, outValues.data());
            }
        }
    };

    auto normalizeIfRotation = [&]() noexcept {
        if (path == AnimationPath::Rotation && stride >= 4) {
            glm::quat q(outValues[3], outValues[0], outValues[1], outValues[2]);
            q = glm::normalize(q);
            outValues[0] = q.x;
            outValues[1] = q.y;
            outValues[2] = q.z;
            outValues[3] = q.w;
        }
    };

    // 1. Clamp before first keyframe
    if (t <= timestamps.front()) {
        copyKeyframe(0);
        normalizeIfRotation();
        return;
    }

    // 2. Clamp after last keyframe
    if (t >= timestamps.back()) {
        copyKeyframe(keyframeCount - 1);
        normalizeIfRotation();
        return;
    }

    // 3. Find surrounding keyframes k and k1 where timestamps[k] <= t <= timestamps[k1]
    const auto it = std::ranges::upper_bound(timestamps, t);
    const size_t k = (it == timestamps.begin()) ? 0 : std::distance(timestamps.begin(), it) - 1;
    const size_t k1 = std::min(k + 1, keyframeCount - 1);

    if (k == k1 || interpolation == AnimationInterpolation::Step) {
        copyKeyframe(k);
        normalizeIfRotation();
        return;
    }

    const float t0 = timestamps[k];
    const float t1 = timestamps[k1];
    const float dt = t1 - t0;
    const float alpha = (dt > 1e-7f) ? std::clamp((t - t0) / dt, 0.0f, 1.0f) : 0.0f;

    // 4. Linear Interpolation
    if (interpolation == AnimationInterpolation::Linear) {
        const size_t offset0 = k * stride;
        const size_t offset1 = k1 * stride;
        if (offset1 + stride > keyframeValues.size()) return;

        if (path == AnimationPath::Rotation && stride >= 4) {
            glm::quat q0(keyframeValues[offset0 + 3], keyframeValues[offset0 + 0], keyframeValues[offset0 + 1], keyframeValues[offset0 + 2]);
            glm::quat q1(keyframeValues[offset1 + 3], keyframeValues[offset1 + 0], keyframeValues[offset1 + 1], keyframeValues[offset1 + 2]);
            // Ensure shortest path for spherical linear interpolation
            if (glm::dot(q0, q1) < 0.0f) {
                q1 = -q1;
            }
            const glm::quat q = glm::normalize(glm::slerp(q0, q1, alpha));
            outValues[0] = q.x;
            outValues[1] = q.y;
            outValues[2] = q.z;
            outValues[3] = q.w;
        } else {
            for (size_t i = 0; i < stride; ++i) {
                outValues[i] = glm::mix(keyframeValues[offset0 + i], keyframeValues[offset1 + i], alpha);
            }
        }
        return;
    }

    // 5. Cubic Spline Interpolation (Hermite Spline)
    if (interpolation == AnimationInterpolation::CubicSpline) {
        // Layout per keyframe: [in-tangent (stride), value (stride), out-tangent (stride)]
        const size_t kStride3 = k * 3 * stride;
        const size_t k1Stride3 = k1 * 3 * stride;
        if (k1Stride3 + 3 * stride > keyframeValues.size()) return;

        const size_t v0Offset = kStride3 + stride;         // v_k
        const size_t b0Offset = kStride3 + 2 * stride;     // out-tangent b_k
        const size_t a1Offset = k1Stride3;                 // in-tangent a_{k+1}
        const size_t v1Offset = k1Stride3 + stride;        // v_{k+1}

        const float s = alpha;
        const float s2 = s * s;
        const float s3 = s2 * s;

        const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
        const float h10 = s3 - 2.0f * s2 + s;
        const float h01 = -2.0f * s3 + 3.0f * s2;
        const float h11 = s3 - s2;

        for (size_t i = 0; i < stride; ++i) {
            const float vk = keyframeValues[v0Offset + i];
            const float bk = keyframeValues[b0Offset + i];
            const float vk1 = keyframeValues[v1Offset + i];
            const float ak1 = keyframeValues[a1Offset + i];

            outValues[i] = h00 * vk + h10 * dt * bk + h01 * vk1 + h11 * dt * ak1;
        }
        normalizeIfRotation();
    }
}

} // namespace bgl::scene

