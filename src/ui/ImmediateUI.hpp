/**
 * @file ImmediateUI.hpp
 * @brief C++23 Immediate Mode GUI (IMGUI) with std::pmr per-frame arena allocation,
 *        FreeType SDF font rendering, single-draw-call batching, and embedded WGSL shader.
 */
#pragma once

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "text/FontAtlas.hpp"
#include "text/TextShaper.hpp"
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::ui {

/**
 * @struct UiVertex
 * @brief 68-byte vertex layout for single-batch SDF UI boxes, drop shadows, glass panels, HSV pickers, and text.
 */
struct UiVertex {
    glm::vec2 pos{0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f};
    glm::vec4 sdfRect{0.0f};  ///< (halfW, halfH, cornerRadius, borderOrBlur) or HSV params
    glm::vec4 auxColor{0.0f}; ///< Border RGBA, glass tint, or gradient endpoint RGBA
    float isSdf{0.0f};        ///< 0=solid/grad, 1=SDF glyph, 2=SDF box, 3=shadow, 4=frosted glass, 5=HSV
};

/**
 * @struct UiRect
 * @brief Screen-space axis-aligned rectangle in pixels.
 */
struct UiRect {
    float x{0.0f};
    float y{0.0f};
    float w{0.0f};
    float h{0.0f};

    [[nodiscard]] constexpr bool Contains(float px, float py) const noexcept {
        return px >= x && px <= (x + w) && py >= y && py <= (y + h);
    }
};

/**
 * @struct UiInputState
 * @brief Per-frame mouse state passed to ImmediateUI::BeginFrame().
 */
struct UiInputState {
    float mouseX{0.0f};
    float mouseY{0.0f};
    bool mouseDown{false};
    bool mousePressed{false};
    bool mouseReleased{false};
    float wheelDelta{0.0f};
};

/**
 * @struct UiStyle
 * @brief Sleek glassmorphic dark UI theme styling constants.
 */
struct UiStyle {
    glm::vec4 windowBg{0.075f, 0.085f, 0.110f, 0.88f};
    glm::vec4 windowBorder{0.28f, 0.34f, 0.44f, 0.85f};
    glm::vec4 headerBg{0.115f, 0.140f, 0.190f, 0.96f};
    glm::vec4 widgetBg{0.145f, 0.165f, 0.215f, 1.0f};
    glm::vec4 widgetHover{0.21f, 0.25f, 0.33f, 1.0f};
    glm::vec4 widgetActive{0.18f, 0.48f, 0.82f, 1.0f};
    glm::vec4 accent{0.24f, 0.62f, 0.96f, 1.0f};
    glm::vec4 textPrimary{0.93f, 0.95f, 0.98f, 1.0f};
    glm::vec4 textMuted{0.64f, 0.69f, 0.76f, 1.0f};
    glm::vec4 popupBg{0.105f, 0.120f, 0.155f, 0.97f};
    glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 0.50f};
    glm::vec4 sliderTrack{0.11f, 0.13f, 0.17f, 1.0f};
    glm::vec4 sparklineFill{0.24f, 0.62f, 0.96f, 0.24f};

    float padding{10.0f};
    float itemSpacing{6.0f};
    float rowHeight{24.0f};
    float headerHeight{28.0f};
    float textScale{0.625f}; ///< Scales 24px SDF font atlas to ~15px crisp UI text
    float cornerRadius{6.0f};
    float widgetRadius{4.0f};
    float shadowBlur{14.0f};
};

/**
 * @class ImmediateUI
 * @brief Stateless Immediate Mode GUI with PMR monotonic frame arena and single-draw-call WebGPU batching.
 */
class ImmediateUI {
public:
    ImmediateUI();
    ~ImmediateUI();

    ImmediateUI(const ImmediateUI&) = delete;
    ImmediateUI& operator=(const ImmediateUI&) = delete;

    /**
     * @brief Initializes WebGPU pipeline (from `#embed` WGSL shader) and FreeType SDF font atlas.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount = 1);

    /**
     * @brief Attaches a blurred HDR scene mip view for frosted glassmorphic window panels (nullable).
     */
    void SetBackdropTextureView(WGPUTextureView view) noexcept;

    /**
     * @brief Releases WebGPU pipeline, buffers, and font atlas resources.
     */
    void Shutdown() noexcept;

    /**
     * @brief Resets the std::pmr::monotonic_buffer_resource arena and begins a new UI frame.
     */
    void BeginFrame(const UiInputState& input, uint32_t viewportWidth, uint32_t viewportHeight);

    /**
     * @brief Opens a draggable glassmorphic UI panel with auto-layout vertical cursor.
     */
    void BeginWindow(std::string_view title, float x, float y, float width);

    /**
     * @brief Finalizes the current window background, drop shadow & border sizing behind its widgets.
     */
    void EndWindow();

    /**
     * @brief Renders a text label at the current vertical layout cursor.
     */
    void Label(std::string_view text, const glm::vec4& color = glm::vec4{0.93f, 0.95f, 0.98f, 1.0f});

    /**
     * @brief Renders a subtle horizontal divider rule between sections.
     */
    void Separator();

    /**
     * @brief Renders a collapsible section header with persisted open/closed state.
     */
    bool CollapsingHeader(std::string_view label, bool defaultOpen = true);

    /**
     * @brief Renders an interactive button; returns true on click (AABB intersection).
     */
    bool Button(std::string_view label);

    /**
     * @brief Renders a toggleable checkbox; flips state and returns true on click.
     */
    bool Checkbox(std::string_view label, bool& state);

    /**
     * @brief Renders an interactive continuous horizontal slider; returns true when value changes.
     */
    bool SliderFloat(std::string_view label, float& value, float minVal, float maxVal,
                     std::string_view valueFormat = "{:.2f}");

    /**
     * @brief Renders an RGB swatch row with an interactive deferred HSV color-picker popup.
     */
    bool ColorEdit3(std::string_view label, glm::vec3& color);

    /**
     * @brief Renders a rounded mini-chart with shaded area fill and crisp polyline segments.
     */
    void Sparkline(std::string_view label, std::span<const float> values, float minVal = 0.0f, float maxVal = 0.0f,
                   float height = 40.0f, const glm::vec4& lineColor = glm::vec4{0.24f, 0.78f, 0.96f, 1.0f});

    /**
     * @brief Renders a dropdown selector; opens an overlay stack on click and returns true when selection changes.
     */
    bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex);

    /**
     * @brief Opens a wheel-scrollable scissor-clipped region inside the active window.
     */
    void BeginScrollArea(std::string_view id, float maxHeight);

    /**
     * @brief Closes the active scrollable region and emits its proportional scrollbar thumb.
     */
    void EndScrollArea();

    /**
     * @brief Renders a sleek rounded progress bar with optional caption text and percentage readout.
     */
    void ProgressBar(float fraction, std::string_view statusText = {}, float height = 18.0f,
                     const glm::vec4& fillColor = glm::vec4{0.0f});

    /**
     * @brief Draws a centered floating frosted-glass loading banner (visible even when panels are hidden).
     */
    void DrawLoadingBanner(std::string_view title, float fraction, std::string_view detailText, bool isError = false,
                           float alphaMultiplier = 1.0f);

    /**
     * @brief Draws a compact rounded pill badge at arbitrary screen coordinates (e.g. 3D anchor label).
     */
    void DrawFloatingBadge(float x, float y, std::string_view text, const glm::vec4& accentColor);

    /**
     * @brief Merges deferred overlay batches (open dropdowns / color pickers) and finalizes draw commands.
     */
    void EndFrame();

    /**
     * @brief Uploads batched vertices/indices and dispatches single-batch or scissor-split UI draw calls.
     */
    void Render(WGPURenderPassEncoder pass, uint32_t framebufferWidth = 0, uint32_t framebufferHeight = 0);

    /**
     * @brief Returns true if the mouse cursor is hovering over any UI window or open popup overlay,
     *        so 3D camera/world picking events can be blocked cleanly.
     */
    [[nodiscard]] bool WantsMouse() const noexcept { return wantsMouse_; }

    /**
     * @brief Returns the active vertex/index/draw-command counts generated in the current frame.
     */
    [[nodiscard]] size_t GetVertexCount() const noexcept { return vertices_.size(); }
    [[nodiscard]] size_t GetIndexCount() const noexcept { return indices_.size(); }
    [[nodiscard]] size_t GetDrawCommandCount() const noexcept { return drawCmds_.size(); }
    [[nodiscard]] const UiRect& GetLastWindowRect() const noexcept { return lastCompletedWindowRect_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return gpuInitialized_; }
    [[nodiscard]] bool IsSdfFontReady() const noexcept { return fontReady_; }
    [[nodiscard]] static std::string_view GetEmbeddedShaderSource() noexcept;

    [[nodiscard]] static glm::vec3 RgbToHsv(const glm::vec3& rgb) noexcept;
    [[nodiscard]] static glm::vec3 HsvToRgb(const glm::vec3& hsv) noexcept;

    [[nodiscard]] UiStyle& Style() noexcept { return style_; }
    [[nodiscard]] const UiStyle& Style() const noexcept { return style_; }

private:
    struct PendingDropdownOverlay {
        uint64_t id{0};
        UiRect anchorRect{};
        UiRect popupRect{};
        std::span<const char* const> options{};
        size_t selectedIndex{0};
        float scrollOffset{0.0f};
        float contentHeight{0.0f};
        bool scrollable{false};
    };

    struct PendingColorOverlay {
        uint64_t id{0};
        UiRect anchorRect{};
        UiRect popupRect{};
        UiRect svRect{};
        UiRect hueRect{};
        glm::vec3 hsv{0.0f, 1.0f, 1.0f};
        glm::vec3 rgb{1.0f};
    };

    struct DrawCommand {
        uint32_t firstIndex{0};
        uint32_t indexCount{0};
        bool hasScissor{false};
        UiRect scissorRect{};
    };

    void FlushDrawCommand(bool hasScissor, const UiRect& scissorRect);
    void EmitColorPickerOverlay();
    void EmitDropdownOverlay();
    void EmitQuad(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y, float w,
                  float h, const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec4& color, float isSdf,
                  const glm::vec4& sdfRect = glm::vec4{0.0f}, const glm::vec4& auxColor = glm::vec4{0.0f}) const;
    void EmitRect(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y, float w,
                  float h, const glm::vec4& color) const;
    void EmitRectWithBorder(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                            float w, float h, const glm::vec4& fill, const glm::vec4& border,
                            float borderWidth = 1.0f) const;
    void EmitRoundedBox(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y, float w,
                        float h, float radius, const glm::vec4& fill, const glm::vec4& border, float borderWidth = 1.0f,
                        bool frostedGlass = false) const;
    void EmitShadowBox(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y, float w,
                       float h, float radius, float blurRadius, const glm::vec4& shadowColor) const;
    void EmitLineSegment(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, const glm::vec2& a,
                         const glm::vec2& b, float thickness, const glm::vec4& color) const;
    void EmitText(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, std::string_view text, float x,
                  float y, const glm::vec4& color, float scale);

    void EnsureGpuBuffers(size_t vertexCount, size_t indexCount);
    void ReleaseDynamicBuffers() noexcept;
    void RebuildBindGroup();
    [[nodiscard]] float MeasureTextWidth(std::string_view text, float scale);
    [[nodiscard]] std::string EllipsizeText(std::string_view text, float maxWidth, float scale);
    [[nodiscard]] bool IsBlockedByPopup() const noexcept;
    [[nodiscard]] bool IsMouseInClipArea() const noexcept;
    [[nodiscard]] static uint64_t HashId(std::string_view label) noexcept;

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};
    uint32_t sampleCount_{1};
    bool gpuInitialized_{false};
    bool fontReady_{false};

    text::FontAtlas fontAtlas_{};
    gpu::BindGroupLayoutHandle bindGroupLayout_{};
    gpu::RenderPipelineHandle pipeline_{};
    gpu::BufferHandle uniformBuffer_{};
    gpu::SamplerHandle backdropSampler_{};
    gpu::TextureHandle fallbackBackdropTexture_{};
    gpu::TextureViewHandle fallbackBackdropView_{};
    WGPUTextureView currentBackdropView_{nullptr};
    gpu::BindGroupHandle bindGroup_{};
    gpu::BufferHandle vertexBuffer_{};
    gpu::BufferHandle indexBuffer_{};
    size_t vertexCapacity_{0};
    size_t indexCapacity_{0};
    size_t vertexBufferBytes_{0};
    size_t indexBufferBytes_{0};

    // Per-frame std::pmr monotonic arena (cleared in O(1) at BeginFrame)
    static constexpr size_t kArenaBytes = 512 * 1024;
    std::vector<std::byte> arenaBacking_{};
    std::pmr::monotonic_buffer_resource frameArena_{};
    std::pmr::vector<UiVertex> vertices_{&frameArena_};
    std::pmr::vector<uint32_t> indices_{&frameArena_};
    std::pmr::vector<UiVertex> overlayVertices_{&frameArena_};
    std::pmr::vector<uint32_t> overlayIndices_{&frameArena_};
    std::pmr::vector<UiRect> hitRects_{&frameArena_};
    std::pmr::vector<DrawCommand> drawCmds_{&frameArena_};
    uint32_t batchStartIndex_{0};

    UiInputState input_{};
    UiStyle style_{};
    uint32_t viewportWidth_{1280};
    uint32_t viewportHeight_{720};
    bool wantsMouse_{false};

    // Active window layout cursor state
    bool inWindow_{false};
    uint64_t currentWindowId_{0};
    UiRect currentWindowRect_{};
    UiRect lastCompletedWindowRect_{};
    float cursorX_{0.0f};
    float cursorY_{0.0f};
    float contentWidth_{0.0f};
    size_t windowShadowVertexOffset_{0};
    size_t windowBgVertexOffset_{0};

    // Persistent widget interaction states
    std::unordered_map<uint64_t, glm::vec2> windowOffsets_{};
    std::unordered_map<uint64_t, bool> collapsingHeaders_{};
    std::unordered_map<uint64_t, float> scrollOffsets_{};
    std::unordered_map<uint64_t, float> scrollContentHeights_{};
    std::unordered_map<uint64_t, glm::vec3> colorPickerHsv_{};

    uint64_t activeDragWindowId_{0};
    glm::vec2 dragStartMouse_{0.0f};
    glm::vec2 dragStartOffset_{0.0f};
    uint64_t activeSliderId_{0};
    uint64_t activeScrollbarId_{0};
    uint64_t activeColorSvId_{0};
    uint64_t activeColorHueId_{0};

    // Active scroll-area state
    bool inScrollArea_{false};
    uint64_t activeScrollId_{0};
    UiRect scrollRect_{};
    float scrollSavedCursorX_{0.0f};
    float scrollSavedCursorY_{0.0f};
    float scrollSavedContentW_{0.0f};
    float scrollContentStartY_{0.0f};
    float scrollAppliedOffset_{0.0f};

    // Persistent popup states (dropdown / color picker)
    uint64_t openDropdownId_{0};
    bool dropdownToggledThisFrame_{false};
    bool popupConsumedInputThisFrame_{false};
    uint64_t activeDropdownScrollId_{0};
    std::unordered_map<uint64_t, float> dropdownScrollOffsets_{};
    UiRect activePopupRect_{};
    bool hasActivePopupRect_{false};
    PendingDropdownOverlay pendingOverlay_{};
    bool hasPendingOverlay_{false};

    uint64_t openColorId_{0};
    bool colorToggledThisFrame_{false};
    PendingColorOverlay pendingColorOverlay_{};
    bool hasPendingColorOverlay_{false};
};

/**
 * @namespace bgl::ui::UI
 * @brief Ergonomic static-style immediate-mode widget facade.
 */
namespace UI {
void SetContext(ImmediateUI* ctx) noexcept;
[[nodiscard]] ImmediateUI* GetContext() noexcept;
void Label(std::string_view text, const glm::vec4& color = glm::vec4{0.93f, 0.95f, 0.98f, 1.0f});
void Separator();
bool CollapsingHeader(std::string_view label, bool defaultOpen = true);
bool Button(std::string_view label);
bool Checkbox(std::string_view label, bool& state);
bool SliderFloat(std::string_view label, float& value, float minVal, float maxVal,
                 std::string_view valueFormat = "{:.2f}");
bool ColorEdit3(std::string_view label, glm::vec3& color);
void Sparkline(std::string_view label, std::span<const float> values, float minVal = 0.0f, float maxVal = 0.0f,
               float height = 40.0f, const glm::vec4& lineColor = glm::vec4{0.24f, 0.78f, 0.96f, 1.0f});
bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex);
void ProgressBar(float fraction, std::string_view statusText = {}, float height = 18.0f,
                 const glm::vec4& fillColor = glm::vec4{0.0f});
void BeginScrollArea(std::string_view id, float maxHeight);
void EndScrollArea();
} // namespace UI

} // namespace bgl::ui
