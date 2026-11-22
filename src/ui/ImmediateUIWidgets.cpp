/**
 * @file ImmediateUIWidgets.cpp
 * @brief Immediate-mode windowing and SDF widgets with unified optional hover animations.
 */
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bgl::ui {

namespace {

[[nodiscard]] constexpr glm::vec4 WithAlpha(const glm::vec4& c, float alpha) noexcept {
    return glm::vec4{c.r, c.g, c.b, alpha};
}

void FormatFloatLabel(char* dst, size_t dstSize, std::string_view fmt, float value) noexcept {
    int precision = 2;
    std::string_view suffix;
    if (const size_t dotPos = fmt.find('.'); dotPos != std::string_view::npos && dotPos + 1 < fmt.size()) {
        const char digit = fmt[dotPos + 1];
        if (digit >= '0' && digit <= '6') {
            precision = digit - '0';
        }
        const size_t endBrace = fmt.find('}', dotPos);
        if (endBrace != std::string_view::npos && endBrace + 1 < fmt.size()) {
            suffix = fmt.substr(endBrace + 1);
        } else if (const size_t fPos = fmt.find('f', dotPos); fPos != std::string_view::npos && fPos + 1 < fmt.size()) {
            suffix = fmt.substr(fPos + 1);
        }
    }
    if (suffix.empty()) {
        std::snprintf(dst, dstSize, "%.*f", precision, static_cast<double>(value));
    } else {
        std::snprintf(dst, dstSize, "%.*f%.*s", precision, static_cast<double>(value), static_cast<int>(suffix.size()),
                      suffix.data());
    }
}

} // namespace

void ImmediateUI::BeginWindow(std::string_view title, float x, float y, float width) {
    if (inWindow_) {
        EndWindow();
    }
    inWindow_ = true;
    currentWindowId_ = HashId(title);

    glm::vec2& dragOffset = windowOffsets_[currentWindowId_];
    float ax = x + dragOffset.x;
    float ay = y + dragOffset.y;

    const UiRect headerRect{ax, ay, width, style_.headerHeight};
    const bool canDrag =
        activeSliderId_ == 0 && activeScrollbarId_ == 0 && activeColorSvId_ == 0 && activeColorHueId_ == 0;
    const HoverEval hdrHover = EvalHover(currentWindowId_ ^ 0x484452ULL, headerRect, canDrag, false);

    if (activeDragWindowId_ == 0 && canDrag && input_.mousePressed && hdrHover.hovered) {
        activeDragWindowId_ = currentWindowId_;
        dragStartMouse_ = {input_.mouseX, input_.mouseY};
        dragStartOffset_ = dragOffset;
    }

    if (activeDragWindowId_ == currentWindowId_) {
        if (input_.mouseDown) {
            dragOffset = dragStartOffset_ + (glm::vec2{input_.mouseX, input_.mouseY} - dragStartMouse_);
            const float maxX = std::max(0.0f, static_cast<float>(viewportWidth_) - 80.0f);
            const float maxY = std::max(0.0f, static_cast<float>(viewportHeight_) - 40.0f);
            dragOffset.x = std::clamp(x + dragOffset.x, 0.0f, maxX) - x;
            dragOffset.y = std::clamp(y + dragOffset.y, 0.0f, maxY) - y;
            ax = x + dragOffset.x;
            ay = y + dragOffset.y;
        } else {
            activeDragWindowId_ = 0;
        }
    }

    currentWindowRect_ = UiRect{ax, ay, width, style_.headerHeight + style_.padding};
    cursorX_ = ax + style_.padding;
    cursorY_ = ay + style_.headerHeight + style_.padding;
    contentWidth_ = std::max(40.0f, width - 2.0f * style_.padding);

    windowShadowVertexOffset_ = vertices_.size();
    EmitShadowBox(vertices_, indices_, ax, ay + 3.0f, width, currentWindowRect_.h, style_.cornerRadius,
                  style_.shadowBlur, style_.shadowColor);

    windowBgVertexOffset_ = vertices_.size();
    EmitRoundedBox(vertices_, indices_, ax, ay, width, currentWindowRect_.h, style_.cornerRadius, style_.windowBg,
                   style_.windowBorder, 1.0f, true);

    const float headerMix = (activeDragWindowId_ == currentWindowId_) ? 1.0f : hdrHover.mix;
    const glm::vec4 headerFill = glm::mix(style_.headerBg, style_.widgetHover, headerMix * 0.45f);
    EmitRoundedBox(vertices_, indices_, ax + 1.0f, ay + 1.0f, width - 2.0f, style_.headerHeight - 1.0f,
                   std::max(1.0f, style_.cornerRadius - 1.0f), headerFill,
                   headerMix > 0.01f ? WithAlpha(style_.accent, 0.45f * headerMix) : glm::vec4{0.0f},
                   headerMix > 0.01f ? 1.0f : 0.0f, false);
    EmitRoundedBox(vertices_, indices_, ax + 6.0f, ay + 7.0f, 3.0f, style_.headerHeight - 14.0f, 1.5f, style_.accent,
                   glm::vec4{0.0f}, 0.0f, false);
    EmitText(vertices_, indices_, SplitDisplayLabel(title), ax + style_.padding + 4.0f, ay + 6.0f, style_.textPrimary,
             style_.textScale);

    if (activeDragWindowId_ == currentWindowId_) {
        hitRects_.push_back(UiRect{ax, ay, width, style_.headerHeight});
    }
    lastItemRect_ = UiRect{ax, ay, width, style_.headerHeight};
    lastItemHovered_ = hdrHover.hovered;
    PushScopeHash(currentWindowId_);
}

void ImmediateUI::EndWindow() {
    if (!inWindow_) {
        return;
    }
    if (inScrollArea_) {
        EndScrollArea();
    }

    const float finalHeight = (cursorY_ - currentWindowRect_.y) + style_.padding - style_.itemSpacing;
    currentWindowRect_.h = std::max(style_.headerHeight + style_.padding, finalHeight);

    const float x = currentWindowRect_.x;
    const float y = currentWindowRect_.y;
    const float w = currentWindowRect_.w;
    const float h = currentWindowRect_.h;

    PatchBoxGeometry(windowShadowVertexOffset_, x, y + 3.0f, w, h, style_.shadowBlur, style_.cornerRadius,
                     style_.shadowBlur);
    PatchBoxGeometry(windowBgVertexOffset_, x, y, w, h, 1.5f, style_.cornerRadius, 1.0f);

    lastCompletedWindowRect_ = currentWindowRect_;
    hitRects_.push_back(currentWindowRect_);
    PopScopeHash();
    inWindow_ = false;
}

void ImmediateUI::Label(std::string_view text, const glm::vec4& color) {
    const float h = 18.0f;
    const UiRect rowRect{cursorX_, cursorY_, contentWidth_, h};
    const HoverEval hv = EvalHover(HashId(text) ^ 0x4C424CULL, rowRect, true, false);
    if (hv.mix > 0.01f) {
        EmitHoverHighlight(vertices_, indices_, UiRect{rowRect.x - 3.0f, rowRect.y, rowRect.w + 6.0f, rowRect.h},
                           hv.mix * 0.85f, style_.widgetRadius);
    }
    const glm::vec4 effectiveColor =
        (hv.mix > 0.01f)
            ? glm::mix(color, glm::vec4{style_.textPrimary.r, style_.textPrimary.g, style_.textPrimary.b, color.a},
                       hv.mix * 0.35f)
            : color;
    EmitText(vertices_, indices_, SplitDisplayLabel(text), cursorX_, cursorY_ + 2.0f, effectiveColor, style_.textScale);
    AdvanceCursor(h + style_.itemSpacing);
}

void ImmediateUI::Separator() {
    const UiRect rowRect{cursorX_, cursorY_, contentWidth_, 7.0f};
    const HoverEval hv = EvalHover(HashId("separator") ^ static_cast<uint64_t>(cursorY_), rowRect, true, false);
    const glm::vec4 baseLine{style_.windowBorder.r, style_.windowBorder.g, style_.windowBorder.b, 0.45f};
    const glm::vec4 lineCol = (hv.mix > 0.01f) ? glm::mix(baseLine, WithAlpha(style_.accent, 0.85f), hv.mix) : baseLine;
    EmitRect(vertices_, indices_, cursorX_, cursorY_ + 3.0f, contentWidth_, (hv.mix > 0.25f) ? 1.5f : 1.0f, lineCol);
    AdvanceCursor(7.0f + style_.itemSpacing * 0.5f);
}

bool ImmediateUI::CollapsingHeader(std::string_view label, bool defaultOpen) {
    const uint64_t id = HashId(label);
    auto [it, inserted] = collapsingHeaders_.emplace(id, defaultOpen);
    bool& isOpen = it->second;

    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const HoverEval hv = EvalHover(id, rect);
    if (hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0) {
        isOpen = !isOpen;
    }

    const glm::vec4 idleFill{0.13f, 0.155f, 0.21f, 0.92f};
    const glm::vec4 fill = glm::mix(idleFill, style_.widgetHover, hv.mix);
    const glm::vec4 border = glm::mix(WithAlpha(style_.windowBorder, 0.55f), style_.accent, hv.mix);
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, fill, border, 1.0f, false);

    if (isOpen || hv.mix > 0.05f) {
        const glm::vec4 barCol = isOpen ? style_.accent : WithAlpha(style_.accent, 0.45f * hv.mix);
        EmitRoundedBox(vertices_, indices_, rect.x + 2.0f, rect.y + 4.0f, 3.0f, rect.h - 8.0f, 1.5f, barCol,
                       glm::vec4{0.0f}, 0.0f, false);
    }

    const glm::vec4 chevronCol = isOpen ? style_.accent : glm::mix(style_.textMuted, style_.accent, hv.mix * 0.8f);
    EmitText(vertices_, indices_, isOpen ? "v" : ">", rect.x + 9.0f, rect.y + 4.0f, chevronCol,
             style_.textScale * 0.9f);
    EmitText(vertices_, indices_, SplitDisplayLabel(label), rect.x + 22.0f, rect.y + 4.0f, style_.textPrimary,
             style_.textScale);

    AdvanceCursor(style_.rowHeight + style_.itemSpacing);
    return isOpen;
}

bool ImmediateUI::IsBlockedByPopup() const noexcept {
    if (popupConsumedInputThisFrame_ || modalDrawnThisFrame_ || HasOpenModal()) {
        return true;
    }
    if (openDropdownId_ != 0 || openColorId_ != 0) {
        if (hasPendingOverlay_ && pendingOverlay_.popupRect.Contains(input_.mouseX, input_.mouseY)) {
            return true;
        }
        if (hasPendingColorOverlay_ && pendingColorOverlay_.popupRect.Contains(input_.mouseX, input_.mouseY)) {
            return true;
        }
        if (hasActivePopupRect_ && activePopupRect_.Contains(input_.mouseX, input_.mouseY)) {
            return true;
        }
    }
    return false;
}

bool ImmediateUI::IsMouseInClipArea() const noexcept {
    return !inScrollArea_ || scrollRect_.Contains(input_.mouseX, input_.mouseY);
}

bool ImmediateUI::Button(std::string_view label) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const HoverEval hv = EvalHover(HashId(label), rect);
    const bool pressed = hv.hovered && input_.mouseDown && activeDragWindowId_ == 0;
    const bool clicked = hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0;

    const glm::vec4 fill = pressed ? style_.widgetActive : glm::mix(style_.widgetBg, style_.widgetHover, hv.mix);
    const glm::vec4 border = glm::mix(style_.windowBorder, style_.accent, pressed ? 1.0f : hv.mix);
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, fill, border,
                   (hv.mix > 0.2f || pressed) ? 1.25f : 1.0f, false);
    if (hv.mix > 0.05f && !pressed) {
        EmitRect(vertices_, indices_, rect.x + 3.0f, rect.y + 1.5f, std::max(0.0f, rect.w - 6.0f), 1.0f,
                 glm::vec4{1.0f, 1.0f, 1.0f, 0.14f * hv.mix});
    }
    EmitText(vertices_, indices_, SplitDisplayLabel(label), rect.x + 8.0f, rect.y + 4.0f, style_.textPrimary,
             style_.textScale);

    AdvanceCursor(style_.rowHeight + style_.itemSpacing);
    return clicked;
}

bool ImmediateUI::Checkbox(std::string_view label, bool& state) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const HoverEval hv = EvalHover(HashId(label), rect);
    const bool clicked = hv.hovered && input_.mouseReleased && activeDragWindowId_ == 0;
    if (clicked) {
        state = !state;
    }

    if (hv.mix > 0.01f) {
        EmitHoverHighlight(vertices_, indices_, UiRect{rect.x - 3.0f, rect.y, rect.w + 6.0f, rect.h}, hv.mix,
                           style_.widgetRadius);
    }

    const float boxSize = 14.0f;
    const float boxY = rect.y + (rect.h - boxSize) * 0.5f;
    const glm::vec4 boxFill = state ? style_.accent : glm::mix(style_.widgetBg, style_.widgetHover, hv.mix);
    const glm::vec4 boxBorder = glm::mix(style_.windowBorder, style_.accent, state ? 1.0f : hv.mix);
    EmitRoundedBox(vertices_, indices_, rect.x, boxY, boxSize, boxSize, 3.5f, boxFill, boxBorder,
                   hv.mix > 0.25f ? 1.25f : 1.0f, false);

    if (state) {
        EmitRoundedBox(vertices_, indices_, rect.x + 3.5f, boxY + 3.5f, boxSize - 7.0f, boxSize - 7.0f, 2.0f,
                       style_.textPrimary, glm::vec4{0.0f}, 0.0f, false);
    }
    EmitText(vertices_, indices_, SplitDisplayLabel(label), rect.x + boxSize + 8.0f, rect.y + 4.0f, style_.textPrimary,
             style_.textScale);

    AdvanceCursor(style_.rowHeight + style_.itemSpacing);
    return clicked;
}

bool ImmediateUI::SliderFloat(std::string_view label, float& value, float minVal, float maxVal,
                              std::string_view valueFormat) {
    if (maxVal <= minVal) {
        maxVal = minVal + 1e-4f;
    }
    value = std::clamp(value, minVal, maxVal);
    const uint64_t id = HashId(label);

    char valBuf[48]{};
    FormatFloatLabel(valBuf, sizeof(valBuf), valueFormat, value);
    const float valW = MeasureTextWidth(valBuf, style_.textScale * 0.9f);

    EmitText(vertices_, indices_, SplitDisplayLabel(label), cursorX_, cursorY_, style_.textMuted,
             style_.textScale * 0.9f);
    EmitText(vertices_, indices_, valBuf, cursorX_ + std::max(0.0f, contentWidth_ - valW), cursorY_, style_.textPrimary,
             style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const float trackH = 16.0f;
    const UiRect trackHit{cursorX_, cursorY_, contentWidth_, trackH};
    const HoverEval hv = EvalHover(id, trackHit);

    if (activeSliderId_ == 0 && activeDragWindowId_ == 0 && hv.hovered && input_.mousePressed) {
        activeSliderId_ = id;
    }

    bool changed = false;
    if (activeSliderId_ == id) {
        if (input_.mouseDown || input_.mouseReleased) {
            const float padX = 5.0f;
            const float span = std::max(1.0f, trackHit.w - 2.0f * padX);
            const float t = std::clamp((input_.mouseX - (trackHit.x + padX)) / span, 0.0f, 1.0f);
            const float nextVal = minVal + t * (maxVal - minVal);
            if (std::abs(nextVal - value) > 1e-6f) {
                value = nextVal;
                changed = true;
            }
        }
        if (!input_.mouseDown || input_.mouseReleased) {
            activeSliderId_ = 0;
        }
    }

    const float t = std::clamp((value - minVal) / (maxVal - minVal), 0.0f, 1.0f);
    const float effectMix = (activeSliderId_ == id) ? 1.0f : hv.mix;
    const float barY = trackHit.y + 5.0f;
    const float barH = 6.0f;
    EmitRoundedBox(vertices_, indices_, trackHit.x, barY, trackHit.w, barH, 3.0f, style_.sliderTrack,
                   glm::mix(style_.windowBorder, style_.accent, effectMix), 1.0f, false);

    const float fillW = std::max(4.0f, t * trackHit.w);
    EmitRoundedBox(vertices_, indices_, trackHit.x, barY, fillW, barH, 3.0f,
                   glm::mix(style_.widgetActive, style_.accent, effectMix), glm::vec4{0.0f}, 0.0f, false);

    const float thumbSize = 11.0f + 2.5f * effectMix;
    const float thumbX =
        std::clamp(trackHit.x + t * (trackHit.w - thumbSize), trackHit.x, trackHit.x + trackHit.w - thumbSize);
    const float thumbY = trackHit.y + (trackHit.h - thumbSize) * 0.5f;
    EmitRoundedBox(vertices_, indices_, thumbX, thumbY, thumbSize, thumbSize, thumbSize * 0.5f, style_.textPrimary,
                   style_.accent, 1.0f + 0.75f * effectMix, false);

    AdvanceCursor(trackH + style_.itemSpacing);
    return changed;
}

bool ImmediateUI::ColorEdit3(std::string_view label, glm::vec3& color) {
    color = glm::clamp(color, glm::vec3{0.0f}, glm::vec3{1.0f});
    const uint64_t id = HashId(label);
    const UiRect rowRect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const float swatchW = 48.0f;
    const float swatchH = 18.0f;
    const UiRect swatchRect{rowRect.x + rowRect.w - swatchW, rowRect.y + (rowRect.h - swatchH) * 0.5f, swatchW,
                            swatchH};

    const HoverEval rowHover = EvalHover(id ^ 0x524F57ULL, rowRect, true, false);
    const HoverEval swatchHover = EvalHover(id, swatchRect, true, true);
    const bool isOpen = (openColorId_ == id);
    bool colorChanged = false;

    const float svSize = 132.0f;
    const float hueW = 18.0f;
    const float pad = 8.0f;
    const float popW = pad * 3.0f + svSize + hueW;
    const float popH = pad * 2.0f + svSize + 22.0f;
    const float popX = std::clamp(swatchRect.x + swatchRect.w - popW, 4.0f,
                                  std::max(4.0f, static_cast<float>(viewportWidth_) - popW - 4.0f));
    const float popY = (swatchRect.y + swatchRect.h + 4.0f + popH <= static_cast<float>(viewportHeight_))
                           ? (swatchRect.y + swatchRect.h + 4.0f)
                           : std::max(4.0f, swatchRect.y - popH - 4.0f);
    const UiRect popupRect{popX, popY, popW, popH};
    const UiRect svRect{popX + pad, popY + pad, svSize, svSize};
    const UiRect hueRect{popX + pad * 2.0f + svSize, popY + pad, hueW, svSize};

    glm::vec3& storedHsv = colorPickerHsv_.try_emplace(id, RgbToHsv(color)).first->second;
    if (!isOpen) {
        const glm::vec3 extHsv = RgbToHsv(color);
        if (extHsv.y > 0.01f) {
            storedHsv.x = extHsv.x;
        }
        if (extHsv.z > 0.01f) {
            storedHsv.y = extHsv.y;
        }
        storedHsv.z = extHsv.z;
    }

    if (isOpen && !HasOpenModal()) {
        hitRects_.push_back(popupRect);
        if (input_.mousePressed) {
            if (svRect.Contains(input_.mouseX, input_.mouseY)) {
                activeColorSvId_ = id;
            } else if (hueRect.Contains(input_.mouseX, input_.mouseY)) {
                activeColorHueId_ = id;
            } else if (!popupRect.Contains(input_.mouseX, input_.mouseY) && !swatchHover.hovered) {
                openColorId_ = 0;
                colorToggledThisFrame_ = true;
            }
        }
        if (activeColorSvId_ == id && (input_.mouseDown || input_.mouseReleased)) {
            storedHsv.y = std::clamp((input_.mouseX - svRect.x) / svRect.w, 0.0f, 1.0f);
            storedHsv.z = 1.0f - std::clamp((input_.mouseY - svRect.y) / svRect.h, 0.0f, 1.0f);
            const glm::vec3 nextRgb = HsvToRgb(storedHsv);
            if (glm::length(nextRgb - color) > 1e-4f) {
                color = nextRgb;
                colorChanged = true;
            }
            if (!input_.mouseDown || input_.mouseReleased) {
                activeColorSvId_ = 0;
            }
        }
        if (activeColorHueId_ == id && (input_.mouseDown || input_.mouseReleased)) {
            storedHsv.x = std::clamp((input_.mouseY - hueRect.y) / hueRect.h, 0.0f, 0.9999f);
            const glm::vec3 nextRgb = HsvToRgb(storedHsv);
            if (glm::length(nextRgb - color) > 1e-4f) {
                color = nextRgb;
                colorChanged = true;
            }
            if (!input_.mouseDown || input_.mouseReleased) {
                activeColorHueId_ = 0;
            }
        }
    }

    if (swatchHover.hovered && input_.mouseReleased && !colorToggledThisFrame_ && !IsBlockedByPopup()) {
        openColorId_ = isOpen ? 0 : id;
        openDropdownId_ = 0;
        colorToggledThisFrame_ = true;
    }

    const bool nowOpen = (openColorId_ == id);
    if (rowHover.mix > 0.01f) {
        EmitHoverHighlight(vertices_, indices_, UiRect{rowRect.x - 3.0f, rowRect.y, rowRect.w + 6.0f, rowRect.h},
                           rowHover.mix * 0.8f, style_.widgetRadius);
    }
    EmitText(vertices_, indices_, SplitDisplayLabel(label), rowRect.x, rowRect.y + 4.0f, style_.textPrimary,
             style_.textScale);
    const glm::vec4 swatchBorder = glm::mix(style_.windowBorder, style_.accent, nowOpen ? 1.0f : swatchHover.mix);
    EmitRoundedBox(vertices_, indices_, swatchRect.x, swatchRect.y, swatchRect.w, swatchRect.h, 4.0f,
                   glm::vec4{color, 1.0f}, swatchBorder, 1.25f + 0.5f * (nowOpen ? 1.0f : swatchHover.mix), false);

    if (nowOpen) {
        hasPendingColorOverlay_ = true;
        hasActivePopupRect_ = true;
        activePopupRect_ = popupRect;
        pendingColorOverlay_ = PendingColorOverlay{.id = id,
                                                   .anchorRect = swatchRect,
                                                   .popupRect = popupRect,
                                                   .svRect = svRect,
                                                   .hueRect = hueRect,
                                                   .hsv = storedHsv,
                                                   .rgb = color};
    }

    lastItemRect_ = rowRect;
    lastItemHovered_ = rowHover.hovered || swatchHover.hovered;
    AdvanceCursor(style_.rowHeight + style_.itemSpacing);
    return colorChanged;
}

void ImmediateUI::Sparkline(std::string_view label, std::span<const float> values, float minVal, float maxVal,
                            float height, const glm::vec4& lineColor) {
    float curVal = 0.0f;
    float lo = minVal;
    float hi = maxVal;
    if (!values.empty()) {
        curVal = values.back();
        if (hi <= lo) {
            lo = values.front();
            hi = values.front();
            for (float v : values) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            const float padSpan = std::max(1.0f, (hi - lo) * 0.15f);
            lo = std::max(0.0f, lo - padSpan * 0.25f);
            hi += padSpan * 0.25f;
        }
    } else {
        hi = 1.0f;
    }
    const float span = std::max(1e-4f, hi - lo);

    const float labelY = cursorY_;
    cursorY_ += 17.0f;

    const UiRect box{cursorX_, cursorY_, contentWidth_, std::max(24.0f, height)};
    const HoverEval hv = EvalHover(HashId(label), box);

    const glm::vec4 boxBorder = glm::mix(style_.windowBorder, style_.accent, hv.mix);
    EmitRoundedBox(vertices_, indices_, box.x, box.y, box.w, box.h, style_.widgetRadius,
                   glm::vec4{0.06f, 0.07f, 0.095f, 0.95f}, boxBorder, 1.0f + 0.3f * hv.mix, false);

    const float midY = box.y + box.h * 0.5f;
    EmitRect(vertices_, indices_, box.x + 4.0f, midY, std::max(0.0f, box.w - 8.0f), 1.0f,
             WithAlpha(style_.windowBorder, 0.25f));

    float displayVal = curVal;
    if (values.size() >= 2) {
        const float plotX = box.x + 4.0f;
        const float plotY = box.y + 4.0f;
        const float plotW = std::max(4.0f, box.w - 8.0f);
        const float plotH = std::max(4.0f, box.h - 8.0f);
        const float bottomY = plotY + plotH;
        const glm::vec4 topFill = WithAlpha(lineColor, 0.26f + 0.12f * hv.mix);
        const glm::vec4 botFill = WithAlpha(lineColor, 0.02f);

        for (size_t i = 0; i + 1 < values.size(); ++i) {
            const float t0 = static_cast<float>(i) / static_cast<float>(values.size() - 1);
            const float t1 = static_cast<float>(i + 1) / static_cast<float>(values.size() - 1);
            const float n0 = std::clamp((values[i] - lo) / span, 0.0f, 1.0f);
            const float n1 = std::clamp((values[i + 1] - lo) / span, 0.0f, 1.0f);
            const glm::vec2 p0{plotX + t0 * plotW, bottomY - n0 * plotH};
            const glm::vec2 p1{plotX + t1 * plotW, bottomY - n1 * plotH};

            const uint32_t base = static_cast<uint32_t>(vertices_.size());
            vertices_.push_back(UiVertex{p0, {0.0f, 0.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            vertices_.push_back(UiVertex{p1, {1.0f, 0.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            vertices_.push_back(UiVertex{{p1.x, bottomY}, {1.0f, 1.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            vertices_.push_back(UiVertex{{p0.x, bottomY}, {0.0f, 1.0f}, topFill, glm::vec4{0.0f}, botFill, 0.0f});
            indices_.push_back(base + 0);
            indices_.push_back(base + 1);
            indices_.push_back(base + 2);
            indices_.push_back(base + 0);
            indices_.push_back(base + 2);
            indices_.push_back(base + 3);

            EmitLineSegment(vertices_, indices_, p0, p1, 1.75f + 0.35f * hv.mix, lineColor);
        }

        if (hv.hovered && AreHoverEffectsEnabled()) {
            const float relX = std::clamp((input_.mouseX - plotX) / plotW, 0.0f, 1.0f);
            const size_t nearestIdx = std::min(
                values.size() - 1, static_cast<size_t>(std::lround(relX * static_cast<float>(values.size() - 1))));
            displayVal = values[nearestIdx];
            const float normV = std::clamp((displayVal - lo) / span, 0.0f, 1.0f);
            const float snapX =
                plotX + (static_cast<float>(nearestIdx) / static_cast<float>(values.size() - 1)) * plotW;
            const float snapY = bottomY - normV * plotH;
            EmitRect(vertices_, indices_, snapX - 0.5f, plotY, 1.0f, plotH, WithAlpha(style_.accent, 0.65f));
            EmitRoundedBox(vertices_, indices_, snapX - 3.5f, snapY - 3.5f, 7.0f, 7.0f, 3.5f, style_.textPrimary,
                           lineColor, 1.5f, false);
        }
    }

    char valBuf[32]{};
    std::snprintf(valBuf, sizeof(valBuf), "%.1f", static_cast<double>(displayVal));
    const float valW = MeasureTextWidth(valBuf, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, SplitDisplayLabel(label), cursorX_, labelY, style_.textMuted,
             style_.textScale * 0.9f);
    EmitText(vertices_, indices_, valBuf, cursorX_ + std::max(0.0f, contentWidth_ - valW), labelY, lineColor,
             style_.textScale * 0.9f);

    AdvanceCursor(box.h + style_.itemSpacing);
}

} // namespace bgl::ui
