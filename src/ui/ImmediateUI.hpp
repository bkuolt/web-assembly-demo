/**
 * @file ImmediateUI.hpp
 * @brief C++23 Immediate Mode GUI (IMGUI) with std::pmr per-frame arena allocation,
 *        multi-font FreeType SDF rendering, optional hover animations across all widgets,
 *        modals (MessageBox, LoadingScreen, Tooltips), and single-batch WebGPU rendering.
 */
#ifndef BGL_UI_IMMEDIATEUI_HPP
#define BGL_UI_IMMEDIATEUI_HPP

#include "core/Types.hpp"
#include "gpu/Handle.hpp"
#include "text/FontAtlas.hpp"
#include "text/FontLibrary.hpp"
#include "text/TextShaper.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
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
 * @brief Vertex layout for batched SDF UI boxes, drop shadows, frosted glass panels, HSV pickers, and text.
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
 * @enum MessageBoxType
 * @brief Semantic severity and accent style for frosted-glass modal message boxes.
 */
enum class MessageBoxType : uint8_t {
    Info = 0,
    Warning = 1,
    Error = 2,
    Confirm = 3,
};

/**
 * @enum ContainerLayout
 * @brief Flow direction for widgets placed inside a `BeginContainer`/`EndContainer` block.
 */
enum class ContainerLayout : uint8_t {
    Vertical = 0,
    Horizontal = 1,
};

/**
 * @struct ContainerOptions
 * @brief Layout and visual styling options for `ImmediateUI::BeginContainer()`.
 */
struct ContainerOptions {
    ContainerLayout layout{ContainerLayout::Vertical};
    float padding{6.0f};
    float spacing{6.0f};
    float width{0.0f};     ///< 0 = fill available content width
    float height{0.0f};    ///< Minimum container height in pixels
    uint32_t columns{0};   ///< Horizontal wrap column count (0 = single row / auto 2-col)
    float itemWidth{0.0f}; ///< Explicit item width for horizontal layout (0 = auto-divide)
    bool showBackground{false};
    bool showBorder{false};
    bool hoverable{false};
    glm::vec4 backgroundColor{0.0f};
    glm::vec4 borderColor{0.0f};
    float cornerRadius{-1.0f}; ///< < 0 uses UiStyle::widgetRadius
};

/**
 * @struct PanelOptions
 * @brief Styling and behavior options for `ImmediateUI::BeginPanel()`.
 */
struct PanelOptions {
    bool frosted{true};
    bool showShadow{true};
    bool hoverable{true};
    bool collapsible{false};
    bool defaultOpen{true};
    float padding{10.0f};
    float cornerRadius{-1.0f};       ///< < 0 uses UiStyle::cornerRadius
    glm::vec4 backgroundColor{0.0f}; ///< alpha <= 0 uses UiStyle::panelBg
    glm::vec4 borderColor{0.0f};     ///< alpha <= 0 uses UiStyle::windowBorder
};

/**
 * @struct UiStyle
 * @brief Sleek glassmorphic dark UI theme styling and optional hover animation parameters.
 */
struct UiStyle {
    glm::vec4 windowBg{0.075f, 0.085f, 0.110f, 0.88f};
    glm::vec4 panelBg{0.095f, 0.110f, 0.145f, 0.90f};
    glm::vec4 groupBg{0.065f, 0.075f, 0.100f, 0.72f};
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
    glm::vec4 hoverHighlight{0.24f, 0.62f, 0.96f, 0.14f};
    glm::vec4 hoverBorder{0.24f, 0.62f, 0.96f, 0.75f};
    glm::vec4 modalBackdrop{0.025f, 0.035f, 0.060f, 0.72f};

    float padding{10.0f};
    float itemSpacing{6.0f};
    float rowHeight{24.0f};
    float headerHeight{28.0f};
    float textScale{0.625f}; ///< Scales 24px SDF font atlas to ~15px crisp UI text
    float cornerRadius{6.0f};
    float widgetRadius{4.0f};
    float shadowBlur{14.0f};

    bool enableHoverEffects{true};     ///< Master toggle for optional hover highlights/animations on all widgets
    float hoverTransitionSpeed{14.0f}; ///< Smoothing rate (1/s) for hover visual transitions
    float hoverGlowStrength{1.0f};     ///< Intensity multiplier for hover highlights and rim borders
};

/**
 * @class ImmediateUI
 * @brief Stateless Immediate Mode GUI with PMR monotonic frame arena, multi-font FreeType SDF atlases,
 *        optional animated hover highlights across every widget, frosted-glass modals/loading screens,
 *        and WebGPU draw-call batching.
 */
class ImmediateUI {
public:
    ImmediateUI();
    ~ImmediateUI();

    ImmediateUI(const ImmediateUI&) = delete;
    ImmediateUI& operator=(const ImmediateUI&) = delete;

    /**
     * @brief Initializes WebGPU pipeline (from `#embed` WGSL shader) and default FreeType SDF font atlas.
     */
    core::Result<void> Initialize(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFormat,
                                  uint32_t sampleCount = 1);

    /**
     * @brief Attaches a blurred HDR scene mip view for frosted glassmorphic window panels (nullable).
     */
    void SetBackdropTextureView(WGPUTextureView view) noexcept;

    /**
     * @brief Releases WebGPU pipeline, buffers, and all loaded font atlas resources.
     */
    void Shutdown() noexcept;

    /**
     * @brief Resets the std::pmr::monotonic_buffer_resource arena and begins a new UI frame.
     */
    void BeginFrame(const UiInputState& input, uint32_t viewportWidth, uint32_t viewportHeight,
                    float deltaTime = 0.016f);

    /**
     * @brief Opens a draggable glassmorphic UI panel with auto-layout vertical cursor.
     */
    void BeginWindow(std::string_view title, float x, float y, float width);

    /**
     * @brief Finalizes the current window background, drop shadow & border sizing behind its widgets.
     */
    void EndWindow();

    /**
     * @brief Renders a text label at the current vertical layout cursor with optional hover feedback.
     */
    void Label(std::string_view text, const glm::vec4& color = glm::vec4{0.93f, 0.95f, 0.98f, 1.0f});

    /**
     * @brief Renders a subtle horizontal divider rule between sections with optional hover feedback.
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
     * @brief Renders an animated pill-switch toggle box with optional description and hover glow.
     * @return True on click when `state` is toggled.
     */
    bool ToggleBox(std::string_view label, bool& state, std::string_view description = {});

    /**
     * @brief Renders a single radio-style option row; returns true on click.
     */
    bool Option(std::string_view label, bool selected);

    /**
     * @brief Convenience radio option bound to a typed value; assigns `optionValue` and returns true on click.
     */
    template <typename T>
    bool Option(std::string_view label, T& currentValue, const T& optionValue) {
        if (Option(label, currentValue == optionValue)) {
            currentValue = optionValue;
            return true;
        }
        return false;
    }

    /**
     * @brief Renders a vertical radio group or horizontal segmented option bar; returns true when selection changes.
     */
    bool OptionGroup(std::string_view label, std::span<const std::string_view> options, size_t& selectedIndex,
                     bool horizontal = false);
    bool OptionGroup(std::string_view label, std::span<const char* const> options, size_t& selectedIndex,
                     bool horizontal = false);

    /**
     * @brief Opens a nestable layout container (vertical or horizontal/grid) with optional card/hover styling.
     */
    void BeginContainer(std::string_view id, const ContainerOptions& options = {});

    /**
     * @brief Closes the active layout container and restores the parent cursor/width.
     */
    void EndContainer();

    /**
     * @brief Opens an in-flow glassmorphic panel card at the current layout cursor. Always pair with `EndPanel()`.
     * @return True when the panel body is open (always true unless `options.collapsible` is set and collapsed).
     */
    bool BeginPanel(std::string_view title, const PanelOptions& options = {});

    /**
     * @brief Opens a standalone fixed/docked glassmorphic panel at explicit screen coordinates. Always pair with
     * `EndPanel()`.
     */
    bool BeginPanel(std::string_view id, float x, float y, float width, float height = 0.0f,
                    std::string_view title = {}, const PanelOptions& options = {});

    /**
     * @brief Finalizes the active panel's shadow, background, border, and hover state.
     */
    void EndPanel();

    /**
     * @brief Opens a visually grouped section card with optional header label and hover-reactive border.
     */
    void BeginGroup(std::string_view label = {}, bool border = true);

    /**
     * @brief Closes the active group card and advances the parent layout cursor.
     */
    void EndGroup();

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
     * @brief Renders a rounded mini-chart with shaded area fill, polyline segments, and hover sample indicator.
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
     * @brief Draws a fullscreen frosted-glass loading screen overlay with optional Cancel button.
     * @param title Main loading headline.
     * @param fraction Progress in `[0..1]`, or `< 0` for smooth animated indeterminate mode.
     * @param statusText Optional detail/status subtitle.
     * @param isError Applies error accent styling when true.
     * @param cancellable Shows a frosted interactive Cancel button when true.
     * @param alphaMultiplier Smooth fade multiplier `[0..1]`.
     * @return `true` when the user clicks the Cancel button in this frame.
     */
    bool DrawLoadingScreen(std::string_view title, float fraction = -1.0f, std::string_view statusText = {},
                           bool isError = false, bool cancellable = false, float alphaMultiplier = 1.0f);

    /**
     * @brief Shows or updates a persistent stateful fullscreen loading screen rendered automatically in `EndFrame()`.
     */
    void ShowLoadingScreen(std::string_view title, float fraction = -1.0f, std::string_view statusText = {},
                           bool isError = false);

    /**
     * @brief Hides the persistent stateful loading screen immediately.
     */
    void HideLoadingScreen() noexcept;

    /**
     * @brief Renders an immediate frosted-glass modal message box with wrapped body text and action buttons.
     * @return Clicked button index `[0..buttons.size()-1]`, or `-1` while open with no click.
     */
    int MessageBox(std::string_view id, std::string_view title, std::string_view message,
                   std::span<const std::string_view> buttons = {}, MessageBoxType type = MessageBoxType::Info,
                   bool dimBackground = true);

    /**
     * @brief Queues a persistent modal message box dismissed automatically when a button is clicked.
     */
    void ShowMessageBox(std::string_view id, std::string_view title, std::string_view message,
                        MessageBoxType type = MessageBoxType::Info, std::span<const std::string_view> buttons = {},
                        std::function<void(int)> onResult = {}, bool dimBackground = true);

    /**
     * @brief Dismisses the queued modal matching `id` (or the active top modal if `id` is empty).
     */
    void CloseMessageBox(std::string_view id = {});

    /** @brief Returns true when any modal MessageBox or fullscreen LoadingScreen is currently active. */
    [[nodiscard]] bool HasOpenModal() const noexcept;

    /**
     * @brief Draws a compact rounded pill badge at arbitrary screen coordinates (e.g. 3D anchor label).
     */
    void DrawFloatingBadge(float x, float y, std::string_view text, const glm::vec4& accentColor);

    /**
     * @brief Enables or disables hover visual highlights cleanly for subsequent items within a Push/Pop scope.
     */
    void PushHoverEnabled(bool enabled);
    void PopHoverEnabled();
    [[nodiscard]] bool AreHoverEffectsEnabled() const noexcept;

    /**
     * @brief Pushes/pops a scoped widget ID onto the ID stack to prevent label collisions across panels or loops.
     */
    void PushId(std::string_view id);
    void PushId(int intId);
    void PopId();

    /**
     * @brief Disables interaction and dims subsequent widgets until the matching `EndDisabled()`.
     */
    void BeginDisabled(bool disabled = true);
    void EndDisabled();
    [[nodiscard]] bool IsDisabled() const noexcept;

    /** @brief Returns true if the most recently emitted widget/element is hovered this frame. */
    [[nodiscard]] bool IsItemHovered() const noexcept { return lastItemHovered_; }

    /** @brief Queues a floating frosted-glass tooltip near the cursor if the previous UI item is hovered. */
    void Tooltip(std::string_view text);

    /**
     * @brief Discovers installed system and asset fonts on demand via FontLibrary.
     */
    [[nodiscard]] std::vector<text::FontDescriptor> GetAvailableFonts() const;

    /**
     * @brief Loads (or replaces) a named font slot using FontLibrary::Find (`path`, `family`, `"sans"`, `"mono"`,
     *        or `"serif"`). Falls back cleanly to the embedded font when `allowEmbeddedFallback` is true.
     */
    core::Result<void> LoadFont(std::string_view slotName, const text::FontQuery& query, float pixelSize = 24.0f,
                                bool allowEmbeddedFallback = false);
    core::Result<void> LoadFont(std::string_view slotName, std::string_view nameOrPath, float pixelSize = 24.0f);
    core::Result<void> LoadFontFromMemory(std::string_view slotName, std::span<const uint8_t> fontData,
                                          float pixelSize = 24.0f);

    /**
     * @brief Replaces the `"default"` font slot used by panels and widgets when no font is pushed.
     */
    bool SetDefaultFont(std::string_view nameOrPath, float pixelSize = 24.0f);

    /**
     * @brief Activates a previously loaded font slot until the matching `PopFont()`.
     */
    bool PushFont(std::string_view slotName);
    void PopFont();
    [[nodiscard]] std::string_view GetActiveFontName() const noexcept;

    /**
     * @brief Merges deferred overlay batches (popups, queued modals, loading screens, tooltips) and finalizes draw
     * calls.
     */
    void EndFrame();

    /**
     * @brief Uploads batched vertices/indices and dispatches single-batch or scissor/font-split UI draw calls.
     */
    void Render(WGPURenderPassEncoder pass, uint32_t framebufferWidth = 0, uint32_t framebufferHeight = 0);

    /**
     * @brief Returns true if the mouse cursor is hovering over any UI window, modal overlay, or popup,
     *        so 3D camera/world picking events can be blocked cleanly.
     */
    [[nodiscard]] bool WantsMouse() const noexcept { return wantsMouse_; }

    /** @brief Active frame geometry & batch inspection accessors. */
    [[nodiscard]] size_t GetVertexCount() const noexcept { return vertices_.size(); }
    [[nodiscard]] size_t GetIndexCount() const noexcept { return indices_.size(); }
    [[nodiscard]] size_t GetDrawCommandCount() const noexcept { return drawCmds_.size(); }
    [[nodiscard]] const UiRect& GetLastWindowRect() const noexcept { return lastCompletedWindowRect_; }
    [[nodiscard]] const UiRect& GetLastItemRect() const noexcept { return lastItemRect_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return gpuInitialized_; }
    [[nodiscard]] bool IsSdfFontReady() const noexcept;
    [[nodiscard]] static std::string_view GetEmbeddedShaderSource() noexcept;

    [[nodiscard]] static glm::vec3 RgbToHsv(const glm::vec3& rgb) noexcept;
    [[nodiscard]] static glm::vec3 HsvToRgb(const glm::vec3& hsv) noexcept;

    [[nodiscard]] UiStyle& Style() noexcept { return style_; }
    [[nodiscard]] const UiStyle& Style() const noexcept { return style_; }

private:
    struct FontSlot {
        std::string name{};
        std::unique_ptr<text::FontAtlas> atlas{};
        float basePixelSize{24.0f};
        gpu::BindGroupHandle bindGroup{};
        uint64_t boundGeneration{0};
        bool ready{false};
    };

    struct HoverEval {
        bool hovered{false};
        float mix{0.0f};
    };

    struct QueuedMessageBox {
        std::string idKey{};
        std::string title{};
        std::string message{};
        MessageBoxType type{MessageBoxType::Info};
        std::vector<std::string> buttons{};
        std::function<void(int)> callback{};
        bool dimBackground{true};
    };

    struct PersistentLoadingState {
        bool active{false};
        std::string title{};
        float fraction{-1.0f};
        std::string status{};
        bool isError{false};
    };

    struct PendingDropdownOverlay {
        uint64_t id{0};
        UiRect anchorRect{};
        UiRect popupRect{};
        std::vector<std::string> options{};
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
        uint32_t fontSlot{0};
        bool hasScissor{false};
        UiRect scissorRect{};
    };

    void EnsureDefaultFontSlot();
    [[nodiscard]] FontSlot& ActiveFontSlot() noexcept;
    [[nodiscard]] const FontSlot& ActiveFontSlot() const noexcept;
    [[nodiscard]] float EffectiveTextScale(float relativeScale) const noexcept;
    [[nodiscard]] glm::vec4 GetMessageAccentColor(MessageBoxType type) const noexcept;

    [[nodiscard]] HoverEval EvalHover(uint64_t id, const UiRect& rect, bool allowInteraction = true,
                                      bool trackHit = true, bool ignoreModalBlock = false);
    void EmitHoverHighlight(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, const UiRect& rect,
                            float hoverMix, float radius) const;
    void EmitPanelCard(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, const UiRect& rect,
                       float radius, const glm::vec4& bg, const glm::vec4& border, bool frosted = true,
                       float borderWidth = 1.0f) const;
    void EmitProgressTrack(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs, const UiRect& trackRect,
                           float fraction, const glm::vec4& accentColor, float alpha = 1.0f,
                           float hoverMix = 0.0f) const;

    void FlushDrawCommand(bool hasScissor, const UiRect& scissorRect);
    void EmitColorPickerOverlay();
    void EmitDropdownOverlay();
    void EmitTooltipOverlay();
    bool DrawLoadingScreenInternal(std::string_view title, float fraction, std::string_view statusText, bool isError,
                                   bool cancellable, float alphaMultiplier);
    int DrawMessageBoxInternal(std::string_view id, std::string_view title, std::string_view message,
                               std::span<const std::string_view> buttons, MessageBoxType type, bool dimBackground);

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

    void AdvanceCursor(float itemHeight);
    void PatchBoxGeometry(size_t vOff, float rx, float ry, float rw, float rh, float pad, float radius, float param4,
                          const glm::vec4* fillOverride = nullptr, const glm::vec4* borderOverride = nullptr);
    bool PushPanelInternal(std::string_view idKey, float x, float y, float width, float height, std::string_view title,
                           const PanelOptions& options, bool standalone);
    bool EmitRadioRow(uint64_t id, const UiRect& rect, std::string_view label, bool selected);

    void EnsureGpuBuffers(size_t vertexCount, size_t indexCount);
    void ReleaseDynamicBuffers() noexcept;
    void SyncFontBindGroup(FontSlot& slot);
    void RebuildAllFontBindGroups();
    [[nodiscard]] float MeasureTextWidth(std::string_view text, float scale);
    [[nodiscard]] std::string EllipsizeText(std::string_view text, float maxWidth, float scale);
    [[nodiscard]] bool IsBlockedByPopup() const noexcept;
    [[nodiscard]] bool IsMouseInClipArea() const noexcept;
    [[nodiscard]] uint64_t HashId(std::string_view label) const noexcept;
    [[nodiscard]] static std::string_view SplitDisplayLabel(std::string_view label) noexcept;
    void PushScopeHash(uint64_t scopeHash);
    void PopScopeHash();

    WGPUDevice device_{nullptr};
    WGPUQueue queue_{nullptr};
    WGPUTextureFormat surfaceFormat_{WGPUTextureFormat_BGRA8Unorm};
    uint32_t sampleCount_{1};
    bool gpuInitialized_{false};

    std::vector<FontSlot> fontSlots_{};
    std::vector<uint32_t> fontStack_{};
    uint32_t activeFontSlot_{0};

    gpu::BindGroupLayoutHandle bindGroupLayout_{};
    gpu::RenderPipelineHandle pipeline_{};
    gpu::BufferHandle uniformBuffer_{};
    gpu::SamplerHandle backdropSampler_{};
    gpu::TextureHandle fallbackBackdropTexture_{};
    gpu::TextureViewHandle fallbackBackdropView_{};
    WGPUTextureView currentBackdropView_{nullptr};
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
    uint32_t batchFontSlot_{0};

    UiInputState input_{};
    UiStyle style_{};
    uint32_t viewportWidth_{1280};
    uint32_t viewportHeight_{720};
    float frameDeltaTime_{0.016f};
    float timeSec_{0.0f};
    bool wantsMouse_{false};

    // Active window & item interaction state
    bool inWindow_{false};
    uint64_t currentWindowId_{0};
    UiRect currentWindowRect_{};
    UiRect lastCompletedWindowRect_{};
    UiRect lastItemRect_{};
    bool lastItemHovered_{false};
    std::vector<bool> hoverOverrideStack_{};
    std::vector<bool> disabledStack_{};
    std::vector<uint64_t> idStack_{};
    std::string pendingTooltip_{};
    float cursorX_{0.0f};
    float cursorY_{0.0f};
    float contentWidth_{0.0f};
    size_t windowShadowVertexOffset_{0};
    size_t windowBgVertexOffset_{0};

    struct ActiveContainer {
        uint64_t id{0};
        ContainerOptions options{};
        UiRect boxRect{};
        float savedCursorX{0.0f};
        float savedCursorY{0.0f};
        float savedContentWidth{0.0f};
        float startContentY{0.0f};
        float contentStartX{0.0f};
        float contentStartY{0.0f};
        float maxExtentY{0.0f};
        uint32_t itemCount{0};
        size_t bgVertexOffset{0};
        bool hasBackground{false};
    };

    struct ActivePanel {
        uint64_t id{0};
        PanelOptions options{};
        UiRect boxRect{};
        float savedCursorX{0.0f};
        float savedCursorY{0.0f};
        float savedContentWidth{0.0f};
        float minHeight{0.0f};
        size_t shadowVertexOffset{0};
        size_t bgVertexOffset{0};
        bool standalone{false};
        bool open{true};
    };

    struct ActiveGroup {
        uint64_t id{0};
        UiRect boxRect{};
        float savedCursorX{0.0f};
        float savedCursorY{0.0f};
        float savedContentWidth{0.0f};
        size_t bgVertexOffset{0};
        bool hasBorder{true};
    };

    // Persistent widget & hover animation states
    std::unordered_map<uint64_t, float> hoverAmounts_{};
    std::unordered_map<uint64_t, float> toggleAmounts_{};
    std::unordered_map<uint64_t, glm::vec2> windowOffsets_{};
    std::unordered_map<uint64_t, bool> collapsingHeaders_{};
    std::unordered_map<uint64_t, float> scrollOffsets_{};
    std::unordered_map<uint64_t, float> scrollContentHeights_{};
    std::unordered_map<uint64_t, glm::vec3> colorPickerHsv_{};

    std::vector<ActiveContainer> containerStack_{};
    std::vector<ActivePanel> panelStack_{};
    std::vector<ActiveGroup> groupStack_{};

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

    // Persistent popup states (dropdown / color picker / modals / loading screen)
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

    bool modalDrawnThisFrame_{false};
    bool immediateModalDrawnThisFrame_{false};
    std::vector<QueuedMessageBox> modalQueue_{};
    PersistentLoadingState loadingState_{};
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
bool ToggleBox(std::string_view label, bool& state, std::string_view description = {});
bool Option(std::string_view label, bool selected);
bool OptionGroup(std::string_view label, std::span<const std::string_view> options, size_t& selectedIndex,
                 bool horizontal = false);
bool OptionGroup(std::string_view label, std::span<const char* const> options, size_t& selectedIndex,
                 bool horizontal = false);
void BeginContainer(std::string_view id, const ContainerOptions& options = {});
void EndContainer();
bool BeginPanel(std::string_view title, const PanelOptions& options = {});
bool BeginPanel(std::string_view id, float x, float y, float width, float height = 0.0f, std::string_view title = {},
                const PanelOptions& options = {});
void EndPanel();
void BeginGroup(std::string_view label = {}, bool border = true);
void EndGroup();
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
void PushHover(bool enabled);
void PopHover();
void PushId(std::string_view id);
void PushId(int intId);
void PopId();
void BeginDisabled(bool disabled = true);
void EndDisabled();
[[nodiscard]] bool IsHovered() noexcept;
void Tooltip(std::string_view text);
bool PushFont(std::string_view slotName);
void PopFont();
int MessageBox(std::string_view id, std::string_view title, std::string_view message,
               std::span<const std::string_view> buttons = {}, MessageBoxType type = MessageBoxType::Info);
bool LoadingScreen(std::string_view title, float fraction = -1.0f, std::string_view statusText = {},
                   bool isError = false, bool cancellable = false);
} // namespace UI

} // namespace bgl::ui

#endif // BGL_UI_IMMEDIATEUI_HPP
