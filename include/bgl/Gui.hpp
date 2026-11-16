/**
 * @file Gui.hpp
 * @brief Lightweight immediate-mode GUI helpers callable inside `App::OnGui()`, plus font discovery,
 *        optional hover feedback, tooltips, frosted-glass modals, and loading screens.
 */
#ifndef BGL_PUBLIC_GUI_HPP
#define BGL_PUBLIC_GUI_HPP

#include "bgl/Types.hpp"
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgl::gui {

/**
 * @enum MessageType
 * @brief Visual severity category for modal message boxes.
 */
enum class MessageType : uint8_t {
    Info = 0,    ///< Informational dialog with accent styling.
    Warning = 1, ///< Warning dialog with amber accent.
    Error = 2,   ///< Critical error dialog with red accent.
    Confirm = 3, ///< Confirmation dialog prompting user action.
};

/**
 * @struct FontInfo
 * @brief Metadata describing an installed system or workspace font face.
 */
struct FontInfo {
    std::string family{};    ///< Typographic family name (e.g., "Liberation Sans").
    std::string style{};     ///< Style name (e.g., "Regular", "Bold", "Italic").
    std::string path{};      ///< Filesystem path to the font file (empty for built-in embedded fallback).
    bool isMonospace{false}; ///< True when the face has fixed-advance glyphs.
    bool isBold{false};      ///< True when FreeType marks the face bold.
    bool isItalic{false};    ///< True when FreeType marks the face italic/oblique.
    bool isEmbedded{false};  ///< True for the built-in zero-dependency Liberation Mono fallback.
};

// --- Windows & Containers ---

/**
 * @brief Opens a sleek glassmorphic window docked cleanly near the top-right corner.
 * @param title Window header caption.
 */
void Begin(std::string_view title);

/**
 * @brief Opens a sleek glassmorphic window at explicit screen coordinates and width.
 * @param title Window header caption.
 * @param x Horizontal position in logical pixels from the top-left viewport corner.
 * @param y Vertical position in logical pixels from the top-left viewport corner.
 * @param width Window width in logical pixels.
 */
void Begin(std::string_view title, float x, float y, float width);

/**
 * @brief Closes the current window started with `Begin()`. Must match every `Begin()`.
 */
void End();

/**
 * @brief Inserts a subtle horizontal separator rule inside the current window.
 */
void Separator();

/**
 * @brief Renders a collapsible section header with persisted open/closed state.
 * @param label Header title.
 * @param defaultOpen Initial expanded state when first rendered.
 * @return `true` while the section body is expanded.
 */
bool Section(std::string_view label, bool defaultOpen = true);

/**
 * @brief Begins a scrollable clipped vertical region inside the active window.
 * @param id Unique identifier string for persisted scroll offset.
 * @param maxHeight Maximum region height in logical pixels before scrolling activates.
 */
void BeginScroll(std::string_view id, float maxHeight);

/**
 * @brief Closes the active scroll region started with `BeginScroll()`.
 */
void EndScroll();

// --- Core Widgets ---

/**
 * @brief Renders a plain text label inside the current window.
 * @param text Label text rendered using the active UI font.
 */
void Label(std::string_view text);

/**
 * @brief Renders a tinted text label inside the current window.
 * @param text Label text rendered using the active UI font.
 * @param color RGBA text color multiplier in `[0, 1]`.
 */
void Label(std::string_view text, const glm::vec4& color);

/**
 * @brief Renders an interactive clickable button.
 * @param label Button caption centered inside the rounded button rectangle.
 * @return `true` on the frame the user clicks the button.
 */
bool Button(std::string_view label);

/**
 * @brief Renders a boolean toggle checkbox.
 * @param label Caption next to the checkbox.
 * @param value Mutable boolean toggled in-place on click.
 * @return `true` on the frame the value changed.
 */
bool Checkbox(std::string_view label, bool& value);

/**
 * @brief Renders a continuous horizontal float slider.
 * @param label Descriptive caption displayed beside the numeric readout.
 * @param value Mutable scalar updated while dragging.
 * @param minValue Minimum slider value.
 * @param maxValue Maximum slider value.
 * @param valueFormat `std::format` format string applied to the value readout (e.g., `"{:.2f}"`).
 * @return `true` on frames where the value changed.
 */
bool Slider(std::string_view label, float& value, float minValue, float maxValue,
            std::string_view valueFormat = "{:.2f}");

/**
 * @brief Renders an interactive dropdown selector with scrollable overlay popup.
 * @param label Selector label shown above the dropdown box.
 * @param options Array of option C-strings.
 * @param selectedIndex Mutable zero-based index updated when an option is picked.
 * @return `true` on the frame `selectedIndex` changed.
 */
bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex);

/**
 * @brief Renders an RGB color swatch opening an interactive frosted-glass HSV picker popup.
 * @param label Swatch label.
 * @param color Mutable RGB color vector in `[0, 1]`.
 * @return `true` while/when the color changes.
 */
bool ColorPicker(std::string_view label, glm::vec3& color);

/**
 * @brief Renders a rounded progress bar (`fraction < 0` activates smooth indeterminate animation).
 * @param fraction Normalized progress in `[0, 1]`, or negative for indeterminate shimmer.
 * @param statusText Optional right-aligned caption text.
 * @param height Track height in pixels.
 * @param fillColor Optional fill tint override (`alpha <= 0` uses active theme accent).
 */
void Progress(float fraction, std::string_view statusText = {}, float height = 18.0f,
              const glm::vec4& fillColor = glm::vec4{0.0f});

/**
 * @brief Renders a real-time sparkline mini-chart with interactive hover crosshair.
 * @param label Chart title shown beside the latest sample readout.
 * @param values Historical sample series.
 * @param minValue Explicit lower y-axis bound (`0 == maxValue` enables auto-scaling).
 * @param maxValue Explicit upper y-axis bound.
 * @param height Chart height in pixels.
 * @param lineColor Polyline RGBA tint.
 */
void Sparkline(std::string_view label, std::span<const float> values, float minValue = 0.0f, float maxValue = 0.0f,
               float height = 40.0f, const glm::vec4& lineColor = glm::vec4{0.24f, 0.78f, 0.96f, 1.0f});

/**
 * @brief Draws a compact rounded pill badge at arbitrary viewport coordinates.
 * @param x Viewport X position in logical pixels.
 * @param y Viewport Y position in logical pixels.
 * @param text Short caption rendered inside the badge.
 * @param accentColor Border and subtle tint RGBA color.
 */
void Badge(float x, float y, std::string_view text,
           const glm::vec4& accentColor = glm::vec4{0.24f, 0.62f, 0.96f, 1.0f});

// --- Hover Effects, Tooltips & Theme ---

/**
 * @brief Globally enables or disables smooth hover highlights across all UI elements.
 * @param enabled `true` to enable animated hover feedback; `false` to disable.
 */
void SetHoverEffects(bool enabled);

/**
 * @brief Returns whether global UI hover effects are currently enabled.
 * @return `true` when hover animations/highlights are active on widgets.
 */
[[nodiscard]] bool AreHoverEffectsEnabled();

/**
 * @brief Temporarily overrides hover highlights for subsequent widgets until `PopHover()`.
 * @param enabled Local hover override for nested widgets.
 */
void PushHover(bool enabled);

/**
 * @brief Restores the previous hover highlight setting pushed by `PushHover()`.
 */
void PopHover();

/**
 * @brief Returns `true` if the most recently emitted widget is currently hovered by the mouse cursor.
 * @return Hover state of the preceding element.
 */
[[nodiscard]] bool IsHovered();

/**
 * @brief Displays a sleek floating frosted-glass tooltip near the cursor when the previous widget is hovered.
 * @param text Tooltip message string.
 */
void Tooltip(std::string_view text);

/**
 * @brief Updates the active theme accent RGB color applied to sliders, checkboxes, progress bars, and highlights.
 * @param rgb Normalized accent color in `[0, 1]`.
 */
void SetAccentColor(const glm::vec3& rgb);

// --- Frosted-Glass Modals & Loading Screens ---

/**
 * @brief Renders an immediate frosted-glass modal message box with action buttons.
 * @param id Unique identifier string for modal layout stability.
 * @param title Header title text.
 * @param message Wrapped body text.
 * @param buttons Button labels (defaults to `{"OK"}` when empty).
 * @param type Severity accent category.
 * @param dimBackground Dims the scene behind the modal card when true.
 * @return Zero-based clicked button index, or `-1` while awaiting interaction.
 */
int MessageBox(std::string_view id, std::string_view title, std::string_view message,
               std::span<const std::string_view> buttons = {}, MessageType type = MessageType::Info,
               bool dimBackground = true);

/**
 * @brief Displays a queued informational/warning/error dialog dismissed automatically on OK.
 * @param title Dialog header title.
 * @param message Wrapped dialog body text.
 * @param type Visual severity style.
 */
void ShowMessage(std::string_view title, std::string_view message, MessageType type = MessageType::Info);

/**
 * @brief Displays a queued OK/Cancel confirmation modal invoking `onConfirm(true)` on confirmation.
 * @param title Confirmation dialog header.
 * @param message Wrapped prompt text.
 * @param onConfirm Callback receiving `true` for OK or `false` for Cancel.
 */
void ShowConfirm(std::string_view title, std::string_view message, std::function<void(bool)> onConfirm);

/**
 * @brief Programmatically dismisses a queued modal message box (`id` empty closes the top modal).
 * @param id Identifier used when opening the modal, or empty string to close the active top dialog.
 */
void CloseMessageBox(std::string_view id = {});

/**
 * @brief Renders an immediate fullscreen frosted-glass loading screen overlay matching the engine UI style.
 * @param title Loading headline text.
 * @param fraction Normalized progress `[0, 1]`, or `< 0` for indeterminate shimmer animation.
 * @param statusText Optional subtitle or asset path status line.
 * @param isError Applies error accent color when true.
 * @param cancellable Shows a clickable Cancel button when true.
 * @return `true` on the frame the user clicks Cancel.
 */
bool LoadingScreen(std::string_view title, float fraction = -1.0f, std::string_view statusText = {},
                   bool isError = false, bool cancellable = false);

/**
 * @brief Activates or updates a persistent fullscreen loading screen drawn automatically every frame until hidden.
 * @param title Loading headline text.
 * @param fraction Normalized progress `[0, 1]`, or `< 0` for indeterminate animation.
 * @param statusText Optional detail text below the progress bar.
 * @param isError Applies error tinting when true.
 */
void ShowLoadingScreen(std::string_view title, float fraction = -1.0f, std::string_view statusText = {},
                       bool isError = false);

/**
 * @brief Hides the persistent fullscreen loading screen activated via `ShowLoadingScreen()`.
 */
void HideLoadingScreen();

// --- Cross-Platform Font Discovery & Multi-Font Loading ---

/**
 * @brief Discovers installed system and project asset TrueType/OpenType fonts across Windows, Linux, macOS, and WASM.
 * @return Sorted metadata list covering discovered font faces plus the built-in embedded fallback.
 */
[[nodiscard]] std::vector<FontInfo> DiscoverFonts();

/**
 * @brief Loads or replaces a named UI font slot by file path, family name, or generic role (`"sans"`, `"mono"`,
 * `"serif"`).
 * @param slotName Logical slot key used later with `PushFont()`.
 * @param nameOrPath Font file path, family name (e.g., `"DejaVu Sans"`), or role alias (`"sans"`, `"mono"`, `"serif"`).
 * @param pixelSize Rasterization height in pixels for the SDF atlas.
 * @return `true` when loaded successfully.
 */
bool LoadFont(std::string_view slotName, std::string_view nameOrPath, float pixelSize = 24.0f);

/**
 * @brief Replaces the default UI font used by windows and widgets outside explicit `PushFont()` scopes.
 * @param nameOrPath Font file path, family name, or generic role (`"sans"`, `"mono"`, `"serif"`).
 * @param pixelSize Rasterization height in pixels for the SDF atlas.
 * @return `true` if a matching font was found and activated.
 */
bool SetDefaultFont(std::string_view nameOrPath, float pixelSize = 24.0f);

/**
 * @brief Activates a previously loaded named font slot for subsequent text/widgets until `PopFont()`.
 * @param slotName Slot name previously passed to `LoadFont()` (or `"default"`).
 * @return `true` if the named font slot exists and was pushed.
 */
bool PushFont(std::string_view slotName);

/**
 * @brief Restores the previous UI font active prior to `PushFont()`.
 */
void PopFont();

} // namespace bgl::gui

#endif // BGL_PUBLIC_GUI_HPP
