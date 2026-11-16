/**
 * @file Gui.cpp
 * @brief Implementation of the public bgl::gui immediate-mode facade backed by bgl::ui::ImmediateUI and FontLibrary.
 */
#include "bgl/Gui.hpp"
#include "engine/GuiInternal.hpp"
#include "text/FontLibrary.hpp"
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace bgl::gui {

namespace {

[[nodiscard]] ui::MessageBoxType ToInternalType(MessageType type) noexcept {
    switch (type) {
    case MessageType::Info:
        return ui::MessageBoxType::Info;
    case MessageType::Warning:
        return ui::MessageBoxType::Warning;
    case MessageType::Error:
        return ui::MessageBoxType::Error;
    case MessageType::Confirm:
        return ui::MessageBoxType::Confirm;
    }
    return ui::MessageBoxType::Info;
}

} // namespace

void Begin(std::string_view title) {
    if (!engine::g_currentGui) {
        return;
    }
    constexpr float kPanelWidth = 310.0f;
    const float panelX = std::max(16.0f, static_cast<float>(engine::g_currentViewportWidth) - kPanelWidth - 16.0f);
    engine::g_currentGui->BeginWindow(title, panelX, 16.0f, kPanelWidth);
}

void Begin(std::string_view title, float x, float y, float width) {
    if (engine::g_currentGui) {
        engine::g_currentGui->BeginWindow(title, x, y, width);
    }
}

void End() {
    if (engine::g_currentGui) {
        engine::g_currentGui->EndWindow();
    }
}

void Separator() {
    if (engine::g_currentGui) {
        engine::g_currentGui->Separator();
    }
}

bool Section(std::string_view label, bool defaultOpen) {
    return engine::g_currentGui ? engine::g_currentGui->CollapsingHeader(label, defaultOpen) : false;
}

void BeginScroll(std::string_view id, float maxHeight) {
    if (engine::g_currentGui) {
        engine::g_currentGui->BeginScrollArea(id, maxHeight);
    }
}

void EndScroll() {
    if (engine::g_currentGui) {
        engine::g_currentGui->EndScrollArea();
    }
}

void Label(std::string_view text) {
    if (engine::g_currentGui) {
        engine::g_currentGui->Label(text);
    }
}

void Label(std::string_view text, const glm::vec4& color) {
    if (engine::g_currentGui) {
        engine::g_currentGui->Label(text, color);
    }
}

bool Button(std::string_view label) {
    return engine::g_currentGui ? engine::g_currentGui->Button(label) : false;
}

bool Checkbox(std::string_view label, bool& value) {
    return engine::g_currentGui ? engine::g_currentGui->Checkbox(label, value) : false;
}

bool Slider(std::string_view label, float& value, float minValue, float maxValue, std::string_view valueFormat) {
    return engine::g_currentGui ? engine::g_currentGui->SliderFloat(label, value, minValue, maxValue, valueFormat)
                                : false;
}

bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex) {
    return engine::g_currentGui ? engine::g_currentGui->Dropdown(label, options, selectedIndex) : false;
}

bool ColorPicker(std::string_view label, glm::vec3& color) {
    return engine::g_currentGui ? engine::g_currentGui->ColorEdit3(label, color) : false;
}

void Progress(float fraction, std::string_view statusText, float height, const glm::vec4& fillColor) {
    if (engine::g_currentGui) {
        engine::g_currentGui->ProgressBar(fraction, statusText, height, fillColor);
    }
}

void Sparkline(std::string_view label, std::span<const float> values, float minValue, float maxValue, float height,
               const glm::vec4& lineColor) {
    if (engine::g_currentGui) {
        engine::g_currentGui->Sparkline(label, values, minValue, maxValue, height, lineColor);
    }
}

void Badge(float x, float y, std::string_view text, const glm::vec4& accentColor) {
    if (engine::g_currentGui) {
        engine::g_currentGui->DrawFloatingBadge(x, y, text, accentColor);
    }
}

void SetHoverEffects(bool enabled) {
    if (auto* gui = engine::ActiveOrRegisteredGui()) {
        gui->Style().enableHoverEffects = enabled;
    }
}

bool AreHoverEffectsEnabled() {
    if (const auto* gui = engine::ActiveOrRegisteredGui()) {
        return gui->AreHoverEffectsEnabled();
    }
    return true;
}

void PushHover(bool enabled) {
    if (engine::g_currentGui) {
        engine::g_currentGui->PushHoverEnabled(enabled);
    }
}

void PopHover() {
    if (engine::g_currentGui) {
        engine::g_currentGui->PopHoverEnabled();
    }
}

bool IsHovered() {
    return engine::g_currentGui ? engine::g_currentGui->IsItemHovered() : false;
}

void Tooltip(std::string_view text) {
    if (engine::g_currentGui) {
        engine::g_currentGui->Tooltip(text);
    }
}

void SetAccentColor(const glm::vec3& rgb) {
    if (auto* gui = engine::ActiveOrRegisteredGui()) {
        gui->Style().accent = glm::vec4{rgb, 1.0f};
        gui->Style().hoverHighlight = glm::vec4{rgb, 0.14f};
        gui->Style().sparklineFill = glm::vec4{rgb, 0.24f};
    }
}

int MessageBox(std::string_view id, std::string_view title, std::string_view message,
               std::span<const std::string_view> buttons, MessageType type, bool dimBackground) {
    if (!engine::g_currentGui) {
        return -1;
    }
    return engine::g_currentGui->MessageBox(id, title, message, buttons, ToInternalType(type), dimBackground);
}

void ShowMessage(std::string_view title, std::string_view message, MessageType type) {
    auto* gui = engine::ActiveOrRegisteredGui();
    if (!gui) {
        return;
    }
    constexpr std::array<std::string_view, 1> kOkButton{"OK"};
    gui->ShowMessageBox(title, title, message, ToInternalType(type), kOkButton);
}

void ShowConfirm(std::string_view title, std::string_view message, std::function<void(bool)> onConfirm) {
    auto* gui = engine::ActiveOrRegisteredGui();
    if (!gui) {
        return;
    }
    constexpr std::array<std::string_view, 2> kConfirmButtons{"Cancel", "OK"};
    gui->ShowMessageBox(title, title, message, ui::MessageBoxType::Confirm, kConfirmButtons,
                        [cb = std::move(onConfirm)](int buttonIdx) {
                            if (cb) {
                                cb(buttonIdx == 1);
                            }
                        });
}

void CloseMessageBox(std::string_view id) {
    if (auto* gui = engine::ActiveOrRegisteredGui()) {
        gui->CloseMessageBox(id);
    }
}

bool LoadingScreen(std::string_view title, float fraction, std::string_view statusText, bool isError,
                   bool cancellable) {
    if (!engine::g_currentGui) {
        return false;
    }
    return engine::g_currentGui->DrawLoadingScreen(title, fraction, statusText, isError, cancellable);
}

void ShowLoadingScreen(std::string_view title, float fraction, std::string_view statusText, bool isError) {
    if (auto* gui = engine::ActiveOrRegisteredGui()) {
        gui->ShowLoadingScreen(title, fraction, statusText, isError);
    }
}

void HideLoadingScreen() {
    if (auto* gui = engine::ActiveOrRegisteredGui()) {
        gui->HideLoadingScreen();
    }
}

std::vector<FontInfo> DiscoverFonts() {
    const auto found = text::FontLibrary::Discover();
    std::vector<FontInfo> result;
    result.reserve(found.size());
    for (const auto& f : found) {
        result.push_back(FontInfo{
            .family = f.family,
            .style = f.style,
            .path = f.path,
            .isMonospace = f.isMonospace,
            .isBold = f.isBold,
            .isItalic = f.isItalic,
            .isEmbedded = f.isEmbedded,
        });
    }
    return result;
}

bool LoadFont(std::string_view slotName, std::string_view nameOrPath, float pixelSize) {
    auto* gui = engine::ActiveOrRegisteredGui();
    if (!gui) {
        return false;
    }
    return static_cast<bool>(gui->LoadFont(slotName, nameOrPath, pixelSize));
}

bool SetDefaultFont(std::string_view nameOrPath, float pixelSize) {
    auto* gui = engine::ActiveOrRegisteredGui();
    if (!gui) {
        return false;
    }
    return gui->SetDefaultFont(nameOrPath, pixelSize);
}

bool PushFont(std::string_view slotName) {
    return engine::g_currentGui ? engine::g_currentGui->PushFont(slotName) : false;
}

void PopFont() {
    if (engine::g_currentGui) {
        engine::g_currentGui->PopFont();
    }
}

} // namespace bgl::gui
