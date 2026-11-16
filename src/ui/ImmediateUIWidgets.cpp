/**
 * @file ImmediateUIWidgets.cpp
 * @brief Immediate-mode windowing, SDF widgets, deferred popups, progress overlays, and bgl::ui::UI facade.
 */
#include "ui/ImmediateUI.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bgl::ui {

namespace {

ImmediateUI* g_activeUiContext = nullptr;

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
    const bool headerHovered = !IsBlockedByPopup() && headerRect.Contains(input_.mouseX, input_.mouseY);
    if (activeDragWindowId_ == 0 && activeSliderId_ == 0 && activeScrollbarId_ == 0 && activeColorSvId_ == 0 &&
        activeColorHueId_ == 0 && input_.mousePressed && headerHovered) {
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

    EmitRoundedBox(vertices_, indices_, ax + 1.0f, ay + 1.0f, width - 2.0f, style_.headerHeight - 1.0f,
                   std::max(1.0f, style_.cornerRadius - 1.0f), style_.headerBg, glm::vec4{0.0f}, 0.0f, false);
    EmitRoundedBox(vertices_, indices_, ax + 6.0f, ay + 7.0f, 3.0f, style_.headerHeight - 14.0f, 1.5f, style_.accent,
                   glm::vec4{0.0f}, 0.0f, false);
    EmitText(vertices_, indices_, title, ax + style_.padding + 4.0f, ay + 6.0f, style_.textPrimary, style_.textScale);

    if (activeDragWindowId_ == currentWindowId_) {
        hitRects_.push_back(UiRect{ax, ay, width, style_.headerHeight});
    }
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

    auto patchSdfBox = [&](size_t vOff, float rx, float ry, float rw, float rh, float pad, float radius, float param4) {
        if (vOff + 4 > vertices_.size()) {
            return;
        }
        const float hw = rw * 0.5f;
        const float hh = rh * 0.5f;
        const glm::vec4 sdfVec{hw, hh, radius, param4};
        vertices_[vOff + 0].pos = {rx - pad, ry - pad};
        vertices_[vOff + 1].pos = {rx + rw + pad, ry - pad};
        vertices_[vOff + 2].pos = {rx + rw + pad, ry + rh + pad};
        vertices_[vOff + 3].pos = {rx - pad, ry + rh + pad};
        vertices_[vOff + 0].uv = {-hw - pad, -hh - pad};
        vertices_[vOff + 1].uv = {hw + pad, -hh - pad};
        vertices_[vOff + 2].uv = {hw + pad, hh + pad};
        vertices_[vOff + 3].uv = {-hw - pad, hh + pad};
        for (size_t i = 0; i < 4; ++i) {
            vertices_[vOff + i].sdfRect = sdfVec;
        }
    };

    patchSdfBox(windowShadowVertexOffset_, x, y + 3.0f, w, h, style_.shadowBlur, style_.cornerRadius,
                style_.shadowBlur);
    patchSdfBox(windowBgVertexOffset_, x, y, w, h, 1.5f, style_.cornerRadius, 1.0f);

    lastCompletedWindowRect_ = currentWindowRect_;
    hitRects_.push_back(currentWindowRect_);
    inWindow_ = false;
}

void ImmediateUI::Label(std::string_view text, const glm::vec4& color) {
    const float h = 18.0f;
    EmitText(vertices_, indices_, text, cursorX_, cursorY_ + 2.0f, color, style_.textScale);
    cursorY_ += h + style_.itemSpacing;
}

void ImmediateUI::Separator() {
    EmitRect(vertices_, indices_, cursorX_, cursorY_ + 3.0f, contentWidth_, 1.0f,
             glm::vec4{style_.windowBorder.r, style_.windowBorder.g, style_.windowBorder.b, 0.45f});
    cursorY_ += 7.0f + style_.itemSpacing * 0.5f;
}

bool ImmediateUI::CollapsingHeader(std::string_view label, bool defaultOpen) {
    const uint64_t id = HashId(label);
    auto [it, inserted] = collapsingHeaders_.emplace(id, defaultOpen);
    bool& isOpen = it->second;

    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    if (hovered && input_.mouseReleased && activeDragWindowId_ == 0) {
        isOpen = !isOpen;
    }

    const glm::vec4 fill = hovered ? style_.widgetHover : glm::vec4{0.13f, 0.155f, 0.21f, 0.92f};
    const glm::vec4 border = hovered ? style_.accent : WithAlpha(style_.windowBorder, 0.55f);
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, fill, border, 1.0f, false);

    if (isOpen) {
        EmitRoundedBox(vertices_, indices_, rect.x + 2.0f, rect.y + 4.0f, 3.0f, rect.h - 8.0f, 1.5f, style_.accent,
                       glm::vec4{0.0f}, 0.0f, false);
    }

    EmitText(vertices_, indices_, isOpen ? "v" : ">", rect.x + 9.0f, rect.y + 4.0f,
             isOpen ? style_.accent : style_.textMuted, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, label, rect.x + 22.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return isOpen;
}

bool ImmediateUI::IsBlockedByPopup() const noexcept {
    if (popupConsumedInputThisFrame_) {
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
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    const bool pressed = hovered && input_.mouseDown && activeDragWindowId_ == 0;
    const bool clicked = hovered && input_.mouseReleased && activeDragWindowId_ == 0;

    const glm::vec4 fill = pressed ? style_.widgetActive : (hovered ? style_.widgetHover : style_.widgetBg);
    const glm::vec4 border = hovered ? style_.accent : style_.windowBorder;
    EmitRoundedBox(vertices_, indices_, rect.x, rect.y, rect.w, rect.h, style_.widgetRadius, fill, border, 1.0f, false);
    EmitText(vertices_, indices_, label, rect.x + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return clicked;
}

bool ImmediateUI::Checkbox(std::string_view label, bool& state) {
    const UiRect rect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && rect.Contains(input_.mouseX, input_.mouseY);
    const bool clicked = hovered && input_.mouseReleased && activeDragWindowId_ == 0;
    if (clicked) {
        state = !state;
    }

    const float boxSize = 14.0f;
    const float boxY = rect.y + (rect.h - boxSize) * 0.5f;
    const glm::vec4 boxFill = state ? style_.accent : (hovered ? style_.widgetHover : style_.widgetBg);
    const glm::vec4 boxBorder = hovered ? style_.accent : style_.windowBorder;
    EmitRoundedBox(vertices_, indices_, rect.x, boxY, boxSize, boxSize, 3.5f, boxFill, boxBorder, 1.0f, false);

    if (state) {
        EmitRoundedBox(vertices_, indices_, rect.x + 3.5f, boxY + 3.5f, boxSize - 7.0f, boxSize - 7.0f, 2.0f,
                       style_.textPrimary, glm::vec4{0.0f}, 0.0f, false);
    }
    EmitText(vertices_, indices_, label, rect.x + boxSize + 8.0f, rect.y + 4.0f, style_.textPrimary, style_.textScale);

    hitRects_.push_back(rect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
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

    EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, valBuf, cursorX_ + std::max(0.0f, contentWidth_ - valW), cursorY_, style_.textPrimary,
             style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const float trackH = 16.0f;
    const UiRect trackHit{cursorX_, cursorY_, contentWidth_, trackH};
    const bool hovered = IsMouseInClipArea() && !IsBlockedByPopup() && trackHit.Contains(input_.mouseX, input_.mouseY);

    if (activeSliderId_ == 0 && activeDragWindowId_ == 0 && hovered && input_.mousePressed) {
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
    const bool activeOrHover = (activeSliderId_ == id) || hovered;
    const float barY = trackHit.y + 5.0f;
    const float barH = 6.0f;
    EmitRoundedBox(vertices_, indices_, trackHit.x, barY, trackHit.w, barH, 3.0f, style_.sliderTrack,
                   activeOrHover ? style_.accent : style_.windowBorder, 1.0f, false);

    const float fillW = std::max(4.0f, t * trackHit.w);
    EmitRoundedBox(vertices_, indices_, trackHit.x, barY, fillW, barH, 3.0f,
                   activeOrHover ? style_.accent : style_.widgetActive, glm::vec4{0.0f}, 0.0f, false);

    const float thumbSize = activeOrHover ? 13.0f : 11.0f;
    const float thumbX =
        std::clamp(trackHit.x + t * (trackHit.w - thumbSize), trackHit.x, trackHit.x + trackHit.w - thumbSize);
    const float thumbY = trackHit.y + (trackHit.h - thumbSize) * 0.5f;
    EmitRoundedBox(vertices_, indices_, thumbX, thumbY, thumbSize, thumbSize, thumbSize * 0.5f, style_.textPrimary,
                   style_.accent, activeOrHover ? 1.5f : 1.0f, false);

    hitRects_.push_back(trackHit);
    cursorY_ += trackH + style_.itemSpacing;
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

    const bool hovered = IsMouseInClipArea() && swatchRect.Contains(input_.mouseX, input_.mouseY);
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
        // Synchronize external color changes cleanly without discarding saved hue on grayscale colors
        const glm::vec3 extHsv = RgbToHsv(color);
        if (extHsv.y > 0.01f) {
            storedHsv.x = extHsv.x;
        }
        if (extHsv.z > 0.01f) {
            storedHsv.y = extHsv.y;
        }
        storedHsv.z = extHsv.z;
    }

    if (isOpen) {
        hitRects_.push_back(popupRect);
        if (input_.mousePressed) {
            if (svRect.Contains(input_.mouseX, input_.mouseY)) {
                activeColorSvId_ = id;
            } else if (hueRect.Contains(input_.mouseX, input_.mouseY)) {
                activeColorHueId_ = id;
            } else if (!popupRect.Contains(input_.mouseX, input_.mouseY) && !hovered) {
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

    if (hovered && input_.mouseReleased && !colorToggledThisFrame_ && !IsBlockedByPopup()) {
        openColorId_ = isOpen ? 0 : id;
        openDropdownId_ = 0;
        colorToggledThisFrame_ = true;
    }

    const bool nowOpen = (openColorId_ == id);
    EmitText(vertices_, indices_, label, rowRect.x, rowRect.y + 4.0f, style_.textPrimary, style_.textScale);
    EmitRoundedBox(vertices_, indices_, swatchRect.x, swatchRect.y, swatchRect.w, swatchRect.h, 4.0f,
                   glm::vec4{color, 1.0f}, (hovered || nowOpen) ? style_.accent : style_.windowBorder, 1.5f, false);

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

    hitRects_.push_back(swatchRect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
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

    char valBuf[32]{};
    std::snprintf(valBuf, sizeof(valBuf), "%.1f", static_cast<double>(curVal));
    const float valW = MeasureTextWidth(valBuf, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
    EmitText(vertices_, indices_, valBuf, cursorX_ + std::max(0.0f, contentWidth_ - valW), cursorY_, lineColor,
             style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const UiRect box{cursorX_, cursorY_, contentWidth_, std::max(24.0f, height)};
    EmitRoundedBox(vertices_, indices_, box.x, box.y, box.w, box.h, style_.widgetRadius,
                   glm::vec4{0.06f, 0.07f, 0.095f, 0.95f}, style_.windowBorder, 1.0f, false);

    const float midY = box.y + box.h * 0.5f;
    EmitRect(vertices_, indices_, box.x + 4.0f, midY, std::max(0.0f, box.w - 8.0f), 1.0f,
             WithAlpha(style_.windowBorder, 0.25f));

    if (values.size() >= 2) {
        const float plotX = box.x + 4.0f;
        const float plotY = box.y + 4.0f;
        const float plotW = std::max(4.0f, box.w - 8.0f);
        const float plotH = std::max(4.0f, box.h - 8.0f);
        const float bottomY = plotY + plotH;
        const glm::vec4 topFill = WithAlpha(lineColor, 0.26f);
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

            EmitLineSegment(vertices_, indices_, p0, p1, 1.75f, lineColor);
        }
    }

    hitRects_.push_back(box);
    cursorY_ += box.h + style_.itemSpacing;
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

    EmitText(vertices_, indices_, label, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.9f);
    cursorY_ += 17.0f;

    const UiRect anchorRect{cursorX_, cursorY_, contentWidth_, style_.rowHeight};
    const bool isOpen = (openDropdownId_ == id);
    const bool hoveredAnchor = IsMouseInClipArea() && anchorRect.Contains(input_.mouseX, input_.mouseY);

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
    if (isOpen) {
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
            } else if (!hoveredAnchor && !popupRect.Contains(input_.mouseX, input_.mouseY)) {
                openDropdownId_ = 0;
                dropdownToggledThisFrame_ = true;
                popupConsumedInputThisFrame_ = true;
            }
        }
    }

    if (hoveredAnchor && input_.mouseReleased && !dropdownToggledThisFrame_ && activeDragWindowId_ == 0) {
        openDropdownId_ = isOpen ? 0 : id;
        openColorId_ = 0;
        dropdownToggledThisFrame_ = true;
        if (!isOpen && scrollable) {
            scrollOffset =
                std::clamp((static_cast<float>(selectedIndex) + 0.5f) * itemH - popupH * 0.5f, 0.0f, maxScroll);
        }
    }

    const bool nowOpen = (openDropdownId_ == id);
    const glm::vec4 fill = (hoveredAnchor || nowOpen) ? style_.widgetHover : style_.widgetBg;
    const glm::vec4 border = nowOpen ? style_.accent : style_.windowBorder;
    EmitRoundedBox(vertices_, indices_, anchorRect.x, anchorRect.y, anchorRect.w, anchorRect.h, style_.widgetRadius,
                   fill, border, 1.0f, false);

    const std::string_view currentRaw = options[selectedIndex] ? std::string_view{options[selectedIndex]} : "";
    const std::string currentText = EllipsizeText(currentRaw, std::max(24.0f, anchorRect.w - 30.0f), style_.textScale);
    EmitText(vertices_, indices_, currentText, anchorRect.x + 8.0f, anchorRect.y + 4.0f, style_.textPrimary,
             style_.textScale);
    EmitText(vertices_, indices_, nowOpen ? "^" : "v", anchorRect.x + anchorRect.w - 18.0f, anchorRect.y + 4.0f,
             style_.textMuted, style_.textScale * 0.85f);

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

    hitRects_.push_back(anchorRect);
    cursorY_ += style_.rowHeight + style_.itemSpacing;
    return selectionChanged;
}

void ImmediateUI::ProgressBar(float fraction, std::string_view statusText, float height, const glm::vec4& fillColor) {
    if (!statusText.empty()) {
        const std::string clipped = EllipsizeText(statusText, contentWidth_, style_.textScale * 0.88f);
        EmitText(vertices_, indices_, clipped, cursorX_, cursorY_, style_.textMuted, style_.textScale * 0.88f);
        cursorY_ += 16.0f;
    }

    const float barH = std::max(14.0f, height);
    const float radius = std::min(style_.widgetRadius, barH * 0.5f);
    const UiRect trackRect{cursorX_, cursorY_, contentWidth_, barH};
    EmitRoundedBox(vertices_, indices_, trackRect.x, trackRect.y, trackRect.w, trackRect.h, radius, style_.sliderTrack,
                   WithAlpha(style_.windowBorder, 0.72f), 1.0f, false);

    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    const glm::vec4 barColor = (fillColor.a > 0.001f) ? fillColor : style_.accent;
    if (clamped > 0.002f) {
        const float fillW = std::max(std::min(trackRect.w, barH * 0.7f), trackRect.w * clamped);
        EmitRoundedBox(vertices_, indices_, trackRect.x, trackRect.y, fillW, trackRect.h, radius, barColor,
                       glm::vec4{0.0f}, 0.0f, false);
        if (fillW > 10.0f && barH >= 12.0f) {
            EmitRoundedBox(vertices_, indices_, trackRect.x + 2.0f, trackRect.y + 1.5f, fillW - 4.0f, 3.0f, 1.5f,
                           glm::vec4{1.0f, 1.0f, 1.0f, 0.22f}, glm::vec4{0.0f}, 0.0f, false);
        }
    }

    char pctBuf[16]{};
    const int pct = static_cast<int>(std::lround(clamped * 100.0f));
    std::snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
    const float pctScale = style_.textScale * 0.82f;
    const float pctW = MeasureTextWidth(pctBuf, pctScale);
    EmitText(vertices_, indices_, pctBuf, trackRect.x + (trackRect.w - pctW) * 0.5f,
             trackRect.y + (barH - 14.0f) * 0.5f, style_.textPrimary, pctScale);

    hitRects_.push_back(trackRect);
    cursorY_ += barH + style_.itemSpacing;
}

void ImmediateUI::DrawLoadingBanner(std::string_view title, float fraction, std::string_view detailText, bool isError,
                                    float alphaMultiplier) {
    const float alpha = std::clamp(alphaMultiplier, 0.0f, 1.0f);
    if (alpha <= 0.01f) {
        return;
    }

    const float vpW = static_cast<float>(viewportWidth_);
    const float vpH = static_cast<float>(viewportHeight_);
    const float cardW = std::clamp(vpW - 36.0f, 240.0f, 440.0f);
    const float cardH = 68.0f;
    const float cardX = (vpW - cardW) * 0.5f;
    const float cardY = std::max(12.0f, vpH - cardH - 22.0f);

    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    glm::vec4 barAccent = style_.accent;
    if (isError) {
        barAccent = glm::vec4{0.92f, 0.28f, 0.34f, 1.0f};
    } else if (clamped >= 0.999f) {
        barAccent = glm::vec4{0.22f, 0.82f, 0.52f, 1.0f};
    }
    barAccent.a *= alpha;

    const glm::vec4 bg = WithAlpha(style_.popupBg, style_.popupBg.a * alpha);
    const glm::vec4 shadow = WithAlpha(style_.shadowColor, style_.shadowColor.a * alpha);
    const glm::vec4 primaryText = WithAlpha(style_.textPrimary, style_.textPrimary.a * alpha);
    const glm::vec4 mutedText = WithAlpha(style_.textMuted, style_.textMuted.a * alpha);
    const glm::vec4 trackFill = WithAlpha(style_.sliderTrack, style_.sliderTrack.a * alpha);

    EmitShadowBox(overlayVertices_, overlayIndices_, cardX, cardY + 3.0f, cardW, cardH, style_.cornerRadius, 12.0f,
                  shadow);
    EmitRoundedBox(overlayVertices_, overlayIndices_, cardX, cardY, cardW, cardH, style_.cornerRadius, bg, barAccent,
                   1.15f, true);

    const std::string titleClipped = EllipsizeText(title, cardW - 76.0f, style_.textScale);
    EmitText(overlayVertices_, overlayIndices_, titleClipped, cardX + 14.0f, cardY + 9.0f, primaryText,
             style_.textScale);

    char rightBuf[24]{};
    if (isError) {
        std::snprintf(rightBuf, sizeof(rightBuf), "Error");
    } else {
        std::snprintf(rightBuf, sizeof(rightBuf), "%d%%", static_cast<int>(std::lround(clamped * 100.0f)));
    }
    const float rightScale = style_.textScale * 0.92f;
    const float rightW = MeasureTextWidth(rightBuf, rightScale);
    EmitText(overlayVertices_, overlayIndices_, rightBuf, cardX + cardW - 14.0f - rightW, cardY + 9.0f, barAccent,
             rightScale);

    const float barX = cardX + 14.0f;
    const float barY = cardY + 31.0f;
    const float barW = cardW - 28.0f;
    const float barH = 10.0f;
    EmitRoundedBox(overlayVertices_, overlayIndices_, barX, barY, barW, barH, 5.0f, trackFill,
                   WithAlpha(style_.windowBorder, 0.55f * alpha), 1.0f, false);
    if (clamped > 0.002f) {
        const float fillW = std::max(std::min(barW, 10.0f), barW * clamped);
        EmitRoundedBox(overlayVertices_, overlayIndices_, barX, barY, fillW, barH, 5.0f, barAccent, glm::vec4{0.0f},
                       0.0f, false);
    }

    if (!detailText.empty()) {
        const float detailScale = style_.textScale * 0.84f;
        const std::string detailClipped = EllipsizeText(detailText, barW, detailScale);
        EmitText(overlayVertices_, overlayIndices_, detailClipped, barX, cardY + 46.0f, mutedText, detailScale);
    }
}

void ImmediateUI::BeginScrollArea(std::string_view id, float maxHeight) {
    if (inScrollArea_) {
        EndScrollArea();
    }
    const uint64_t hash = HashId(id);
    const float clampedH = std::max(40.0f, maxHeight);
    scrollRect_ = UiRect{cursorX_, cursorY_, contentWidth_, clampedH};

    float& scrollOffset = scrollOffsets_[hash];
    const float prevContentH = scrollContentHeights_[hash];
    const float maxScroll = std::max(0.0f, prevContentH - clampedH);

    if (!IsBlockedByPopup() && scrollRect_.Contains(input_.mouseX, input_.mouseY) &&
        std::abs(input_.wheelDelta) > 1e-4f) {
        scrollOffset -= input_.wheelDelta * 26.0f;
    }

    const UiRect trackRect{scrollRect_.x + scrollRect_.w - 8.0f, scrollRect_.y + 2.0f, 6.0f,
                           std::max(12.0f, clampedH - 4.0f)};
    if (maxScroll > 1.0f && activeScrollbarId_ == 0 && activeDragWindowId_ == 0 && input_.mousePressed &&
        trackRect.Contains(input_.mouseX, input_.mouseY)) {
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

    EmitRoundedBox(vertices_, indices_, scrollRect_.x, scrollRect_.y, scrollRect_.w, scrollRect_.h, style_.widgetRadius,
                   glm::vec4{0.055f, 0.065f, 0.088f, 0.85f}, WithAlpha(style_.windowBorder, 0.55f), 1.0f, false);

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
        const bool thumbActive = (activeScrollbarId_ == activeScrollId_);
        EmitRoundedBox(vertices_, indices_, trackX, thumbY, 4.0f, thumbH, 2.0f,
                       thumbActive ? style_.accent : style_.windowBorder, glm::vec4{0.0f}, 0.0f, false);
    }

    hitRects_.push_back(scrollRect_);
}

void ImmediateUI::DrawFloatingBadge(float x, float y, std::string_view text, const glm::vec4& accentColor) {
    if (text.empty()) {
        return;
    }
    const float scale = style_.textScale * 0.85f;
    const float textW = MeasureTextWidth(text, scale);
    const float badgeW = std::max(36.0f, textW + 18.0f);
    const float badgeH = 20.0f;
    EmitShadowBox(overlayVertices_, overlayIndices_, x, y + 1.5f, badgeW, badgeH, 5.0f, 6.0f, style_.shadowColor);
    EmitRoundedBox(overlayVertices_, overlayIndices_, x, y, badgeW, badgeH, 5.0f, style_.popupBg, accentColor, 1.25f,
                   false);
    EmitRoundedBox(overlayVertices_, overlayIndices_, x + 5.0f, y + 6.0f, 6.0f, 8.0f, 3.0f, accentColor,
                   glm::vec4{0.0f}, 0.0f, false);
    EmitText(overlayVertices_, overlayIndices_, text, x + 14.0f, y + 3.0f, style_.textPrimary, scale);
}

void ImmediateUI::EmitColorPickerOverlay() {
    if (!hasPendingColorOverlay_ || openColorId_ != pendingColorOverlay_.id) {
        return;
    }
    const auto& cp = pendingColorOverlay_;
    EmitShadowBox(vertices_, indices_, cp.popupRect.x, cp.popupRect.y + 3.0f, cp.popupRect.w, cp.popupRect.h,
                  style_.cornerRadius, 12.0f, style_.shadowColor);
    EmitRoundedBox(vertices_, indices_, cp.popupRect.x, cp.popupRect.y, cp.popupRect.w, cp.popupRect.h,
                   style_.cornerRadius, style_.popupBg, style_.accent, 1.0f, false);

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
    EmitShadowBox(vertices_, indices_, ov.popupRect.x, ov.popupRect.y + 3.0f, ov.popupRect.w, ov.popupRect.h,
                  style_.widgetRadius, 10.0f, style_.shadowColor);
    EmitRoundedBox(vertices_, indices_, ov.popupRect.x, ov.popupRect.y, ov.popupRect.w, ov.popupRect.h,
                   style_.widgetRadius, style_.popupBg, style_.accent, 1.0f, false);

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
        const bool itemHovered = mouseInsideClip && itemRect.Contains(input_.mouseX, input_.mouseY);
        const bool isSelected = (i == ov.selectedIndex);

        if (itemHovered || isSelected) {
            const glm::vec4 itemColor = itemHovered ? style_.widgetHover : style_.widgetActive;
            EmitRoundedBox(vertices_, indices_, itemRect.x, itemRect.y, itemRect.w, itemRect.h, 3.0f, itemColor,
                           glm::vec4{0.0f}, 0.0f, false);
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

} // namespace UI

} // namespace bgl::ui
