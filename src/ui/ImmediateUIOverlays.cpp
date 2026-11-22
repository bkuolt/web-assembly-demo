/**
 * @file ImmediateUIOverlays.cpp
 * @brief Dropdowns, scroll areas, progress bars, deferred popups, and bgl::ui::UI facade.
 */
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bgl::ui {

namespace {

thread_local ImmediateUI* g_activeUiContext = nullptr;

[[nodiscard]] constexpr glm::vec4 WithAlpha(const glm::vec4& c, float alpha) noexcept {
    return glm::vec4{c.r, c.g, c.b, alpha};
}

} // namespace

void ImmediateUI::EmitProgressTrack(std::pmr::vector<UiVertex>& verts, std::pmr::vector<uint32_t>& idxs,
                                    const UiRect& trackRect, float fraction, const glm::vec4& accentColor, float alpha,
                                    float hoverMix) const {
    const float radius = std::min(style_.widgetRadius, trackRect.h * 0.5f);
    const glm::vec4 trackFill = WithAlpha(style_.sliderTrack, style_.sliderTrack.a * alpha);
    const glm::vec4 trackBorder =
        WithAlpha(glm::mix(style_.windowBorder, style_.accent, hoverMix), (0.68f + 0.25f * hoverMix) * alpha);
    EmitRoundedBox(verts, idxs, trackRect.x, trackRect.y, trackRect.w, trackRect.h, radius, trackFill, trackBorder,
                   1.0f + 0.35f * hoverMix, false);

    if (fraction < 0.0f) {
        // Animated smooth indeterminate pill sweeping cleanly across track
        const float cycle = std::fmod(timeSec_ * 0.85f, 1.0f);
        const float travel = 0.5f - 0.5f * std::cos(cycle * 6.2831853f);
        const float segW = std::max(trackRect.h * 1.4f, trackRect.w * 0.28f);
        const float segX = trackRect.x + travel * std::max(0.0f, trackRect.w - segW);
        EmitRoundedBox(verts, idxs, segX, trackRect.y, segW, trackRect.h, radius, accentColor, glm::vec4{0.0f}, 0.0f,
                       false);
        return;
    }

    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    if (clamped > 0.002f) {
        const float fillW = std::max(std::min(trackRect.w, trackRect.h * 0.7f), trackRect.w * clamped);
        EmitRoundedBox(verts, idxs, trackRect.x, trackRect.y, fillW, trackRect.h, radius, accentColor, glm::vec4{0.0f},
                       0.0f, false);
        if (fillW > 10.0f && trackRect.h >= 10.0f) {
            EmitRoundedBox(verts, idxs, trackRect.x + 2.0f, trackRect.y + 1.5f, fillW - 4.0f, 3.0f, 1.5f,
                           glm::vec4{1.0f, 1.0f, 1.0f, (0.22f + 0.14f * hoverMix) * alpha}, glm::vec4{0.0f}, 0.0f,
                           false);
        }
    }
}

bool ImmediateUI::Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex) {
    if (options.empty()) {
        Label(label, style_.textMuted);
        return false;
    }
    if (selectedIndex >= options.size()) {
        selectedIndex = 0;
    }

    const uint64_t id = HashId(label);

    EmitText(vertices_, indices_, SplitDisplayLabel(label), cursorX_, cursorY_, style_.textMuted,
             style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const UiRect anchorRect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool isOpen = (openDropdownId_ == id);
    const HoverEval anchorHover = EvalHover(id, anchorRect);

    bool selectionChanged = false;

    const float itemH = style_.rowHeight;
    const float rawPopupH = itemH * static_cast<float>(options.size());
    const float maxPopupH = std::max(itemH * 3.0f, std::min(264.0f, static_cast<float>(viewportHeight_) - 24.0f));
    const float popupH = std::min(rawPopupH, maxPopupH);
    const bool scrollable = rawPopupH > popupH + 0.5f;

    const float belowY = anchorRect.y + anchorRect.h + 2.0f;
    const float spaceBelow = static_cast<float>(viewportHeight_) - belowY - 6.0f;
    const float spaceAbove = anchorRect.y - 8.0f;
    float popupY = belowY;
    if (popupH > spaceBelow) {
        if (spaceAbove > spaceBelow) {
            popupY = std::max(6.0f, anchorRect.y - 2.0f - popupH);
        } else {
            popupY = std::max(6.0f, static_cast<float>(viewportHeight_) - popupH - 6.0f);
        }
    }
    const UiRect popupRect{anchorRect.x, popupY, anchorRect.w, popupH};

    float& scrollOffset = dropdownScrollOffsets_[id];
    const float maxScroll = std::max(0.0f, rawPopupH - popupH);

    bool interactedScrollbar = false;
    if (isOpen && !HasOpenModal()) {
        hitRects_.push_back(popupRect);
        if (scrollable && popupRect.Contains(input_.mouseX, input_.mouseY) && std::abs(input_.wheelDelta) > 1e-4f) {
            scrollOffset -= input_.wheelDelta * (itemH * 2.2f);
        }

        const UiRect scrollTrackRect{popupRect.x + popupRect.w - 10.0f, popupRect.y + 3.0f, 8.0f,
                                     std::max(12.0f, popupRect.h - 6.0f)};
        if (scrollable && input_.mousePressed && scrollTrackRect.Contains(input_.mouseX, input_.mouseY)) {
            activeDropdownScrollId_ = id;
        }
        if (activeDropdownScrollId_ == id) {
            interactedScrollbar = true;
            if (input_.mouseDown || input_.mouseReleased) {
                const float frac = std::clamp((input_.mouseY - scrollTrackRect.y) / scrollTrackRect.h, 0.0f, 1.0f);
                scrollOffset = frac * maxScroll;
            }
            if (!input_.mouseDown || input_.mouseReleased) {
                activeDropdownScrollId_ = 0;
            }
        }
        scrollOffset = std::clamp(scrollOffset, 0.0f, maxScroll);

        if (input_.mouseReleased && !interactedScrollbar) {
            const UiRect itemsHitRect{popupRect.x, popupRect.y, scrollable ? (popupRect.w - 10.0f) : popupRect.w,
                                      popupRect.h};
            if (itemsHitRect.Contains(input_.mouseX, input_.mouseY)) {
                const int idx = static_cast<int>((input_.mouseY - popupRect.y + scrollOffset) / itemH);
                if (idx >= 0 && static_cast<size_t>(idx) < options.size()) {
                    if (selectedIndex != static_cast<size_t>(idx)) {
                        selectedIndex = static_cast<size_t>(idx);
                        selectionChanged = true;
                    }
                }
                openDropdownId_ = 0;
                dropdownToggledThisFrame_ = true;
                popupConsumedInputThisFrame_ = true;
            } else if (!anchorHover.hovered && !popupRect.Contains(input_.mouseX, input_.mouseY)) {
                openDropdownId_ = 0;
                dropdownToggledThisFrame_ = true;
                popupConsumedInputThisFrame_ = true;
            }
        }
    }

    if (anchorHover.hovered && input_.mouseReleased && !dropdownToggledThisFrame_ && activeDragWindowId_ == 0) {
        openDropdownId_ = isOpen ? 0 : id;
        openColorId_ = 0;
        dropdownToggledThisFrame_ = true;
        if (!isOpen && scrollable) {
            scrollOffset =
                std::clamp((static_cast<float>(selectedIndex) + 0.5f) * itemH - popupH * 0.5f, 0.0f, maxScroll);
        }
    }

    const bool nowOpen = (openDropdownId_ == id);
    const glm::vec4 fill = glm::mix(style_.widgetBg, style_.widgetHover, nowOpen ? 1.0f : anchorHover.mix);
    const glm::vec4 border = glm::mix(style_.windowBorder, style_.accent, nowOpen ? 1.0f : anchorHover.mix);
    EmitRoundedBox(vertices_, indices_, anchorRect.x, anchorRect.y, anchorRect.w, anchorRect.h, style_.widgetRadius,
                   fill, border, 1.0f + 0.25f * (nowOpen ? 1.0f : anchorHover.mix), false);

    const std::string_view currentRaw = options[selectedIndex] ? std::string_view{options[selectedIndex]} : "";
    const std::string currentText = EllipsizeText(currentRaw, std::max(24.0f, anchorRect.w - 30.0f), style_.textScale);
    EmitText(vertices_, indices_, currentText, anchorRect.x + 8.0f, anchorRect.y + 4.0f, style_.textPrimary,
             style_.textScale);
    EmitText(vertices_, indices_, nowOpen ? "^" : "v", anchorRect.x + anchorRect.w - 18.0f, anchorRect.y + 4.0f,
             glm::mix(style_.textMuted, style_.accent, nowOpen ? 1.0f : anchorHover.mix), style_.textScale * 0.85f);

    if (nowOpen) {
        hasPendingOverlay_ = true;
        hasActivePopupRect_ = true;
        activePopupRect_ = popupRect;
        pendingOverlay_ = PendingDropdownOverlay{.id = id,
                                                 .anchorRect = anchorRect,
                                                 .popupRect = popupRect,
                                                 .options = options,
                                                 .selectedIndex = selectedIndex,
                                                 .scrollOffset = std::clamp(scrollOffset, 0.0f, maxScroll),
                                                 .contentHeight = rawPopupH,
                                                 .scrollable = scrollable};
    } else if (isOpen) {
        hasActivePopupRect_ = false;
    }

    lastItemRect_ = anchorRect;
    lastItemHovered_ = anchorHover.hovered;
    AdvanceCursor(style_.rowHeight + style_.itemSpacing);
    return selectionChanged;
}

void ImmediateUI::ProgressBar(float fraction, std::string_view statusText, float height, const glm::vec4& fillColor) {
    if (!statusText.empty()) {
        const std::string clipped = EllipsizeText(statusText, contentWidth_, style_.textScale * 0.88f);
        EmitText(vertices_, indices_, clipped, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.88f);
        cursorY_ += 16.0f;
    }

    const float barH = std::max(14.0f, height);
    const UiRect trackRect{cursorX_, cursorY_, contentWidth_, barH};
    const HoverEval hv = EvalHover(HashId(statusText.empty() ? "ProgressBar" : statusText), trackRect);
    const glm::vec4 barColor = (fillColor.a > 0.001f) ? fillColor : style_.accent;

    EmitProgressTrack(vertices_, indices_, trackRect, fraction, barColor, 1.0f, hv.mix);

    if (fraction >= 0.0f) {
        char pctBuf[16]{};
        const int pct = static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * 100.0f));
        std::snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
        const float pctScale = style_.textScale * 0.82f;
        const float pctW = MeasureTextWidth(pctBuf, pctScale);
        EmitText(vertices_, indices_, pctBuf, trackRect.x + (trackRect.w - pctW) * 0.5f,
                 trackRect.y + (barH - 14.0f) * 0.5f, style_.textPrimary, pctScale);
    }

    AdvanceCursor(barH + style_.itemSpacing);
}

void ImmediateUI::BeginScrollArea(std::string_view id, float maxHeight) {
    if (inScrollArea_) {
        EndScrollArea();
    }
    const uint64_t hash = HashId(id);
    const float clampedH = std::max(40.0f, maxHeight);
    scrollRect_ = UiRect{cursorX_, cursorY_, contentWidth_, clampedH};

    const HoverEval areaHover = EvalHover(hash ^ 0x534352ULL, scrollRect_, true, false);

    float& scrollOffset = scrollOffsets_[hash];
    const float prevContentH = scrollContentHeights_[hash];
    const float maxScroll = std::max(0.0f, prevContentH - clampedH);

    if (!IsBlockedByPopup() && scrollRect_.Contains(input_.mouseX, input_.mouseY) &&
        std::abs(input_.wheelDelta) > 1e-4f) {
        scrollOffset -= input_.wheelDelta * 26.0f;
    }

    const UiRect trackRect{scrollRect_.x + scrollRect_.w - 8.0f, scrollRect_.y + 2.0f, 6.0f,
                           std::max(12.0f, clampedH - 4.0f)};
    if (maxScroll > 1.0f && activeScrollbarId_ == 0 && activeDragWindowId_ == 0 && !IsBlockedByPopup() &&
        input_.mousePressed && trackRect.Contains(input_.mouseX, input_.mouseY)) {
        activeScrollbarId_ = hash;
    }
    if (activeScrollbarId_ == hash) {
        if (input_.mouseDown || input_.mouseReleased) {
            const float frac = std::clamp((input_.mouseY - trackRect.y) / trackRect.h, 0.0f, 1.0f);
            scrollOffset = frac * maxScroll;
        }
        if (!input_.mouseDown || input_.mouseReleased) {
            activeScrollbarId_ = 0;
        }
    }
    scrollOffset = std::clamp(scrollOffset, 0.0f, maxScroll);

    const glm::vec4 boxBorder = glm::mix(WithAlpha(style_.windowBorder, 0.55f), style_.accent, areaHover.mix * 0.7f);
    EmitRoundedBox(vertices_, indices_, scrollRect_.x, scrollRect_.y, scrollRect_.w, scrollRect_.h, style_.widgetRadius,
                   glm::vec4{0.055f, 0.065f, 0.088f, 0.85f}, boxBorder, 1.0f, false);

    FlushDrawCommand(false, UiRect{});

    inScrollArea_ = true;
    activeScrollId_ = hash;
    scrollSavedCursorX_ = cursorX_;
    scrollSavedCursorY_ = cursorY_;
    scrollSavedContentW_ = contentWidth_;
    scrollAppliedOffset_ = scrollOffset;

    cursorX_ = scrollRect_.x + 6.0f;
    cursorY_ = scrollRect_.y + 4.0f - scrollOffset;
    contentWidth_ = std::max(30.0f, scrollRect_.w - 18.0f);
    scrollContentStartY_ = scrollRect_.y + 4.0f;
}

void ImmediateUI::EndScrollArea() {
    if (!inScrollArea_) {
        return;
    }

    FlushDrawCommand(true, scrollRect_);

    const float measuredContentH = std::max(0.0f, (cursorY_ + scrollAppliedOffset_) - scrollContentStartY_ + 6.0f);
    scrollContentHeights_[activeScrollId_] = measuredContentH;

    cursorX_ = scrollSavedCursorX_;
    cursorY_ = scrollRect_.y + scrollRect_.h + style_.itemSpacing;
    contentWidth_ = scrollSavedContentW_;
    inScrollArea_ = false;

    const float maxScroll = std::max(0.0f, measuredContentH - scrollRect_.h);
    if (maxScroll > 1.0f) {
        const float trackX = scrollRect_.x + scrollRect_.w - 7.0f;
        const float trackY = scrollRect_.y + 4.0f;
        const float trackH = std::max(12.0f, scrollRect_.h - 8.0f);
        EmitRoundedBox(vertices_, indices_, trackX, trackY, 4.0f, trackH, 2.0f, style_.sliderTrack, glm::vec4{0.0f},
                       0.0f, false);
        const float visibleRatio = std::clamp(scrollRect_.h / std::max(scrollRect_.h, measuredContentH), 0.12f, 1.0f);
        const float thumbH = std::max(14.0f, trackH * visibleRatio);
        const float scrollFrac = std::clamp(scrollAppliedOffset_ / maxScroll, 0.0f, 1.0f);
        const float thumbY = trackY + scrollFrac * std::max(0.0f, trackH - thumbH);
        const UiRect thumbRect{trackX - 2.0f, thumbY, 8.0f, thumbH};
        const HoverEval thumbHover = EvalHover(activeScrollId_ ^ 0x54484DULL, thumbRect, true, false);
        const float thumbMix = (activeScrollbarId_ == activeScrollId_) ? 1.0f : thumbHover.mix;
        EmitRoundedBox(vertices_, indices_, trackX, thumbY, 4.0f, thumbH, 2.0f,
                       glm::mix(style_.windowBorder, style_.accent, thumbMix), glm::vec4{0.0f}, 0.0f, false);
    }

    hitRects_.push_back(scrollRect_);
    lastItemRect_ = scrollRect_;
}

void ImmediateUI::EmitColorPickerOverlay() {
    if (!hasPendingColorOverlay_ || openColorId_ != pendingColorOverlay_.id) {
        return;
    }
    const auto& cp = pendingColorOverlay_;
    EmitPanelCard(vertices_, indices_, cp.popupRect, style_.cornerRadius, style_.popupBg, style_.accent, false, 1.0f);

    EmitQuad(vertices_, indices_, cp.svRect.x, cp.svRect.y, cp.svRect.w, cp.svRect.h, {0.0f, 0.0f}, {1.0f, 1.0f},
             glm::vec4{1.0f}, 5.0f, glm::vec4{cp.hsv.x, 0.0f, 0.0f, 0.0f});
    const float cx = cp.svRect.x + cp.hsv.y * cp.svRect.w;
    const float cy = cp.svRect.y + (1.0f - cp.hsv.z) * cp.svRect.h;
    EmitRoundedBox(vertices_, indices_, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 4.0f, glm::vec4{cp.rgb, 1.0f},
                   glm::vec4{1.0f}, 1.5f, false);

    EmitQuad(vertices_, indices_, cp.hueRect.x, cp.hueRect.y, cp.hueRect.w, cp.hueRect.h, {0.0f, 0.0f}, {1.0f, 1.0f},
             glm::vec4{1.0f}, 5.0f, glm::vec4{cp.hsv.x, 1.0f, 0.0f, 0.0f});
    const float hy = cp.hueRect.y + cp.hsv.x * cp.hueRect.h;
    EmitRect(vertices_, indices_, cp.hueRect.x - 1.0f, hy - 1.5f, cp.hueRect.w + 2.0f, 3.0f, glm::vec4{1.0f});

    char rgbBuf[40]{};
    std::snprintf(rgbBuf, sizeof(rgbBuf), "R %.2f  G %.2f  B %.2f", static_cast<double>(cp.rgb.r),
                  static_cast<double>(cp.rgb.g), static_cast<double>(cp.rgb.b));
    EmitText(vertices_, indices_, rgbBuf, cp.svRect.x, cp.svRect.y + cp.svRect.h + 5.0f, style_.textPrimary,
             style_.textScale * 0.85f);
}

void ImmediateUI::EmitDropdownOverlay() {
    if (!hasPendingOverlay_ || openDropdownId_ != pendingOverlay_.id) {
        return;
    }
    const auto& ov = pendingOverlay_;
    EmitPanelCard(vertices_, indices_, ov.popupRect, style_.widgetRadius, style_.popupBg, style_.accent, false, 1.0f);

    if (ov.scrollable) {
        FlushDrawCommand(false, UiRect{});
    }

    const float itemH = style_.rowHeight;
    const float itemRightPad = ov.scrollable ? 12.0f : 4.0f;
    const float textMaxW = std::max(24.0f, ov.popupRect.w - itemRightPad - 12.0f);
    for (size_t i = 0; i < ov.options.size(); ++i) {
        const float itemY = ov.popupRect.y + static_cast<float>(i) * itemH - ov.scrollOffset + 1.0f;
        if (ov.scrollable && (itemY + itemH < ov.popupRect.y || itemY > ov.popupRect.y + ov.popupRect.h)) {
            continue;
        }
        const UiRect itemRect{ov.popupRect.x + 2.0f, itemY, ov.popupRect.w - itemRightPad,
                              std::max(4.0f, itemH - 2.0f)};
        const bool mouseInsideClip = !ov.scrollable || ov.popupRect.Contains(input_.mouseX, input_.mouseY);
        const HoverEval itemHover =
            EvalHover(ov.id ^ (0x4F5054ULL + static_cast<uint64_t>(i)), itemRect, mouseInsideClip, false, true);
        const bool isSelected = (i == ov.selectedIndex);

        if (itemHover.mix > 0.01f || isSelected) {
            const glm::vec4 itemColor = isSelected
                                            ? glm::mix(style_.widgetActive, style_.widgetHover, itemHover.mix * 0.5f)
                                            : WithAlpha(style_.widgetHover, style_.widgetHover.a * itemHover.mix);
            EmitRoundedBox(vertices_, indices_, itemRect.x, itemRect.y, itemRect.w, itemRect.h, 3.0f, itemColor,
                           itemHover.mix > 0.25f ? WithAlpha(style_.accent, 0.5f * itemHover.mix) : glm::vec4{0.0f},
                           itemHover.mix > 0.25f ? 1.0f : 0.0f, false);
        }
        const std::string_view rawOpt = ov.options[i] ? std::string_view{ov.options[i]} : "";
        const std::string optText = EllipsizeText(rawOpt, textMaxW, style_.textScale);
        EmitText(vertices_, indices_, optText, itemRect.x + 6.0f, itemRect.y + 3.0f, style_.textPrimary,
                 style_.textScale);
    }

    if (ov.scrollable) {
        const UiRect clipRect{ov.popupRect.x + 1.0f, ov.popupRect.y + 1.0f, std::max(1.0f, ov.popupRect.w - 2.0f),
                              std::max(1.0f, ov.popupRect.h - 2.0f)};
        FlushDrawCommand(true, clipRect);

        const float trackX = ov.popupRect.x + ov.popupRect.w - 8.0f;
        const float trackY = ov.popupRect.y + 4.0f;
        const float trackH = std::max(12.0f, ov.popupRect.h - 8.0f);
        EmitRoundedBox(vertices_, indices_, trackX, trackY, 4.0f, trackH, 2.0f, style_.sliderTrack, glm::vec4{0.0f},
                       0.0f, false);
        const float maxScroll = std::max(1.0f, ov.contentHeight - ov.popupRect.h);
        const float visibleRatio = std::clamp(ov.popupRect.h / std::max(ov.popupRect.h, ov.contentHeight), 0.14f, 1.0f);
        const float thumbH = std::max(16.0f, trackH * visibleRatio);
        const float frac = std::clamp(ov.scrollOffset / maxScroll, 0.0f, 1.0f);
        const float thumbY = trackY + frac * std::max(0.0f, trackH - thumbH);
        const bool thumbActive = (activeDropdownScrollId_ == ov.id);
        EmitRoundedBox(vertices_, indices_, trackX, thumbY, 4.0f, thumbH, 2.0f,
                       thumbActive ? style_.accent : style_.windowBorder, glm::vec4{0.0f}, 0.0f, false);
    }
}

namespace UI {

void SetContext(ImmediateUI* ctx) noexcept {
    g_activeUiContext = ctx;
}

ImmediateUI* GetContext() noexcept {
    return g_activeUiContext;
}

void Label(std::string_view text, const glm::vec4& color) {
    if (g_activeUiContext) {
        g_activeUiContext->Label(text, color);
    }
}

void Separator() {
    if (g_activeUiContext) {
        g_activeUiContext->Separator();
    }
}

bool CollapsingHeader(std::string_view label, bool defaultOpen) {
    return g_activeUiContext ? g_activeUiContext->CollapsingHeader(label, defaultOpen) : defaultOpen;
}

bool Button(std::string_view label) {
    return g_activeUiContext ? g_activeUiContext->Button(label) : false;
}

bool Checkbox(std::string_view label, bool& state) {
    return g_activeUiContext ? g_activeUiContext->Checkbox(label, state) : false;
}

bool ToggleBox(std::string_view label, bool& state, std::string_view description) {
    return g_activeUiContext ? g_activeUiContext->ToggleBox(label, state, description) : false;
}

bool Option(std::string_view label, bool selected) {
    return g_activeUiContext ? g_activeUiContext->Option(label, selected) : false;
}

bool OptionGroup(std::string_view label, std::span<const std::string_view> options, size_t& selectedIndex,
                 bool horizontal) {
    return g_activeUiContext ? g_activeUiContext->OptionGroup(label, options, selectedIndex, horizontal) : false;
}

bool OptionGroup(std::string_view label, std::span<const char* const> options, size_t& selectedIndex, bool horizontal) {
    return g_activeUiContext ? g_activeUiContext->OptionGroup(label, options, selectedIndex, horizontal) : false;
}

void BeginContainer(std::string_view id, const ContainerOptions& options) {
    if (g_activeUiContext) {
        g_activeUiContext->BeginContainer(id, options);
    }
}

void EndContainer() {
    if (g_activeUiContext) {
        g_activeUiContext->EndContainer();
    }
}

bool BeginPanel(std::string_view title, const PanelOptions& options) {
    return g_activeUiContext ? g_activeUiContext->BeginPanel(title, options) : false;
}

bool BeginPanel(std::string_view id, float x, float y, float width, float height, std::string_view title,
                const PanelOptions& options) {
    return g_activeUiContext ? g_activeUiContext->BeginPanel(id, x, y, width, height, title, options) : false;
}

void EndPanel() {
    if (g_activeUiContext) {
        g_activeUiContext->EndPanel();
    }
}

void BeginGroup(std::string_view label, bool border) {
    if (g_activeUiContext) {
        g_activeUiContext->BeginGroup(label, border);
    }
}

void EndGroup() {
    if (g_activeUiContext) {
        g_activeUiContext->EndGroup();
    }
}

bool SliderFloat(std::string_view label, float& value, float minVal, float maxVal, std::string_view valueFormat) {
    return g_activeUiContext ? g_activeUiContext->SliderFloat(label, value, minVal, maxVal, valueFormat) : false;
}

bool ColorEdit3(std::string_view label, glm::vec3& color) {
    return g_activeUiContext ? g_activeUiContext->ColorEdit3(label, color) : false;
}

void Sparkline(std::string_view label, std::span<const float> values, float minVal, float maxVal, float height,
               const glm::vec4& lineColor) {
    if (g_activeUiContext) {
        g_activeUiContext->Sparkline(label, values, minVal, maxVal, height, lineColor);
    }
}

bool Dropdown(std::string_view label, std::span<const char* const> options, size_t& selectedIndex) {
    return g_activeUiContext ? g_activeUiContext->Dropdown(label, options, selectedIndex) : false;
}

void ProgressBar(float fraction, std::string_view statusText, float height, const glm::vec4& fillColor) {
    if (g_activeUiContext) {
        g_activeUiContext->ProgressBar(fraction, statusText, height, fillColor);
    }
}

void BeginScrollArea(std::string_view id, float maxHeight) {
    if (g_activeUiContext) {
        g_activeUiContext->BeginScrollArea(id, maxHeight);
    }
}

void EndScrollArea() {
    if (g_activeUiContext) {
        g_activeUiContext->EndScrollArea();
    }
}

void PushHover(bool enabled) {
    if (g_activeUiContext) {
        g_activeUiContext->PushHoverEnabled(enabled);
    }
}

void PopHover() {
    if (g_activeUiContext) {
        g_activeUiContext->PopHoverEnabled();
    }
}

void PushId(std::string_view id) {
    if (g_activeUiContext) {
        g_activeUiContext->PushId(id);
    }
}

void PushId(int intId) {
    if (g_activeUiContext) {
        g_activeUiContext->PushId(intId);
    }
}

void PopId() {
    if (g_activeUiContext) {
        g_activeUiContext->PopId();
    }
}

void BeginDisabled(bool disabled) {
    if (g_activeUiContext) {
        g_activeUiContext->BeginDisabled(disabled);
    }
}

void EndDisabled() {
    if (g_activeUiContext) {
        g_activeUiContext->EndDisabled();
    }
}

bool IsHovered() noexcept {
    return g_activeUiContext ? g_activeUiContext->IsItemHovered() : false;
}

void Tooltip(std::string_view text) {
    if (g_activeUiContext) {
        g_activeUiContext->Tooltip(text);
    }
}

bool PushFont(std::string_view slotName) {
    return g_activeUiContext ? g_activeUiContext->PushFont(slotName) : false;
}

void PopFont() {
    if (g_activeUiContext) {
        g_activeUiContext->PopFont();
    }
}

int MessageBox(std::string_view id, std::string_view title, std::string_view message,
               std::span<const std::string_view> buttons, MessageBoxType type) {
    return g_activeUiContext ? g_activeUiContext->MessageBox(id, title, message, buttons, type) : -1;
}

bool LoadingScreen(std::string_view title, float fraction, std::string_view statusText, bool isError,
                   bool cancellable) {
    return g_activeUiContext ? g_activeUiContext->DrawLoadingScreen(title, fraction, statusText, isError, cancellable)
                             : false;
}

} // namespace UI

} // namespace bgl::ui
