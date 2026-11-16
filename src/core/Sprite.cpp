/**
 * @file Sprite.cpp
 * @brief Implementation of SpriteTexture generators, SpriteClip slicing, SpriteAnimator, and Billboard basis math.
 */
#include "core/Sprite.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>

namespace bgl::core {

namespace {

std::atomic<uint64_t> g_nextSpriteTextureId{1};

constexpr float kMinFrameDuration = 1e-4f;

[[nodiscard]] glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) noexcept {
    const float lenSq = glm::dot(v, v);
    if (lenSq <= 1e-10f) {
        return fallback;
    }
    return v * (1.0f / std::sqrt(lenSq));
}

void ComputeGridCellUv(uint32_t col, uint32_t row, uint32_t cols, uint32_t rows, uint32_t texW, uint32_t texH,
                       glm::vec2& outMin, glm::vec2& outMax) noexcept {
    if (cols == 0 || rows == 0) {
        outMin = glm::vec2{0.0f, 0.0f};
        outMax = glm::vec2{1.0f, 1.0f};
        return;
    }
    const uint32_t clampedCol = std::min(col, cols - 1u);
    const uint32_t clampedRow = std::min(row, rows - 1u);
    const float invC = 1.0f / static_cast<float>(cols);
    const float invR = 1.0f / static_cast<float>(rows);

    const float padU = (texW > 0) ? (0.25f / static_cast<float>(texW)) : 0.0f;
    const float padV = (texH > 0) ? (0.25f / static_cast<float>(texH)) : 0.0f;

    outMin = glm::vec2{static_cast<float>(clampedCol) * invC + padU, static_cast<float>(clampedRow) * invR + padV};
    outMax =
        glm::vec2{static_cast<float>(clampedCol + 1u) * invC - padU, static_cast<float>(clampedRow + 1u) * invR - padV};
    if (outMax.x <= outMin.x) {
        outMin.x = static_cast<float>(clampedCol) * invC;
        outMax.x = static_cast<float>(clampedCol + 1u) * invC;
    }
    if (outMax.y <= outMin.y) {
        outMin.y = static_cast<float>(clampedRow) * invR;
        outMax.y = static_cast<float>(clampedRow + 1u) * invR;
    }
}

void ComputePixelRectUv(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t texW, uint32_t texH, glm::vec2& outMin,
                        glm::vec2& outMax) noexcept {
    if (texW == 0 || texH == 0 || w == 0 || h == 0) {
        outMin = glm::vec2{0.0f, 0.0f};
        outMax = glm::vec2{1.0f, 1.0f};
        return;
    }
    const float invW = 1.0f / static_cast<float>(texW);
    const float invH = 1.0f / static_cast<float>(texH);
    outMin = glm::vec2{static_cast<float>(x) * invW, static_cast<float>(y) * invH};
    outMax = glm::vec2{static_cast<float>(x + w) * invW, static_cast<float>(y + h) * invH};
}

void ApplyFlipUv(const glm::vec2& uvMin, const glm::vec2& uvMax, bool flipX, bool flipY, glm::vec2& outMin,
                 glm::vec2& outMax) noexcept {
    outMin.x = flipX ? uvMax.x : uvMin.x;
    outMax.x = flipX ? uvMin.x : uvMax.x;
    outMin.y = flipY ? uvMax.y : uvMin.y;
    outMax.y = flipY ? uvMin.y : uvMax.y;
}

[[nodiscard]] uint8_t FloatToByte(float v) noexcept {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

SpriteClip SpriteClip::FromGrid(std::string_view clipName, uint32_t cols, uint32_t rows, uint32_t startFrame,
                                uint32_t frameCount, float fps, SpritePlaybackMode playback, uint32_t texWidth,
                                uint32_t texHeight) {
    SpriteClip clip;
    clip.name = std::string{clipName};
    clip.mode = playback;
    if (cols == 0 || rows == 0) {
        return clip;
    }

    const uint32_t totalCells = cols * rows;
    if (startFrame >= totalCells) {
        return clip;
    }

    const uint32_t avail = totalCells - startFrame;
    const uint32_t count = (frameCount == 0) ? avail : std::min(frameCount, avail);
    const float frameDur = (fps > 1e-4f) ? (1.0f / fps) : 0.1f;
    clip.frames.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t cell = startFrame + i;
        const uint32_t c = cell % cols;
        const uint32_t r = cell / cols;
        SpriteFrame f{};
        f.duration = frameDur;
        ComputeGridCellUv(c, r, cols, rows, texWidth, texHeight, f.uvMin, f.uvMax);
        clip.frames.push_back(f);
    }
    return clip;
}

void SpriteClip::AddPixelFrame(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t texWidth, uint32_t texHeight,
                               float duration) {
    SpriteFrame f{};
    f.duration = std::max(kMinFrameDuration, duration);
    ComputePixelRectUv(x, y, w, h, texWidth, texHeight, f.uvMin, f.uvMax);
    frames.push_back(f);
}

float SpriteClip::TotalDuration() const noexcept {
    float sum = 0.0f;
    for (const auto& f : frames) {
        sum += std::max(kMinFrameDuration, f.duration);
    }
    return sum;
}

std::shared_ptr<SpriteTexture> SpriteTexture::FromRGBA8(std::span<const uint8_t> rgba, uint32_t width, uint32_t height,
                                                        bool nearestFilter, bool srgb) {
    if (width == 0 || height == 0) {
        return nullptr;
    }
    const size_t requiredBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (rgba.size() < requiredBytes) {
        return nullptr;
    }

    auto tex = std::make_shared<SpriteTexture>();
    tex->id = g_nextSpriteTextureId.fetch_add(1u, std::memory_order_relaxed);
    tex->width = width;
    tex->height = height;
    tex->srgb = srgb;
    tex->nearestFilter = nearestFilter;
    tex->rgbaPixels.assign(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(requiredBytes));
    return tex;
}

std::shared_ptr<SpriteTexture> SpriteTexture::CreateSolid(const glm::vec4& color) {
    const std::array<uint8_t, 4> px{FloatToByte(color.r), FloatToByte(color.g), FloatToByte(color.b),
                                    FloatToByte(color.a)};
    return FromRGBA8(px, 1, 1, true, true);
}

std::shared_ptr<SpriteTexture> SpriteTexture::CreateRadialGlow(uint32_t size, const glm::vec4& innerColor,
                                                               bool nearestFilter) {
    const uint32_t dim = std::clamp(size, 4u, 512u);
    std::vector<uint8_t> rgba(static_cast<size_t>(dim) * dim * 4u);
    const float half = static_cast<float>(dim - 1u) * 0.5f;
    const float invHalf = (half > 0.0f) ? (1.0f / half) : 1.0f;

    for (uint32_t y = 0; y < dim; ++y) {
        for (uint32_t x = 0; x < dim; ++x) {
            const float dx = (static_cast<float>(x) - half) * invHalf;
            const float dy = (static_cast<float>(y) - half) * invHalf;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float t = std::clamp(1.0f - r, 0.0f, 1.0f);
            const float falloff = t * t * (3.0f - 2.0f * t);
            const size_t idx = (static_cast<size_t>(y) * dim + x) * 4u;
            rgba[idx + 0] = FloatToByte(innerColor.r);
            rgba[idx + 1] = FloatToByte(innerColor.g);
            rgba[idx + 2] = FloatToByte(innerColor.b);
            rgba[idx + 3] = FloatToByte(innerColor.a * falloff);
        }
    }
    return FromRGBA8(rgba, dim, dim, nearestFilter, true);
}

std::shared_ptr<SpriteTexture> SpriteTexture::CreateRing(uint32_t size, float innerRadius, float outerRadius,
                                                         const glm::vec4& color) {
    const uint32_t dim = std::clamp(size, 4u, 512u);
    std::vector<uint8_t> rgba(static_cast<size_t>(dim) * dim * 4u);
    const float half = static_cast<float>(dim - 1u) * 0.5f;
    const float invHalf = (half > 0.0f) ? (1.0f / half) : 1.0f;
    const float lo = std::clamp(std::min(innerRadius, outerRadius), 0.0f, 0.99f);
    const float hi = std::clamp(std::max(innerRadius, outerRadius), lo + 0.01f, 1.0f);
    const float mid = 0.5f * (lo + hi);
    const float halfThickness = std::max(0.01f, 0.5f * (hi - lo));

    for (uint32_t y = 0; y < dim; ++y) {
        for (uint32_t x = 0; x < dim; ++x) {
            const float dx = (static_cast<float>(x) - half) * invHalf;
            const float dy = (static_cast<float>(y) - half) * invHalf;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float normDist = std::abs(r - mid) / halfThickness;
            const float alpha = std::clamp(1.0f - normDist, 0.0f, 1.0f);
            const float smooth = alpha * alpha * (3.0f - 2.0f * alpha);
            const size_t idx = (static_cast<size_t>(y) * dim + x) * 4u;
            rgba[idx + 0] = FloatToByte(color.r);
            rgba[idx + 1] = FloatToByte(color.g);
            rgba[idx + 2] = FloatToByte(color.b);
            rgba[idx + 3] = FloatToByte(color.a * smooth);
        }
    }
    return FromRGBA8(rgba, dim, dim, false, true);
}

std::shared_ptr<SpriteTexture> SpriteTexture::CreateChecker(uint32_t size, uint32_t cellSize, const glm::vec4& colorA,
                                                            const glm::vec4& colorB, bool nearestFilter) {
    const uint32_t dim = std::clamp(size, 2u, 512u);
    const uint32_t cell = std::max(1u, cellSize);
    std::vector<uint8_t> rgba(static_cast<size_t>(dim) * dim * 4u);
    for (uint32_t y = 0; y < dim; ++y) {
        for (uint32_t x = 0; x < dim; ++x) {
            const bool even = (((x / cell) + (y / cell)) & 1u) == 0u;
            const glm::vec4& c = even ? colorA : colorB;
            const size_t idx = (static_cast<size_t>(y) * dim + x) * 4u;
            rgba[idx + 0] = FloatToByte(c.r);
            rgba[idx + 1] = FloatToByte(c.g);
            rgba[idx + 2] = FloatToByte(c.b);
            rgba[idx + 3] = FloatToByte(c.a);
        }
    }
    return FromRGBA8(rgba, dim, dim, nearestFilter, true);
}

void SpriteAnimator::AddClip(SpriteClip clip) {
    for (size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].name == clip.name) {
            clips_[i] = std::move(clip);
            if (hasActiveClip_ && activeClip_ == i && frameIndex_ >= clips_[i].frames.size()) {
                frameIndex_ = 0;
                timer_ = 0.0f;
            }
            return;
        }
    }
    clips_.push_back(std::move(clip));
}

bool SpriteAnimator::HasClip(std::string_view name) const noexcept {
    for (const auto& c : clips_) {
        if (c.name == name) {
            return true;
        }
    }
    return false;
}

void SpriteAnimator::ClearClips() noexcept {
    clips_.clear();
    activeClip_ = 0;
    hasActiveClip_ = false;
    frameIndex_ = 0;
    pingPongDir_ = 1;
    timer_ = 0.0f;
    playing_ = false;
    finished_ = false;
}

bool SpriteAnimator::Play(std::string_view clipName, bool restart) {
    for (size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].name == clipName) {
            if (hasActiveClip_ && activeClip_ == i && playing_ && !restart && !finished_) {
                return true;
            }
            activeClip_ = i;
            hasActiveClip_ = true;
            frameIndex_ = 0;
            pingPongDir_ = 1;
            timer_ = 0.0f;
            playing_ = !clips_[i].frames.empty();
            finished_ = false;
            return true;
        }
    }
    return false;
}

void SpriteAnimator::Stop() noexcept {
    playing_ = false;
    finished_ = false;
    frameIndex_ = 0;
    pingPongDir_ = 1;
    timer_ = 0.0f;
}

void SpriteAnimator::SetSpeed(float speed) noexcept {
    speed_ = std::max(0.0f, speed);
}

std::string_view SpriteAnimator::ActiveClipName() const noexcept {
    return (hasActiveClip_ && activeClip_ < clips_.size()) ? std::string_view{clips_[activeClip_].name}
                                                           : std::string_view{};
}

const SpriteFrame* SpriteAnimator::CurrentFrame() const noexcept {
    if (!hasActiveClip_ || activeClip_ >= clips_.size()) {
        return nullptr;
    }
    const auto& frames = clips_[activeClip_].frames;
    if (frames.empty()) {
        return nullptr;
    }
    const size_t idx = std::min(frameIndex_, frames.size() - 1u);
    return &frames[idx];
}

bool SpriteAnimator::Update(float deltaTime) {
    if (!playing_ || finished_ || !hasActiveClip_ || activeClip_ >= clips_.size() || deltaTime <= 0.0f ||
        speed_ <= 0.0f) {
        return false;
    }

    const SpriteClip& clip = clips_[activeClip_];
    if (clip.frames.empty()) {
        playing_ = false;
        return false;
    }

    const size_t initialFrame = frameIndex_;
    timer_ += deltaTime * speed_;

    while (playing_ && !finished_) {
        const float frameDur = std::max(kMinFrameDuration, clip.frames[frameIndex_].duration);
        if (timer_ < frameDur) {
            break;
        }
        timer_ -= frameDur;

        if (clip.frames.size() == 1u) {
            if (clip.mode == SpritePlaybackMode::Once) {
                finished_ = true;
                playing_ = false;
                break;
            }
            continue;
        }

        if (clip.mode == SpritePlaybackMode::Loop) {
            frameIndex_ = (frameIndex_ + 1u) % clip.frames.size();
        } else if (clip.mode == SpritePlaybackMode::Once) {
            if (frameIndex_ + 1u >= clip.frames.size()) {
                frameIndex_ = clip.frames.size() - 1u;
                finished_ = true;
                playing_ = false;
                break;
            }
            ++frameIndex_;
        } else { // PingPong
            const int next = static_cast<int>(frameIndex_) + pingPongDir_;
            if (next >= static_cast<int>(clip.frames.size())) {
                pingPongDir_ = -1;
                frameIndex_ = clip.frames.size() - 2u;
            } else if (next < 0) {
                pingPongDir_ = 1;
                frameIndex_ = 1u;
            } else {
                frameIndex_ = static_cast<size_t>(next);
            }
        }
    }

    return frameIndex_ != initialFrame || finished_;
}

void Sprite::SetFrame(const SpriteFrame& frame) noexcept {
    uvMin = frame.uvMin;
    uvMax = frame.uvMax;
}

void Sprite::SetGridFrame(uint32_t col, uint32_t row, uint32_t cols, uint32_t rows) noexcept {
    const uint32_t tw = texture ? texture->width : 0u;
    const uint32_t th = texture ? texture->height : 0u;
    ComputeGridCellUv(col, row, cols, rows, tw, th, uvMin, uvMax);
}

void Sprite::SetPixelRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h) noexcept {
    if (!texture) {
        return;
    }
    ComputePixelRectUv(x, y, w, h, texture->width, texture->height, uvMin, uvMax);
}

void Sprite::ComputeEffectiveUVs(glm::vec2& outMin, glm::vec2& outMax) const noexcept {
    ApplyFlipUv(uvMin, uvMax, flipX, flipY, outMin, outMax);
}

bool AnimatedSprite::Play(std::string_view clipName, bool restart) {
    if (!animator_.Play(clipName, restart)) {
        return false;
    }
    if (const SpriteFrame* f = animator_.CurrentFrame()) {
        SetFrame(*f);
    }
    return true;
}

void AnimatedSprite::Stop() noexcept {
    animator_.Stop();
    if (const SpriteFrame* f = animator_.CurrentFrame()) {
        SetFrame(*f);
    }
}

bool AnimatedSprite::Update(float deltaTime) {
    const bool changed = animator_.Update(deltaTime);
    if (const SpriteFrame* f = animator_.CurrentFrame()) {
        SetFrame(*f);
    }
    return changed;
}

void Billboard::SetFrame(const SpriteFrame& frame) noexcept {
    uvMin = frame.uvMin;
    uvMax = frame.uvMax;
}

void Billboard::SetGridFrame(uint32_t col, uint32_t row, uint32_t cols, uint32_t rows) noexcept {
    const uint32_t tw = texture ? texture->width : 0u;
    const uint32_t th = texture ? texture->height : 0u;
    ComputeGridCellUv(col, row, cols, rows, tw, th, uvMin, uvMax);
}

void Billboard::SetPixelRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h) noexcept {
    if (!texture) {
        return;
    }
    ComputePixelRectUv(x, y, w, h, texture->width, texture->height, uvMin, uvMax);
}

void Billboard::ComputeEffectiveUVs(glm::vec2& outMin, glm::vec2& outMax) const noexcept {
    ApplyFlipUv(uvMin, uvMax, flipX, flipY, outMin, outMax);
}

void Billboard::ComputeBasis(const glm::vec3& cameraPos, const glm::mat4& viewMatrix, glm::vec3& outRight,
                             glm::vec3& outUp) const noexcept {
    glm::vec3 baseRight{1.0f, 0.0f, 0.0f};
    glm::vec3 baseUp{0.0f, 1.0f, 0.0f};

    if (mode == BillboardMode::ScreenAligned) {
        baseRight = SafeNormalize(glm::vec3{viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]}, baseRight);
        baseUp = SafeNormalize(glm::vec3{viewMatrix[0][1], viewMatrix[1][1], viewMatrix[2][1]}, baseUp);
    } else if (mode == BillboardMode::CylindricalY) {
        const glm::vec3 toCamXZ = SafeNormalize(glm::vec3{cameraPos.x - position.x, 0.0f, cameraPos.z - position.z},
                                                glm::vec3{0.0f, 0.0f, 1.0f});
        baseUp = glm::vec3{0.0f, 1.0f, 0.0f};
        baseRight = SafeNormalize(glm::cross(baseUp, toCamXZ), glm::vec3{1.0f, 0.0f, 0.0f});
    } else { // Spherical
        const glm::vec3 forward = SafeNormalize(cameraPos - position, glm::vec3{0.0f, 0.0f, 1.0f});
        const glm::vec3 worldUp = (std::abs(glm::dot(forward, glm::vec3{0.0f, 1.0f, 0.0f})) > 0.99f)
                                      ? glm::vec3{0.0f, 0.0f, 1.0f}
                                      : glm::vec3{0.0f, 1.0f, 0.0f};
        baseRight = SafeNormalize(glm::cross(worldUp, forward), glm::vec3{1.0f, 0.0f, 0.0f});
        baseUp = SafeNormalize(glm::cross(forward, baseRight), glm::vec3{0.0f, 1.0f, 0.0f});
    }

    if (std::abs(rollRad) > 1e-6f) {
        const float c = std::cos(rollRad);
        const float s = std::sin(rollRad);
        outRight = baseRight * c + baseUp * s;
        outUp = -baseRight * s + baseUp * c;
    } else {
        outRight = baseRight;
        outUp = baseUp;
    }
}

bool AnimatedBillboard::Play(std::string_view clipName, bool restart) {
    if (!animator_.Play(clipName, restart)) {
        return false;
    }
    if (const SpriteFrame* f = animator_.CurrentFrame()) {
        SetFrame(*f);
    }
    return true;
}

void AnimatedBillboard::Stop() noexcept {
    animator_.Stop();
    if (const SpriteFrame* f = animator_.CurrentFrame()) {
        SetFrame(*f);
    }
}

bool AnimatedBillboard::Update(float deltaTime) {
    const bool changed = animator_.Update(deltaTime);
    if (const SpriteFrame* f = animator_.CurrentFrame()) {
        SetFrame(*f);
    }
    return changed;
}

} // namespace bgl::core
