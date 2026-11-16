/**
 * @file Sprite.hpp
 * @brief Platform-independent 2D Sprites, Animated Sprites, Sprite Sheets/Clips, and 3D Billboards.
 */
#ifndef BGL_CORE_SPRITE_HPP
#define BGL_CORE_SPRITE_HPP

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgl::core {

/**
 * @enum SpriteBlendMode
 * @brief Alpha compositing mode used when rendering 2D sprites and 3D billboards.
 */
enum class SpriteBlendMode : uint8_t {
    Alpha = 0,    ///< Standard SrcAlpha / OneMinusSrcAlpha blending.
    Additive = 1, ///< Glowing additive SrcAlpha / One blending (feeds HDR bloom cleanly in 3D).
};

/**
 * @enum BillboardMode
 * @brief Orientation constraint controlling how a 3D billboard faces the active camera.
 */
enum class BillboardMode : uint8_t {
    Spherical = 0,     ///< Full 3D rotation toward camera position (flares, particles, orbs).
    CylindricalY = 1,  ///< Locks world +Y vertical axis and rotates horizontally toward camera (trees, characters).
    ScreenAligned = 2, ///< Parallel to camera image plane using camera Right and Up axes (floating icons, markers).
};

/**
 * @enum SpritePlaybackMode
 * @brief Playback loop behavior for sprite-sheet animation clips.
 */
enum class SpritePlaybackMode : uint8_t {
    Loop = 0,     ///< Wraps continuously back to frame 0 after reaching the final frame.
    Once = 1,     ///< Plays once cleanly and freezes at the last frame (`IsFinished() == true`).
    PingPong = 2, ///< Bounces forward and backward smoothly between first and last frames.
};

/**
 * @struct SpriteFrame
 * @brief Normalized atlas/sprite-sheet region and frame display duration.
 */
struct SpriteFrame {
    glm::vec2 uvMin{0.0f, 0.0f}; ///< Top-left UV coordinate in `[0, 1]`.
    glm::vec2 uvMax{1.0f, 1.0f}; ///< Bottom-right UV coordinate in `[0, 1]`.
    float duration{0.1f};        ///< Frame duration in seconds.
};

/**
 * @struct SpriteClip
 * @brief Named sequence of `SpriteFrame` regions representing one animation state.
 */
struct SpriteClip {
    std::string name{};
    std::vector<SpriteFrame> frames{};
    SpritePlaybackMode mode{SpritePlaybackMode::Loop};

    /**
     * @brief Builds a uniform grid animation clip from a sprite-sheet layout.
     * @param clipName Unique name used with `Play()`.
     * @param cols Number of grid columns across the sheet.
     * @param rows Number of grid rows down the sheet.
     * @param startFrame Linear zero-based starting cell index (row-major).
     * @param frameCount Number of frames to extract (`0` extracts until sheet end).
     * @param fps Frames per second playback cadence.
     * @param playback Loop mode (`Loop`, `Once`, or `PingPong`).
     * @param texWidth Optional texture width in pixels for half-texel bleed prevention.
     * @param texHeight Optional texture height in pixels for half-texel bleed prevention.
     * @return Populated `SpriteClip`.
     */
    [[nodiscard]] static SpriteClip FromGrid(std::string_view clipName, uint32_t cols, uint32_t rows,
                                             uint32_t startFrame = 0, uint32_t frameCount = 0, float fps = 12.0f,
                                             SpritePlaybackMode playback = SpritePlaybackMode::Loop,
                                             uint32_t texWidth = 0, uint32_t texHeight = 0);

    /**
     * @brief Appends a custom pixel-aligned sub-rectangle frame from an irregular sprite sheet.
     */
    void AddPixelFrame(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t texWidth, uint32_t texHeight,
                       float duration = 0.1f);

    /**
     * @brief Returns sum of all frame durations in seconds.
     */
    [[nodiscard]] float TotalDuration() const noexcept;
};

/**
 * @struct SpriteTexture
 * @brief CPU pixel payload & sampler configuration lazily uploaded to WebGPU by `SpriteRenderer`.
 */
struct SpriteTexture {
    uint64_t id{0};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgbaPixels{};
    bool srgb{true};
    bool nearestFilter{false};

    [[nodiscard]] bool IsValid() const noexcept {
        return id != 0 && width > 0 && height > 0 &&
               rgbaPixels.size() >= static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    }

    /**
     * @brief Creates a `SpriteTexture` from raw tightly packed RGBA8 pixels.
     * @return Valid shared texture handle, or `nullptr` if dimensions/size are invalid.
     */
    [[nodiscard]] static std::shared_ptr<SpriteTexture> FromRGBA8(std::span<const uint8_t> rgba, uint32_t width,
                                                                  uint32_t height, bool nearestFilter = false,
                                                                  bool srgb = true);

    /** @brief Creates a 1x1 solid RGBA texture. */
    [[nodiscard]] static std::shared_ptr<SpriteTexture> CreateSolid(const glm::vec4& color = glm::vec4{1.0f});

    /** @brief Creates a procedural soft radial glow texture suited for additive lights/flares/particles. */
    [[nodiscard]] static std::shared_ptr<SpriteTexture>
    CreateRadialGlow(uint32_t size = 64, const glm::vec4& innerColor = glm::vec4{1.0f}, bool nearestFilter = false);

    /** @brief Creates a procedural smooth ring reticle texture suited for 3D markers and beacons. */
    [[nodiscard]] static std::shared_ptr<SpriteTexture> CreateRing(uint32_t size = 64, float innerRadius = 0.65f,
                                                                   float outerRadius = 0.92f,
                                                                   const glm::vec4& color = glm::vec4{1.0f});

    /** @brief Creates a procedural checkerboard texture useful for pixel-grid previews and tests. */
    [[nodiscard]] static std::shared_ptr<SpriteTexture>
    CreateChecker(uint32_t size = 64, uint32_t cellSize = 8, const glm::vec4& colorA = glm::vec4{1.0f},
                  const glm::vec4& colorB = glm::vec4{0.25f, 0.25f, 0.30f, 1.0f}, bool nearestFilter = true);
};

/**
 * @class SpriteAnimator
 * @brief Clip-based timeline controller shared by `AnimatedSprite` and `AnimatedBillboard`.
 */
class SpriteAnimator {
public:
    void AddClip(SpriteClip clip);
    [[nodiscard]] bool HasClip(std::string_view name) const noexcept;
    void ClearClips() noexcept;

    bool Play(std::string_view clipName, bool restart = false);
    void Stop() noexcept;
    void Pause() noexcept { playing_ = false; }
    void Resume() noexcept {
        if (!finished_) {
            playing_ = true;
        }
    }

    void SetSpeed(float speed) noexcept;
    [[nodiscard]] float GetSpeed() const noexcept { return speed_; }
    [[nodiscard]] bool IsPlaying() const noexcept { return playing_; }
    [[nodiscard]] bool IsFinished() const noexcept { return finished_; }
    [[nodiscard]] std::string_view ActiveClipName() const noexcept;
    [[nodiscard]] size_t CurrentFrameIndex() const noexcept { return frameIndex_; }
    [[nodiscard]] const SpriteFrame* CurrentFrame() const noexcept;

    /**
     * @brief Advances playback timer cleanly; returns true if active frame index or finished state changed.
     */
    bool Update(float deltaTime);

private:
    std::vector<SpriteClip> clips_{};
    size_t activeClip_{0};
    bool hasActiveClip_{false};
    size_t frameIndex_{0};
    int pingPongDir_{1};
    float timer_{0.0f};
    float speed_{1.0f};
    bool playing_{false};
    bool finished_{false};
};

/**
 * @struct Sprite
 * @brief 2D screen-space quad sprite rendered in the crisp swapchain pass.
 */
struct Sprite {
    std::shared_ptr<const SpriteTexture> texture{};
    glm::vec2 position{0.0f, 0.0f};          ///< Anchor screen position in logical pixels.
    glm::vec2 size{64.0f, 64.0f};            ///< Width and height in logical pixels.
    glm::vec2 anchor{0.5f, 0.5f};            ///< Pivot in `[0, 1]` (`{0,0}`=top-left, `{0.5,0.5}`=center).
    float rotationRad{0.0f};                 ///< Clockwise rotation around `anchor` in radians.
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Multiplicative RGBA tint.
    glm::vec2 uvMin{0.0f, 0.0f};             ///< Top-left UV coordinate in `texture`.
    glm::vec2 uvMax{1.0f, 1.0f};             ///< Bottom-right UV coordinate in `texture`.
    bool flipX{false};                       ///< Horizontally mirrors UVs.
    bool flipY{false};                       ///< Vertically mirrors UVs.
    float layer{0.0f};                       ///< Ascending 2D sort layer (higher values draw on top).
    SpriteBlendMode blend{SpriteBlendMode::Alpha};
    bool visible{true};

    void SetFrame(const SpriteFrame& frame) noexcept;
    void SetGridFrame(uint32_t col, uint32_t row, uint32_t cols, uint32_t rows) noexcept;
    void SetPixelRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h) noexcept;
    void ComputeEffectiveUVs(glm::vec2& outMin, glm::vec2& outMax) const noexcept;
};

/**
 * @class AnimatedSprite
 * @brief `Sprite` bundled with a `SpriteAnimator` that automatically updates UVs per frame.
 */
class AnimatedSprite : public Sprite {
public:
    void AddClip(SpriteClip clip) { animator_.AddClip(std::move(clip)); }
    bool Play(std::string_view clipName, bool restart = false);
    void Stop() noexcept;
    void Pause() noexcept { animator_.Pause(); }
    void Resume() noexcept { animator_.Resume(); }
    void SetSpeed(float speed) noexcept { animator_.SetSpeed(speed); }

    bool Update(float deltaTime);

    [[nodiscard]] bool IsPlaying() const noexcept { return animator_.IsPlaying(); }
    [[nodiscard]] bool IsFinished() const noexcept { return animator_.IsFinished(); }
    [[nodiscard]] std::string_view ActiveClipName() const noexcept { return animator_.ActiveClipName(); }
    [[nodiscard]] size_t CurrentFrameIndex() const noexcept { return animator_.CurrentFrameIndex(); }
    [[nodiscard]] SpriteAnimator& Animator() noexcept { return animator_; }
    [[nodiscard]] const SpriteAnimator& Animator() const noexcept { return animator_; }

private:
    SpriteAnimator animator_{};
};

/**
 * @struct Billboard
 * @brief 3D world-space camera-facing quad rendered inside the MSAA HDR transparent pass.
 */
struct Billboard {
    std::shared_ptr<const SpriteTexture> texture{};
    glm::vec3 position{0.0f, 0.0f, 0.0f}; ///< World-space anchor position.
    glm::vec2 size{1.0f, 1.0f};           ///< World-space width and height in meters.
    glm::vec2 anchor{0.5f, 0.5f};         ///< Pivot in `[0, 1]` (`{0.5,1.0}`=bottom-center standing pivot).
    float rollRad{0.0f};                  ///< In-plane roll angle in radians.
    BillboardMode mode{BillboardMode::Spherical};
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Linear RGBA tint/intensity (values `> 1.0` emit HDR bloom).
    glm::vec2 uvMin{0.0f, 0.0f};
    glm::vec2 uvMax{1.0f, 1.0f};
    bool flipX{false};
    bool flipY{false};
    SpriteBlendMode blend{SpriteBlendMode::Alpha};
    bool depthTest{true}; ///< Tests scene depth buffer (`LessEqual`) when true; draws over world when false.
    bool visible{true};

    void SetFrame(const SpriteFrame& frame) noexcept;
    void SetGridFrame(uint32_t col, uint32_t row, uint32_t cols, uint32_t rows) noexcept;
    void SetPixelRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h) noexcept;
    void ComputeEffectiveUVs(glm::vec2& outMin, glm::vec2& outMax) const noexcept;

    /**
     * @brief Computes orthonormal world-space Right and Up vectors for the configured `BillboardMode` and `rollRad`.
     */
    void ComputeBasis(const glm::vec3& cameraPos, const glm::mat4& viewMatrix, glm::vec3& outRight,
                      glm::vec3& outUp) const noexcept;
};

/**
 * @class AnimatedBillboard
 * @brief `Billboard` bundled with a `SpriteAnimator` for animated 3D world sprites/particles/effects.
 */
class AnimatedBillboard : public Billboard {
public:
    void AddClip(SpriteClip clip) { animator_.AddClip(std::move(clip)); }
    bool Play(std::string_view clipName, bool restart = false);
    void Stop() noexcept;
    void Pause() noexcept { animator_.Pause(); }
    void Resume() noexcept { animator_.Resume(); }
    void SetSpeed(float speed) noexcept { animator_.SetSpeed(speed); }

    bool Update(float deltaTime);

    [[nodiscard]] bool IsPlaying() const noexcept { return animator_.IsPlaying(); }
    [[nodiscard]] bool IsFinished() const noexcept { return animator_.IsFinished(); }
    [[nodiscard]] std::string_view ActiveClipName() const noexcept { return animator_.ActiveClipName(); }
    [[nodiscard]] size_t CurrentFrameIndex() const noexcept { return animator_.CurrentFrameIndex(); }
    [[nodiscard]] SpriteAnimator& Animator() noexcept { return animator_; }
    [[nodiscard]] const SpriteAnimator& Animator() const noexcept { return animator_; }

private:
    SpriteAnimator animator_{};
};

} // namespace bgl::core

#endif // BGL_CORE_SPRITE_HPP
