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
#include <vector>
#include <webgpu/webgpu.h>

namespace bgl::ui {

/**
 * @struct UiVertex
 * @brief 36-byte vertex layout for batched IMGUI quads and SDF glyphs.
 */
struct UiVertex {
    glm::vec2 pos{0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f};
    float isSdf{0.0f}; ///< 0.0 = solid UI quad/border, 1.0 = SDF font glyph
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
};

/**
 * @struct UiStyle
 * @brief Minimalist flat dark theme styling constants.
 */
struct UiStyle {
    glm::vec4 windowBg{0.09f, 0.10f, 0.12f, 0.92f};
    glm::vec4 windowBorder{0.24f, 0.27f, 0.33f, 0.95f};
    glm::vec4 headerBg{0.14f, 0.16f, 0.20f, 0.98f};
    glm::vec4 widgetBg{0.16f, 0.18f, 0.22f, 1.0f};
    glm::vec4 widgetHover{0.23f, 0.26f, 0.32f, 1.0f};
    glm::vec4 widgetActive{0.18f, 0.48f, 0.82f, 1.0f};
    glm::vec4 accent{0.24f, 0.62f, 0.96f, 1.0f};
    glm::vec4 textPrimary{0.93f, 0.95f, 0.98f, 1.0f};
    glm::vec4 textMuted{0.64f, 0.69f, 0.76f, 1.0f};
    glm::vec4 popupBg{0.12f, 0.13f, 0.16f, 0.98f};

    float padding{10.0f};
    float itemSpacing{6.0f};
    float rowHeight{24.0f};
    float headerHeight{28.0f};
    float textScale{0.625f}; ///< Scales 24px SDF font atlas to ~15px crisp UI text
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
     * @brief Initializes WebGPU pipeline (from #embed WGSL shader) and FreeType SDF font atlas.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount = 1);

    /**
     * @brief Releases WebGPU pipeline, buffers, and font atlas resources.
     */
    void Shutdown() noexcept;

    /**
     * @brief Resets the std::pmr::monotonic_buffer_resource arena and begins a new UI frame.
     */
    void BeginFrame(const UiInputState& input, uint32_t viewportWidth, uint32_t viewportHeight);

    /**
     * @brief Opens a dark flat-styled UI panel with auto-layout vertical cursor.
     */
    void BeginWindow(std::string_view title, float x, float y, float width);

    /**
     * @brief Finalizes the current window background & border sizing behind its widgets.
     */
    void EndWindow();

    /**
     * @brief Renders a text label at the current vertical layout cursor.
     */
    void Label(std::string_view text, const glm::vec4& color = glm::vec4{0.93f, 0.95f, 0.98f, 1.0f});

    /**
     * @brief Renders an interactive button; returns true on click (AABB intersection).
     */
    bool Button(std::string_view label);

    /**
     * @brief Renders a toggleable checkbox; flips state and returns true on click.
     */
    bool Checkbox(std::string_view label, bool& state);

    /**
     * @brief Renders a dropdown selector; opens an overlay stack on click and returns true when selection changes.
     */
    bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex);

    /**
     * @brief Merges deferred overlay batches (open dropdowns) and prepares single-draw-call geometry.
     */
    void EndFrame();

    /**
     * @brief Uploads batched vertices/indices and draws the entire UI in a single WebGPU draw call.
     */
    void Render(WGPURenderPassEncoder pass);

    /**
     * @brief Returns true if the mouse cursor is hovering over any UI window or open dropdown overlay,
     *        so 3D camera/world picking events can be blocked cleanly.
     */
    [[nodiscard]] bool WantsMouse() const noexcept { return wantsMouse_; }

    /**
     * @brief Returns the active vertex/index counts generated in the current frame (for diagnostics/testing).
     */
    [[nodiscard]] size_t GetVertexCount() const noexcept { return vertices_.size(); }
    [[nodiscard]] size_t GetIndexCount() const noexcept { return indices_.size(); }
    [[nodiscard]] bool IsInitialized() const noexcept { return gpuInitialized_; }
    [[nodiscard]] bool IsSdfFontReady() const noexcept { return fontReady_; }
    [[nodiscard]] static std::string_view GetEmbeddedShaderSource() noexcept;

    [[nodiscard]] UiStyle& Style() noexcept { return style_; }
    [[nodiscard]] const UiStyle& Style() const noexcept { return style_; }

private:
    struct PendingDropdownOverlay {
        uint64_t id{0};
        UiRect anchorRect{};
        UiRect popupRect{};
        std::span<const char* const> options{};
        size_t selectedIndex{0};
    };

    void EmitQuad(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y, float w,
                  float h, const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec4& color, float isSdf) const;
    void EmitRect(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y, float w,
                  float h, const glm::vec4& color) const;
    void EmitRectWithBorder(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, float x, float y,
                            float w, float h, const glm::vec4& fill, const glm::vec4& border,
                            float borderWidth = 1.0f) const;
    void EmitText(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, std::string_view text, float x,
                  float y, const glm::vec4& color, float scale);

    void EnsureGpuBuffers(size_t vertexCount, size_t indexCount);
    void ReleaseDynamicBuffers() noexcept;
    [[nodiscard]] bool IsBlockedByPopup() const noexcept;
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
    gpu::BindGroupHandle bindGroup_{};
    gpu::BufferHandle vertexBuffer_{};
    gpu::BufferHandle indexBuffer_{};
    size_t vertexCapacity_{0};
    size_t indexCapacity_{0};
    size_t vertexBufferBytes_{0};
    size_t indexBufferBytes_{0};

    // Per-frame std::pmr monotonic arena (cleared in O(1) at BeginFrame)
    static constexpr size_t kArenaBytes = 256 * 1024;
    std::vector<std::byte> arenaBacking_{};
    std::pmr::monotonic_buffer_resource frameArena_{};
    std::pmr::vector<UiVertex> vertices_{&frameArena_};
    std::pmr::vector<uint32_t> indices_{&frameArena_};
    std::pmr::vector<UiVertex> overlayVertices_{&frameArena_};
    std::pmr::vector<uint32_t> overlayIndices_{&frameArena_};
    std::pmr::vector<UiRect> hitRects_{&frameArena_};

    UiInputState input_{};
    UiStyle style_{};
    uint32_t viewportWidth_{1280};
    uint32_t viewportHeight_{720};
    bool wantsMouse_{false};

    // Active window layout cursor state
    bool inWindow_{false};
    UiRect currentWindowRect_{};
    float cursorX_{0.0f};
    float cursorY_{0.0f};
    float contentWidth_{0.0f};
    size_t windowBgVertexOffset_{0};

    // Persistent dropdown popup state
    uint64_t openDropdownId_{0};
    bool dropdownToggledThisFrame_{false};
    UiRect activePopupRect_{};
    bool hasActivePopupRect_{false};
    PendingDropdownOverlay pendingOverlay_{};
    bool hasPendingOverlay_{false};
};

/**
 * @namespace bgl::ui::UI
 * @brief Ergonomic static-style immediate-mode widget facade matching UI::Label/Button/Checkbox/Dropdown.
 */
namespace UI {
void SetContext(ImmediateUI* ctx) noexcept;
[[nodiscard]] ImmediateUI* GetContext() noexcept;
void Label(std::string_view text, const glm::vec4& color = glm::vec4{0.93f, 0.95f, 0.98f, 1.0f});
bool Button(std::string_view label);
bool Checkbox(std::string_view label, bool& state);
bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex);
} // namespace UI

} // namespace bgl::ui
